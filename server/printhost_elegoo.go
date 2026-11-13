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
	"mime/multipart"
	"net"
	"net/http"
	"net/url"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"
)

type elegooModelKind int

const (
	elegooOther elegooModelKind = iota
	elegooCC
	elegooCC2
)

const elegooChunkSize = 1048576

func classifyElegooModel(p *PrinterConfig) elegooModelKind {
	for _, candidate := range []string{strings.TrimSpace(p.model), strings.TrimSpace(p.Name), strings.TrimSpace(p.Printer)} {
		if strings.HasPrefix(candidate, "Elegoo Centauri") {
			if strings.HasSuffix(candidate, "2") {
				return elegooCC2
			}
			return elegooCC
		}
	}
	return elegooOther
}

func elegooToken(secret string) string {
	if secret != "" {
		return secret
	}
	return "123456"
}

func elegooUUID() string {
	var b [16]byte
	rand.Read(b[:])
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%08x-%04x-%04x-%04x-%012x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func testElegoo(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	kind := classifyElegooModel(p)
	if kind == elegooOther && strings.TrimSpace(p.model) == "" && !strings.HasPrefix(strings.TrimSpace(p.Printer), "Elegoo ") {
		if res := testElegooCC(ctx, p, secret); res.OK {
			return res
		}
		if res := testElegooCC2(ctx, p, secret); res.OK {
			return res
		}
	}
	switch kind {
	case elegooCC2:
		return testElegooCC2(ctx, p, secret)
	case elegooCC:
		return testElegooCC(ctx, p, secret)
	default:
		octo := *p
		octo.Kind = "octoprint"
		return testHost(ctx, &octo, secret)
	}
}

func testElegooCC(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return hostFailure(err)
	}
	req, err := http.NewRequestWithContext(ctx, "GET", base.JoinPath("").String(), nil)
	if err != nil {
		return PrinterTest{Stage: "invalid", Message: err.Error()}
	}
	resp, err := h.do(req)
	if err != nil {
		return hostFailure(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 256<<10))
	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		return PrinterTest{Stage: "auth", Message: credentialsRejected(p).Error()}
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return PrinterTest{Stage: "host", Message: fmt.Sprintf("The printer returned HTTP %d%s", resp.StatusCode, formatElegooHTTPBody(body))}
	}
	if !strings.Contains(strings.ToUpper(string(body)), "ELEGOO") {
		return PrinterTest{Stage: "host", Message: "That address responded, but it doesn't look like an Elegoo Centauri printer."}
	}
	return PrinterTest{OK: true, Stage: "ok", Message: "Connected to " + p.Name + "."}
}

func testElegooCC2(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return hostFailure(err)
	}
	token := elegooToken(secret)
	u := *base
	u.Path = strings.TrimRight(base.Path, "/") + "/system/info"
	q := u.Query()
	q.Set("X-Token", token)
	u.RawQuery = q.Encode()
	req, err := http.NewRequestWithContext(ctx, "GET", u.String(), nil)
	if err != nil {
		return PrinterTest{Stage: "invalid", Message: err.Error()}
	}
	req.Header.Set("X-Token", token)
	req.Header.Set("Accept", "application/json")
	resp, err := h.do(req)
	if err != nil {
		return hostFailure(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		return PrinterTest{Stage: "auth", Message: credentialsRejected(p).Error()}
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return PrinterTest{Stage: "host", Message: fmt.Sprintf("The printer returned HTTP %d%s", resp.StatusCode, formatElegooHTTPBody(body))}
	}
	var parsed struct {
		ErrorCode  int `json:"error_code"`
		SystemInfo struct {
			SN      string `json:"sn"`
			Machine string `json:"machine"`
		} `json:"system_info"`
	}
	if err := json.Unmarshal(body, &parsed); err != nil {
		return PrinterTest{Stage: "host", Message: "Received an unexpected response from Elegoo Centauri 2."}
	}
	if parsed.ErrorCode != 0 {
		return PrinterTest{Stage: "auth", Message: fmt.Sprintf("Elegoo Centauri 2 rejected the request (error_code=%d); check your LAN access code.", parsed.ErrorCode)}
	}
	if parsed.SystemInfo.SN == "" {
		return PrinterTest{Stage: "host", Message: "Elegoo Centauri 2 returned no serial number in system_info."}
	}
	return PrinterTest{OK: true, Stage: "ok", Model: parsed.SystemInfo.Machine, Message: "Connected to " + p.Name + "."}
}

