package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type PrinterMessage struct {
	Code    string `json:"code"`
	Message string `json:"message,omitempty"`
}

type PrinterStatus struct {
	NozzleTemps []NozzleTemp       `json:"nozzle_temps,omitempty"`
	Job         *deviceJob         `json:"job,omitempty"`
	StartedAt   *time.Time         `json:"started_at,omitempty"`
	SpeedMode   *int               `json:"speed_mode,omitempty"`
	Light       *bool              `json:"light,omitempty"`
	Messages    []PrinterMessage   `json:"messages,omitempty"`
	Printer     string             `json:"printer"`
	Online      bool               `json:"online"`
	State       string             `json:"state"`
	Detail      string             `json:"detail,omitempty"`
	File        string             `json:"file,omitempty"`
	Progress    *float64           `json:"progress,omitempty"`
	Remaining   *int               `json:"remaining_s,omitempty"`
	Layer       *int               `json:"layer,omitempty"`
	Layers      *int               `json:"layers,omitempty"`
	Nozzle      *Temp              `json:"nozzle,omitempty"`
	Bed         *Temp              `json:"bed,omitempty"`
	Chamber     *Temp              `json:"chamber,omitempty"`
	Nozzles     []NozzleStatus     `json:"nozzles,omitempty"`
	Fans        map[string]float64 `json:"fans,omitempty"`
	Controls    *BambuControls     `json:"controls,omitempty"`
	Trays       []AMSTray          `json:"trays,omitempty"`
	At          time.Time          `json:"at"`
}

type Temp struct {
	Actual float64 `json:"actual"`
	Target float64 `json:"target"`
}

func ptr[T any](v T) *T { return &v }

func httpSession(ctx context.Context, p *PrinterConfig, secret string) (*hostSession, *url.URL, error) {
	base, _ := url.Parse(p.URL)
	client, err := hostClient(ctx, p, base)
	if err != nil {
		return nil, nil, err
	}
	return &hostSession{client: client, printer: p, secret: secret}, base, nil
}

func (h *hostSession) call(ctx context.Context, method, target string, body []byte, out any) (int, error) {
	for attempt := 0; ; attempt++ {
		var reader io.Reader
		if body != nil {
			reader = bytes.NewReader(body)
		}
		req, err := http.NewRequestWithContext(ctx, method, target, reader)
		if err != nil {
			return 0, err
		}
		if body != nil {
			req.Header.Set("Content-Type", "application/json")
		}
		resp, err := h.do(req)
		if err != nil {
			return 0, err
		}
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
		resp.Body.Close()
		if resp.StatusCode == 401 && h.printer.Auth == "digest" && attempt == 0 {
			if h.digest = parseDigest(resp.Header.Get("WWW-Authenticate")); h.digest != nil {
				continue
			}
		}
		switch {
		case resp.StatusCode == 401 || resp.StatusCode == 403:
			return resp.StatusCode, credentialsRejected(h.printer)
		case resp.StatusCode < 200 || resp.StatusCode > 299:
			return resp.StatusCode, fmt.Errorf("The printer returned HTTP %d%s", resp.StatusCode, jsonMessage(raw))
		}
		if out != nil && len(raw) > 0 {
			if err := json.Unmarshal(raw, out); err != nil {
				return resp.StatusCode, fmt.Errorf("The printer returned an unexpected response format.")
			}
		}
		return resp.StatusCode, nil
	}
}

func jsonMessage(raw []byte) string {
	var j struct {
		Error   json.RawMessage `json:"error"`
		Message string          `json:"message"`
	}
	if json.Unmarshal(raw, &j) != nil {
		return ""
	}
	text := j.Message
	var nested struct {
		Message string `json:"message"`
	}
	if text == "" && json.Unmarshal(j.Error, &nested) == nil {
		text = nested.Message
	}
	if text == "" {
		json.Unmarshal(j.Error, &text)
	}
	if len(text) > 200 {
		text = text[:200] + "..."
	}
	if text == "" {
		return ""
	}
	return ": " + text
}

