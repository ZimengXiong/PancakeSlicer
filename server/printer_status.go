package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type statusHub struct {
	m        *SendManager
	mu       sync.Mutex
	subs     int
	status   map[string]PrinterStatus
	monitors map[string]*monitor
	wake     chan struct{}
	idle     *time.Timer
}

type monitor struct {
	cfg    *PrinterConfig
	cancel context.CancelFunc
	poke   chan struct{}
}

const pollInterval = 2 * time.Second
const monitorLinger = time.Minute

func newStatusHub(m *SendManager) *statusHub {
	return &statusHub{m: m, status: map[string]PrinterStatus{}, monitors: map[string]*monitor{}, wake: make(chan struct{})}
}

func (h *statusHub) bump() {
	h.mu.Lock()
	close(h.wake)
	h.wake = make(chan struct{})
	h.mu.Unlock()
}

func (h *statusHub) changed() {
	h.bump()
	go h.reconcile()
}

func (h *statusHub) subscribe() func() {
	h.mu.Lock()
	h.subs++
	if h.idle != nil {
		h.idle.Stop()
		h.idle = nil
	}
	h.mu.Unlock()
	h.reconcile()
	return func() {
		h.mu.Lock()
		defer h.mu.Unlock()
		if h.subs--; h.subs == 0 {
			h.idle = time.AfterFunc(monitorLinger, func() {
				h.mu.Lock()
				h.idle = nil
				h.mu.Unlock()
				h.reconcile()
			})
		}
	}
}

func (h *statusHub) reconcile() {
	conns := h.m.connections()
	h.mu.Lock()
	defer h.mu.Unlock()
	watching := h.subs > 0 || h.idle != nil
	want := map[string]*PrinterConfig{}
	if watching {
		for _, p := range conns {
			if p.Printer != "" && p.Kind != "bambu-connect" {
				want[p.Printer] = p
			}
		}
	}
	for key, mon := range h.monitors {
		if want[key] != mon.cfg {
			mon.cancel()
			delete(h.monitors, key)
			delete(h.status, key)
		}
	}
	for key, p := range want {
		if h.monitors[key] == nil {
			ctx, cancel := context.WithCancel(context.Background())
			mon := &monitor{cfg: p, cancel: cancel, poke: make(chan struct{}, 1)}
			h.monitors[key] = mon
			h.status[key] = PrinterStatus{Printer: key, State: "connecting", At: time.Now().UTC()}
			go h.run(ctx, mon)
		}
	}
	close(h.wake)
	h.wake = make(chan struct{})
}

func (h *statusHub) set(from *monitor, st PrinterStatus) {
	key := from.cfg.Printer
	st.Printer = key
	h.mu.Lock()
	prev, ok := h.status[key]
	if h.monitors[key] != from {
		h.mu.Unlock()
		return
	}
	prev.At, st.At = time.Time{}, time.Time{}
	a, _ := json.Marshal(prev)
	b, _ := json.Marshal(st)
	st.At = time.Now().UTC()
	if ok && bytes.Equal(a, b) {
		h.mu.Unlock()
		return
	}
	h.status[key] = st
	close(h.wake)
	h.wake = make(chan struct{})
	h.mu.Unlock()
	if h.m.history.confirmStarted(st) || h.m.history.recordEnd(st) {
		h.bump()
	}
}

func (h *statusHub) current(key string) (PrinterStatus, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	st, ok := h.status[key]
	return st, ok
}

func (h *statusHub) poke(key string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if mon := h.monitors[key]; mon != nil {
		select {
		case mon.poke <- struct{}{}:
		default:
		}
	}
}

