package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type flashforgeConsole struct {
	conn net.Conn
	r    *bufio.Reader
	stop func() bool
}

func flashforgeTarget(ctx context.Context, p *PrinterConfig, port string) (string, error) {
	target, err := p.hosts.resolve(ctx, &url.URL{Scheme: "tcp", Host: net.JoinHostPort(p.Host, port)})
	if err != nil {
		var refused *hostRefusal
		if errors.As(err, &refused) {
			audit(map[string]any{"event": "printer_host_refused", "printer": p.Printer, "reason": refused.reason})
		}
		return "", err
	}
	return target.String(), nil
}

func flashforgeDial(ctx context.Context, p *PrinterConfig) (*flashforgeConsole, error) {
	target, err := flashforgeTarget(ctx, p, "8899")
	if err != nil {
		return nil, err
	}
	conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", target)
	if err != nil {
		return nil, fmt.Errorf("Couldn't reach the Flashforge printer at %s: %w", p.Host, err)
	}
	limitSendBuffer(conn)
	c := &flashforgeConsole{conn: conn, r: bufio.NewReader(conn), stop: context.AfterFunc(ctx, func() { conn.Close() })}
	if _, err := c.command("M601 S1", 10*time.Second); err != nil {
		c.conn.Close()
		return nil, err
	}
	return c, nil
}

func (c *flashforgeConsole) command(cmd string, timeout time.Duration) (string, error) {
	c.conn.SetDeadline(time.Now().Add(timeout))
	if _, err := io.WriteString(c.conn, "~"+cmd+"\r\n"); err != nil {
		return "", err
	}
	var lines []string
	for {
		line, err := c.r.ReadString('\n')
		if err != nil {
			return "", fmt.Errorf("The Flashforge printer didn't finish answering %s: %w", strings.Fields(cmd)[0], err)
		}
		line = strings.TrimSpace(line)
		if strings.EqualFold(line, "ok") {
			return strings.Join(lines, "\n"), nil
		}
		if line != "" {
			lines = append(lines, line)
		}
	}
}

func (c *flashforgeConsole) close() {
	c.command("M602", 3*time.Second)
	c.stop()
	c.conn.Close()
}

var flashforgeUnsafe = regexp.MustCompile(`[^A-Za-z0-9._-]`)

func flashforgeFilename(name string) string {
	name = flashforgeUnsafe.ReplaceAllString(name, "_")
	if name == "" {
		return "print.gcode"
	}
	return name
}

func flashforgeAPI(p *PrinterConfig, secret string) bool { return p.Serial != "" && secret != "" }

func sendFlashforge(ctx context.Context, u *upload) (string, error) {
	name := flashforgeFilename(u.filename)
	if flashforgeAPI(u.printer, u.secret) {
		return sendFlashforgeAPI(ctx, u, name)
	}
	c, err := flashforgeDial(ctx, u.printer)
	if err != nil {
		return "", err
	}
	defer c.close()
	if _, err := c.command(fmt.Sprintf("M28 %d 0:/user/%s", u.size, name), 30*time.Second); err != nil {
		return "", err
	}
	c.conn.SetDeadline(time.Time{})
	if _, err := io.Copy(c.conn, progressReader{io.LimitReader(u.file, u.size), u.progress}); err != nil {
		return "", err
	}
	if _, err := c.command("M29", 2*time.Minute); err != nil {
		return "", fmt.Errorf("The Flashforge printer didn't confirm it saved %s: %w", name, err)
	}
	if !u.start {
		return "Uploaded " + name + " to " + u.printer.Name + ".", nil
	}
	u.starting()
	if _, err := c.command("M23 0:/user/"+name, 30*time.Second); err != nil {
		return "", fmt.Errorf("Uploaded %s, but couldn't start the print automatically: %w", name, err)
	}
	return "Uploaded " + name + " to " + u.printer.Name + " and started printing.", nil
}

