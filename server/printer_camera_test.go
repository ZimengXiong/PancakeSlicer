package main

import (
	"bufio"
	"context"
	"crypto/ed25519"
	"crypto/md5"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/textproto"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

func TestCameraIntermediateRecovery(t *testing.T) {
	const serial = "H2D-example"
	root := issueCameraTestCert(t, "Bambu root", true, nil)
	intermediate := issueCameraTestCert(t, "Bambu intermediate", true, &root)
	mqtt := issueCameraTestCert(t, serial, false, &intermediate)
	camera := issueCameraTestCert(t, serial, false, &intermediate)
	wrongSerial := issueCameraTestCert(t, "different-serial", false, &intermediate)
	untrusted := issueCameraTestCert(t, serial, false, nil)
	roots := x509.NewCertPool()
	roots.AddCert(root.Leaf)
	p := &PrinterConfig{TLS: "bambu-ca", Serial: serial, bambuRoots: roots}

	for _, tc := range []struct {
		name        string
		cameraChain []*x509.Certificate
		mqtt        tls.Certificate
		wantMQTT    bool
		wantErr     string
	}{
		{name: "complete camera chain", cameraChain: []*x509.Certificate{camera.Leaf, intermediate.Leaf}},
		{name: "recover camera intermediate", cameraChain: []*x509.Certificate{camera.Leaf}, mqtt: mqtt, wantMQTT: true},
		{name: "check recovered camera serial", cameraChain: []*x509.Certificate{wrongSerial.Leaf}, mqtt: mqtt, wantMQTT: true, wantErr: "different-serial"},
		{name: "require trusted camera leaf", cameraChain: []*x509.Certificate{untrusted.Leaf}, mqtt: mqtt, wantMQTT: true, wantErr: "Bambu printer CA"},
		{name: "check MQTT serial", cameraChain: []*x509.Certificate{camera.Leaf}, mqtt: wrongSerial, wantMQTT: true, wantErr: "different-serial"},
		{name: "require trusted MQTT chain", cameraChain: []*x509.Certificate{camera.Leaf}, mqtt: untrusted, wantMQTT: true, wantErr: "Bambu printer CA"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			config := p.tlsConfig(p.Serial)
			mqttCalled := false
			recoverCameraIntermediates(config, func(mqttConfig *tls.Config) (*tls.Conn, error) {
				mqttCalled = true
				if !tc.wantMQTT {
					t.Fatal("MQTT dial with a complete camera chain")
				}
				broker := httptest.NewUnstartedServer(http.NotFoundHandler())
				broker.TLS = &tls.Config{Certificates: []tls.Certificate{tc.mqtt}}
				broker.StartTLS()
				t.Cleanup(broker.Close)
				return tls.DialWithDialer(&net.Dialer{Timeout: 5 * time.Second}, "tcp", broker.Listener.Addr().String(), mqttConfig)
			})
			err := config.VerifyConnection(tls.ConnectionState{PeerCertificates: tc.cameraChain})
			if tc.wantErr == "" && err != nil || tc.wantErr != "" && (err == nil || !strings.Contains(err.Error(), tc.wantErr)) {
				t.Fatalf("verification error = %v, want %q", err, tc.wantErr)
			}
			if mqttCalled != tc.wantMQTT {
				t.Fatalf("MQTT dialed = %t, want %t", mqttCalled, tc.wantMQTT)
			}
		})
	}
}

func issueCameraTestCert(t *testing.T, name string, ca bool, parent *tls.Certificate) tls.Certificate {
	t.Helper()
	pub, key, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	cert := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: name},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(time.Hour),
		BasicConstraintsValid: true,
		IsCA:                  ca,
		KeyUsage:              x509.KeyUsageDigitalSignature,
	}
	if ca {
		cert.KeyUsage |= x509.KeyUsageCertSign
	}
	issuer, signer := cert, any(key)
	if parent != nil {
		issuer, signer = parent.Leaf, parent.PrivateKey
	}
	der, err := x509.CreateCertificate(rand.Reader, cert, issuer, pub, signer)
	if err != nil {
		t.Fatal(err)
	}
	chain := [][]byte{der}
	if parent != nil {
		chain = append(chain, parent.Certificate...)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	return tls.Certificate{Certificate: chain, PrivateKey: key, Leaf: leaf}
}

