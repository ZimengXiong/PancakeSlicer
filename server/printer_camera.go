package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"mime"
	"mime/multipart"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
)

type cameraSource struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Format   string `json:"format"`
	URL      string `json:"url"`
	upstream string
}
type cameraPacket struct {
	data []byte
	err  error
}
type cameraFeed struct {
	cfg     *PrinterConfig
	cancel  context.CancelFunc
	viewers map[chan cameraPacket]bool
	init    []byte
}
type cameraHub struct {
	mu    sync.Mutex
	feeds map[string]*cameraFeed
}

func (h *cameraHub) subscribe(p *PrinterConfig, camera cameraSource) (<-chan cameraPacket, func()) {
	key := p.Printer + "\x00" + camera.ID
	h.mu.Lock()
	defer h.mu.Unlock()
	feed := h.feeds[key]
	if feed != nil && (feed.cfg.Kind != p.Kind || feed.cfg.Host != p.Host || feed.cfg.Serial != p.Serial || feed.cfg.URL != p.URL || feed.cfg.CredentialFile != p.CredentialFile) {
		feed.cancel()
		for ch := range feed.viewers {
			close(ch)
			delete(feed.viewers, ch)
		}
		delete(h.feeds, key)
		feed = nil
	}
	ch := make(chan cameraPacket, 8)
	if feed == nil {
		ctx, cancel := context.WithCancel(context.Background())
		feed = &cameraFeed{cfg: p, cancel: cancel, viewers: map[chan cameraPacket]bool{}}
		h.feeds[key] = feed
		go func(f *cameraFeed) {
			emit := func(data []byte, init bool) {
				h.mu.Lock()
				defer h.mu.Unlock()
				if h.feeds[key] != f {
					return
				}
				if init {
					f.init = data
				}
				for out := range f.viewers {
					select {
					case out <- cameraPacket{data: data}:
					default:
						close(out)
						delete(f.viewers, out)
					}
				}
				if len(f.viewers) == 0 {
					f.cancel()
				}
			}
			err := runCamera(ctx, p, camera, emit)
			if ctx.Err() == nil {
				log.Printf("printer %s: camera %s stopped: %v", p.Printer, camera.ID, err)
			}
			h.mu.Lock()
			defer h.mu.Unlock()
			if h.feeds[key] != f {
				return
			}
			for out := range f.viewers {
				select {
				case out <- cameraPacket{err: err}:
				default:
				}
				close(out)
				delete(f.viewers, out)
			}
			delete(h.feeds, key)
		}(feed)
	}
	feed.viewers[ch] = true
	if len(feed.init) > 0 {
		ch <- cameraPacket{data: feed.init}
	}
	return ch, func() {
		h.mu.Lock()
		defer h.mu.Unlock()
		if feed.viewers[ch] {
			delete(feed.viewers, ch)
			close(ch)
		}
		if len(feed.viewers) == 0 {
			feed.cancel()
			if h.feeds[key] == feed {
				delete(h.feeds, key)
			}
		}
	}
}

