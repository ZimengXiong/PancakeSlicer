package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type JobState string

const (
	StateIdle        JobState = "idle"
	StateQueued      JobState = "queued"
	StatePreparing   JobState = "preparing"
	StateSlicing     JobState = "slicing"
	StateStoring     JobState = "storing"
	StateCanceling   JobState = "canceling"
	StateDone        JobState = "done"
	StateFailed      JobState = "failed"
	StateCancelled   JobState = "cancelled"
	StateInterrupted JobState = "interrupted"
	StateStale       JobState = "stale"
)

var jobTransitions = map[JobState]map[JobState]bool{
	StateIdle:        {StateQueued: true, StateInterrupted: true},
	StateQueued:      {StatePreparing: true, StateCanceling: true, StateInterrupted: true},
	StatePreparing:   {StateSlicing: true, StateStoring: true, StateDone: true, StateFailed: true, StateCanceling: true, StateInterrupted: true},
	StateSlicing:     {StateStoring: true, StateDone: true, StateFailed: true, StateCanceling: true, StateInterrupted: true},
	StateStoring:     {StateDone: true, StateFailed: true, StateCanceling: true, StateInterrupted: true},
	StateCanceling:   {StateCancelled: true, StateInterrupted: true},
	StateDone:        {StateStale: true},
	StateFailed:      {},
	StateCancelled:   {},
	StateInterrupted: {},
	StateStale:       {},
}

func (s JobState) Finished() bool {
	return s == StateDone || s == StateFailed || s == StateCancelled || s == StateInterrupted || s == StateStale
}

type JobTransition struct {
	Seq      uint64    `json:"seq"`
	At       time.Time `json:"at"`
	From     JobState  `json:"from"`
	To       JobState  `json:"to"`
	Accepted bool      `json:"accepted"`
	Reason   string    `json:"reason,omitempty"`
}

const maxTransitions = 256

func (j *Job) Transitions() []JobTransition {
	j.mu.Lock()
	defer j.mu.Unlock()
	return append([]JobTransition{}, j.transitions...)
}

func (j *Job) setState(s JobState, reason string) bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.setStateLocked(s, reason)
}