func startCameraTestRelay(t *testing.T, authority string) (context.Context, string, net.Conn) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	t.Cleanup(cancel)
	config, err := cameraTunnelTLS()
	if err != nil {
		t.Fatal(err)
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	upstream, printer := net.Pipe()
	t.Cleanup(func() { printer.Close() })
	deadline, _ := ctx.Deadline()
	printer.SetDeadline(deadline)
	go relayCameraRTSP(ctx, listener, upstream, authority, config)
	return ctx, listener.Addr().String(), printer
}

func TestCameraRelayPreservesRTSPRequests(t *testing.T) {
	const authority = "[fd00::42]:322"
	for _, scheme := range []string{"rtsp", "rtsps"} {
		t.Run(scheme, func(t *testing.T) {
			ctx, proxy, printer := startCameraTestRelay(t, authority)
			local, err := (&net.Dialer{}).DialContext(ctx, "tcp", proxy)
			if err != nil {
				t.Fatal(err)
			}
			defer local.Close()
			deadline, _ := ctx.Deadline()
			local.SetDeadline(deadline)
			if _, err := fmt.Fprintf(local, "CONNECT %s HTTP/1.1\r\nHost: %s\r\n\r\n", authority, proxy); err != nil {
				t.Fatal(err)
			}
			res, err := http.ReadResponse(bufio.NewReader(local), &http.Request{Method: http.MethodConnect})
			if err != nil || res.StatusCode != http.StatusOK {
				t.Fatalf("CONNECT response = %v, %v", res, err)
			}
			client := tls.Client(local, &tls.Config{InsecureSkipVerify: true})
			uri := scheme + "://" + authority + "/streaming/live/1?track=%2F"
			request := fmt.Sprintf("SET_PARAMETER %s RTSP/1.0\r\nAuthorization: Digest uri=\"%s\", response=\"abc\"\r\nContent-Length: 4\r\n\r\nbody", uri, uri)
			wire := request + "$\x01\x00\x04rtcp"
			if _, err := io.WriteString(client, wire); err != nil {
				t.Fatal(err)
			}
			got := make([]byte, len(wire))
			if _, err := io.ReadFull(printer, got); err != nil || string(got) != wire {
				t.Fatalf("relayed request = %q, %v, want %q", got, err, wire)
			}
		})
	}
}