func (s *Server) printerCameras(w http.ResponseWriter, r *http.Request) {
	p := s.sends.printer(r.PathValue("printer"))
	if p == nil {
		fail(w, 404, "printer_doesnt_have_network")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	cameras, err := cameraSources(ctx, p)
	if err != nil {
		fail(w, 502, "couldnt_read_camera_settings")
		return
	}
	writeJSON(w, 200, map[string]any{"cameras": cameras})
}
func (s *Server) printerCamera(w http.ResponseWriter, r *http.Request) {
	p := s.sends.printer(r.PathValue("printer"))
	if p == nil {
		fail(w, 404, "printer_doesnt_have_network")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	sources, err := cameraSources(ctx, p)
	cancel()
	if err != nil {
		fail(w, 502, "couldnt_read_camera_settings")
		return
	}
	var camera *cameraSource
	for i := range sources {
		if sources[i].ID == r.PathValue("camera") {
			camera = &sources[i]
		}
	}
	if camera == nil {
		fail(w, 404, "no_camera_set_up")
		return
	}
	if r.Method == "HEAD" {
		content := "video/mp4"
		if camera.Format == "mjpeg" {
			content = "multipart/x-mixed-replace; boundary=orca-camera"
		}
		w.Header().Set("Content-Type", content)
		w.Header().Set("Cache-Control", "no-store")
		return
	}
	packets, leave := s.sends.cameras.subscribe(p, *camera)
	defer leave()
	controller := http.NewResponseController(w)
	defer controller.SetWriteDeadline(time.Time{})
	first := true
	for {
		var packet cameraPacket
		var ok bool
		select {
		case <-r.Context().Done():
			return
		case packet, ok = <-packets:
		case <-time.After(30 * time.Second):
			if first {
				fail(w, 504, "didnt_receive_video_frame")
			}
			return
		}
		if !ok || packet.err != nil {
			if first {
				fail(w, 502, "camera_unavailable_check_camera")
			}
			return
		}
		if first {
			content := "video/mp4"
			if camera.Format == "mjpeg" {
				content = "multipart/x-mixed-replace; boundary=orca-camera"
			}
			w.Header().Set("Content-Type", content)
			w.Header().Set("Cache-Control", "no-store")
			w.Header().Set("X-Accel-Buffering", "no")
			first = false
		}
		if err := controller.SetWriteDeadline(time.Now().Add(10 * time.Second)); err != nil {
			return
		}
		if camera.Format == "mjpeg" {
			if _, err := fmt.Fprintf(w, "--orca-camera\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len(packet.data)); err != nil {
				return
			}
		}
		if _, err := w.Write(packet.data); err != nil {
			return
		}
		if camera.Format == "mjpeg" {
			if _, err := io.WriteString(w, "\r\n"); err != nil {
				return
			}
		}
		if err := controller.Flush(); err != nil {
			return
		}
	}
}

func cameraSources(ctx context.Context, p *PrinterConfig) ([]cameraSource, error) {
	out := []cameraSource{}
	add := func(name, format, upstream string) {
		id := strconv.Itoa(len(out))
		out = append(out, cameraSource{ID: id, Name: name, Format: format, URL: "/api/printer-connections/" + url.PathEscape(p.Printer) + "/camera/" + id, upstream: upstream})
	}
	if p.Kind == "bambu-lan" {
		format := "mjpeg"
		model := strings.ToUpper(p.model)
		if model == "" && len(p.Serial) >= 3 {
			model = strings.ToUpper(bambuModel("", p.Serial[:3]))
		}
		switch {
		case strings.Contains(model, "A1"), strings.Contains(model, "P1"):
		case strings.Contains(model, "X1"), strings.Contains(model, "H2"), strings.Contains(model, "P2"):
			format = "mp4"
		default:
			return out, nil
		}
		add("Chamber camera", format, "")
		return out, nil
	}
	if p.Kind != "moonraker" && p.Kind != "octoprint" {
		return out, nil
	}
	secret, err := p.secret()
	if err != nil {
		return nil, err
	}
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return nil, err
	}
	addHTTP := func(name, typed string) {
		u, err := url.Parse(typed)
		if err != nil || typed == "" {
			return
		}
		u = base.ResolveReference(u)
		if u.Scheme != "http" && u.Scheme != "https" || u.User != nil || u.Fragment != "" {
			return
		}
		if _, err = p.hosts.resolve(ctx, u); err != nil {
			return
		}
		add(name, "mjpeg", u.String())
	}
	if p.Kind == "moonraker" {
		var response struct {
			Result struct {
				Webcams []struct {
					Name     string `json:"name"`
					Enabled  bool   `json:"enabled"`
					Stream   string `json:"stream_url"`
					Snapshot string `json:"snapshot_url"`
					Service  string `json:"service"`
				} `json:"webcams"`
			} `json:"result"`
		}
		_, err = h.call(ctx, "GET", base.JoinPath("server/webcams/list").String(), nil, &response)
		if err != nil {
			return nil, err
		}
		for _, cam := range response.Result.Webcams {
			if cam.Enabled {
				stream := cam.Stream
				if strings.Contains(strings.ToLower(cam.Service), "webrtc") || strings.Contains(stream, "webrtc") {
					stream = cam.Snapshot
				}
				addHTTP(cam.Name, stream)
			}
		}
	} else {
		var response struct {
			Webcam struct {
				Stream   string `json:"streamUrl"`
				Snapshot string `json:"snapshotUrl"`
			} `json:"webcam"`
		}
		_, err = h.call(ctx, "GET", base.JoinPath("api/settings").String(), nil, &response)
		if err != nil {
			return nil, err
		}
		addHTTP("Webcam", cmpOr(response.Webcam.Stream, response.Webcam.Snapshot))
	}
	return out, nil
}
func runCamera(ctx context.Context, p *PrinterConfig, camera cameraSource, emit func([]byte, bool)) error {
	if p.Kind == "bambu-lan" {
		secret, err := p.secret()
		if err != nil {
			return err
		}
		if camera.Format == "mp4" {
			return rtspCamera(ctx, p, secret, emit)
		}
		return jpegCamera(ctx, p, secret, emit)
	}
	err := httpCamera(ctx, p, camera.upstream, emit)
	var uerr *url.Error
	if errors.As(err, &uerr) {
		u, perr := url.Parse(uerr.URL)
		if perr != nil {
			u = &url.URL{}
		}
		uerr.URL = (&url.URL{Scheme: u.Scheme, Host: u.Host, Path: u.Path}).String()
	}
	return err
}

const cameraRTSPPort = 322

func cameraTLS(ctx context.Context, p *PrinterConfig, port int) (net.Conn, error) {
	u := &url.URL{Scheme: "tcp", Host: net.JoinHostPort(p.Host, strconv.Itoa(port))}
	target, err := p.hosts.resolve(ctx, u)
	if err != nil {
		return nil, err
	}
	host := target.Addr().String()
	config := p.tlsConfig(p.Serial)
	if port == cameraRTSPPort && p.TLS == "bambu-ca" {
		recoverCameraIntermediates(config, func(mqttConfig *tls.Config) (*tls.Conn, error) {
			const mqttPort = 8883
			if slices.Contains(p.hosts.ports, mqttPort) {
				return nil, &hostRefusal{reason: "port 8883 is this service's own"}
			}
			return dialTLS(ctx, host, mqttPort, mqttConfig)
		})
	}
	conn, err := dialTLS(ctx, host, port, config)
	if err != nil {
		return nil, err
	}
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	return &cameraConn{Conn: conn, stop: stop}, nil
}

func recoverCameraIntermediates(config *tls.Config, dialMQTT func(*tls.Config) (*tls.Conn, error)) {
	mqttConfig := config.Clone()
	verify := config.VerifyConnection
	config.VerifyConnection = func(cs tls.ConnectionState) error {
		err := verify(cs)
		var unknown x509.UnknownAuthorityError
		if !errors.As(err, &unknown) {
			return err
		}
		// Some H2D firmware omits the intermediate on the camera port.
		mqtt, err := dialMQTT(mqttConfig)
		if err != nil {
			return err
		}
		defer mqtt.Close()
		cs.PeerCertificates = slices.Concat(cs.PeerCertificates, mqtt.ConnectionState().PeerCertificates[1:])
		return verify(cs)
	}
}

type cameraConn struct {
	net.Conn
	stop func() bool
}

func (c *cameraConn) Close() error { c.stop(); return c.Conn.Close() }
func jpegCamera(ctx context.Context, p *PrinterConfig, secret string, emit func([]byte, bool)) error {
	if len(secret) > 32 {
		return errors.New("Invalid camera access code.")
	}
	conn, err := cameraTLS(ctx, p, 6000)
	if err != nil {
		return err
	}
	defer conn.Close()
	auth := make([]byte, 80)
	binary.LittleEndian.PutUint32(auth, 0x40)
	binary.LittleEndian.PutUint32(auth[4:], 0x3000)
	copy(auth[16:], "bblp")
	copy(auth[48:], secret)
	conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if _, err = conn.Write(auth); err != nil {
		return err
	}
	for {
		conn.SetReadDeadline(time.Now().Add(25 * time.Second))
		header := make([]byte, 16)
		if _, err = io.ReadFull(conn, header); err != nil {
			return err
		}
		size := binary.LittleEndian.Uint32(header)
		if size < 4 || size > 16<<20 {
			return errors.New("invalid camera frame size")
		}
		frame := make([]byte, size)
		if _, err = io.ReadFull(conn, frame); err != nil {
			return err
		}
		if !bytes.HasPrefix(frame, []byte{255, 216}) || !bytes.HasSuffix(frame, []byte{255, 217}) {
			return errors.New("invalid JPEG camera frame")
		}
		emit(frame, false)
	}
}
func httpCamera(ctx context.Context, p *PrinterConfig, typed string, emit func([]byte, bool)) error {
	u, err := url.Parse(typed)
	if err != nil {
		return err
	}
	client, err := hostClient(ctx, p, u)
	if err != nil {
		return err
	}
	secret, err := p.secret()
	if err != nil {
		return err
	}
	base, _ := url.Parse(p.URL)
	h := &hostSession{client: client, printer: p, secret: secret}
	for ctx.Err() == nil {
		req, _ := http.NewRequestWithContext(ctx, "GET", u.String(), nil)
		if u.Host == base.Host && u.Scheme == base.Scheme {
			h.authorize(req)
		}
		resp, err := client.Do(req)
		if err != nil {
			return err
		}
		if resp.StatusCode != 200 {
			resp.Body.Close()
			return errors.New("The printer's camera refused the connection.")
		}
		kind, params, _ := mime.ParseMediaType(resp.Header.Get("Content-Type"))
		if kind == "multipart/x-mixed-replace" {
			reader := multipart.NewReader(resp.Body, params["boundary"])
			for {
				part, err := reader.NextPart()
				if err != nil {
					resp.Body.Close()
					return err
				}
				frame, err := io.ReadAll(io.LimitReader(part, (16<<20)+1))
				if err != nil || len(frame) > 16<<20 {
					resp.Body.Close()
					return errors.New("invalid webcam frame")
				}
				part.Close()
				if len(frame) > 3 && bytes.HasPrefix(frame, []byte{255, 216}) {
					emit(frame, false)
				}
			}
		}
		frame, err := io.ReadAll(io.LimitReader(resp.Body, (16<<20)+1))
		resp.Body.Close()
		if err != nil {
			return err
		}
		if len(frame) > 16<<20 || !bytes.HasPrefix(frame, []byte{255, 216}) {
			return errors.New("The webcam URL didn't return a supported JPEG or MJPEG stream.")
		}
		emit(frame, false)
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(time.Second):
		}
	}
	return ctx.Err()
}

