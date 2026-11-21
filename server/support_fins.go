package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const (
	supportFinsTimeout   = 2 * time.Minute
	supportFinsMemoryMB  = 1536
	supportFinsFileMB    = 128
	supportFinsStdoutMax = 8 << 20
	supportFinsStderrMax = 512 << 10
)

type supportFinsOutput struct {
	buf      bytes.Buffer
	max      int
	total    int64
	exceeded bool
}

func (w *supportFinsOutput) Write(p []byte) (int, error) {
	w.total += int64(len(p))
	remaining := w.max - w.buf.Len()
	if remaining > 0 {
		n := len(p)
		if n > remaining {
			n = remaining
		}
		_, _ = w.buf.Write(p[:n])
	}
	if w.total > int64(w.max) {
		w.exceeded = true
	}
	return len(p), nil
}

type supportFinsReport struct {
	Output *string         `json:"output"`
	Stats  json.RawMessage `json:"stats"`
	Report string          `json:"report"`
}

func supportFinsFailure(j *Job, code, message string) {
	j.failWith(code, "Support Fins "+message)
}

func supportFinsProcessError(stderr *supportFinsOutput, fallback string) string {
	text := strings.TrimSpace(stderr.buf.String())
	if text == "" {
		return fallback
	}
	lines := strings.Split(text, "\n")
	text = lines[len(lines)-1]
	for _, line := range lines {
		lower := strings.ToLower(line)
		if strings.Contains(lower, "allocation failed") || strings.Contains(lower, "out of memory") {
			text = line
			break
		}
	}
	if len(text) > 4000 {
		text = text[:4000]
	}
	return sanitizeToolError(errors.New(text))
}

func (m *JobManager) runSupportFins(parent context.Context, j *Job) {
	ctx, cancel := context.WithTimeout(parent, supportFinsTimeout)
	defer cancel()
	if m.cfg.SupportFinsNode == "" || m.cfg.SupportFinsCLI == "" {
		supportFinsFailure(j, "support_fins_unavailable", "is not available in this server installation.")
		return
	}
	input := filepath.Join(j.Dir, "input", j.Request.Files[0])
	output := j.OutputPath("stl")
	args := []string{
		"--max-old-space-size=384", m.cfg.SupportFinsCLI,
		input, "-o", output,
		"--fins-only", "--json",
		"--nozzle", strconv.FormatFloat(j.Request.SupportFinsNozzle, 'f', -1, 64),
		"--layer-height", strconv.FormatFloat(j.Request.SupportFinsLayer, 'f', -1, 64),
	}
	j.log("server", "support-fins-node "+quoteArgs(args))
	cmd := exec.CommandContext(ctx, m.cfg.SupportFinsNode, args...)
	cmd.Dir = j.Dir
	cmd.Env = append(engineEnv(filepath.Join(j.Dir, "tmp")), "ELECTRON_RUN_AS_NODE=1")
	if runtime.GOOS == "linux" && !m.cfg.SupportFinsElectron {
		if err := limitProcessMemory(cmd, supportFinsMemoryMB, supportFinsFileMB); err != nil {
			supportFinsFailure(j, "support_fins_setup_failed", "could not limit its helper process: "+sanitizeToolError(err))
			return
		}
	}
	configureJobProcess(cmd)
	cmd.WaitDelay = 5 * time.Second
	stdout := &supportFinsOutput{max: supportFinsStdoutMax}
	stderr := &supportFinsOutput{max: supportFinsStderrMax}
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	if !j.setState(StateSlicing, "") {
		return
	}
	j.log("server", "progress slicing 0")
	if err := cmd.Start(); err != nil {
		switch {
		case errors.Is(ctx.Err(), context.Canceled):
			j.setState(StateCancelled, "cancelled")
		case errors.Is(ctx.Err(), context.DeadlineExceeded):
			supportFinsFailure(j, "support_fins_timeout", "exceeded the two-minute time limit.")
		default:
			supportFinsFailure(j, "support_fins_setup_failed", "could not start: "+sanitizeToolError(err))
		}
		return
	}
	waitErr := cmd.Wait()
	code := cmd.ProcessState.ExitCode()
	j.mu.Lock()
	j.exitCode = &code
	j.mu.Unlock()
	j.log("server", fmt.Sprintf("support fins helper exit=%d stdout=%d stderr=%d", code, stdout.total, stderr.total))
	for _, line := range strings.Split(strings.TrimSpace(stderr.buf.String()), "\n") {
		if line != "" {
			j.log("stderr", line)
		}
	}
	switch {
	case errors.Is(ctx.Err(), context.Canceled):
		j.setState(StateCancelled, "cancelled")
		return
	case errors.Is(ctx.Err(), context.DeadlineExceeded):
		supportFinsFailure(j, "support_fins_timeout", "exceeded the time limit; no geometry was imported.")
		return
	case stdout.exceeded || stderr.exceeded:
		supportFinsFailure(j, "support_fins_output_limit", "produced too much diagnostic output; no geometry was imported.")
		return
	case waitErr != nil:
		detail := supportFinsProcessError(stderr, waitErr.Error())
		if strings.Contains(strings.ToLower(detail), "allocation failed") || strings.Contains(strings.ToLower(detail), "out of memory") {
			supportFinsFailure(j, "support_fins_resource_limit", detail)
		} else if code == 2 {
			supportFinsFailure(j, "support_fins_arguments", detail)
		} else {
			supportFinsFailure(j, "support_fins_failed", detail)
		}
		return
	}
	var report supportFinsReport
	if err := json.Unmarshal([]byte(strings.TrimSpace(stdout.buf.String())), &report); err != nil || len(report.Stats) == 0 || string(report.Stats) == "null" {
		supportFinsFailure(j, "support_fins_output_unreadable", "returned an unreadable result; no geometry was imported.")
		return
	}
	fins := report.Output != nil
	if fins && filepath.Clean(*report.Output) != filepath.Clean(output) {
		supportFinsFailure(j, "support_fins_output_unreadable", "returned an unexpected output path; no geometry was imported.")
		return
	}
	info, err := os.Stat(output)
	if fins && (err != nil || info.Size() > supportFinsFileMB<<20) {
		supportFinsFailure(j, "support_fins_output_limit", "did not produce a bounded STL output; no geometry was imported.")
		return
	}
	if !fins && !errors.Is(err, os.ErrNotExist) {
		supportFinsFailure(j, "support_fins_output_unreadable", "reported no fins but wrote an output; no geometry was imported.")
		return
	}
	if fins {
		contents, readErr := os.ReadFile(output)
		if readErr != nil || len(contents) < 84 {
			supportFinsFailure(j, "support_fins_output_unreadable", "did not produce a readable STL; no geometry was imported.")
			return
		}
		triangles := binary.LittleEndian.Uint32(contents[80:84])
		if triangles == 0 || uint64(len(contents)) != 84+uint64(triangles)*50 {
			supportFinsFailure(j, "support_fins_output_unreadable", "did not produce a valid binary STL; no geometry was imported.")
			return
		}
	}
	record, _ := json.Marshal(map[string]any{
		"schema": "orca.support-fins.v1",
		"fins":   fins,
		"stats":  report.Stats,
		"report": report.Report,
	})
	j.mu.Lock()
	j.slice = record
	j.mu.Unlock()
	j.log("server", "progress slicing 100")
	j.setState(StateDone, "")
}
