package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net"
	"net/http"
	"net/url"
	"os/exec"
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
	return httpCamera(ctx, p, camera.upstream, emit)
}

func cameraTLS(ctx context.Context, p *PrinterConfig, port int) (net.Conn, error) {
	u := &url.URL{Scheme: "tcp", Host: net.JoinHostPort(p.Host, strconv.Itoa(port))}
	target, err := p.hosts.resolve(ctx, u)
	if err != nil {
		return nil, err
	}
	conn, err := dialTLS(ctx, target.Addr().String(), port, p.tlsConfig(p.Serial))
	if err != nil {
		return nil, err
	}
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	return &cameraConn{Conn: conn, stop: stop}, nil
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

func relayRTSPRequests(dst io.Writer, src io.Reader, localAuthority, printerAuthority string) error {
	reader := bufio.NewReaderSize(src, 64<<10)
	for {
		prefix, err := reader.Peek(1)
		if err != nil {
			return err
		}
		if prefix[0] == '$' {
			header := make([]byte, 4)
			if _, err = io.ReadFull(reader, header); err != nil {
				return err
			}
			if _, err = dst.Write(header); err != nil {
				return err
			}
			length := int64(binary.BigEndian.Uint16(header[2:]))
			if _, err = io.CopyN(dst, reader, length); err != nil {
				return err
			}
			continue
		}

		line, err := reader.ReadSlice('\n')
		if err != nil {
			return fmt.Errorf("invalid RTSP request line: %w", err)
		}
		if !bytes.HasSuffix(line, []byte("\r\n")) {
			return errors.New("invalid RTSP request line ending")
		}
		requestLine := string(line)
		parts := strings.Split(strings.TrimSuffix(requestLine, "\r\n"), " ")
		if len(parts) == 3 && parts[2] == "RTSP/1.0" {
			u, parseErr := url.Parse(parts[1])
			if parseErr == nil && strings.EqualFold(u.Scheme, "rtsp") && strings.EqualFold(u.Host, localAuthority) {
				u.Scheme = "rtsps"
				u.Host = printerAuthority
				requestLine = strings.Replace(requestLine, parts[1], u.String(), 1)
			}
		}
		if _, err = io.WriteString(dst, requestLine); err != nil {
			return err
		}

		headerBytes := 0
		var contentLength int64
		for {
			line, err = reader.ReadSlice('\n')
			if err != nil {
				return fmt.Errorf("invalid RTSP header: %w", err)
			}
			headerBytes += len(line)
			if headerBytes > 1<<20 || !bytes.HasSuffix(line, []byte("\r\n")) {
				return errors.New("invalid RTSP headers")
			}
			if _, err = dst.Write(line); err != nil {
				return err
			}
			if len(line) == 2 {
				break
			}
			name, value, ok := strings.Cut(string(line), ":")
			if ok && strings.EqualFold(strings.TrimSpace(name), "Content-Length") {
				contentLength, err = strconv.ParseInt(strings.TrimSpace(value), 10, 64)
				if err != nil || contentLength < 0 || contentLength > 1<<20 {
					return errors.New("invalid RTSP Content-Length")
				}
			}
		}
		if _, err = io.CopyN(dst, reader, contentLength); err != nil {
			return err
		}
	}
}

func rtspCamera(ctx context.Context, p *PrinterConfig, secret string, emit func([]byte, bool)) error {
	executable, err := exec.LookPath("ffmpeg")
	if err != nil {
		return errors.New("Streaming RTSPS camera video requires ffmpeg to be installed on the server.")
	}
	conn, err := cameraTLS(ctx, p, 322)
	if err != nil {
		return err
	}
	defer conn.Close()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer listener.Close()
	stop := context.AfterFunc(ctx, func() { listener.Close() })
	defer stop()
	go func() {
		local, err := listener.Accept()
		if err != nil {
			return
		}
		defer local.Close()
		listener.Close()
		go func() {
			_ = relayRTSPRequests(conn, local, listener.Addr().String(), net.JoinHostPort(p.Host, "322"))
			conn.Close()
		}()
		io.Copy(local, conn)
	}()
	target := url.URL{Scheme: "rtsp", Host: listener.Addr().String(), User: url.UserPassword("bblp", secret), Path: "/streaming/live/1"}
	cmd := exec.CommandContext(ctx, executable, "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp", "-analyzeduration", "500000", "-probesize", "32768", "-timeout", "15000000", "-i", target.String(), "-map", "0:v:0", "-an", "-c:v", "copy", "-movflags", "frag_keyframe+empty_moov+default_base_moof", "-flush_packets", "1", "-f", "mp4", "pipe:1")
	pipe, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err = cmd.Start(); err != nil {
		return err
	}
	defer func() { cmd.Process.Kill(); cmd.Wait() }()
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