func cameraTunnelTLS() (*tls.Config, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}
	now := time.Now()
	cert := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		NotBefore:    now.Add(-time.Minute),
		NotAfter:     now.Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, cert, cert, key.Public(), key)
	if err != nil {
		return nil, err
	}
	return &tls.Config{
		MinVersion:   tls.VersionTLS12,
		Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}},
	}, nil
}

func relayCameraRTSP(ctx context.Context, listener net.Listener, upstream net.Conn, authority string, config *tls.Config) {
	defer upstream.Close()
	stop := context.AfterFunc(ctx, func() { listener.Close() })
	defer stop()
	local, err := listener.Accept()
	if err != nil {
		return
	}
	listener.Close()
	defer local.Close()
	stopLocal := context.AfterFunc(ctx, func() { local.Close() })
	defer stopLocal()
	local.SetDeadline(time.Now().Add(15 * time.Second))

	reader := bufio.NewReader(io.LimitReader(local, 16<<10))
	request, err := http.ReadRequest(reader)
	if err != nil || request.Method != http.MethodConnect || !strings.EqualFold(request.RequestURI, authority) ||
		request.ContentLength != 0 || len(request.TransferEncoding) != 0 || reader.Buffered() != 0 {
		return
	}
	if _, err := io.WriteString(local, "HTTP/1.1 200 Connection Established\r\n\r\n"); err != nil {
		return
	}
	tunnel := tls.Server(local, config)
	if err := tunnel.HandshakeContext(ctx); err != nil {
		return
	}
	local.SetDeadline(time.Time{})
	go func() {
		io.Copy(upstream, tunnel)
		upstream.Close()
	}()
	io.Copy(tunnel, upstream)
}

