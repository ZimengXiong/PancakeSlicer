package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/md5"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"slices"
	"strconv"
	"strings"
	"time"
)

// Elegoo Link covers the Centauri family, following upstream ElegooLink. Centauri Carbon and Centauri take a chunked
// multipart upload on the web port and speak SDCP over a WebSocket on port 3030 for status, start and controls.
// Centauri 2 and Centauri Carbon 2 take a token-authenticated chunked PUT and cannot start prints over the LAN.
// Other Elegoo printers run OctoPrint- or Moonraker-compatible firmware and use those connection kinds.

const elegooChunk = 1 << 20

func elegooCC2(p *PrinterConfig) bool { return strings.HasSuffix(strings.TrimSpace(p.model), "2") }

func elegooUUID() string {
	var b [16]byte
	rand.Read(b[:])
	b[6] = b[6]&0x0f | 0x40
	b[8] = b[8]&0x3f | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func testElegoo(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	h, base, err := httpSession(ctx, p, "")
	if err == nil {
		if elegooCC2(p) {
			err = elegooCC2Info(ctx, h, base, secret)
		} else {
			err = elegooProbe(ctx, h, base)
		}
	}
	if err != nil {
		if c := (*connError)(nil); errors.As(err, &c) {
			return PrinterTest{Stage: c.stage, Message: c.msg}
		}
		return hostFailure(err)
	}
	return PrinterTest{OK: true, Stage: "ok", Message: "Connected to " + p.Name + "."}
}

func elegooProbe(ctx context.Context, h *hostSession, base *url.URL) error {
	req, err := http.NewRequestWithContext(ctx, "GET", base.String(), nil)
	if err != nil {
		return err
	}
	resp, err := h.do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return fmt.Errorf("The printer returned HTTP %d%s", resp.StatusCode, printerMessage(resp))
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 256<<10))
	if !bytes.Contains(bytes.ToUpper(body), []byte("ELEGOO")) {
		return errors.New("That address answered, but it isn't an Elegoo Centauri printer.")
	}
	return nil
}

func elegooToken(secret string) string { return cmpOr(secret, "123456") }

func elegooCC2Info(ctx context.Context, h *hostSession, base *url.URL, secret string) error {
	u := base.JoinPath("system/info")
	u.RawQuery = url.Values{"X-Token": {elegooToken(secret)}}.Encode()
	req, err := http.NewRequestWithContext(ctx, "GET", u.String(), nil)
	if err != nil {
		return err
	}
	req.Header.Set("X-Token", elegooToken(secret))
	resp, err := h.do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	var info struct {
		ErrorCode  int `json:"error_code"`
		SystemInfo struct {
			SN string `json:"sn"`
		} `json:"system_info"`
	}
	if err := elegooReply(resp, &info); err != nil {
		return err
	}
	if info.ErrorCode != 0 {
		return &connError{"access_code", fmt.Sprintf("The printer rejected the access code (error %d)—check it in Connection settings.", info.ErrorCode)}
	}
	if info.SystemInfo.SN == "" {
		return errors.New("That address answered, but it isn't an Elegoo Centauri 2 printer.")
	}
	return nil
}

func elegooReply(resp *http.Response, out any) error {
	switch {
	case resp.StatusCode == 401 || resp.StatusCode == 403:
		return &connError{"access_code", "The printer rejected the access code—check it in Connection settings."}
	case resp.StatusCode < 200 || resp.StatusCode > 299:
		return fmt.Errorf("The printer returned HTTP %d%s", resp.StatusCode, printerMessage(resp))
	}
	if json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(out) != nil {
		return errors.New("The printer returned an unexpected response format.")
	}
	return nil
}

