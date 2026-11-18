package main

import (
	"context"
	"math"
	"net/http"
	"net/url"
	"time"
)

type moonrakerStatus struct {
	PrintStats struct {
		State         string  `json:"state"`
		Filename      string  `json:"filename"`
		PrintDuration float64 `json:"print_duration"`
		TotalDuration float64 `json:"total_duration"`
		Message       string  `json:"message"`
		Info          struct {
			Current *int `json:"current_layer"`
			Total   *int `json:"total_layer"`
		} `json:"info"`
	} `json:"print_stats"`
	SD struct {
		Progress     *float64 `json:"progress"`
		FilePosition *float64 `json:"file_position"`
	} `json:"virtual_sdcard"`
	Display struct {
		Progress *float64 `json:"progress"`
	} `json:"display_status"`
	Extruder *moonrakerHeater `json:"extruder"`
	Bed      *moonrakerHeater `json:"heater_bed"`
}

type moonrakerHeater struct {
	Temperature float64 `json:"temperature"`
	Target      float64 `json:"target"`
}

type moonrakerMetadata struct {
	EstimatedTime *float64 `json:"estimated_time"`
	StartByte     *float64 `json:"gcode_start_byte"`
	EndByte       *float64 `json:"gcode_end_byte"`
	LayerCount    *int     `json:"layer_count"`
}

func statusMoonraker(ctx context.Context, h *hostSession, base *url.URL) (PrinterStatus, error) {
	st := PrinterStatus{Online: true}
	var q struct {
		Result struct {
			Status moonrakerStatus `json:"status"`
		} `json:"result"`
	}
	code, err := h.call(ctx, "GET", base.JoinPath("/printer/objects/query").String()+"?print_stats&virtual_sdcard&display_status&extruder&heater_bed", nil, &q)
	if err != nil {
		if code == http.StatusServiceUnavailable || code == http.StatusBadRequest {
			st.State, st.Detail = "error", err.Error()
			return st, nil
		}
		return st, err
	}
	s := q.Result.Status
	var metadata moonrakerMetadata
	if s.PrintStats.Filename != "" && (s.PrintStats.State == "printing" || s.PrintStats.State == "paused") {
		var response struct {
			Result moonrakerMetadata `json:"result"`
		}
		target := base.JoinPath("/server/files/metadata")
		query := target.Query()
		query.Set("filename", s.PrintStats.Filename)
		target.RawQuery = query.Encode()
		code, err := h.call(ctx, "GET", target.String(), nil, &response)
		if err == nil {
			metadata = response.Result
		} else if code != http.StatusNotFound {
			st.Messages = append(st.Messages, PrinterMessage{Code: "moonraker_metadata", Message: "Oops! Couldn't read print file metadata: " + err.Error()})
		}
	}
	applyMoonrakerJob(&st, s, metadata)
	if s.Extruder != nil {
		st.Nozzle = &Temp{Actual: s.Extruder.Temperature, Target: s.Extruder.Target}
	}
	if s.Bed != nil {
		st.Bed = &Temp{Actual: s.Bed.Temperature, Target: s.Bed.Target}
	}
	return st, nil
}

func applyMoonrakerJob(st *PrinterStatus, s moonrakerStatus, metadata moonrakerMetadata) {
	stats := s.PrintStats
	st.Detail, st.File = stats.State, stats.Filename
	st.State = map[string]string{"standby": "idle", "printing": "printing", "paused": "paused", "complete": "finished", "cancelled": "idle", "error": "failed"}[stats.State]
	if stats.State == "error" && stats.Message != "" {
		st.Detail = stats.Message
	}
	st.Layer, st.Layers = stats.Info.Current, stats.Info.Total
	if st.State != "printing" && st.State != "paused" {
		return
	}
	if st.Layers == nil {
		st.Layers = metadata.LayerCount
	}
	progress := s.SD.Progress
	if position, start, end := s.SD.FilePosition, metadata.StartByte, metadata.EndByte; position != nil && start != nil && end != nil && *end > *start {
		progress = ptr((*position - *start) / (*end - *start))
	}
	if progress != nil {
		progress = ptr(math.Max(0, math.Min(1, *progress)))
	}
	fileProgress := progress
	if progress == nil {
		progress = s.Display.Progress
	}
	if progress != nil {
		st.Progress = ptr(math.Max(0, math.Min(1, *progress)))
	}
	duration := stats.PrintDuration
	st.Elapsed = ptr(int(math.Max(0, math.Floor(duration))))
	if fileProgress != nil && *fileProgress >= 0.01 && duration > 0 {
		total := duration / *fileProgress
		st.FileEstimate = ptr(int(math.Floor(total)))
		st.Remaining = ptr(int(math.Max(0, math.Floor(total-duration))))
	}
	if metadata.EstimatedTime != nil && *metadata.EstimatedTime > 0 {
		st.SlicerEstimate = ptr(int(math.Floor(*metadata.EstimatedTime)))
		st.Remaining = ptr(int(math.Max(0, math.Floor(*metadata.EstimatedTime-duration))))
	}
	if stats.TotalDuration > 0 {
		started := time.Now().Add(-time.Duration(stats.TotalDuration * float64(time.Second))).UTC()
		st.StartedAt = &started
	}
}
