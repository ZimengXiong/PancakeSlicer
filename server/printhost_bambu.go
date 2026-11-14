package main

import (
	"archive/zip"
	"bufio"
	"context"
	"crypto/md5"
	"crypto/rand"
	"crypto/tls"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const bambuUser = "bblp"

func sendBambu(ctx context.Context, u *upload) (string, error) {
	p := u.printer
	if strings.ContainsAny(u.filename, "\r\n/") {
		return "", errors.New("That file name contains characters that can't be sent over FTP.")
	}
	plate, err := packagePlate(u.file)
	if err != nil {
		return "", err
	}
	digest := md5.New()
	if _, err := io.Copy(digest, u.file); err != nil {
		return "", err
	}
	if _, err := u.file.Seek(0, io.SeekStart); err != nil {
		return "", err
	}
	tlsConfig := p.tlsConfig(p.Serial)
	tlsConfig.ClientSessionCache = tls.NewLRUClientSessionCache(4)
	host, err := p.resolvedHost(ctx)
	if err != nil {
		return "", err
	}
	if err := ftpsUpload(ctx, host, tlsConfig, u); err != nil {
		return "", err
	}
	u.stored()
	if !u.start {
		return u.placed() + ".", nil
	}
	u.starting()
	return mqttProjectFile(ctx, p, u, plate, strings.ToUpper(hex.EncodeToString(digest.Sum(nil))))
}

var plateEntry = regexp.MustCompile(`^Metadata/plate_[0-9]+\.gcode$`)

func packagePlate(f *os.File) (string, error) {
	st, err := f.Stat()
	if err != nil {
		return "", err
	}
	z, err := zip.NewReader(f, st.Size())
	if err != nil {
		return "", errors.New("The exported file isn't a valid .gcode.3mf package.")
	}
	for _, entry := range z.File {
		if plateEntry.MatchString(entry.Name) {
			return entry.Name, nil
		}
	}
	return "", errors.New("The .gcode.3mf package doesn't contain any sliced plate G-code.")
}

type connError struct{ stage, msg string }

func (e *connError) Error() string { return e.msg }

func dialTLS(ctx context.Context, host string, port int, config *tls.Config) (*tls.Conn, error) {
	dctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	raw, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(dctx, "tcp", net.JoinHostPort(host, strconv.Itoa(port)))
	if err != nil {
		if cause := context.Cause(ctx); cause != nil {
			return nil, cause
		}
		return nil, dialError(err, host, port)
	}
	conn := tls.Client(raw, config)
	if err := conn.HandshakeContext(dctx); err != nil {
		raw.Close()
		if cause := context.Cause(ctx); cause != nil {
			return nil, cause
		}
		return nil, handshakeError(err, port)
	}
	context.AfterFunc(ctx, func() { conn.SetDeadline(time.Unix(1, 0)) })
	return conn, nil
}

func dialError(err error, host string, port int) error {
	var dns *net.DNSError
	msg := fmt.Sprintf("cannot connect to %s on port %d: %s", host, port, lastClause(err))
	switch {
	case errors.Is(err, syscall.ECONNREFUSED):
		msg = fmt.Sprintf("%s refused port %d: LAN-only mode is off, or this is not the printer's address", host, port)
	case errors.Is(err, context.DeadlineExceeded) || os.IsTimeout(err):
		msg = fmt.Sprintf("no answer from %s on port %d: check the IP address and that the printer is on the server's network", host, port)
	case errors.Is(err, syscall.EHOSTUNREACH) || errors.Is(err, syscall.ENETUNREACH):
		msg = fmt.Sprintf("no route from the server to %s", host)
	case errors.As(err, &dns):
		msg = "cannot resolve " + host
	}
	return &connError{"unreachable", msg}
}

func handshakeError(err error, port int) error {
	var serial *serialMismatch
	switch {
	case errors.As(err, &serial):
		return &connError{"serial", serial.Error() + "; check the serial number (Settings > Device on the printer)"}
	case errors.Is(err, errPinMismatch):
		return &connError{"tls", "the printer's certificate changed since it was first trusted; if the printer was replaced or reset, remove it and add it again"}
	}
	return &connError{"tls", fmt.Sprintf("TLS handshake on port %d failed: %s", port, lastClause(err))}
}

type ftpConn struct {
	conn *tls.Conn
	r    *bufio.Reader
}

func (c *ftpConn) reply(timeout time.Duration) (int, string, error) {
	c.conn.SetReadDeadline(time.Now().Add(timeout))
	line, err := c.r.ReadString('\n')
	if err != nil {
		return 0, "", err
	}
	if len(line) < 4 {
		return 0, "", errors.New("malformed FTP reply")
	}
	code, err := strconv.Atoi(line[:3])
	if err != nil {
		return 0, "", errors.New("malformed FTP reply")
	}
	text := strings.TrimSpace(line[4:])
	if line[3] == '-' {
		for {
			next, err := c.r.ReadString('\n')
			if err != nil {
				return 0, "", err
			}
			if strings.HasPrefix(next, line[:3]+" ") {
				text = strings.TrimSpace(next[4:])
				break
			}
		}
	}
	return code, text, nil
}

func (c *ftpConn) cmd(want int, timeout time.Duration, format string, args ...any) (string, error) {
	c.conn.SetWriteDeadline(time.Now().Add(timeout))
	if _, err := fmt.Fprintf(c.conn, format+"\r\n", args...); err != nil {
		return "", err
	}
	code, text, err := c.reply(timeout)
	if err != nil {
		return "", err
	}
	if code/100 != want/100 && code != want {
		return text, &ftpError{code, text}
	}
	return text, nil
}

type ftpError struct {
	code int
	text string
}

func (e *ftpError) Error() string { return fmt.Sprintf("FTP %d %s", e.code, e.text) }

var pasvReply = regexp.MustCompile(`(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)`)

func ftpsLogin(ctx context.Context, host string, config *tls.Config, secret string) (c *ftpConn, err error) {
	conn, err := dialTLS(ctx, host, 990, config)
	if err != nil {
		return nil, err
	}
	c = &ftpConn{conn: conn, r: bufio.NewReader(conn)}
	defer func() {
		if err != nil {
			conn.Close()
			err = ftpFailure(ctx, err, "login")
		}
	}()
	if code, text, rerr := c.reply(15 * time.Second); rerr != nil {
		return nil, rerr
	} else if code != 220 {
		return nil, &ftpError{code, text}
	}
	if _, err = c.cmd(331, 15*time.Second, "USER %s", bambuUser); err != nil {
		return nil, err
	}
	if _, err = c.cmd(230, 15*time.Second, "PASS %s", secret); err != nil {
		return nil, err
	}
	return c, nil
}

func ftpFailure(ctx context.Context, err error, step string) error {
	var ftpErr *ftpError
	var connErr *connError
	switch {
	case context.Cause(ctx) != nil:
		return context.Cause(ctx)
	case errors.As(err, &connErr):
		return err
	case errors.As(err, &ftpErr) && ftpErr.code == 530:
		return &connError{"access_code", "the printer rejected the access code (FTPS); check it on the printer's screen (Settings > Network)"}
	case errors.As(err, &ftpErr) && ftpErr.code == 522:
		return errors.New("The printer declined the FTP data transfer (TLS session reuse). Please report this along with your printer's firmware version.")
	case errors.As(err, &ftpErr):
		return fmt.Errorf("File transfer to the printer failed at %s: %v", step, ftpErr)
	}
	return netError(err, "the printer (FTPS port 990, "+step+")")
}

func ftpsUpload(ctx context.Context, host string, config *tls.Config, u *upload) (err error) {
	c, err := ftpsLogin(ctx, host, config, u.secret)
	if err != nil {
		return err
	}
	defer c.conn.Close()
	step := "setup"
	defer func() {
		if err != nil {
			err = ftpFailure(ctx, err, step)
		}
	}()
	for _, command := range []string{"PBSZ 0", "PROT P", "TYPE I"} {
		if _, err = c.cmd(200, 15*time.Second, "%s", command); err != nil {
			return err
		}
	}
	if u.resend {
		if text, err := c.cmd(213, 15*time.Second, "SIZE %s", u.filename); err == nil && strings.TrimSpace(text) == strconv.FormatInt(u.size, 10) {
			u.onCard = true
			u.progress.Store(u.size)
			c.cmd(221, 5*time.Second, "QUIT")
			return nil
		}
	}
	text, err := c.cmd(227, 15*time.Second, "PASV")
	if err != nil {
		return err
	}
	m := pasvReply.FindStringSubmatch(text)
	if m == nil {
		return errors.New("malformed PASV reply")
	}
	hi, _ := strconv.Atoi(m[5])
	lo, _ := strconv.Atoi(m[6])
	step = "data connection"
	raw, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", net.JoinHostPort(host, strconv.Itoa(hi<<8|lo)))
	if err != nil {
		return err
	}
	defer raw.Close()
	limitSendBuffer(raw)
	step = "upload"
	if _, err = c.cmd(150, 15*time.Second, "STOR %s", u.filename); err != nil {
		return err
	}
	data := tls.Client(raw, config)
	stop := context.AfterFunc(ctx, func() { raw.SetDeadline(time.Unix(1, 0)) })
	defer stop()
	data.SetDeadline(time.Now().Add(15 * time.Second))
	if err = data.HandshakeContext(ctx); err != nil {
		return err
	}
	data.SetDeadline(time.Time{})
	buf := make([]byte, 64<<10)
	for {
		n, rerr := u.file.Read(buf)
		if n > 0 {
			data.SetWriteDeadline(time.Now().Add(sendStall))
			if _, err = data.Write(buf[:n]); err != nil {
				return err
			}
			u.progress.Add(int64(n))
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			return rerr
		}
	}
	if err = data.CloseWrite(); err != nil {
		return err
	}
	data.SetReadDeadline(time.Now().Add(2 * time.Second))
	io.Copy(io.Discard, data)
	data.Close()
	step = "completion"
	code, text, err := c.reply(2 * time.Minute)
	if err != nil {
		return err
	}
	if code != 226 && code != 250 {
		return &ftpError{code, text}
	}
	c.cmd(221, 5*time.Second, "QUIT")
	return nil
}

func mqttSession(ctx context.Context, p *PrinterConfig, secret string) (*tls.Conn, *bufio.Reader, error) {
	host, err := p.resolvedHost(ctx)
	if err != nil {
		return nil, nil, err
	}
	conn, err := dialTLS(ctx, host, 8883, p.tlsConfig(p.Serial))
	if err != nil {
		return nil, nil, err
	}
	fail := func(err error) (*tls.Conn, *bufio.Reader, error) {
		conn.Close()
		return nil, nil, err
	}
	r := bufio.NewReader(conn)
	id := make([]byte, 6)
	rand.Read(id)
	var connect []byte
	connect = append(connect, 0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 60)
	connect = append(connect, mqttString("pancakeslicer-"+hex.EncodeToString(id))...)
	connect = append(connect, mqttString(bambuUser)...)
	connect = append(connect, mqttString(secret)...)
	conn.SetDeadline(time.Now().Add(15 * time.Second))
	if err := mqttWrite(conn, 0x10, connect); err != nil {
		return fail(netError(err, "the printer (MQTT port 8883)"))
	}
	kind, body, err := mqttRead(r)
	if err != nil {
		if errors.Is(err, io.EOF) {
			return fail(&connError{"access_code", "the printer closed the MQTT connection at login; check the access code"})
		}
		return fail(netError(err, "the printer (MQTT port 8883)"))
	}
	if kind != 2 || len(body) != 2 {
		return fail(errors.New("unexpected MQTT reply"))
	}
	switch body[1] {
	case 0:
	case 4, 5:
		return fail(&connError{"access_code", "the printer rejected the access code (MQTT); check it on the printer's screen (Settings > Network)"})
	default:
		return fail(fmt.Errorf("The printer declined the MQTT connection (code %d).", body[1]))
	}
	subscribe := append([]byte{0, 1}, mqttString("device/"+p.Serial+"/report")...)
	subscribe = append(subscribe, 0)
	if err := mqttWrite(conn, 0x82, subscribe); err != nil {
		return fail(err)
	}
	return conn, r, nil
}

func mqttPublish(conn *tls.Conn, p *PrinterConfig, message any) error {
	command, _ := json.Marshal(message)
	return mqttWrite(conn, 0x30, append(mqttString("device/"+p.Serial+"/request"), command...))
}

func mqttPayload(kind byte, body []byte) []byte {
	if kind != 3 || len(body) < 2 {
		return nil
	}
	topicLength := int(binary.BigEndian.Uint16(body))
	if 2+topicLength > len(body) {
		return nil
	}
	return body[2+topicLength:]
}

func refusedForAuth(result, reason string, code int) bool {
	return !strings.EqualFold(result, "success") && (strings.Contains(strings.ToLower(reason), "verif") || code == 84033543)
}

func mqttProjectFile(ctx context.Context, p *PrinterConfig, u *upload, plate, md5sum string) (string, error) {
	fail := func(err error) (string, error) {
		if cause := context.Cause(ctx); cause != nil {
			err = cause
		}
		return "", fmt.Errorf("%s, but the printer couldn't start the print automatically: %w", u.placed(), err)
	}
	conn, r, err := mqttSession(ctx, p, u.secret)
	if err != nil {
		return fail(err)
	}
	defer conn.Close()
	sequence := strconv.FormatInt(time.Now().UnixMilli()%1_000_000_000, 10)
	name := strings.TrimSuffix(u.filename, ".gcode.3mf")
	mapping := []int{}
	if u.options.amsMapping != nil {
		mapping = u.options.amsMapping
	} else if u.options.useAMS {
		mapping = []int{0}
	}
	legacyMapping, addresses, err := bambuMapping(mapping, u.options.amsMapping2, len(mapping))
	if err != nil {
		return fail(err)
	}
	if err := mqttPublish(conn, p, map[string]any{"print": map[string]any{
		"sequence_id": sequence, "command": "project_file", "param": plate,
		"project_id": "0", "profile_id": "0", "task_id": "0", "subtask_id": "0", "subtask_name": name,
		"file": u.filename, "url": "file:///sdcard/" + u.filename, "md5": md5sum,
		"timelapse": u.options.timelapse, "bed_type": "auto", "bed_leveling": u.options.bedLeveling, "flow_cali": u.options.flowCali,
		"vibration_cali": u.options.vibrationCali, "layer_inspect": false, "use_ams": u.options.useAMS, "ams_mapping": legacyMapping, "ams_mapping2": addresses,
	}}); err != nil {
		return fail(err)
	}
	conn.SetDeadline(time.Now().Add(20 * time.Second))
	for {
		kind, body, err := mqttRead(r)
		if err != nil {
			if os.IsTimeout(err) {
				mqttWrite(conn, 0xE0, nil)
				u.outcome = "waiting_for_printer"
				return "Sent, waiting for the printer to start. No confirmation within 20 s; check its screen before sending again.", nil
			}
			return fail(err)
		}
		payload := mqttPayload(kind, body)
		if payload == nil {
			continue
		}
		var message struct {
			Print struct {
				Command   string `json:"command"`
				Sequence  string `json:"sequence_id"`
				Result    string `json:"result"`
				Reason    string `json:"reason"`
				State     string `json:"gcode_state"`
				Subtask   string `json:"subtask_name"`
				ErrorCode int    `json:"print_error"`
				ErrCode   int    `json:"err_code"`
				GcodeFile string `json:"gcode_file"`
			} `json:"print"`
		}
		if json.Unmarshal(payload, &message) != nil {
			continue
		}
		m := message.Print
		if m.Command == "project_file" && m.Sequence == sequence {
			if strings.EqualFold(m.Result, "success") {
				mqttWrite(conn, 0xE0, nil)
				u.outcome = "printing"
				return u.placed() + " and started printing.", nil
			}
			reason := m.Reason
			if reason == "" {
				reason = m.Result
			}
			if refusedForAuth(m.Result, m.Reason, m.ErrCode) {
				return fail(fmt.Errorf("The printer declined the start command (%s)—make sure LAN Only mode and Developer Mode are enabled on the printer.", reason))
			}
			return fail(fmt.Errorf("The printer declined the print job: %s", reason))
		}
		if (m.State == "PREPARE" || m.State == "RUNNING") && (m.Subtask == name || m.GcodeFile == u.filename) {
			mqttWrite(conn, 0xE0, nil)
			u.outcome = "printing"
			return u.placed() + " and started printing.", nil
		}
	}
}

func mqttString(s string) []byte {
	return append(binary.BigEndian.AppendUint16(nil, uint16(len(s))), s...)
}

func mqttWrite(w io.Writer, header byte, body []byte) error {
	packet := []byte{header}
	n := len(body)
	for {
		b := byte(n % 128)
		n /= 128
		if n > 0 {
			b |= 0x80
		}
		packet = append(packet, b)
		if n == 0 {
			break
		}
	}
	_, err := w.Write(append(packet, body...))
	return err
}

func mqttRead(r *bufio.Reader) (byte, []byte, error) {
	header, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	length, shift := 0, 0
	for i := 0; ; i++ {
		b, err := r.ReadByte()
		if err != nil {
			return 0, nil, err
		}
		length |= int(b&0x7f) << shift
		shift += 7
		if b&0x80 == 0 {
			break
		}
		if i == 3 {
			return 0, nil, errors.New("malformed MQTT length")
		}
	}
	if length > 4<<20 {
		return 0, nil, errors.New("MQTT packet too large")
	}
	body := make([]byte, length)
	if _, err := io.ReadFull(r, body); err != nil {
		return 0, nil, err
	}
	return header >> 4, body, nil
}

type PrinterTest struct {
	OK      bool   `json:"ok"`
	Stage   string `json:"stage"`
	Message string `json:"message"`
	Model   string `json:"model,omitempty"`
}

func testBambu(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	failed := func(err error) PrinterTest {
		var c *connError
		if errors.As(err, &c) {
			return PrinterTest{Stage: c.stage, Message: c.msg}
		}
		return PrinterTest{Stage: "unreachable", Message: err.Error()}
	}
	conn, r, err := mqttSession(ctx, p, secret)
	if err != nil {
		return failed(err)
	}
	defer conn.Close()
	sequence := strconv.FormatInt(time.Now().UnixMilli()%1_000_000_000, 10)
	mqttPublish(conn, p, map[string]any{"info": map[string]any{"sequence_id": sequence, "command": "get_version"}})
	mqttPublish(conn, p, map[string]any{"print": map[string]any{"sequence_id": sequence, "command": "gcode_line", "param": "; PancakeSlicer connection test\n"}})
	model, answered := "", false
	conn.SetDeadline(time.Now().Add(8 * time.Second))
	for model == "" || !answered {
		kind, body, err := mqttRead(r)
		if err != nil {
			break
		}
		var message struct {
			Print struct {
				Command  string `json:"command"`
				Sequence string `json:"sequence_id"`
				Result   string `json:"result"`
				Reason   string `json:"reason"`
				ErrCode  int    `json:"err_code"`
			} `json:"print"`
			Info struct {
				Command string `json:"command"`
				Module  []struct {
					Product string `json:"product_name"`
					Project string `json:"project_name"`
				} `json:"module"`
			} `json:"info"`
		}
		if json.Unmarshal(mqttPayload(kind, body), &message) != nil {
			continue
		}
		if m := message.Print; m.Command == "gcode_line" && m.Sequence == sequence {
			if refusedForAuth(m.Result, m.Reason, m.ErrCode) {
				mqttWrite(conn, 0xE0, nil)
				return PrinterTest{Stage: "developer_mode", Message: "the printer refused a LAN command (" + m.Reason + ")"}
			}
			answered = true
		}
		if message.Info.Command == "get_version" {
			for _, m := range message.Info.Module {
				if model == "" {
					model = bambuModel(m.Product, m.Project)
				}
			}
			if model == "" {
				model = "?"
			}
		}
	}
	mqttWrite(conn, 0xE0, nil)
	if model == "" || model == "?" {
		model = bambuModel("", p.Serial[:3])
	}
	host, err := p.resolvedHost(ctx)
	if err != nil {
		return failed(err)
	}
	c, err := ftpsLogin(ctx, host, p.tlsConfig(p.Serial), secret)
	if err != nil {
		result := failed(err)
		if result.Stage == "unreachable" {
			result.Message = "MQTT works, but " + result.Message
		}
		return result
	}
	c.cmd(221, 5*time.Second, "QUIT")
	c.conn.Close()
	message := "Connected"
	if model != "" {
		message += " to " + model
	}
	if !answered {
		message += "; the printer did not answer the test command, so Developer Mode is unconfirmed"
	}
	return PrinterTest{OK: true, Stage: "ok", Message: message, Model: model}
}

func bambuModel(product, code string) string {
	if product != "" {
		return strings.TrimPrefix(product, "Bambu Lab ")
	}
	return map[string]string{
		"N1": "A1 mini", "N2S": "A1", "C11": "P1P", "C12": "P1S", "C13": "X1E", "BL-P001": "X1 Carbon", "BL-P002": "X1", "O1D": "H2D",
		"030": "A1 mini", "039": "A1", "01S": "P1P", "01P": "P1S", "00M": "X1 Carbon", "00W": "X1", "03W": "X1E", "094": "H2D",
	}[code]
}

type AMSTray struct {
	Kind       string `json:"kind,omitempty"`
	Extruder   *int   `json:"extruder,omitempty"`
	ID         int    `json:"id"`
	AMS        int    `json:"ams"`
	Slot       int    `json:"slot"`
	Type       string `json:"type"`
	Color      string `json:"color"`
	Name       string `json:"name,omitempty"`
	FilamentID string `json:"filament_id,omitempty"`
	Spool      string `json:"spool,omitempty"`
	Remaining  *int   `json:"remaining,omitempty"`
	Active     bool   `json:"active,omitempty"`
}

type bambuExternalTray struct {
	ID        string `json:"id"`
	Type      string `json:"tray_type"`
	Color     string `json:"tray_color"`
	Idx       string `json:"tray_info_idx"`
	UUID      string `json:"tray_uuid"`
	Remaining *int   `json:"remain"`
}

func spoolID(uuid string) string {
	if strings.Trim(uuid, "0") == "" {
		return ""
	}
	return uuid
}

type bambuReport struct {
	SpeedMode *int            `json:"spd_lvl"`
	StartTime json.RawMessage `json:"start_time"`
	Lights    []struct {
		Node string `json:"node"`
		Mode string `json:"mode"`
	} `json:"lights_report"`
	HMS []struct {
		Attr    uint32 `json:"attr"`
		Code    uint32 `json:"code"`
		Message string `json:"message"`
	} `json:"hms"`
	State               string          `json:"gcode_state"`
	Percent             *float64        `json:"mc_percent"`
	Remaining           *int            `json:"mc_remaining_time"`
	Layer               *int            `json:"layer_num"`
	Layers              *int            `json:"total_layer_num"`
	Nozzle              *float64        `json:"nozzle_temper"`
	NozzleSet           *float64        `json:"nozzle_target_temper"`
	Bed                 *float64        `json:"bed_temper"`
	BedSet              *float64        `json:"bed_target_temper"`
	Chamber             *float64        `json:"chamber_temper"`
	ChamberSet          *float64        `json:"ctt"`
	ChamberEditable     bool            `json:"support_chamber_temp_edit"`
	ChamberRange        []int           `json:"support_chamber_temp_edit_range"`
	AuxSupported        *bool           `json:"support_aux_fan"`
	ChamberFanSupported *bool           `json:"support_chamber_fan"`
	Device              *bambuDevice    `json:"device"`
	NozzleDiameter      json.RawMessage `json:"nozzle_diameter"`
	NozzleType          string          `json:"nozzle_type"`
	Flag3               *uint32         `json:"flag3"`
	FanGear             *uint32         `json:"fan_gear"`
	PartFan             json.RawMessage `json:"cooling_fan_speed"`
	AuxFan              json.RawMessage `json:"big_fan1_speed"`
	ChamberFan          json.RawMessage `json:"big_fan2_speed"`
	Subtask             string          `json:"subtask_name"`
	Error               int64           `json:"print_error"`
	AMS                 *struct {
		TrayNow json.RawMessage `json:"tray_now"`
		AMS     []struct {
			ID   string `json:"id"`
			Info string `json:"info"`
			Tray []struct {
				Remaining *int   `json:"remain"`
				ID        string `json:"id"`
				Type      string `json:"tray_type"`
				Color     string `json:"tray_color"`
				Name      string `json:"tray_sub_brands"`
				Idx       string `json:"tray_info_idx"`
				UUID      string `json:"tray_uuid"`
			} `json:"tray"`
		} `json:"ams"`
	} `json:"ams"`
	External      *bambuExternalTray  `json:"vt_tray"`
	ExternalSlots []bambuExternalTray `json:"vir_slot"`
}

func (b *bambuReport) trays() []AMSTray {
	if b.AMS == nil && b.External == nil && b.ExternalSlots == nil {
		return nil
	}
	color := func(c string) string {
		if len(c) >= 6 {
			return "#" + strings.ToUpper(c[:6])
		}
		return ""
	}
	trays := []AMSTray{}
	if b.AMS != nil {
		for _, unit := range b.AMS.AMS {
			a, err := strconv.Atoi(unit.ID)
			if err != nil || a < 0 || a > 255 {
				continue
			}
			info, infoErr := strconv.ParseUint(strings.TrimPrefix(unit.Info, "0x"), 16, 64)
			kind := "ams"
			var extruder *int
			if infoErr == nil {
				kind = map[uint64]string{1: "ams", 2: "ams-lite", 3: "ams-2-pro", 4: "ams-ht", 5: "ams-lite-mixed"}[info&15]
				if binding := (info >> 8) & 15; binding < 2 {
					extruder = ptr(1 - int(binding))
				}
			}
			for _, t := range unit.Tray {
				slot, err := strconv.Atoi(t.ID)
				if err != nil || slot < 0 || slot > 3 {
					continue
				}
				id := a*4 + slot
				if kind == "ams-ht" || a >= 128 {
					id = a
				}
				if kind == "ams-lite-mixed" {
					id = 24 + slot
				}
				trays = append(trays, AMSTray{ID: id, AMS: a, Slot: slot, Kind: kind, Extruder: extruder, Remaining: t.Remaining, Active: b.activeTray(a, slot, false), Type: t.Type, Color: color(t.Color), Name: t.Name, FilamentID: t.Idx, Spool: spoolID(t.UUID)})
			}
		}
	}
	externals := b.ExternalSlots
	if externals == nil && b.External != nil {
		externals = []bambuExternalTray{*b.External}
	}
	for _, e := range externals {
		id, err := strconv.Atoi(e.ID)
		if err != nil || !isExternalTray(id) {
			id = 255
		}
		var extruder *int
		if len(externals) > 1 {
			extruder = ptr(1 - (255 - id))
		}
		trays = append(trays, AMSTray{ID: id, AMS: id, Slot: 0, Kind: "external", Active: b.activeTray(id, 0, true), Extruder: extruder, Remaining: e.Remaining, Type: e.Type, Color: color(e.Color), FilamentID: e.Idx, Spool: spoolID(e.UUID)})
	}
	return trays
}

func (b *bambuReport) status() PrinterStatus {
	st := PrinterStatus{Online: true, Detail: b.State, File: b.Subtask, Layer: b.Layer, Layers: b.Layers, Trays: b.trays(), Nozzles: b.nozzles(), Fans: b.fans()}
	st.Controls = b.controls()
	if b.Device != nil && b.Device.Extruder != nil {
		for _, extruder := range b.Device.Extruder.Info {
			st.NozzleTemps = append(st.NozzleTemps, NozzleTemp{ID: extruder.ID, Actual: float64(extruder.Temp & 65535), Target: float64(extruder.Temp >> 16)})
		}
	}
	st.SpeedMode = b.SpeedMode
	if started := bambuNumber(b.StartTime); started != nil && *started > 0 {
		t := time.Unix(int64(*started), 0).UTC()
		st.StartedAt = &t
	}
	for _, light := range b.Lights {
		if light.Node == "chamber_light" {
			st.Light = ptr(light.Mode == "on")
		}
	}
	for _, h := range b.HMS {
		st.Messages = append(st.Messages, PrinterMessage{Code: fmt.Sprintf("%08X-%08X", h.Attr, h.Code), Message: h.Message})
	}
	if b.Error != 0 && b.Error != bambuCancelled {
		st.Messages = append(st.Messages, PrinterMessage{Code: fmt.Sprintf("%08X", b.Error)})
	}
	st.Messages = describeHMS(st.Messages)
	st.State = map[string]string{"IDLE": "idle", "PREPARE": "busy", "SLICING": "busy", "RUNNING": "printing", "PAUSE": "paused", "FINISH": "finished", "FAILED": "failed"}[b.State]
	if st.State == "" {
		st.State = "busy"
	}
	if b.Percent != nil && (st.State == "printing" || st.State == "paused" || st.State == "busy") {
		st.Progress = ptr(*b.Percent / 100)
	}
	if b.Remaining != nil && (st.State == "printing" || st.State == "paused") {
		st.Remaining = ptr(*b.Remaining * 60)
	}
	if b.Nozzle != nil {
		st.Nozzle = &Temp{*b.Nozzle, deref(b.NozzleSet)}
	}
	if b.Bed != nil {
		st.Bed = &Temp{*b.Bed, deref(b.BedSet)}
	}
	if b.Chamber != nil {
		st.Chamber = &Temp{*b.Chamber, deref(b.ChamberSet)}
	}
	switch {
	case b.State == "FAILED" && b.Error == bambuCancelled:
		st.State, st.Detail = "idle", "cancelled"
	case b.Error != 0:
		st.Detail = fmt.Sprintf("%s, error %08X", b.State, b.Error)
	}
	return st
}

const bambuCancelled = 0x0300400C

func mergeReport(into, delta map[string]any) {
	for k, v := range delta {
		if dm, ok := v.(map[string]any); ok {
			if im, ok := into[k].(map[string]any); ok {
				mergeReport(im, dm)
				continue
			}
		}
		into[k] = v
	}
}

func amsBambu(ctx context.Context, p *PrinterConfig, secret string) ([]AMSTray, error) {
	conn, r, err := mqttSession(ctx, p, secret)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	sequence := strconv.FormatInt(time.Now().UnixMilli()%1_000_000_000, 10)
	if err := mqttPublish(conn, p, map[string]any{"pushing": map[string]any{"sequence_id": sequence, "command": "pushall"}}); err != nil {
		return nil, err
	}
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	for {
		kind, body, err := mqttRead(r)
		if err != nil {
			return nil, fmt.Errorf("Couldn't read AMS status from the printer: %w", err)
		}
		var message struct {
			Print *bambuReport `json:"print"`
		}
		if json.Unmarshal(mqttPayload(kind, body), &message) != nil || message.Print == nil {
			continue
		}
		if trays := message.Print.trays(); trays != nil {
			return trays, nil
		}
	}
}

func monitorBambu(ctx context.Context, p *PrinterConfig, secret string, publish func(PrinterStatus)) error {
	conn, r, err := mqttSession(ctx, p, secret)
	if err != nil {
		return err
	}
	defer conn.Close()
	sequence := strconv.FormatInt(time.Now().UnixMilli()%1_000_000_000, 10)
	if err := mqttPublish(conn, p, map[string]any{"pushing": map[string]any{"sequence_id": sequence, "command": "pushall"}}); err != nil {
		return err
	}
	state := map[string]any{}
	heard := time.Now()
	for {
		conn.SetReadDeadline(time.Now().Add(20 * time.Second))
		kind, body, err := mqttRead(r)
		if err != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			if os.IsTimeout(err) && time.Since(heard) < 65*time.Second {
				conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
				if err := mqttWrite(conn, 0xC0, nil); err != nil {
					return netError(err, "the printer (MQTT port 8883)")
				}
				continue
			}
			return netError(err, "the printer (MQTT port 8883)")
		}
		heard = time.Now()
		var message struct {
			Print map[string]any `json:"print"`
		}
		if json.Unmarshal(mqttPayload(kind, body), &message) != nil || message.Print == nil {
			continue
		}
		mergeReport(state, message.Print)
		raw, _ := json.Marshal(state)
		var report bambuReport
		if json.Unmarshal(raw, &report) == nil && report.State != "" {
			publish(report.status())
		}
	}
}