func sendElegoo(ctx context.Context, u *upload) (string, error) {
	p := u.printer
	h, base, err := httpSession(ctx, p, "")
	if err != nil {
		return "", err
	}
	cc2 := elegooCC2(p)
	if cc2 {
		err = elegooCC2Info(ctx, h, base, u.secret)
	} else {
		err = elegooProbe(ctx, h, base)
	}
	if err != nil {
		return "", err
	}
	sum := md5.New()
	if _, err := io.Copy(sum, io.NewSectionReader(u.file, 0, u.size)); err != nil {
		return "", err
	}
	digest, id := hex.EncodeToString(sum.Sum(nil)), elegooUUID()
	buf := make([]byte, elegooChunk)
	for offset := int64(0); offset < u.size; {
		n := min(int64(len(buf)), u.size-offset)
		if _, err := io.ReadFull(u.file, buf[:n]); err != nil {
			return "", err
		}
		chunk := progressReader{bytes.NewReader(buf[:n]), u.progress}
		var req *http.Request
		if cc2 {
			if req, err = http.NewRequestWithContext(ctx, "PUT", base.JoinPath("upload").String(), chunk); err == nil {
				req.ContentLength = n
				req.Header.Set("Content-Type", "application/octet-stream")
				req.Header.Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", offset, offset+n-1, u.size))
				req.Header.Set("X-File-Name", u.filename)
				req.Header.Set("X-File-MD5", digest)
				req.Header.Set("X-Token", elegooToken(u.secret))
			}
		} else {
			req, err = multipartFile(ctx, base.JoinPath("uploadFile/upload").String(), "File", u.filename, n, chunk, [][2]string{
				{"Check", "1"}, {"S-File-MD5", digest}, {"Offset", strconv.FormatInt(offset, 10)}, {"Uuid", id}, {"TotalSize", strconv.FormatInt(u.size, 10)}})
		}
		if err != nil {
			return "", err
		}
		resp, err := h.do(req)
		if err != nil {
			return "", err
		}
		var reply struct {
			Code      string `json:"code"`
			ErrorCode int    `json:"error_code"`
			Messages  []struct {
				Message string `json:"message"`
			} `json:"messages"`
		}
		err = elegooReply(resp, &reply)
		resp.Body.Close()
		switch {
		case err != nil:
			return "", err
		case cc2 && reply.ErrorCode != 0:
			return "", fmt.Errorf("The printer rejected the upload (error %d).", reply.ErrorCode)
		case !cc2 && reply.Code != "000000":
			detail := ""
			if len(reply.Messages) > 0 && reply.Messages[0].Message != "" {
				detail = ": " + reply.Messages[0].Message
			}
			return "", fmt.Errorf("The printer rejected the upload (code %s)%s", reply.Code, detail)
		}
		offset += n
	}
	switch {
	case !u.start:
		return "Uploaded " + u.filename + " to " + p.Name + ".", nil
	case cc2:
		u.outcome = "uploaded"
		return "Uploaded " + u.filename + " to " + p.Name + ". Centauri 2 can't start prints over the network—start it from the printer's screen.", nil
	}
	u.starting()
	if err := startElegoo(ctx, u); err != nil {
		return "", err
	}
	return "Uploaded " + u.filename + " to " + p.Name + " and started printing.", nil
}

func startElegoo(ctx context.Context, u *upload) error {
	ctx, cancel := context.WithTimeoutCause(ctx, 90*time.Second, errors.New("The printer took too long to check the uploaded file."))
	defer cancel()
	err := startSDCP(ctx, u)
	if cause := context.Cause(ctx); err != nil && cause != nil {
		return cause
	}
	return err
}

