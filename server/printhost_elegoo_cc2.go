package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"math/rand/v2"
	"net"
	"net/url"
	"slices"
	"strings"
	"time"
)

type cc2Conn struct {
	conn       net.Conn
	r          *bufio.Reader
	sn, client string
	next       int
	stop       func() bool
}

func dialCC2(ctx context.Context, p *PrinterConfig, secret string) (*cc2Conn, error) {
	h, base, err := httpSession(ctx, p, "")
	if err != nil {
		return nil, err
	}
	sn, err := elegooCC2Info(ctx, h, base, secret)
	if err != nil {
		return nil, err
	}
	target, err := p.hosts.resolve(ctx, &url.URL{Scheme: "http", Host: net.JoinHostPort(base.Hostname(), "1883")})
	if err != nil {
		return nil, err
	}
	conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", target.String())
	if err != nil {
		return nil, netError(err, "the printer (MQTT port 1883)")
	}
	client := fmt.Sprintf("1_PC_%d", 1000+rand.IntN(9000))
	c := &cc2Conn{conn: conn, r: bufio.NewReader(conn), sn: sn, client: client, stop: context.AfterFunc(ctx, func() { conn.Close() })}
	if err := c.login(elegooToken(secret)); err != nil {
		c.close()
		return nil, err
	}
	return c, nil
}

func (c *cc2Conn) login(password string) error {
	c.conn.SetDeadline(time.Now().Add(15 * time.Second))
	defer c.conn.SetDeadline(time.Time{})
	connect := append([]byte{0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 60}, mqttString(c.client)...)
	connect = append(append(connect, mqttString("elegoo")...), mqttString(password)...)
	if err := mqttWrite(c.conn, 0x10, connect); err != nil {
		return netError(err, "the printer (MQTT port 1883)")
	}
	kind, body, err := mqttRead(c.r)
	switch {
	case err != nil:
		return netError(err, "the printer (MQTT port 1883)")
	case kind != 2 || len(body) != 2:
		return errors.New("The printer sent an unexpected MQTT reply.")
	case body[1] == 4 || body[1] == 5:
		return &connError{"access_code", "The printer rejected the access code—check it in Connection settings."}
	case body[1] != 0:
		return fmt.Errorf("The printer declined the MQTT connection (code %d).", body[1])
	}
	registered := "elegoo/" + c.sn + "/" + c.client + "_req/register_response"
	subscribe := []byte{0, 1}
	for _, topic := range []string{c.topic("api_response"), registered} {
		subscribe = append(append(subscribe, mqttString(topic)...), 0)
	}
	if err := mqttWrite(c.conn, 0x82, subscribe); err != nil {
		return netError(err, "the printer (MQTT port 1883)")
	}
	if err := c.publish("elegoo/"+c.sn+"/api_register", map[string]string{"client_id": c.client, "request_id": c.client + "_req"}); err != nil {
		return err
	}
	for {
		topic, payload, err := c.read()
		if err != nil {
			return err
		}
		var reply struct {
			ClientID string `json:"client_id"`
			Error    string `json:"error"`
		}
		if topic != registered || json.Unmarshal(payload, &reply) != nil || reply.ClientID != c.client {
			continue
		}
		switch {
		case reply.Error == "ok":
			return nil
		case strings.Contains(reply.Error, "too many clients"):
			return &connError{"busy", "The printer has too many apps connected; close one and try again."}
		}
		return fmt.Errorf("The printer refused the connection: %s", reply.Error)
	}
}

func (c *cc2Conn) topic(name string) string { return "elegoo/" + c.sn + "/" + c.client + "/" + name }

func (c *cc2Conn) publish(topic string, message any) error {
	body, _ := json.Marshal(message)
	c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	if err := mqttWrite(c.conn, 0x30, append(mqttString(topic), body...)); err != nil {
		return netError(err, "the printer (MQTT port 1883)")
	}
	return nil
}

func (c *cc2Conn) read() (string, []byte, error) {
	for {
		kind, body, err := mqttRead(c.r)
		if err != nil {
			return "", nil, netError(err, "the printer (MQTT port 1883)")
		}
		if payload := mqttPayload(kind, body); payload != nil {
			return string(body[2 : len(body)-len(payload)]), payload, nil
		}
	}
}

func (c *cc2Conn) call(method int, params any, timeout time.Duration) (json.RawMessage, error) {
	c.next++
	id := c.next
	if params == nil {
		params = map[string]any{}
	}
	if err := c.publish(c.topic("api_request"), map[string]any{"id": id, "method": method, "params": params}); err != nil {
		return nil, err
	}
	c.conn.SetReadDeadline(time.Now().Add(timeout))
	defer c.conn.SetReadDeadline(time.Time{})
	for {
		topic, payload, err := c.read()
		if err != nil {
			return nil, err
		}
		var reply struct {
			ID     int             `json:"id"`
			Result json.RawMessage `json:"result"`
		}
		if topic != c.topic("api_response") || json.Unmarshal(payload, &reply) != nil || reply.ID != id {
			continue
		}
		var result struct {
			ErrorCode *int `json:"error_code"`
		}
		if json.Unmarshal(reply.Result, &result) != nil || result.ErrorCode == nil {
			return nil, errors.New("The printer returned an unexpected response format.")
		}
		switch code := *result.ErrorCode; code {
		case 0:
			return reply.Result, nil
		case 1000:
			return nil, &connError{"access_code", "The printer denied the request—check the access code in Connection settings."}
		case 1009:
			return nil, &connError{"busy", "The printer is busy; wait for its current job to finish."}
		case 1021:
			return nil, errors.New("The printer couldn't find the uploaded file.")
		case 1026:
			return nil, errors.New("The printer has no bed leveling data yet; turn on bed leveling for this print.")
		default:
			return nil, fmt.Errorf("The printer declined the request (code %d).", code)
		}
	}
}