func cameraRTSPCommand(ctx context.Context, executable string, target url.URL, proxyAddr string) *exec.Cmd {
	env := systemLibraryPath(os.Environ())
	args := []string{"-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp", "-analyzeduration", "500000", "-probesize", "32768", "-timeout", "15000000"}
	// Older demuxers rely on the TLS protocol's default and don't expose this option.
	probe := exec.CommandContext(ctx, executable, "-hide_banner", "-h", "demuxer=rtsp")
	probe.Env = env
	if help, err := probe.Output(); err == nil && bytes.Contains(help, []byte("-tls_verify ")) {
		args = append(args, "-tls_verify", "0")
	}
	args = append(args, "-i", target.String(), "-map", "0:v:0", "-an", "-c:v", "copy", "-movflags", "frag_keyframe+empty_moov+default_base_moof", "-flush_packets", "1", "-f", "mp4", "pipe:1")
	cmd := exec.CommandContext(ctx, executable, args...)
	cmd.WaitDelay = 5 * time.Second
	// FFmpeg's RTSPS transport uses CONNECT. Keeping the printer URI in FFmpeg
	// lets it sign the same target that reaches the printer. Go verifies upstream TLS.
	cmd.Env = append(env, "http_proxy=http://"+proxyAddr, "no_proxy=")
	return cmd
}