func startSDCP(ctx context.Context, u *upload) error {
	ws, err := dialSDCP(ctx, u.printer)
	if err != nil {
		return err
	}
	defer ws.close()
	// The printer verifies a fresh upload before it accepts a start; wait while CurrentStatus holds 8, like upstream.
	for {
		st, err := ws.status()
		if err != nil {
			return err
		}
		if !slices.Contains(st.CurrentStatus, 8) {
			break
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(time.Second):
		}
	}
	ack, err := ws.request(128, map[string]any{"Filename": "/local/" + u.filename, "StartLayer": 0,
		"Calibration_switch": boolInt(u.options.bedLeveling), "PrintPlatformType": u.options.bedType, "Tlp_Switch": boolInt(u.options.timelapse)})
	if err != nil {
		return err
	}
	switch ack {
	case 0:
		return nil
	case 1:
		return &connError{"busy", "The printer is busy; wait for its current job to finish."}
	case 2:
		return errors.New("The printer couldn't find the uploaded file.")
	case 3:
		return errors.New("The uploaded file arrived corrupted (MD5 check failed); send it again.")
	case 7:
		return errors.New("The printer says this file was sliced for a different model.")
	}
	return fmt.Errorf("The printer couldn't start the print (code %d).", ack)
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

type sdcpStatus struct {
	CurrentStatus    []int   `json:"CurrentStatus"`
	TempOfNozzle     float64 `json:"TempOfNozzle"`
	TempTargetNozzle float64 `json:"TempTargetNozzle"`
	TempOfHotbed     float64 `json:"TempOfHotbed"`
	TempTargetHotbed float64 `json:"TempTargetHotbed"`
	TempOfBox        float64 `json:"TempOfBox"`
	LightStatus      *struct {
		SecondLight *int `json:"SecondLight"`
	} `json:"LightStatus"`
	PrintInfo *struct {
		Status       int     `json:"Status"`
		CurrentLayer int     `json:"CurrentLayer"`
		TotalLayer   int     `json:"TotalLayer"`
		CurrentTicks float64 `json:"CurrentTicks"`
		TotalTicks   float64 `json:"TotalTicks"`
		Filename     string  `json:"Filename"`
	} `json:"PrintInfo"`
}

type sdcpMessage struct {
	Status *sdcpStatus `json:"Status"`
	Data   *struct {
		Cmd  int `json:"Cmd"`
		Data struct {
			Ack *int `json:"Ack"`
		} `json:"Data"`
	} `json:"Data"`
}

// sdcpConn is a minimal RFC 6455 client: SDCP sends small unfragmented JSON text frames.
type sdcpConn struct {
	conn net.Conn
	br   *bufio.Reader
	stop func() bool
}

func dialSDCP(ctx context.Context, p *PrinterConfig) (*sdcpConn, error) {
	base, _ := url.Parse(p.URL)
	host := net.JoinHostPort(base.Hostname(), "3030")
	target, err := p.hosts.resolve(ctx, &url.URL{Scheme: base.Scheme, Host: host})
	if err != nil {
		return nil, err
	}
	conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", target.String())
	if err != nil {
		return nil, netError(err, "the printer")
	}
	if base.Scheme == "https" {
		tc := tls.Client(conn, p.tlsConfig(base.Hostname()))
		if err := tc.HandshakeContext(ctx); err != nil {
			conn.Close()
			return nil, netError(err, "the printer")
		}
		conn = tc
	}
	c := &sdcpConn{conn: conn, br: bufio.NewReader(conn), stop: context.AfterFunc(ctx, func() { conn.Close() })}
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	var nonce [16]byte
	rand.Read(nonce[:])
	key := base64.StdEncoding.EncodeToString(nonce[:])
	fmt.Fprintf(conn, "GET /websocket HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", host, key)
	resp, err := http.ReadResponse(c.br, nil)
	if err != nil {
		c.stop()
		conn.Close()
		return nil, netError(err, "the printer")
	}
	resp.Body.Close()
	accept := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	if resp.StatusCode != http.StatusSwitchingProtocols || resp.Header.Get("Sec-WebSocket-Accept") != base64.StdEncoding.EncodeToString(accept[:]) {
		c.stop()
		conn.Close()
		return nil, errors.New("The printer refused the SDCP connection on port 3030.")
	}
	conn.SetDeadline(time.Time{})
	return c, nil
}

func (c *sdcpConn) close() {
	c.conn.SetWriteDeadline(time.Now().Add(time.Second))
	c.writeFrame(0x8, nil)
	c.stop()
	c.conn.Close()
}

func (c *sdcpConn) writeFrame(opcode byte, payload []byte) error {
	frame := []byte{0x80 | opcode}
	switch n := len(payload); {
	case n < 126:
		frame = append(frame, 0x80|byte(n))
	case n <= 0xffff:
		frame = append(frame, 0x80|126, byte(n>>8), byte(n))
	default:
		frame = binary.BigEndian.AppendUint64(append(frame, 0x80|127), uint64(n))
	}
	var mask [4]byte
	rand.Read(mask[:])
	frame = append(frame, mask[:]...)
	for i, b := range payload {
		frame = append(frame, b^mask[i%4])
	}
	_, err := c.conn.Write(frame)
	return err
}

func (c *sdcpConn) readMessage() ([]byte, error) {
	var msg []byte
	for {
		var hdr [2]byte
		if _, err := io.ReadFull(c.br, hdr[:]); err != nil {
			return nil, err
		}
		length := uint64(hdr[1] & 0x7f)
		switch length {
		case 126:
			var ext [2]byte
			if _, err := io.ReadFull(c.br, ext[:]); err != nil {
				return nil, err
			}
			length = uint64(binary.BigEndian.Uint16(ext[:]))
		case 127:
			var ext [8]byte
			if _, err := io.ReadFull(c.br, ext[:]); err != nil {
				return nil, err
			}
			length = binary.BigEndian.Uint64(ext[:])
		}
		if length > 2<<20 || uint64(len(msg))+length > 2<<20 {
			return nil, errors.New("The printer sent an oversized SDCP message.")
		}
		var mask [4]byte
		if hdr[1]&0x80 != 0 {
			if _, err := io.ReadFull(c.br, mask[:]); err != nil {
				return nil, err
			}
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(c.br, payload); err != nil {
			return nil, err
		}
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
		switch hdr[0] & 0x0f {
		case 0x8:
			return nil, io.EOF
		case 0x9:
			c.writeFrame(0xA, payload)
		case 0x0, 0x1, 0x2:
			if msg = append(msg, payload...); hdr[0]&0x80 != 0 {
				return msg, nil
			}
		}
	}
}

func (c *sdcpConn) send(cmd int, data any) error {
	if data == nil {
		data = map[string]any{}
	}
	b, _ := json.Marshal(map[string]any{"Id": "", "Data": map[string]any{"Cmd": cmd, "Data": data, "RequestID": elegooUUID(),
		"MainboardID": "", "TimeStamp": time.Now().UnixMilli(), "From": 1}})
	c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return c.writeFrame(0x1, b)
}

// next reads messages until want accepts one, within the timeout.
func (c *sdcpConn) next(timeout time.Duration, want func(sdcpMessage) bool) (sdcpMessage, error) {
	c.conn.SetReadDeadline(time.Now().Add(timeout))
	defer c.conn.SetReadDeadline(time.Time{})
	for {
		raw, err := c.readMessage()
		if err != nil {
			return sdcpMessage{}, netError(err, "the printer")
		}
		var msg sdcpMessage
		if json.Unmarshal(raw, &msg) == nil && want(msg) {
			return msg, nil
		}
	}
}

func (c *sdcpConn) status() (*sdcpStatus, error) {
	if err := c.send(0, nil); err != nil {
		return nil, netError(err, "the printer")
	}
	msg, err := c.next(10*time.Second, func(m sdcpMessage) bool { return m.Status != nil })
	return msg.Status, err
}

func (c *sdcpConn) request(cmd int, data any) (int, error) {
	if err := c.send(cmd, data); err != nil {
		return 0, netError(err, "the printer")
	}
	msg, err := c.next(30*time.Second, func(m sdcpMessage) bool { return m.Data != nil && m.Data.Cmd == cmd && m.Data.Data.Ack != nil })
	if err != nil {
		return 0, err
	}
	return *msg.Data.Data.Ack, nil
}

func pollElegoo(ctx context.Context, p *PrinterConfig, secret string, publish func(PrinterStatus), poke chan struct{}) error {
	var next func() (PrinterStatus, error)
	if elegooCC2(p) {
		// Centauri 2 exposes no print status over HTTP; report it as connected (idle) while it answers.
		h, base, err := httpSession(ctx, p, "")
		if err != nil {
			return err
		}
		next = func() (PrinterStatus, error) {
			rctx, cancel := context.WithTimeout(ctx, 10*time.Second)
			defer cancel()
			return PrinterStatus{Online: true, State: "idle"}, elegooCC2Info(rctx, h, base, secret)
		}
	} else {
		ws, err := dialSDCP(ctx, p)
		if err != nil {
			return err
		}
		defer ws.close()
		next = func() (PrinterStatus, error) {
			s, err := ws.status()
			if err != nil {
				return PrinterStatus{}, err
			}
			return elegooStatus(s), nil
		}
	}
	for {
		st, err := next()
		if err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return err
		}
		publish(st)
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-poke:
		case <-time.After(pollInterval):
		}
	}
}