func formatElegooHTTPBody(body []byte) string {
	s := strings.TrimSpace(string(body))
	if s == "" {
		return ""
	}
	if len(s) > 160 {
		s = s[:160] + "..."
	}
	return ": " + s
}

func fileMD5Hex(f interface {
	io.Reader
	io.Seeker
}, size int64) (string, error) {
	if _, err := f.Seek(0, io.SeekStart); err != nil {
		return "", err
	}
	h := md5.New()
	if _, err := io.Copy(h, io.LimitReader(f, size)); err != nil {
		return "", err
	}
	if _, err := f.Seek(0, io.SeekStart); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func sendElegoo(ctx context.Context, u *upload) (string, error) {
	p := u.printer
	kind := classifyElegooModel(p)
	if kind == elegooOther && strings.TrimSpace(p.model) == "" && !strings.HasPrefix(strings.TrimSpace(p.Printer), "Elegoo ") {
		if testElegooCC(ctx, p, u.secret).OK {
			kind = elegooCC
		} else if testElegooCC2(ctx, p, u.secret).OK {
			kind = elegooCC2
		}
	}
	switch kind {
	case elegooCC:
		return sendElegooCC(ctx, u)
	case elegooCC2:
		return sendElegooCC2(ctx, u)
	default:
		octo := *p
		octo.Kind = "octoprint"
		clone := *u
		clone.printer = &octo
		return sendHTTP(ctx, &clone)
	}
}

func sendElegooCC(ctx context.Context, u *upload) (string, error) {
	p := u.printer
	if res := testElegooCC(ctx, p, u.secret); !res.OK {
		return "", errors.New(res.Message)
	}
	base, _ := url.Parse(p.URL)
	client, err := hostClient(ctx, p, base)
	if err != nil {
		return "", err
	}
	h := &hostSession{client: client, printer: p, secret: u.secret}
	fileMD5, err := fileMD5Hex(u.file, u.size)
	if err != nil {
		return "", err
	}
	uploadUUID := elegooUUID()
	endpoint := base.JoinPath("uploadFile/upload").String()
	buf := make([]byte, elegooChunkSize)
	for offset := int64(0); offset < u.size || u.size == 0; {
		chunkLen := min(int64(elegooChunkSize), u.size-offset)
		if chunkLen > 0 {
			if _, err := io.ReadFull(u.file, buf[:chunkLen]); err != nil {
				return "", err
			}
		}
		if err := uploadElegooCCChunk(ctx, h, endpoint, u, fileMD5, uploadUUID, offset, buf[:chunkLen]); err != nil {
			return "", err
		}
		offset += chunkLen
		if u.size == 0 {
			break
		}
	}
	if !u.start {
		return "Uploaded " + u.filename + " to " + p.Name + ".", nil
	}
	if u.starting != nil {
		u.starting()
	}
	if err := startElegooCCPrint(ctx, u); err != nil {
		return "", err
	}
	return "Uploaded " + u.filename + " to " + p.Name + " and started printing.", nil
}

func uploadElegooCCChunk(ctx context.Context, h *hostSession, endpoint string, u *upload, fileMD5, uploadUUID string, offset int64, chunk []byte) error {
	pr, pw := io.Pipe()
	mw := multipart.NewWriter(pw)
	go func() {
		defer pw.Close()
		fields := [][2]string{
			{"Check", "1"},
			{"S-File-MD5", fileMD5},
			{"Offset", strconv.FormatInt(offset, 10)},
			{"Uuid", uploadUUID},
			{"TotalSize", strconv.FormatInt(u.size, 10)},
		}
		for _, field := range fields {
			if err := mw.WriteField(field[0], field[1]); err != nil {
				pw.CloseWithError(err)
				return
			}
		}
		part, err := mw.CreateFormFile("File", filepath.Base(u.filename))
		if err != nil {
			pw.CloseWithError(err)
			return
		}
		if _, err := io.Copy(part, progressReader{bytes.NewReader(chunk), u.progress}); err != nil {
			pw.CloseWithError(err)
			return
		}
		if err := mw.Close(); err != nil {
			pw.CloseWithError(err)
			return
		}
	}()
	req, err := http.NewRequestWithContext(ctx, "POST", endpoint, pr)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", mw.FormDataContentType())
	resp, err := h.do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		return credentialsRejected(h.printer)
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return fmt.Errorf("The printer returned HTTP %d%s", resp.StatusCode, formatElegooHTTPBody(body))
	}
	var parsed struct {
		Code     string `json:"code"`
		Messages []struct {
			Field   string `json:"field"`
			Message string `json:"message"`
		} `json:"messages"`
	}
	if err := json.Unmarshal(body, &parsed); err != nil {
		return fmt.Errorf("Invalid upload response from printer%s", formatElegooHTTPBody(body))
	}
	if parsed.Code != "000000" {
		parts := make([]string, 0, len(parsed.Messages))
		for _, m := range parsed.Messages {
			if m.Message != "" {
				if m.Field != "" {
					parts = append(parts, m.Field+": "+m.Message)
				} else {
					parts = append(parts, m.Message)
				}
			}
		}
		if len(parts) > 0 {
			return fmt.Errorf("Elegoo upload failed (code %s): %s", parsed.Code, strings.Join(parts, "; "))
		}
		return fmt.Errorf("Elegoo upload failed (code %s)", parsed.Code)
	}
	return nil
}