func testHost(ctx context.Context, p *PrinterConfig, secret string) PrinterTest {
	if p.Kind == "elegoolink" {
		return testElegoo(ctx, p, secret)
	}
	if p.Kind == "duet" {
		h, base, err := httpSession(ctx, p, secret)
		if err == nil {
			var board string
			if board, err = duetConnect(ctx, h, base); err == nil {
				duetDisconnect(h, base)
				return PrinterTest{OK: true, Stage: "ok", Message: "Connected to " + cmpOr(board, "the Duet board"), Model: board}
			}
		}
		return hostFailure(err)
	}
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return hostFailure(err)
	}
	probe := map[string]string{"prusalink": "/api/version", "octoprint": "/api/version", "moonraker": "/server/info"}[p.Kind]
	if err := h.preflight(ctx, base.JoinPath(probe).String()); err != nil {
		return hostFailure(err)
	}
	message := "Connected to " + connectionKinds[p.Kind]
	if p.Kind == "moonraker" {
		var info struct {
			Result struct {
				Klippy string `json:"klippy_state"`
			} `json:"result"`
		}
		if _, err := h.call(ctx, "GET", base.JoinPath("/server/info").String(), nil, &info); err == nil && info.Result.Klippy != "" && info.Result.Klippy != "ready" {
			message += "; Klipper is " + info.Result.Klippy
		}
	}
	return PrinterTest{OK: true, Stage: "ok", Message: message}
}

func cmpOr(a, b string) string {
	if a != "" {
		return a
	}
	return b
}

func hostFailure(err error) PrinterTest {
	stage := "host"
	if errors.Is(err, errHostRefused) {
		stage = "address"
	}
	return PrinterTest{Stage: stage, Message: err.Error()}
}

func statusHTTP(ctx context.Context, h *hostSession, base *url.URL) (PrinterStatus, error) {
	st := PrinterStatus{Online: true}
	switch h.printer.Kind {
	case "moonraker":
		var q struct {
			Result struct {
				Status struct {
					PrintStats struct {
						State    string  `json:"state"`
						Filename string  `json:"filename"`
						Duration float64 `json:"print_duration"`
						Message  string  `json:"message"`
						Info     struct {
							Current *int `json:"current_layer"`
							Total   *int `json:"total_layer"`
						} `json:"info"`
					} `json:"print_stats"`
					SD struct {
						Progress float64 `json:"progress"`
					} `json:"virtual_sdcard"`
					Extruder *struct {
						Temperature float64 `json:"temperature"`
						Target      float64 `json:"target"`
					} `json:"extruder"`
					Bed *struct {
						Temperature float64 `json:"temperature"`
						Target      float64 `json:"target"`
					} `json:"heater_bed"`
				} `json:"status"`
			} `json:"result"`
		}
		code, err := h.call(ctx, "GET", base.JoinPath("/printer/objects/query").String()+"?print_stats&virtual_sdcard&extruder&heater_bed", nil, &q)
		if code == 503 || code == 400 {
			st.State, st.Detail = "error", strings.TrimPrefix(err.Error(), "the printer answered ")
			return st, nil
		}
		if err != nil {
			return st, err
		}
		s := q.Result.Status
		st.Detail = s.PrintStats.State
		st.State = map[string]string{"standby": "idle", "printing": "printing", "paused": "paused", "complete": "finished", "cancelled": "idle", "error": "failed"}[s.PrintStats.State]
		if s.PrintStats.State == "error" && s.PrintStats.Message != "" {
			st.Detail = s.PrintStats.Message
		}
		st.File = s.PrintStats.Filename
		if st.State == "printing" || st.State == "paused" {
			st.Progress = ptr(s.SD.Progress)
			if s.SD.Progress > 0.001 {
				st.Remaining = ptr(int(s.PrintStats.Duration/s.SD.Progress - s.PrintStats.Duration))
			}
		}
		st.Layer, st.Layers = s.PrintStats.Info.Current, s.PrintStats.Info.Total
		if s.PrintStats.Duration > 0 && (st.State == "printing" || st.State == "paused") {
			started := time.Now().Add(-time.Duration(s.PrintStats.Duration * float64(time.Second))).UTC()
			st.StartedAt = &started
		}
		if s.Extruder != nil {
			st.Nozzle = &Temp{s.Extruder.Temperature, s.Extruder.Target}
		}
		if s.Bed != nil {
			st.Bed = &Temp{s.Bed.Temperature, s.Bed.Target}
		}
	case "octoprint":
		var job struct {
			Job struct {
				File struct {
					Name string `json:"name"`
				} `json:"file"`
			} `json:"job"`
			Progress struct {
				Completion *float64 `json:"completion"`
				Left       *int     `json:"printTimeLeft"`
			} `json:"progress"`
			State string `json:"state"`
		}
		if _, err := h.call(ctx, "GET", base.JoinPath("/api/job").String(), nil, &job); err != nil {
			return st, err
		}
		st.Detail, st.File = job.State, job.Job.File.Name
		switch word := strings.ToLower(job.State); {
		case strings.HasPrefix(word, "printing"), word == "starting", strings.HasPrefix(word, "sending"):
			st.State = "printing"
		case strings.HasPrefix(word, "paus"), word == "resuming":
			st.State = "paused"
		case word == "operational":
			st.State = "idle"
		case word == "cancelling", word == "finishing":
			st.State = "busy"
		case strings.Contains(word, "error"):
			st.State = "failed"
		default:
			st.State = "offline"
		}
		if job.Progress.Completion != nil && (st.State == "printing" || st.State == "paused") {
			st.Progress = ptr(*job.Progress.Completion / 100)
			st.Remaining = job.Progress.Left
		}
		var printer struct {
			Temperature map[string]struct {
				Actual *float64 `json:"actual"`
				Target *float64 `json:"target"`
			} `json:"temperature"`
		}
		if _, err := h.call(ctx, "GET", base.JoinPath("/api/printer").String(), nil, &printer); err == nil {
			for key, into := range map[string]**Temp{"tool0": &st.Nozzle, "bed": &st.Bed} {
				if t, ok := printer.Temperature[key]; ok && t.Actual != nil {
					*into = &Temp{*t.Actual, deref(t.Target)}
				}
			}
		}
	case "prusalink":
		var status struct {
			Job *struct {
				ID        int      `json:"id"`
				Progress  *float64 `json:"progress"`
				Remaining *int     `json:"time_remaining"`
			} `json:"job"`
			Printer struct {
				State        string   `json:"state"`
				Nozzle       *float64 `json:"temp_nozzle"`
				NozzleTarget *float64 `json:"target_nozzle"`
				Bed          *float64 `json:"temp_bed"`
				BedTarget    *float64 `json:"target_bed"`
			} `json:"printer"`
		}
		if _, err := h.call(ctx, "GET", base.JoinPath("/api/v1/status").String(), nil, &status); err != nil {
			return st, err
		}
		pr := status.Printer
		st.Detail = pr.State
		st.State = map[string]string{"IDLE": "idle", "READY": "idle", "PRINTING": "printing", "PAUSED": "paused", "FINISHED": "finished", "STOPPED": "idle", "ERROR": "failed", "ATTENTION": "failed", "BUSY": "busy"}[pr.State]
		if pr.Nozzle != nil {
			st.Nozzle = &Temp{*pr.Nozzle, deref(pr.NozzleTarget)}
		}
		if pr.Bed != nil {
			st.Bed = &Temp{*pr.Bed, deref(pr.BedTarget)}
		}
		if j := status.Job; j != nil {
			if j.Progress != nil {
				st.Progress = ptr(*j.Progress / 100)
			}
			st.Remaining = j.Remaining
			var job struct {
				File struct {
					Name string `json:"display_name"`
				} `json:"file"`
			}
			if _, err := h.call(ctx, "GET", base.JoinPath("/api/v1/job").String(), nil, &job); err == nil {
				st.File = job.File.Name
			}
		}
	}
	if st.State == "" {
		st.State = "busy"
	}
	return st, nil
}