// elegooStatus maps SDCP status. CurrentStatus 1 means a print job is running (paused included); PrintInfo.Status
// codes follow Elegoo's FDM table: 5/6 pausing/paused, 25/26 filament-unload pauses, 9 complete, 14 error.
func elegooStatus(s *sdcpStatus) PrinterStatus {
	round := func(v float64) float64 { return math.Round(v*10) / 10 }
	st := PrinterStatus{Online: true, State: "idle", Nozzle: &Temp{round(s.TempOfNozzle), round(s.TempTargetNozzle)},
		Bed: &Temp{round(s.TempOfHotbed), round(s.TempTargetHotbed)}}
	if s.TempOfBox != 0 {
		st.Chamber = &Temp{Actual: round(s.TempOfBox)}
	}
	if s.LightStatus != nil && s.LightStatus.SecondLight != nil {
		st.Light = ptr(*s.LightStatus.SecondLight != 0)
	}
	code := -1
	if s.PrintInfo != nil {
		code = s.PrintInfo.Status
	}
	switch {
	case slices.Contains(s.CurrentStatus, 1) && slices.Contains([]int{5, 6, 25, 26}, code):
		st.State = "paused"
	case slices.Contains(s.CurrentStatus, 1):
		st.State = "printing"
	case code == 9:
		st.State = "finished"
	case code == 14:
		st.State = "failed"
	case slices.ContainsFunc(s.CurrentStatus, func(v int) bool { return v != 0 }):
		st.State = "busy"
	}
	if pi := s.PrintInfo; pi != nil && (st.State == "printing" || st.State == "paused") {
		st.File = strings.TrimPrefix(pi.Filename, "/local/")
		if pi.TotalLayer > 0 {
			st.Layer, st.Layers = ptr(pi.CurrentLayer), ptr(pi.TotalLayer)
		}
		if pi.TotalTicks > 0 {
			st.Progress = ptr(min(1, max(0, pi.CurrentTicks/pi.TotalTicks)))
			st.Remaining = ptr(int(math.Round(max(0, pi.TotalTicks-pi.CurrentTicks))))
		} else if pi.TotalLayer > 0 {
			st.Progress = ptr(min(1, float64(pi.CurrentLayer)/float64(pi.TotalLayer)))
		}
	}
	return st
}