func sendElegooCC2(ctx context.Context, u *upload) (string, error) {
	p := u.printer
	if res := testElegooCC2(ctx, p, u.secret); !res.OK {
		return "", errors.New(res.Message)
	}
	base, _ := url.Parse(p.URL)
	client, err := hostClient(ctx, p, base)
	if err != nil {
		return "", err
	}
	h := &hostSession{client: client, printer: p, secret: u.secret}
	token := elegooToken(u.secret)
	fileMD5, err := fileMD5Hex(u.file, u.size)
	if err != nil {
		return "", err
	}
	endpoint := base.JoinPath("upload").String()
	buf := make([]byte, elegooChunkSize)
	for offset := int64(0); offset < u.size; {
		chunkLen := min(int64(elegooChunkSize), u.size-offset)
		if _, err := io.ReadFull(u.file, buf[:chunkLen]); err != nil {
			return "", err
		}
		req, err := http.NewRequestWithContext(ctx, "PUT", endpoint, progressReader{bytes.NewReader(buf[:chunkLen]), u.progress})
		if err != nil {
			return "", err
		}
		req.ContentLength = chunkLen
		req.Header.Set("Accept", "application/json")
		req.Header.Set("Content-Type", "application/octet-stream")
		req.Header.Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", offset, offset+chunkLen-1, u.size))
		req.Header.Set("X-File-Name", u.filename)
		req.Header.Set("X-File-MD5", fileMD5)
		req.Header.Set("X-Token", token)
		resp, err := h.do(req)
		if err != nil {
			return "", err
		}
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
		resp.Body.Close()
		if resp.StatusCode == 401 || resp.StatusCode == 403 {
			return "", credentialsRejected(p)
		}
		if resp.StatusCode < 200 || resp.StatusCode > 299 {
			return "", fmt.Errorf("The printer returned HTTP %d%s", resp.StatusCode, formatElegooHTTPBody(body))
		}
		var parsed struct {
			ErrorCode int `json:"error_code"`
		}
		if err := json.Unmarshal(body, &parsed); err != nil {
			return "", fmt.Errorf("Invalid upload response from Elegoo Centauri 2%s", formatElegooHTTPBody(body))
		}
		if parsed.ErrorCode != 0 {
			return "", fmt.Errorf("Elegoo Centauri 2 upload failed (error_code=%d)", parsed.ErrorCode)
		}
		offset += chunkLen
	}
	if u.start {
		u.outcome = "uploaded"
		return "Uploaded " + u.filename + " to " + p.Name + ". Auto-start isn't supported over LAN on this model—start the print from the printer screen.", nil
	}
	return "Uploaded " + u.filename + " to " + p.Name + ".", nil
}