func TestCameraFFmpegDigest(t *testing.T) {
	executable, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Skip("ffmpeg is unavailable")
	}
	const secret = "test:/+code"
	const realm, nonce = "printer-camera", "printer-nonce"
	target := url.URL{Scheme: "rtsps", Host: "camera.invalid:322", User: url.UserPassword(bambuUser, secret), Path: "/streaming/live/1"}
	ctx, proxy, printer := startCameraTestRelay(t, target.Host)
	t.Setenv("http_proxy", "http://unused.invalid:12345")
	t.Setenv("no_proxy", target.Hostname())
	cmd := cameraRTSPCommand(ctx, executable, target, proxy)
	var stderr strings.Builder
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		if cmd.ProcessState == nil {
			cmd.Process.Kill()
			cmd.Wait()
		}
		if t.Failed() {
			t.Log(stderr.String())
		}
	}()
	target.User = nil
	reader := textproto.NewReader(bufio.NewReader(printer))
	// End with an RTSP error after checking the authenticated retry.
	for _, step := range []struct{ method, status, headers string }{
		{"OPTIONS", "200 OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY\r\n"},
		{"DESCRIBE", "401 Unauthorized", fmt.Sprintf("WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", qop=\"auth\"\r\n", realm, nonce)},
		{"DESCRIBE", "404 Not Found", ""},
	} {
		line, err := reader.ReadLine()
		if err != nil || line != step.method+" "+target.String()+" RTSP/1.0" {
			t.Fatalf("%s request line = %q, %v", step.status, line, err)
		}
		headers, err := reader.ReadMIMEHeader()
		if err != nil {
			t.Fatal(err)
		}
		if step.status == "404 Not Found" {
			auth, ok := strings.CutPrefix(headers.Get("Authorization"), "Digest ")
			if !ok {
				t.Fatal("missing Digest authorization on the retry")
			}
			params := map[string]string{}
			for _, param := range strings.Split(auth, ",") {
				key, value, _ := strings.Cut(strings.TrimSpace(param), "=")
				params[key] = strings.Trim(value, `"`)
			}
			a1 := md5.Sum([]byte(bambuUser + ":" + realm + ":" + secret))
			a2 := md5.Sum([]byte(step.method + ":" + target.String()))
			digest := md5.Sum([]byte(fmt.Sprintf("%x:%s:%s:%s:auth:%x", a1, nonce, params["nc"], params["cnonce"], a2)))
			if params["username"] != bambuUser || params["uri"] != target.String() || params["qop"] != "auth" || params["response"] != fmt.Sprintf("%x", digest) {
				t.Fatalf("invalid Digest authorization for %s", line)
			}
		}
		if _, err := fmt.Fprintf(printer, "RTSP/1.0 %s\r\nCSeq: %s\r\n%s\r\n", step.status, headers.Get("CSeq"), step.headers); err != nil {
			t.Fatal(err)
		}
	}
	var exit *exec.ExitError
	if err := cmd.Wait(); !errors.As(err, &exit) || !strings.Contains(stderr.String(), "404 Not Found") {
		t.Fatalf("expected ffmpeg to report the fake camera's 404, got %v: %s", err, stderr.String())
	}
}

func TestCameraFFmpegErrorHidesAccessCode(t *testing.T) {
	const secret = "test:/+code"
	target := url.URL{Scheme: "rtsps", Host: "192.0.2.7:322", User: url.UserPassword(bambuUser, secret), Path: "/streaming/live/1"}
	stderr := "[in#0 @ 0x1] method DESCRIBE failed: 404 (Not Found)\nError opening input file " + target.String() + ".\n" + secret
	got := cameraFFmpegError(stderr, target, secret).Error()
	if strings.Contains(got, "code") || !strings.Contains(got, "404 (Not Found)") || !strings.Contains(got, "rtsps://bblp:xxxxx@192.0.2.7:322/streaming/live/1") {
		t.Fatalf("ffmpeg error = %q", got)
	}
}

func TestCameraFFmpegSkipsBundledLibraries(t *testing.T) {
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("APPDIR", "/tmp/.mount_pancake")
	bundled := filepath.Join(filepath.Dir(exe), "lib")
	for _, tc := range []struct{ in, want string }{
		{"/tmp/.mount_pancake/usr/lib:" + bundled + "::/opt/ffmpeg/lib", "LD_LIBRARY_PATH=/opt/ffmpeg/lib"},
		{"/tmp/.mount_pancake/usr/lib:" + bundled, ""},
		{"/tmp/.mount_pancake2/lib", "LD_LIBRARY_PATH=/tmp/.mount_pancake2/lib"},
	} {
		env := systemLibraryPath([]string{"PATH=/usr/bin", "LD_LIBRARY_PATH=" + tc.in})
		want := []string{"PATH=/usr/bin"}
		if tc.want != "" {
			want = append(want, tc.want)
		}
		if !slices.Equal(env, want) {
			t.Errorf("systemLibraryPath(%q) = %q, want %q", tc.in, env, want)
		}
	}
}
