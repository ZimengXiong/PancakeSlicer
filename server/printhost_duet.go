package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

func duetTime() string { return time.Now().Format("2006-01-02T15:04:05") }

func duetConnect(ctx context.Context, h *hostSession, base *url.URL) (string, error) {
	var reply struct {
		Err        int    `json:"err"`
		SessionKey *int64 `json:"sessionKey"`
		Board      string `json:"boardType"`
	}
	q := url.Values{"password": {cmpOr(h.secret, "reprap")}, "time": {duetTime()}}
	code, err := h.call(ctx, "GET", base.JoinPath("/rr_connect").String()+"?"+q.Encode(), nil, &reply)
	if code == 404 {
		return "", errors.New("The Duet board didn't respond to rr_connect (SBC mode isn't supported yet—use standalone RepRapFirmware mode).")
	}
	if err != nil {
		return "", err
	}
	switch reply.Err {
	case 0:
	case 1:
		return "", &connError{"access_code", "The Duet board rejected the password—double-check it in your printer's Connection settings."}
	case 2:
		return "", errors.New("The Duet board has no open connection slots—close an open Duet web tab and try again.")
	default:
		return "", fmt.Errorf("The Duet board declined the connection (error %d).", reply.Err)
	}
	if reply.SessionKey != nil {
		h.session = fmt.Sprint(*reply.SessionKey)
	}
	return reply.Board, nil
}

func duetDisconnect(h *hostSession, base *url.URL) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	h.call(ctx, "GET", base.JoinPath("/rr_disconnect").String(), nil, nil)
}

func duetGcode(ctx context.Context, h *hostSession, base *url.URL, gcode string) error {
	var reply struct {
		Err *int `json:"err"`
	}
	if _, err := h.call(ctx, "GET", base.JoinPath("/rr_gcode").String()+"?"+url.Values{"gcode": {gcode}}.Encode(), nil, &reply); err != nil {
		return err
	}
	if reply.Err != nil && *reply.Err != 0 {
		return fmt.Errorf("The Duet board declined %s.", gcode)
	}
	return nil
}

func sendDuet(ctx context.Context, u *upload) (string, error) {
	h, base, err := httpSession(ctx, u.printer, u.secret)
	if err != nil {
		return "", err
	}
	if strings.ContainsAny(u.filename, "\"\r\n") {
		return "", errors.New("That file name contains characters not supported by Duet.")
	}
	if _, err := duetConnect(ctx, h, base); err != nil {
		return "", err
	}
	defer duetDisconnect(h, base)
	q := url.Values{"name": {"0:/gcodes/" + u.filename}, "time": {duetTime()}}
	req, err := http.NewRequestWithContext(ctx, "POST", base.JoinPath("/rr_upload").String()+"?"+q.Encode(), progressReader{io.LimitReader(u.file, u.size), u.progress})
	if err != nil {
		return "", err
	}
	req.ContentLength = u.size
	req.Header.Set("Content-Type", "application/octet-stream")
	resp, err := h.do(req)
	if err != nil {
		return "", err
	}
	answer, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<10))
	resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode > 299 || !strings.Contains(strings.ReplaceAll(string(answer), " ", ""), `"err":0`) {
		return "", fmt.Errorf("The Duet board couldn't save the uploaded file (HTTP %d).", resp.StatusCode)
	}
	if !u.start {
		return "Uploaded " + u.filename + " to " + u.printer.Name + ".", nil
	}
	u.starting()
	if err := duetGcode(ctx, h, base, `M32 "0:/gcodes/`+u.filename+`"`); err != nil {
		return "", fmt.Errorf("Uploaded %s, but couldn't start the print automatically: %w", u.filename, err)
	}
	return "Uploaded " + u.filename + " to " + u.printer.Name + " and started printing.", nil
}

func controlDuet(ctx context.Context, p *PrinterConfig, secret, action string) error {
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return err
	}
	if _, err := duetConnect(ctx, h, base); err != nil {
		return err
	}
	defer duetDisconnect(h, base)
	switch action {
	case "pause":
		return duetGcode(ctx, h, base, "M25")
	case "resume":
		return duetGcode(ctx, h, base, "M24")
	}
	if err := duetGcode(ctx, h, base, "M25"); err != nil {
		return err
	}
	return duetGcode(ctx, h, base, "M0")
}

func statusDuet(ctx context.Context, h *hostSession, base *url.URL) (PrinterStatus, error) {
	st := PrinterStatus{Online: true}
	model := func(key string, out any) error {
		var reply struct {
			Result any `json:"result"`
		}
		reply.Result = out
		_, err := h.call(ctx, "GET", base.JoinPath("/rr_model").String()+"?"+url.Values{"key": {key}, "flags": {"d99vn"}}.Encode(), nil, &reply)
		return err
	}
	var state struct {
		Status string `json:"status"`
	}
	if err := model("state", &state); err != nil {
		return st, err
	}
	st.Detail = state.Status
	st.State = map[string]string{"idle": "idle", "processing": "printing", "simulating": "printing", "paused": "paused", "pausing": "paused", "resuming": "printing",
		"busy": "busy", "changingTool": "busy", "halted": "failed", "off": "offline", "starting": "busy", "updating": "busy"}[state.Status]
	var job struct {
		File *struct {
			Name   string `json:"fileName"`
			Size   int64  `json:"size"`
			Layers *int   `json:"numLayers"`
		} `json:"file"`
		Position  int64 `json:"filePosition"`
		Layer     *int  `json:"layer"`
		TimesLeft struct {
			Slicer *float64 `json:"slicer"`
			File   *float64 `json:"file"`
		} `json:"timesLeft"`
	}
	if model("job", &job) == nil && job.File != nil && job.File.Name != "" {
		st.File = strings.TrimPrefix(job.File.Name, "0:/gcodes/")
		if job.File.Size > 0 {
			st.Progress = ptr(float64(job.Position) / float64(job.File.Size))
		}
		st.Layer, st.Layers = job.Layer, job.File.Layers
		if left := job.TimesLeft.Slicer; left != nil {
			st.Remaining = ptr(int(*left))
		} else if left := job.TimesLeft.File; left != nil {
			st.Remaining = ptr(int(*left))
		}
	}
	var heat struct {
		Bed     []int `json:"bedHeaters"`
		Heaters []*struct {
			Current float64 `json:"current"`
			Active  float64 `json:"active"`
			State   string  `json:"state"`
		} `json:"heaters"`
	}
	if model("heat", &heat) == nil {
		bed := -1
		if len(heat.Bed) > 0 {
			bed = heat.Bed[0]
		}
		for i, hh := range heat.Heaters {
			if hh == nil {
				continue
			}
			t := &Temp{hh.Current, map[bool]float64{true: hh.Active}[hh.State == "active"]}
			if i == bed {
				st.Bed = t
			} else if st.Nozzle == nil {
				st.Nozzle = t
			}
		}
	}
	if st.State == "" {
		st.State = "busy"
	}
	return st, nil
}