type elegooWSConn struct {
	conn net.Conn
	br   *bufio.Reader
}

func dialElegooSDCP(ctx context.Context, p *PrinterConfig) (*elegooWSConn, error) {
	base, err := url.Parse(p.URL)
	if err != nil {
		return nil, err
	}
	wsHost := net.JoinHostPort(base.Hostname(), "3030")
	scheme := "http"
	if base.Scheme == "https" {
		scheme = "https"
	}
	target, err := p.hosts.resolve(ctx, &url.URL{Scheme: scheme, Host: wsHost})
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	conn, err := dialer.DialContext(ctx, "tcp", target.String())
	if err != nil {
		return nil, netError(err, "the printer's SDCP service (port 3030)")
	}
	if base.Scheme == "https" {
		tlsConn := tls.Client(conn, p.tlsConfig(base.Hostname()))
		if err := tlsConn.HandshakeContext(ctx); err != nil {
			conn.Close()
			return nil, netError(err, "the printer's SDCP service (port 3030)")
		}
		conn = tlsConn
	}
	if deadline, ok := ctx.Deadline(); ok {
		conn.SetDeadline(deadline)
	}
	var keyBytes [16]byte
	rand.Read(keyBytes[:])
	wsKey := base64.StdEncoding.EncodeToString(keyBytes[:])
	reqStr := fmt.Sprintf("GET /websocket HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", wsHost, wsKey)
	if _, err := io.WriteString(conn, reqStr); err != nil {
		conn.Close()
		return nil, err
	}
	br := bufio.NewReader(conn)
	req, _ := http.NewRequest("GET", "http://"+wsHost+"/websocket", nil)
	resp, err := http.ReadResponse(br, req)
	if err != nil {
		conn.Close()
		return nil, err
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusSwitchingProtocols {
		conn.Close()
		return nil, fmt.Errorf("SDCP WebSocket upgrade failed with HTTP %d", resp.StatusCode)
	}
	sum := sha1.Sum([]byte(wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	expected := base64.StdEncoding.EncodeToString(sum[:])
	if resp.Header.Get("Sec-WebSocket-Accept") != expected {
		conn.Close()
		return nil, errors.New("SDCP WebSocket handshake returned an invalid Sec-WebSocket-Accept key")
	}
	return &elegooWSConn{conn: conn, br: br}, nil
}

func (c *elegooWSConn) close() {
	_ = c.writeFrame(0x8, nil)
	_ = c.conn.Close()
}

func (c *elegooWSConn) writeFrame(opcode byte, payload []byte) error {
	header := make([]byte, 0, 14)
	header = append(header, 0x80|opcode)
	n := len(payload)
	switch {
	case n < 126:
		header = append(header, 0x80|byte(n))
	case n <= 0xffff:
		header = append(header, 0x80|126, byte(n>>8), byte(n))
	default:
		var lenBuf [8]byte
		binary.BigEndian.PutUint64(lenBuf[:], uint64(n))
		header = append(header, 0x80|127)
		header = append(header, lenBuf[:]...)
	}
	var mask [4]byte
	rand.Read(mask[:])
	header = append(header, mask[:]...)
	masked := make([]byte, n)
	for i, b := range payload {
		masked[i] = b ^ mask[i%4]
	}
	if _, err := c.conn.Write(header); err != nil {
		return err
	}
	if n > 0 {
		if _, err := c.conn.Write(masked); err != nil {
			return err
		}
	}
	return nil
}

func (c *elegooWSConn) writeJSON(v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.writeFrame(0x1, b)
}

func (c *elegooWSConn) readMessage() ([]byte, error) {
	var msg bytes.Buffer
	for {
		var hdr [2]byte
		if _, err := io.ReadFull(c.br, hdr[:]); err != nil {
			return nil, err
		}
		fin := hdr[0]&0x80 != 0
		opcode := hdr[0] & 0x0f
		masked := hdr[1]&0x80 != 0
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
		if length > 2<<20 {
			return nil, errors.New("SDCP WebSocket frame exceeded size limit")
		}
		var mask [4]byte
		if masked {
			if _, err := io.ReadFull(c.br, mask[:]); err != nil {
				return nil, err
			}
		}
		payload := make([]byte, length)
		if _, err := io.ReadFull(c.br, payload); err != nil {
			return nil, err
		}
		if masked {
			for i := range payload {
				payload[i] ^= mask[i%4]
			}
		}
		switch opcode {
		case 0x8:
			return nil, io.EOF
		case 0x9:
			_ = c.writeFrame(0xA, payload)
			continue
		case 0xA:
			continue
		case 0x0, 0x1, 0x2:
			msg.Write(payload)
			if fin {
				return msg.Bytes(), nil
			}
		}
	}
}

func sdcpPacket(cmd int, data any) map[string]any {
	if data == nil {
		data = map[string]any{}
	}
	return map[string]any{
		"Id": "",
		"Data": map[string]any{
			"Cmd":         cmd,
			"Data":        data,
			"RequestID":   elegooUUID(),
			"MainboardID": "",
			"TimeStamp":   time.Now().UnixMilli(),
			"From":        1,
		},
	}
}

type elegooSDCPStatus struct {
	CurrentStatus    []int   `json:"CurrentStatus"`
	TempOfNozzle     float64 `json:"TempOfNozzle"`
	TempTargetNozzle float64 `json:"TempTargetNozzle"`
	TempOfHotbed     float64 `json:"TempOfHotbed"`
	TempTargetHotbed float64 `json:"TempTargetHotbed"`
	TempOfBox        float64 `json:"TempOfBox"`
	CurrentFanSpeed  *struct {
		ModelFan     float64 `json:"ModelFan"`
		AuxiliaryFan float64 `json:"AuxiliaryFan"`
		BoxFan       float64 `json:"BoxFan"`
	} `json:"CurrentFanSpeed"`
	LightStatus *struct {
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

type elegooSDCPMessage struct {
	Topic  string            `json:"Topic"`
	Status *elegooSDCPStatus `json:"Status"`
	Data   *struct {
		Cmd  int `json:"Cmd"`
		Data struct {
			Ack *int `json:"Ack"`
		} `json:"Data"`
	} `json:"Data"`
}

func elegooSleep(ctx context.Context, d time.Duration) error {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-t.C:
		return nil
	}
}

func startElegooCCPrint(ctx context.Context, u *upload) error {
	startCtx, cancel := context.WithTimeout(ctx, 90*time.Second)
	defer cancel()
	ws, err := dialElegooSDCP(startCtx, u.printer)
	if err != nil {
		return err
	}
	defer ws.close()

	if err := elegooSleep(startCtx, time.Second); err != nil {
		return err
	}
	checkDeadline := time.Now().Add(60 * time.Second)
	for {
		if time.Now().After(checkDeadline) {
			return errors.New("Timed out waiting for the printer to finish checking the uploaded file.")
		}
		_ = ws.conn.SetDeadline(time.Now().Add(5 * time.Second))
		if err := ws.writeJSON(sdcpPacket(0, nil)); err != nil {
			return err
		}
		var msg elegooSDCPMessage
		ready := false
		readUntil := time.Now().Add(2 * time.Second)
		for time.Now().Before(readUntil) {
			raw, err := ws.readMessage()
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					break
				}
				return err
			}
			if json.Unmarshal(raw, &msg) == nil && msg.Status != nil {
				if !slices.Contains(msg.Status.CurrentStatus, 8) {
					ready = true
					break
				}
			}
		}
		if ready {
			break
		}
		if err := elegooSleep(startCtx, time.Second); err != nil {
			return err
		}
	}

	if err := elegooSleep(startCtx, time.Second); err != nil {
		return err
	}
	_ = ws.conn.SetDeadline(time.Now().Add(30 * time.Second))
	startData := map[string]any{
		"Filename":           "/local/" + u.filename,
		"StartLayer":         0,
		"Calibration_switch": boolInt(u.options.bedLeveling),
		"PrintPlatformType":  u.options.bedType,
		"Tlp_Switch":         boolInt(u.options.timelapse),
	}
	if err := ws.writeJSON(sdcpPacket(128, startData)); err != nil {
		return err
	}
	for {
		raw, err := ws.readMessage()
		if err != nil {
			return err
		}
		var msg elegooSDCPMessage
		if json.Unmarshal(raw, &msg) != nil || msg.Data == nil || msg.Data.Cmd != 128 {
			continue
		}
		if msg.Data.Data.Ack == nil {
			return errors.New("Printer sent a start-print reply without an acknowledgement code.")
		}
		switch *msg.Data.Data.Ack {
		case 0:
			return nil
		case 1:
			return &connError{"busy", "the printer is busy; wait for its current job to finish"}
		case 2:
			return errors.New("The printer couldn't find the uploaded file.")
		case 3:
			return errors.New("The printer reported that the uploaded file failed MD5 verification.")
		case 4, 5, 6:
			return fmt.Errorf("The printer reported a file transfer error (ack=%d).", *msg.Data.Data.Ack)
		case 7:
			return errors.New("The printer rejected the print command for this model (ack=7).")
		default:
			return fmt.Errorf("The printer rejected the start-print command (ack=%d).", *msg.Data.Data.Ack)
		}
	}
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func pollElegoo(ctx context.Context, p *PrinterConfig, secret string, publish func(PrinterStatus), poke chan struct{}) error {
	kind := classifyElegooModel(p)
	if kind == elegooCC2 {
		for {
			rctx, cancel := context.WithTimeout(ctx, 10*time.Second)
			res := testElegooCC2(rctx, p, secret)
			cancel()
			if !res.OK {
				return errors.New(res.Message)
			}
			publish(PrinterStatus{Printer: p.Printer, Online: true, State: "idle", At: time.Now().UTC()})
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-poke:
			case <-time.After(pollInterval):
			}
		}
	}
	if kind == elegooOther && !testElegooCC(ctx, p, secret).OK {
		octo := *p
		octo.Kind = "octoprint"
		return pollHost(ctx, &octo, secret, func(st PrinterStatus) {
			st.Printer = p.Printer
			publish(st)
		}, poke)
	}
	dctx, cancel := context.WithTimeout(ctx, 8*time.Second)
	ws, err := dialElegooSDCP(dctx, p)
	cancel()
	if err != nil {
		return err
	}
	stop := context.AfterFunc(ctx, func() { _ = ws.conn.Close() })
	defer func() {
		stop()
		ws.close()
	}()
	for {
		_ = ws.conn.SetDeadline(time.Now().Add(8 * time.Second))
		if err := ws.writeJSON(sdcpPacket(0, nil)); err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return err
		}
		for {
			raw, err := ws.readMessage()
			if err != nil {
				if ctx.Err() != nil {
					return ctx.Err()
				}
				return err
			}
			var msg elegooSDCPMessage
			if json.Unmarshal(raw, &msg) != nil || msg.Status == nil {
				continue
			}
			publish(decodeElegooStatus(p.Printer, msg.Status))
			break
		}
		_ = ws.conn.SetDeadline(time.Time{})
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-poke:
		case <-time.After(pollInterval):
		}
	}
}