func flashforgeClient(ctx context.Context, p *PrinterConfig) (*http.Client, string, error) {
	target, err := flashforgeTarget(ctx, p, "8898")
	if err != nil {
		return nil, "", err
	}
	dial := func(ctx context.Context, network, _ string) (net.Conn, error) {
		conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", target)
		if err == nil {
			limitSendBuffer(conn)
		}
		return conn, err
	}
	client := &http.Client{Transport: &http.Transport{Proxy: nil, DialContext: dial, ResponseHeaderTimeout: 2 * time.Minute, DisableKeepAlives: true}}
	return client, "http://" + net.JoinHostPort(p.Host, "8898"), nil
}

func flashforgeReply(resp *http.Response) error {
	var reply struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
	}
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return fmt.Errorf("The printer returned HTTP %d", resp.StatusCode)
	}
	if json.Unmarshal(raw, &reply) != nil {
		return errors.New("The printer returned an unexpected response format.")
	}
	if reply.Code != 0 {
		if strings.Contains(strings.ToLower(reply.Message), "access code") {
			return &connError{"access_code", "The printer rejected the check code—double-check the serial number and check code in Connection settings."}
		}
		return fmt.Errorf("The printer declined the request: %s", cmpOr(reply.Message, fmt.Sprintf("code %d", reply.Code)))
	}
	return nil
}

func sendFlashforgeAPI(ctx context.Context, u *upload, name string) (string, error) {
	client, base, err := flashforgeClient(ctx, u.printer)
	if err != nil {
		return "", err
	}
	detail, _ := json.Marshal(map[string]string{"serialNumber": u.printer.Serial, "checkCode": u.secret})
	req, err := http.NewRequestWithContext(ctx, "POST", base+"/detail", strings.NewReader(string(detail)))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	err = flashforgeReply(resp)
	resp.Body.Close()
	if err != nil {
		return "", err
	}
	req, err = multipartFile(ctx, base+"/uploadGcode", "gcodeFile", name, u.size, progressReader{io.LimitReader(u.file, u.size), u.progress}, nil)
	if err != nil {
		return "", err
	}
	for k, v := range map[string]string{"serialNumber": u.printer.Serial, "checkCode": u.secret, "fileSize": strconv.FormatInt(u.size, 10), "printNow": strconv.FormatBool(u.start),
		"levelingBeforePrint": "false", "flowCalibration": "false", "firstLayerInspection": "false", "timeLapseVideo": "false", "useMatlStation": "false", "gcodeToolCnt": "0", "materialMappings": "W10="} {
		req.Header.Set(k, v)
	}
	if u.start {
		u.starting()
	}
	if resp, err = client.Do(req); err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if err := flashforgeReply(resp); err != nil {
		return "", err
	}
	if u.start {
		return "Uploaded " + name + " to " + u.printer.Name + " and started printing.", nil
	}
	return "Uploaded " + name + " to " + u.printer.Name + ".", nil
}

var (
	flashforgeTemps    = regexp.MustCompile(`(T0|B):\s*(-?[\d.]+)\s*/\s*(-?[\d.]+)`)
	flashforgeProgress = regexp.MustCompile(`SD printing byte\s+(\d+)\s*/\s*(\d+)`)
	flashforgeLayer    = regexp.MustCompile(`Layer:\s*(\d+)\s*/\s*(\d+)`)
)