func systemLibraryPath(env []string) []string {
	var bundled []string
	if exe, err := os.Executable(); err == nil {
		bundled = append(bundled, filepath.Dir(exe))
	}
	if dir := os.Getenv("APPDIR"); dir != "" {
		bundled = append(bundled, dir)
	}
	out := make([]string, 0, len(env))
	for _, kv := range env {
		value, ok := strings.CutPrefix(kv, "LD_LIBRARY_PATH=")
		if !ok {
			out = append(out, kv)
			continue
		}
		kept := slices.DeleteFunc(filepath.SplitList(value), func(path string) bool {
			return path == "" || slices.ContainsFunc(bundled, func(dir string) bool {
				rel, err := filepath.Rel(dir, path)
				return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
			})
		})
		if len(kept) > 0 {
			out = append(out, "LD_LIBRARY_PATH="+strings.Join(kept, string(filepath.ListSeparator)))
		}
	}
	return out
}

func rtspCamera(ctx context.Context, p *PrinterConfig, secret string, emit func([]byte, bool)) error {
	executable, err := exec.LookPath("ffmpeg")
	if err != nil {
		return errors.New("Streaming RTSPS camera video requires ffmpeg to be installed on the server.")
	}
	conn, err := cameraTLS(ctx, p, cameraRTSPPort)
	if err != nil {
		return err
	}
	defer conn.Close()
	config, err := cameraTunnelTLS()
	if err != nil {
		return err
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer listener.Close()
	target := url.URL{Scheme: "rtsps", Host: net.JoinHostPort(p.Host, strconv.Itoa(cameraRTSPPort)), User: url.UserPassword(bambuUser, secret), Path: "/streaming/live/1"}
	go relayCameraRTSP(ctx, listener, conn, target.Host, config)
	cmd := cameraRTSPCommand(ctx, executable, target, listener.Addr().String())
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	stderr := &tailBuffer{}
	cmd.Stderr = stderr
	if err = cmd.Start(); err != nil {
		return err
	}
	err = readCameraMP4(pipe, emit)
	cmd.Process.Kill()
	waitErr := cmd.Wait()
	if ctx.Err() == nil && (errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF)) {
		if msg := strings.TrimSpace(stderr.String()); msg != "" {
			return cameraFFmpegError(msg, target, secret)
		}
		if waitErr != nil {
			return fmt.Errorf("ffmpeg: %v", waitErr)
		}
	}
	return err
}

func cameraFFmpegError(stderr string, target url.URL, secret string) error {
	stderr = strings.ReplaceAll(stderr, target.String(), target.Redacted())
	_, escaped, _ := strings.Cut(target.User.String(), ":")
	for _, s := range []string{secret, escaped, url.QueryEscape(secret)} {
		stderr = redact(stderr, s)
	}
	return fmt.Errorf("ffmpeg: %s", stderr)
}

type tailBuffer struct {
	mu  sync.Mutex
	buf []byte
}

func (t *tailBuffer) Write(p []byte) (int, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.buf = append(t.buf, p...)
	if len(t.buf) > 4<<10 {
		t.buf = t.buf[len(t.buf)-4<<10:]
	}
	return len(p), nil
}

func (t *tailBuffer) String() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return string(t.buf)
}

func readCameraMP4(pipe io.Reader, emit func([]byte, bool)) (err error) {
	var init, fragment []byte
	for {
		header := make([]byte, 8)
		if _, err = io.ReadFull(pipe, header); err != nil {
			return err
		}
		size := uint64(binary.BigEndian.Uint32(header))
		kind := string(header[4:])
		if size == 1 {
			extra := make([]byte, 8)
			if _, err = io.ReadFull(pipe, extra); err != nil {
				return err
			}
			size = binary.BigEndian.Uint64(extra)
			header = append(header, extra...)
		}
		if size < uint64(len(header)) || size > 32<<20 {
			return errors.New("invalid MP4 box")
		}
		box := make([]byte, int(size))
		copy(box, header)
		if _, err = io.ReadFull(pipe, box[len(header):]); err != nil {
			return err
		}
		switch kind {
		case "ftyp", "moov":
			init = append(init, box...)
			if kind == "moov" {
				emit(init, true)
			}
		case "moof":
			fragment = box
		case "mdat":
			if len(fragment) > 0 {
				fragment = append(fragment, box...)
				emit(fragment, false)
				fragment = nil
			}
		}
	}
}

func (h *cameraHub) disconnect(printer string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for key, feed := range h.feeds {
		if feed.cfg.Printer == printer {
			feed.cancel()
			for out := range feed.viewers {
				close(out)
				delete(feed.viewers, out)
			}
			delete(h.feeds, key)
		}
	}
}