func decodeElegooStatus(printer string, s *elegooSDCPStatus) PrinterStatus {
	st := PrinterStatus{
		Printer: printer,
		Online:  true,
		State:   "idle",
		Nozzle:  &Temp{Actual: math.Round(s.TempOfNozzle*10) / 10, Target: math.Round(s.TempTargetNozzle*10) / 10},
		Bed:     &Temp{Actual: math.Round(s.TempOfHotbed*10) / 10, Target: math.Round(s.TempTargetHotbed*10) / 10},
		At:      time.Now().UTC(),
	}
	if s.TempOfBox != 0 {
		st.Chamber = &Temp{Actual: math.Round(s.TempOfBox*10) / 10}
	}
	if s.CurrentFanSpeed != nil {
		st.Fans = map[string]float64{
			"part":      math.Round(s.CurrentFanSpeed.ModelFan),
			"auxiliary": math.Round(s.CurrentFanSpeed.AuxiliaryFan),
			"chamber":   math.Round(s.CurrentFanSpeed.BoxFan),
		}
	}
	if s.LightStatus != nil && s.LightStatus.SecondLight != nil {
		light := *s.LightStatus.SecondLight != 0
		st.Light = &light
	}
	if s.PrintInfo != nil {
		pi := s.PrintInfo
		if pi.Filename != "" {
			st.File = strings.TrimPrefix(pi.Filename, "/local/")
		}
		if pi.TotalLayer > 0 {
			cur, tot := pi.CurrentLayer, pi.TotalLayer
			st.Layer, st.Layers = &cur, &tot
		}
		if pi.TotalTicks > 0 {
			pct := min(1, max(0, pi.CurrentTicks/pi.TotalTicks))
			st.Progress = &pct
			if rem := int(math.Round(max(0, pi.TotalTicks-pi.CurrentTicks))); rem > 0 {
				st.Remaining = &rem
			}
		} else if pi.TotalLayer > 0 {
			pct := min(1, max(0, float64(pi.CurrentLayer)/float64(pi.TotalLayer)))
			st.Progress = &pct
		}
		switch pi.Status {
		case 1, 13, 15, 16, 18, 19, 20:
			st.State = "printing"
		case 5, 6, 12:
			st.State = "paused"
		case 7, 8, 9, 0:
			if slices.Contains(s.CurrentStatus, 1) {
				st.State = "printing"
			} else if slices.Contains(s.CurrentStatus, 2) || slices.Contains(s.CurrentStatus, 8) {
				st.State = "busy"
			}
		case 14:
			st.State = "error"
		}
	} else if slices.Contains(s.CurrentStatus, 1) {
		st.State = "printing"
	} else if slices.Contains(s.CurrentStatus, 2) || slices.Contains(s.CurrentStatus, 8) {
		st.State = "busy"
	}
	if st.State == "idle" || st.State == "error" {
		st.Progress = nil
		st.Remaining = nil
	}
	return st
}

