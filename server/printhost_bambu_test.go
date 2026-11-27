package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestBambuFTPSPaths(t *testing.T) {
	const filename = "benchy.gcode.3mf"
	for _, model := range []string{"H2D", "P2S", "P1S"} {
		t.Run(model, func(t *testing.T) {
			for _, mode := range []string{"upload", "resend existing", "resend missing"} {
				t.Run(mode, func(t *testing.T) {
					client, peer := net.Pipe()
					defer client.Close()
					defer peer.Close()
					peer.SetDeadline(time.Now().Add(5 * time.Second))

					u := &upload{printer: &PrinterConfig{model: model}, filename: filename,
						size: 42, progress: new(atomic.Int64), resend: mode != "upload"}
					c := &ftpConn{conn: client, r: bufio.NewReader(client)}
					ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
					defer cancel()
					result := make(chan error, 1)
					go func() { result <- c.upload(ctx, "127.0.0.1", nil, u) }()

					r := bufio.NewReader(peer)
					reply := func(command, response string) {
						t.Helper()
						line, err := r.ReadString('\n')
						if err != nil || line != command+"\r\n" {
							t.Fatalf("FTPS command = %q (%v), want %q", line, err, command)
						}
						if _, err := io.WriteString(peer, response+"\r\n"); err != nil {
							t.Fatal(err)
						}
					}
					reply("PBSZ 0", "200 OK")
					reply("PROT P", "200 OK")
					reply("TYPE I", "200 OK")

					if mode == "resend existing" {
						reply("SIZE "+filename, "213 42")
						reply("QUIT", "221 Bye")
						if err := <-result; err != nil || !u.onCard || u.progress.Load() != u.size {
							t.Fatalf("resend: err = %v, onCard = %t, progress = %d", err, u.onCard, u.progress.Load())
						}
						return
					}
					if mode == "resend missing" {
						reply("SIZE "+filename, "550 File missing")
					}

					data, err := net.Listen("tcp4", "127.0.0.1:0")
					if err != nil {
						t.Fatal(err)
					}
					defer data.Close()
					port := data.Addr().(*net.TCPAddr).Port
					reply("PASV", fmt.Sprintf("227 Entering Passive Mode (127,0,0,1,%d,%d)", port>>8, port&255))
					// Stop after checking the command, before the data TLS handshake.
					reply("STOR "+filename, "550 Test stopped transfer")
					if err := <-result; err == nil || !strings.Contains(err.Error(), "Test stopped transfer") {
						t.Fatalf("expected the test's STOR rejection, got %v", err)
					}
				})
			}
		})
	}
}

func TestBambuProjectFileRequestPaths(t *testing.T) {
	const filename = "benchy.gcode.3mf"
	for _, tc := range []struct {
		name, model, serial string
		wantFile, wantURL   string
	}{
		{"H2D", "H2D", "", "ftp:///" + filename, "ftp:///" + filename},
		{"P2S", "P2S", "", "ftp:///" + filename, "ftp:///" + filename},
		{"H2D by serial", "", "094001", "ftp:///" + filename, "ftp:///" + filename},
		{"P2S by serial", "", "22E001", "ftp:///" + filename, "ftp:///" + filename},
		{"P2S catalog name", "Bambu Lab P2S", "", "ftp:///" + filename, "ftp:///" + filename},
		{"P1S", "P1S", "", filename, "file:///sdcard/" + filename},
		{"A1", "A1", "", filename, "file:///sdcard/" + filename},
		{"X1 Carbon", "X1 Carbon", "", filename, "file:///sdcard/" + filename},
		{"H2D Pro", "H2D Pro", "094001", filename, "file:///sdcard/" + filename},
		{"unknown", "", "000001", filename, "file:///sdcard/" + filename},
	} {
		t.Run(tc.name, func(t *testing.T) {
			u := &upload{printer: &PrinterConfig{model: tc.model, Serial: tc.serial}, filename: filename}
			body, err := json.Marshal(bambuProjectFileRequest(u, "1", "benchy", "Metadata/plate_1.gcode", ""))
			if err != nil {
				t.Fatal(err)
			}
			var request struct {
				Print struct {
					Command string `json:"command"`
					File    string `json:"file"`
					URL     string `json:"url"`
				} `json:"print"`
			}
			if err := json.Unmarshal(body, &request); err != nil {
				t.Fatal(err)
			}
			p := request.Print
			if p.Command != "project_file" || p.File != tc.wantFile || p.URL != tc.wantURL {
				t.Errorf("MQTT print = %+v, want project_file with file %q and URL %q", p, tc.wantFile, tc.wantURL)
			}
		})
	}
}