func (h *statusHub) run(ctx context.Context, mon *monitor) {
	p := mon.cfg
	backoff := 3 * time.Second
	for ctx.Err() == nil {
		secret, err := p.secret()
		began := time.Now()
		if err == nil {
			publish := func(st PrinterStatus) { h.set(mon, st) }
			switch p.Kind {
			case "bambu-lan":
				err = monitorBambu(ctx, p, secret, publish)
			case "flashforge":
				err = pollFlashforge(ctx, p, publish, mon.poke)
			default:
				err = pollHost(ctx, p, secret, publish, mon.poke)
			}
		}
		if ctx.Err() != nil {
			return
		}
		h.set(mon, PrinterStatus{State: "offline", Detail: redact(fmt.Sprint(err), secret)})
		if time.Since(began) > time.Minute {
			backoff = 3 * time.Second
		}
		select {
		case <-ctx.Done():
			return
		case <-mon.poke:
		case <-time.After(backoff):
		}
		backoff = min(2*backoff, 30*time.Second)
	}
}

func pollHost(ctx context.Context, p *PrinterConfig, secret string, publish func(PrinterStatus), poke chan struct{}) error {
	h, base, err := httpSession(ctx, p, secret)
	if err != nil {
		return err
	}
	if p.Kind == "duet" {
		if _, err := duetConnect(ctx, h, base); err != nil {
			return err
		}
		defer duetDisconnect(h, base)
	}
	for {
		rctx, cancel := context.WithTimeout(ctx, 10*time.Second)
		var st PrinterStatus
		if p.Kind == "duet" {
			st, err = statusDuet(rctx, h, base)
		} else {
			st, err = statusHTTP(rctx, h, base)
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

type statusSnapshot struct {
	Enabled     bool             `json:"enabled"`
	Connections []ConnectionView `json:"connections"`
	Status      []PrinterStatus  `json:"status"`
	Transfers   []Transfer       `json:"transfers"`
}

func (s *Server) snapshot() statusSnapshot {
	m := s.sends
	snap := statusSnapshot{Enabled: m.store != nil, Connections: []ConnectionView{}, Status: []PrinterStatus{}, Transfers: m.activeTransfers()}
	for _, p := range m.connections() {
		snap.Connections = append(snap.Connections, p.view())
	}
	m.hub.mu.Lock()
	for _, st := range m.hub.status {
		snap.Status = append(snap.Status, st)
	}
	m.hub.mu.Unlock()
	for i := range snap.Status {
		snap.Status[i].Job = s.deviceJob(snap.Status[i])
	}
	sort.Slice(snap.Status, func(i, j int) bool { return snap.Status[i].Printer < snap.Status[j].Printer })
	active := map[string]bool{}
	for _, t := range snap.Transfers {
		active[t.ID] = true
	}
	for _, t := range m.history.recent(2 * time.Minute) {
		if !active[t.ID] {
			snap.Transfers = append(snap.Transfers, t)
		}
	}
	return snap
}

func (s *Server) connectionEvents(w http.ResponseWriter, r *http.Request) {
	controller := http.NewResponseController(w)
	if _, ok := w.(http.Flusher); !ok {
		fail(w, 500, "Live progress streaming isn't available in this connection.")
		return
	}
	hub := s.sends.hub
	defer hub.subscribe()()
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	defer controller.SetWriteDeadline(time.Time{})
	heartbeat := time.NewTicker(15 * time.Second)
	defer heartbeat.Stop()
	var last []byte
	for {
		hub.mu.Lock()
		wake := hub.wake
		hub.mu.Unlock()
		snap := s.snapshot()
		b, _ := json.Marshal(snap)
		if err := controller.SetWriteDeadline(time.Now().Add(15 * time.Second)); err != nil {
			return
		}
		if !bytes.Equal(b, last) {
			if _, err := fmt.Fprintf(w, "event: state\ndata: %s\n\n", b); err != nil {
				return
			}
			last = b
		}
		if err := controller.Flush(); err != nil {
			return
		}
		var tick <-chan time.Time
		for _, t := range snap.Transfers {
			if t.active() {
				tick = time.After(250 * time.Millisecond)
			}
		}
		select {
		case <-r.Context().Done():
			return
		case <-wake:
		case <-tick:
		case <-heartbeat.C:
			if _, err := io.WriteString(w, ": keepalive\n\n"); err != nil {
				return
			}
		}
	}
}

type sendHistory struct {
	path string
	mu   sync.Mutex
	list []Transfer
}

const sendHistoryCap = 5000

const jobEndStale = 5 * time.Second

func backupOnce(path string, b []byte) error {
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return writeFileAtomic(path, b)
}

func loadSendHistory(dir string) *sendHistory {
	h := &sendHistory{path: filepath.Join(dir, "sends.json")}
	b, err := os.ReadFile(h.path)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			log.Printf("warning: send history %s: %v", h.path, err)
		}
		return h
	}
	var file struct {
		Sends []Transfer `json:"sends"`
	}
	if err := json.Unmarshal(b, &file); err != nil {
		log.Printf("warning: send history %s is not valid JSON; kept as %s.invalid", h.path, h.path)
		os.Rename(h.path, h.path+".invalid")
		return h
	}
	h.list = file.Sends
	now := time.Now().UTC()
	migrated := 0
	for i := range h.list {
		t := &h.list[i]
		if t.active() {
			t.Error, t.Finished = "The service restarted during this transfer; check the printer before sending again", &now
			t.Outcome, t.Reason, t.Recovery = "failed", "interrupted", "Check the printer's screen before sending again."
			t.to(TransferFailed)
		}
		if t.Outcome == "" {
			t.Outcome = recordedOutcome(*t)
			if t.Outcome == "failed" {
				t.Reason = "unknown"
			}
			migrated++
		}
	}
	stale, ran := 0, 0
	for i := range h.list {
		t := &h.list[i]
		if t.JobEnd == "" || t.Running || t.JobEndAt == nil {
			continue
		}
		if t.Finished != nil && t.JobEndAt.Sub(*t.Finished) < jobEndStale {
			log.Printf("send history: cleared %s of %s on %s (%s): reported %s after the send, before the job ran", t.JobEnd, t.Filename, t.Name, t.ID, t.JobEndAt.Sub(*t.Finished).Round(time.Second))
			t.JobEnd, t.JobEndAt = "", nil
			stale++
		} else {
			t.Running = true
		}
		ran++
	}
	if stale > 0 {
		log.Printf("send history: cleared %d job ends reported before their job ran", stale)
	}
	if ran > 0 {
		if err := backupOnce(h.path+".pre-running", b); err != nil {
			log.Printf("warning: send history: cannot back up %s before migrating it: %v", h.path, err)
			return h
		}
		if migrated == 0 {
			h.writeLocked()
		}
	}
	if migrated > 0 {
		backup := h.path + ".pre-outcome"
		if _, err := os.Stat(backup); errors.Is(err, os.ErrNotExist) {
			if err := writeFileAtomic(backup, b); err != nil {
				log.Printf("warning: send history: cannot back up %s before migrating it: %v", h.path, err)
				return h
			}
		}
		h.writeLocked()
		log.Printf("send history: recorded the outcome of %d earlier sends (backup %s)", migrated, backup)
	}
	return h
}

func recordedOutcome(t Transfer) string {
	switch {
	case t.State != TransferDone:
		return "failed"
	case !t.Start:
		return "uploaded"
	case strings.Contains(t.Message, "did not confirm"):
		return "waiting_for_printer"
	}
	return "printing"
}

func (h *sendHistory) record(t Transfer) {
	if h == nil {
		return
	}
	t.sent, t.cancel = nil, nil
	h.mu.Lock()
	defer h.mu.Unlock()
	replaced := false
	for i := range h.list {
		if h.list[i].ID == t.ID {
			h.list[i], replaced = t, true
		}
	}
	if !replaced {
		h.list = append(h.list, t)
	}
	if len(h.list) > sendHistoryCap {
		h.list = h.list[len(h.list)-sendHistoryCap:]
	}
	h.writeLocked()
}

func (h *sendHistory) writeLocked() {
	b, _ := json.MarshalIndent(struct {
		Sends []Transfer `json:"sends"`
	}{h.list}, "", " ")
	if err := writeFileAtomic(h.path, b); err != nil {
		log.Printf("warning: cannot save the send history: %v", err)
	}
}

func sendStem(name string) string {
	return strings.TrimSuffix(strings.TrimSuffix(filepath.Base(name), ".3mf"), ".gcode")
}

func (h *sendHistory) confirmStarted(st PrinterStatus) bool {
	if h == nil || !st.Online || (st.State != "printing" && st.State != "busy" && st.State != "paused") || st.File == "" {
		return false
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for i := len(h.list) - 1; i >= 0; i-- {
		t := &h.list[i]
		if t.Printer != st.Printer || !t.Start {
			continue
		}
		if t.Outcome != "waiting_for_printer" || st.At.Before(t.Started) || sendStem(t.Filename) != sendStem(st.File) {
			return false
		}
		t.Outcome, t.Message = "printing", "Printing started; confirmed by the printer."
		h.writeLocked()
		return true
	}
	return false
}

func (h *sendHistory) recordEnd(st PrinterStatus) bool {
	end := map[string]string{"finished": "finished", "failed": "failed"}[st.State]
	if st.State == "idle" && st.Detail == "cancelled" {
		end = "cancelled"
	}
	running := st.State == "printing" || st.State == "paused" || st.State == "busy"
	if h == nil || !st.Online || end == "" && !running || st.File == "" {
		return false
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for i := len(h.list) - 1; i >= 0; i-- {
		t := &h.list[i]
		if t.Printer != st.Printer || !t.Start {
			continue
		}
		if t.Outcome != "printing" || t.JobEnd != "" || st.At.Before(t.Started) || sendStem(t.Filename) != sendStem(st.File) {
			return false
		}
		if running || !t.Running {
			changed := running && !t.Running
			t.Running = t.Running || running
			if changed {
				h.writeLocked()
			}
			return changed
		}
		t.JobEnd = end
		at := st.At
		t.JobEndAt = &at
		if end == "failed" {
			t.JobErrors = st.Messages
		}
		h.writeLocked()
		return true
	}
	return false
}

func (h *sendHistory) cards() map[string]string {
	out := map[string]string{}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, t := range h.list {
		key := t.Printer + "\x00" + t.Filename
		switch {
		case t.Kind != "bambu-lan" || t.Printer == "":
		case t.State == "done":
			out[key] = t.Job
		case t.Sent > 0 || t.State == "uploading" || t.State == "starting":
			delete(out, key)
		}
	}
	return out
}

func (h *sendHistory) recent(age time.Duration) []Transfer {
	if h == nil {
		return nil
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	out := []Transfer{}
	latest := map[string]string{}
	for _, t := range h.list {
		latest[t.Printer] = t.ID
	}
	for _, t := range h.list {
		if t.Finished != nil && (time.Since(*t.Finished) < age || latest[t.Printer] == t.ID && (t.Outcome == "waiting_for_printer" || t.Outcome == "printing")) {
			out = append(out, t)
		}
	}
	return out
}

func (s *Server) listSends(w http.ResponseWriter, r *http.Request) {
	h := s.sends.history
	out := []Transfer{}
	q := r.URL.Query()
	offset, _ := strconv.Atoi(q.Get("offset"))
	limit, err := strconv.Atoi(q.Get("limit"))
	if err != nil || limit <= 0 {
		limit = sendHistoryCap
	}
	total := 0
	if h != nil {
		h.mu.Lock()
		for i := len(h.list) - 1; i >= 0; i-- {
			if key := q.Get("printer"); key == "" || h.list[i].Printer == key {
				if total >= offset && len(out) < limit {
					out = append(out, h.list[i])
				}
				total++
			}
		}
		h.mu.Unlock()
	}
	writeJSON(w, 200, map[string]any{"sends": out, "total": total})
}