func sendElegooSDCPCommand(ctx context.Context, p *PrinterConfig, cmd int, data any) error {
	ws, err := dialElegooSDCP(ctx, p)
	if err != nil {
		return err
	}
	defer ws.close()
	return ws.writeJSON(sdcpPacket(cmd, data))
}

func controlElegoo(ctx context.Context, p *PrinterConfig, secret, action string) error {
	if classifyElegooModel(p) == elegooOther && !testElegooCC(ctx, p, secret).OK {
		octo := *p
		octo.Kind = "octoprint"
		return controlHTTP(ctx, &octo, secret, action)
	}
	if classifyElegooModel(p) == elegooCC2 {
		return errors.New("Remote print control isn't supported over LAN on Elegoo Centauri 2.")
	}
	cmd := map[string]int{"pause": 129, "stop": 130, "resume": 131}[action]
	if cmd == 0 {
		return errors.New("Unsupported printer command.")
	}
	return sendElegooSDCPCommand(ctx, p, cmd, nil)
}

func controlElegooDevice(ctx context.Context, p *PrinterConfig, in bambuControlRequest) error {
	if in.Value == nil {
		return errors.New("Please provide a valid printer control and a whole-number value.")
	}
	value := *in.Value
	switch in.Control {
	case "nozzle":
		if value < 0 || value > 320 {
			return errors.New("That target temperature is outside your printer's supported range.")
		}
		return sendElegooSDCPCommand(ctx, p, 403, map[string]any{"TempTargetNozzle": value})
	case "bed":
		if value < 0 || value > 110 {
			return errors.New("That target temperature is outside your printer's supported range.")
		}
		return sendElegooSDCPCommand(ctx, p, 403, map[string]any{"TempTargetHotbed": value})
	case "light":
		if value != 0 && value != 1 {
			return errors.New("Light setting must be 0 (off) or 1 (on).")
		}
		return sendElegooSDCPCommand(ctx, p, 403, map[string]any{"LightStatus": map[string]any{"SecondLight": value, "RgbLight": []int{0, 0, 0}}})
	default:
		return errors.New("This printer connection supports nozzle temperature, bed temperature, and light controls.")
	}
}