func controlBambu(ctx context.Context, p *PrinterConfig, secret, action string) error {
	return bambuCommand(ctx, p, secret, map[string]any{"command": action, "param": ""})
}

func bambuCommand(ctx context.Context, p *PrinterConfig, secret string, command map[string]any) error {
	return bambuScopedCommand(ctx, p, secret, "print", command)
}

func bambuScopedCommand(ctx context.Context, p *PrinterConfig, secret, scope string, command map[string]any) error {
	conn, r, err := mqttSession(ctx, p, secret)
	if err != nil {
		return err
	}
	defer conn.Close()
	sequence := strconv.FormatInt(time.Now().UnixMilli()%1_000_000_000, 10)
	command["sequence_id"] = sequence
	action, _ := command["command"].(string)
	if err := mqttPublish(conn, p, map[string]any{scope: command}); err != nil {
		return err
	}
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	for {
		kind, body, err := mqttRead(r)
		if err != nil {
			mqttWrite(conn, 0xE0, nil)
			return nil
		}
		var message map[string]struct {
			Command  string `json:"command"`
			Sequence string `json:"sequence_id"`
			Result   string `json:"result"`
			Reason   string `json:"reason"`
			ErrCode  int    `json:"err_code"`
		}
		if json.Unmarshal(mqttPayload(kind, body), &message) != nil {
			continue
		}
		if m := message[scope]; m.Command == action && m.Sequence == sequence {
			mqttWrite(conn, 0xE0, nil)
			if refusedForAuth(m.Result, m.Reason, m.ErrCode) {
				return errors.New("The printer declined the command—make sure LAN Only mode and Developer Mode are turned on at the printer.")
			}
			if m.ErrCode != 0 || m.Result != "" && !strings.EqualFold(m.Result, "success") {
				return fmt.Errorf("The printer declined %s: %s (code %d)", action, cmpOr(m.Reason, m.Result), m.ErrCode)
			}
			return nil
		}
	}
}