func flashforgeField(text, key string) string {
	for _, line := range strings.Split(text, "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(line), key+":"); ok {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func statusFlashforge(c *flashforgeConsole) (PrinterStatus, error) {
	st := PrinterStatus{Online: true}
	info, err := c.command("M119", 10*time.Second)
	if err != nil {
		return st, err
	}
	machine := flashforgeField(info, "MachineStatus")
	st.Detail, st.File = machine, flashforgeField(info, "CurrentFile")
	st.State = map[string]string{"READY": "idle", "BUILDING_FROM_SD": "printing", "BUILDING": "printing", "PAUSED": "paused", "BUILDING_COMPLETED": "finished", "BUSY": "busy"}[machine]
	if st.State == "" {
		st.State = "busy"
	}
	if st.State == "printing" && flashforgeField(info, "MoveMode") == "PAUSED" {
		st.State, st.Detail = "paused", "PAUSED"
	}
	if temps, err := c.command("M105", 10*time.Second); err == nil {
		for _, m := range flashforgeTemps.FindAllStringSubmatch(temps, -1) {
			actual, _ := strconv.ParseFloat(m[2], 64)
			target, _ := strconv.ParseFloat(m[3], 64)
			if m[1] == "B" {
				st.Bed = &Temp{actual, target}
			} else {
				st.Nozzle = &Temp{actual, target}
			}
		}
	}
	if st.State == "printing" || st.State == "paused" {
		if job, err := c.command("M27", 10*time.Second); err == nil {
			if m := flashforgeProgress.FindStringSubmatch(job); m != nil {
				done, _ := strconv.ParseFloat(m[1], 64)
				total, _ := strconv.ParseFloat(m[2], 64)
				if total > 0 {
					st.Progress = ptr(done / total)
				}
			}
			if m := flashforgeLayer.FindStringSubmatch(job); m != nil {
				layer, _ := strconv.Atoi(m[1])
				layers, _ := strconv.Atoi(m[2])
				if layers > 0 && (layer < layers || st.Progress != nil && *st.Progress > 0) {
					st.Layer, st.Layers = &layer, &layers
				}
			}
		}
	}
	return st, nil
}

func pollFlashforge(ctx context.Context, p *PrinterConfig, publish func(PrinterStatus), poke chan struct{}) error {
	for {
		rctx, cancel := context.WithTimeout(ctx, 15*time.Second)
		c, err := flashforgeDial(rctx, p)
		var st PrinterStatus
		if err == nil {
			st, err = statusFlashforge(c)
			c.close()
		}
		cancel()
		if err != nil {
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

func testFlashforge(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	c, err := flashforgeDial(ctx, p)
	if err != nil {
		return hostFailure(err)
	}
	info, err := c.command("M115", 10*time.Second)
	c.close()
	if err != nil {
		return hostFailure(err)
	}
	model := flashforgeField(info, "Machine Type")
	if flashforgeAPI(p, secret) {
		client, base, err := flashforgeClient(ctx, p)
		if err != nil {
			return hostFailure(err)
		}
		body, _ := json.Marshal(map[string]string{"serialNumber": p.Serial, "checkCode": secret})
		req, err := http.NewRequestWithContext(ctx, "POST", base+"/detail", strings.NewReader(string(body)))
		if err != nil {
			return hostFailure(err)
		}
		req.Header.Set("Content-Type", "application/json")
		resp, err := client.Do(req)
		if err != nil {
			return hostFailure(err)
		}
		err = flashforgeReply(resp)
		resp.Body.Close()
		if err != nil {
			return PrinterTest{Stage: "access_code", Message: err.Error()}
		}
	} else if serial := flashforgeField(info, "SN"); p.Serial != "" && serial != "" && !strings.EqualFold(serial, p.Serial) {
		return PrinterTest{Stage: "serial", Message: "That printer reports serial number " + serial + ", not " + p.Serial + "."}
	}
	return PrinterTest{OK: true, Stage: "ok", Message: "Connected to " + cmpOr(model, "the Flashforge printer"), Model: model}
}

func controlFlashforge(ctx context.Context, p *PrinterConfig, action string) error {
	c, err := flashforgeDial(ctx, p)
	if err != nil {
		return err
	}
	defer c.close()
	_, err = c.command(map[string]string{"pause": "M25", "resume": "M24", "stop": "M26"}[action], 15*time.Second)
	return err
}