func elegooCommand(ctx context.Context, p *PrinterConfig, cmd int, data any) error {
	if elegooCC2(p) {
		return errors.New("Centauri 2 can't be controlled over the network—use the printer's screen.")
	}
	ws, err := dialSDCP(ctx, p)
	if err != nil {
		return err
	}
	defer ws.close()
	ack, err := ws.request(cmd, data)
	if err == nil && ack != 0 {
		err = fmt.Errorf("The printer declined the command (code %d).", ack)
	}
	return err
}

func controlElegoo(ctx context.Context, p *PrinterConfig, action string) error {
	cmd := map[string]int{"pause": 129, "stop": 130, "resume": 131}[action]
	if cmd == 0 {
		return errors.New("This printer connection only supports pause, resume and stop.")
	}
	return elegooCommand(ctx, p, cmd, nil)
}

func controlElegooDevice(ctx context.Context, p *PrinterConfig, in bambuControlRequest) error {
	if in.Value == nil {
		return errors.New("Please provide a valid printer control and a whole-number value.")
	}
	v := *in.Value
	switch {
	case in.Control == "nozzle" && v >= 0 && v <= 320:
		return elegooCommand(ctx, p, 403, map[string]any{"TempTargetNozzle": v})
	case in.Control == "bed" && v >= 0 && v <= 110:
		return elegooCommand(ctx, p, 403, map[string]any{"TempTargetHotbed": v})
	case in.Control == "light" && (v == 0 || v == 1):
		return elegooCommand(ctx, p, 403, map[string]any{"LightStatus": map[string]any{"SecondLight": v, "RgbLight": []int{0, 0, 0}}})
	case in.Control == "nozzle" || in.Control == "bed":
		return errors.New("That target temperature is outside your printer's supported range.")
	}
	return errors.New("This printer connection supports nozzle temperature, bed temperature, and light controls.")
}