func (c *cc2Conn) close() {
	c.conn.SetWriteDeadline(time.Now().Add(time.Second))
	mqttWrite(c.conn, 0xE0, nil)
	c.stop()
	c.conn.Close()
}

func startCC2(ctx context.Context, u *upload) error {
	c, err := dialCC2(ctx, u.printer, u.secret)
	if err != nil {
		return err
	}
	defer c.close()
	layout := map[int]string{0: "A", 1: "B"}[elegooPlate(u.file, u.size)]
	_, err = c.call(1020, map[string]any{"storage_media": "local", "filename": u.filename, "config": map[string]any{
		"delay_video": u.options.timelapse, "printer_check": u.options.bedLeveling, "print_layout": layout, "bedlevel_force": false, "slot_map": []any{}}}, 30*time.Second)
	return err
}

type cc2Status struct {
	MachineStatus struct {
		Status    int `json:"status"`
		SubStatus int `json:"sub_status"`
		Progress  int `json:"progress"`
	} `json:"machine_status"`
	PrintStatus struct {
		Filename     string `json:"filename"`
		CurrentLayer int    `json:"current_layer"`
		TotalLayer   int    `json:"total_layer"`
		Remaining    int    `json:"remaining_time_sec"`
	} `json:"print_status"`
	Extruder  *cc2Temp `json:"extruder"`
	HeaterBed *cc2Temp `json:"heater_bed"`
	Chamber   *cc2Temp `json:"ztemperature_sensor"`
	LED       *struct {
		Status int `json:"status"`
	} `json:"led"`
}

type cc2Temp struct {
	Temperature float64 `json:"temperature"`
	Target      float64 `json:"target"`
}

func (t *cc2Temp) temp() *Temp {
	if t == nil {
		return nil
	}
	return &Temp{math.Round(t.Temperature*10) / 10, math.Round(t.Target*10) / 10}
}

func cc2PrinterStatus(raw json.RawMessage) (PrinterStatus, error) {
	var s cc2Status
	if err := json.Unmarshal(raw, &s); err != nil {
		return PrinterStatus{}, errors.New("The printer returned an unexpected response format.")
	}
	st := PrinterStatus{Online: true, State: "busy", Nozzle: s.Extruder.temp(), Bed: s.HeaterBed.temp()}
	if s.Chamber != nil {
		st.Chamber = &Temp{Actual: math.Round(s.Chamber.Temperature*10) / 10}
	}
	if s.LED != nil {
		st.Light = ptr(s.LED.Status != 0)
	}
	switch {
	case s.MachineStatus.Status == 1:
		st.State = "idle"
	case s.MachineStatus.Status == 2 && slices.Contains([]int{2501, 2502, 2505}, s.MachineStatus.SubStatus):
		st.State = "paused"
	case s.MachineStatus.Status == 2:
		st.State = "printing"
	}
	if pi := s.PrintStatus; st.State == "printing" || st.State == "paused" {
		st.File = pi.Filename
		st.Progress = ptr(min(1, max(0, float64(s.MachineStatus.Progress)/100)))
		if pi.TotalLayer > 0 {
			st.Layer, st.Layers = ptr(pi.CurrentLayer), ptr(pi.TotalLayer)
		}
		if pi.Remaining > 0 {
			st.Remaining = ptr(pi.Remaining)
		}
	}
	return st, nil
}

func pollCC2(ctx context.Context, p *PrinterConfig, secret string) (func() (PrinterStatus, error), func(), error) {
	c, err := dialCC2(ctx, p, secret)
	if err != nil {
		return nil, nil, err
	}
	return func() (PrinterStatus, error) {
		raw, err := c.call(1002, nil, 10*time.Second)
		if err != nil {
			return PrinterStatus{}, err
		}
		return cc2PrinterStatus(raw)
	}, c.close, nil
}

func controlCC2(ctx context.Context, p *PrinterConfig, secret, action string) error {
	method := map[string]int{"pause": 1021, "stop": 1022}[action]
	if method == 0 {
		return errors.New("Centauri 2 printers only take pause and stop over the network—resume from the printer's screen.")
	}
	c, err := dialCC2(ctx, p, secret)
	if err != nil {
		return err
	}
	defer c.close()
	_, err = c.call(method, nil, 30*time.Second)
	return err
}