func (j *Job) setStateLocked(s JobState, reason string) bool {
	if s.Finished() && j.stopReason != "" {
		if j.stopReason == "server shutting down" {
			s, reason = StateInterrupted, j.stopReason
		} else if j.state == StateCanceling {
			s, reason = StateCancelled, j.stopReason
		}
	}
	j.transitionSeq++
	t := JobTransition{Seq: j.transitionSeq, At: time.Now().UTC(), From: j.state, To: s,
		Accepted: jobTransitions[j.state][s], Reason: reason}
	j.transitions = append(j.transitions, t)
	if len(j.transitions) > maxTransitions {
		j.transitions = j.transitions[len(j.transitions)-maxTransitions:]
	}
	if f, err := os.OpenFile(filepath.Join(j.Dir, "transitions.jsonl"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600); err != nil {
		log.Printf("job %s transition log: %v", j.ID, err)
	} else {
		if err := json.NewEncoder(f).Encode(t); err != nil {
			log.Printf("job %s transition log: %v", j.ID, err)
		}
		f.Close()
	}
	if !t.Accepted {
		log.Printf("job %s illegal transition %s -> %s: %s", j.ID, t.From, t.To, reason)
		return false
	}
	j.state, j.errMsg = s, reason
	if s == StatePreparing {
		j.started = t.At
	}
	if s.Finished() {
		j.finished = t.At
		if !j.stageStarted.IsZero() {
			j.stageDurations[j.stage] = t.At.Sub(j.stageStarted).Seconds()
		}
		if s == StateDone {
			j.stageHistory.observe(j.stageDurations)
		}
		if j.dropped > 0 {
			j.appendLocked(Event{Type: "log", Stream: "server", Line: fmt.Sprintf("%d further output lines not kept", j.dropped)})
		}
	}
	eventType := "state"
	if s.Finished() {
		eventType = "done"
	}
	if s.Finished() {
		j.queuePosition = 0
	}
	pos := j.queuePosition
	pct := 0.0
	if s == StateDone {
		pct = 100
	}
	e := Event{Type: eventType, State: s, Error: reason, QueuePosition: &pos, Stage: string(s), Pct: &pct}
	if !s.Finished() {
		j.progressLocked(&e)
	} else {
		zero := 0.0
		j.stage, j.progress, j.eta = string(s), &pct, &zero
		e.ETA = &zero
	}
	j.appendLocked(e)
	j.persistLocked()
	return true
}

type jobRecord struct {
	ID             string             `json:"id"`
	Client         string             `json:"client"`
	Request        JobRequest         `json:"request"`
	Created        time.Time          `json:"created"`
	State          JobState           `json:"state"`
	Started        time.Time          `json:"started"`
	Finished       time.Time          `json:"finished"`
	Error          string             `json:"error,omitempty"`
	ExitCode       *int               `json:"exit_code,omitempty"`
	Slice          json.RawMessage    `json:"record,omitempty"`
	IdempotencyKey string             `json:"idempotency_key,omitempty"`
	RetryOf        string             `json:"retry_of,omitempty"`
	NextEvent      int                `json:"next_event"`
	TransitionSeq  uint64             `json:"transition_seq"`
	Stage          string             `json:"stage,omitempty"`
	Progress       *float64           `json:"progress,omitempty"`
	QueuePosition  int                `json:"queue_position"`
	StageDurations map[string]float64 `json:"stage_durations,omitempty"`
	TimeoutWarning bool               `json:"timeout_warning,omitempty"`
}

func atomicJobFile(name string, data []byte) error {
	f, err := os.CreateTemp(filepath.Dir(name), ".job-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if err := f.Chmod(0600); err != nil {
		f.Close()
		return err
	}
	_, writeErr := f.Write(data)
	syncErr := f.Sync()
	closeErr := f.Close()
	if err := errors.Join(writeErr, syncErr, closeErr); err != nil {
		return err
	}
	return os.Rename(f.Name(), name)
}

func (j *Job) persistLocked() {
	r := jobRecord{ID: j.ID, Client: j.Client, Request: j.Request, Created: j.Created, State: j.state,
		Started: j.started, Finished: j.finished, Error: j.errMsg, ExitCode: j.exitCode, Slice: j.slice,
		IdempotencyKey: j.IdempotencyKey, RetryOf: j.RetryOf, NextEvent: j.nextEvent, TransitionSeq: j.transitionSeq,
		Stage: j.stage, Progress: j.progress, QueuePosition: j.queuePosition, StageDurations: j.stageDurations, TimeoutWarning: j.timeoutWarning}
	b, err := json.Marshal(r)
	if err == nil {
		err = atomicJobFile(filepath.Join(j.Dir, "job.json"), b)
	}
	j.persistenceErr = err
	if err != nil {
		log.Printf("job %s persist: %v", j.ID, err)
	}
}

func (j *Job) writeEventLocked(e Event, compact bool) {
	name := filepath.Join(j.Dir, "events.jsonl")
	var err error
	if compact {
		var b bytes.Buffer
		enc := json.NewEncoder(&b)
		for _, event := range j.events {
			if err = enc.Encode(event); err != nil {
				break
			}
		}
		if err == nil {
			err = atomicJobFile(name, b.Bytes())
		}
	} else {
		var f *os.File
		f, err = os.OpenFile(name, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
		if err == nil {
			err = json.NewEncoder(f).Encode(e)
			err = errors.Join(err, f.Close())
		}
	}
	if err != nil {
		log.Printf("job %s event journal: %v", j.ID, err)
	}
}

func (m *JobManager) restoreJobs() error {
	entries, err := os.ReadDir(m.cfg.JobRoot)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if !entry.IsDir() || !jobIDPattern.MatchString(entry.Name()) {
			continue
		}
		dir := filepath.Join(m.cfg.JobRoot, entry.Name())
		b, err := os.ReadFile(filepath.Join(dir, "job.json"))
		var r jobRecord
		if err != nil || json.Unmarshal(b, &r) != nil || r.ID != entry.Name() || jobTransitions[r.State] == nil {
			log.Printf("retaining unreadable job directory %s", entry.Name())
			continue
		}
		for _, journal := range []string{"transitions.jsonl", "events.jsonl"} {
			if err := repairJobJournal(filepath.Join(dir, journal)); err != nil {
				return fmt.Errorf("repair job %s journal: %w", r.ID, err)
			}
		}
		var transitions []JobTransition
		if f, err := os.Open(filepath.Join(dir, "transitions.jsonl")); err == nil {
			scanJobJournal(f, func(line string) {
				var t JobTransition
				if json.Unmarshal([]byte(line), &t) != nil {
					return
				}
				if t.Seq > r.TransitionSeq {
					r.TransitionSeq = t.Seq
					if t.Accepted && t.From == r.State && jobTransitions[r.State][t.To] {
						r.State, r.Error = t.To, t.Reason
						if t.To.Finished() {
							r.Finished = t.At
						}
						if t.To == StatePreparing {
							r.Started = t.At
						}
					}
				}
				transitions = append(transitions, t)
				if len(transitions) > maxTransitions {
					transitions = transitions[1:]
				}
			})
			f.Close()
		}
		j := &Job{ID: r.ID, Client: r.Client, Dir: dir, Request: r.Request, Created: r.Created,
			state: r.State, started: r.Started, finished: r.Finished, errMsg: r.Error, exitCode: r.ExitCode, slice: r.Slice,
			IdempotencyKey: r.IdempotencyKey, RetryOf: r.RetryOf, nextEvent: r.NextEvent, transitionSeq: r.TransitionSeq,
			stage: r.Stage, progress: r.Progress, queuePosition: 0, stageHistory: m.stages, timeoutWarning: r.TimeoutWarning,
			stageDurations: r.StageDurations, changed: make(chan struct{}), transitions: transitions}
		if j.stageDurations == nil {
			j.stageDurations = map[string]float64{}
		}
		if f, err := os.Open(filepath.Join(dir, "events.jsonl")); err == nil {
			scanJobJournal(f, func(line string) {
				var e Event
				if json.Unmarshal([]byte(line), &e) != nil {
					return
				}
				if e.ID > j.nextEvent {
					j.nextEvent = e.ID
				}
				j.events = append(j.events, e)
				j.eventBytes += len(e.Line)
				for len(j.events) > maxEvents || j.eventBytes > maxEventBytes {
					j.eventBytes -= len(j.events[0].Line)
					j.events = j.events[1:]
				}
			})
			f.Close()
		}
		m.jobs[j.ID] = j
		if j.state == StateQueued {
			ctx, cancel := context.WithCancel(context.Background())
			j.cancel = cancel
			m.pending = append(m.pending, &queuedJob{job: j, ctx: ctx, cancel: cancel})
		} else if !j.state.Finished() {
			j.setState(StateInterrupted, "server restarted before completion")
		}
		if j.IdempotencyKey != "" {
			e := &idempotencyEntry{done: make(chan struct{}), job: j, settled: true}
			close(e.done)
			m.idempotency[j.IdempotencyKey] = e
		}
		if j.state == StateDone && !j.Request.Orient && j.Request.Cut == "" {
			path := j.OutputPath(j.Request.Output)
			if j.Request.Explode {
				path = filepath.Join(j.ExplodedDir(), "plate.json")
			} else if j.Request.MeshOp != "" {
				path = j.OutputPath(meshExt(j.Request))
			} else if j.Request.Arrange || j.Request.WipeTower || overlayOutput(j.Request.Output) {
				path = j.OutputPath("json")
			}
			if _, err := os.Stat(path); err != nil {
				j.setState(StateStale, "persisted output is unavailable")
			}
		}
		if j.state == StateDone {
			m.stages.observe(j.stageDurations)
		}
	}
	return nil
}

type stageHistory struct {
	mu        sync.Mutex
	durations map[string]float64
}

func (h *stageHistory) observe(samples map[string]float64) {
	if h == nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for stage, seconds := range samples {
		if seconds <= 0 {
			continue
		}
		if prior := h.durations[stage]; prior > 0 {
			seconds = prior*.75 + seconds*.25
		}
		h.durations[stage] = seconds
	}
}

func (h *stageHistory) duration(stage string) float64 {
	if h == nil {
		return 0
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.durations[stage]
}

func (j *Job) progressLocked(e *Event) {
	if j.stageDurations == nil {
		j.stageDurations = map[string]float64{}
	}
	now := time.Now()
	if j.stage != e.Stage {
		if !j.stageStarted.IsZero() {
			j.stageDurations[j.stage] = now.Sub(j.stageStarted).Seconds()
		}
		j.stageStarted = now
	}
	elapsed := now.Sub(j.stageStarted).Seconds()
	remaining := math.Max(0, j.stageHistory.duration(e.Stage)-elapsed)
	if remaining == 0 && *e.Pct > 0 && *e.Pct < 100 {
		remaining = elapsed * (100 - *e.Pct) / *e.Pct
	}
	j.stage, j.progress, j.eta = e.Stage, e.Pct, &remaining
	e.ETA = &remaining
}

func scanJobJournal(r io.Reader, fn func(string)) {
	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 64<<10), 64<<20)
	for s.Scan() {
		fn(s.Text())
	}
	if err := s.Err(); err != nil {
		log.Printf("job journal: %v", err)
	}
}

func repairJobJournal(name string) error {
	f, err := os.OpenFile(name, os.O_RDWR, 0600)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || st.Size() == 0 {
		return err
	}
	var last [1]byte
	if _, err := f.ReadAt(last[:], st.Size()-1); err != nil {
		return err
	}
	if last[0] == '\n' {
		return nil
	}
	buf := make([]byte, 4096)
	for end := st.Size(); end > 0; {
		start := end - int64(len(buf))
		if start < 0 {
			start = 0
		}
		n, err := f.ReadAt(buf[:end-start], start)
		if err != nil {
			return err
		}
		if at := bytes.LastIndexByte(buf[:n], '\n'); at >= 0 {
			return f.Truncate(start + int64(at) + 1)
		}
		end = start
	}
	return f.Truncate(0)
}