func deref(v *float64) float64 {
	if v == nil {
		return 0
	}
	return *v
}

func controlHTTP(ctx context.Context, p *PrinterConfig, secret, action string) error {
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return err
	}
	switch p.Kind {
	case "moonraker":
		verb := map[string]string{"stop": "cancel", "pause": "pause", "resume": "resume"}[action]
		_, err = h.call(ctx, "POST", base.JoinPath("/printer/print", verb).String(), []byte("{}"), nil)
	case "octoprint":
		command := map[string]any{"command": "cancel"}
		if action != "stop" {
			command = map[string]any{"command": "pause", "action": action}
		}
		body, _ := json.Marshal(command)
		_, err = h.call(ctx, "POST", base.JoinPath("/api/job").String(), body, nil)
	case "prusalink":
		var status struct {
			Job *struct {
				ID int `json:"id"`
			} `json:"job"`
		}
		if _, err = h.call(ctx, "GET", base.JoinPath("/api/v1/status").String(), nil, &status); err != nil {
			return err
		}
		if status.Job == nil {
			return errors.New("The printer isn't running a print job right now.")
		}
		job := base.JoinPath("/api/v1/job", fmt.Sprint(status.Job.ID))
		if action == "stop" {
			_, err = h.call(ctx, "DELETE", job.String(), nil, nil)
		} else {
			_, err = h.call(ctx, "PUT", job.JoinPath(action).String(), nil, nil)
		}
	}
	return err
}
