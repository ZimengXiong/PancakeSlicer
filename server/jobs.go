package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

type JobRequest struct {
	Library                *resolveRequest  `json:"library,omitempty"`
	SubmittedFilaments     []string         `json:"submitted_filaments,omitempty"`
	Submission             *submittedConfig `json:"submission,omitempty"`
	SourcePlates           []projectPlate   `json:"source_plates,omitempty"`
	PlateIndex             int              `json:"plate_index,omitempty"`
	Orient                 bool             `json:"orient,omitempty"`
	OrientScores           bool             `json:"orient_scores,omitempty"`
	Arrange                bool             `json:"arrange,omitempty"`
	ArrangeSpacing         float64          `json:"arrange_spacing,omitempty"`
	ArrangeNoRotation      bool             `json:"arrange_no_rotation,omitempty"`
	ArrangeMaterials       string           `json:"arrange_materials,omitempty"`
	WipeTower              bool             `json:"wipe_tower,omitempty"`
	Explode                bool             `json:"explode,omitempty"`
	AllowMixedTemperatures bool             `json:"allow_mixed_temperatures,omitempty"`
	Cut                    string           `json:"cut,omitempty"`
	CutKeep                string           `json:"cut_keep,omitempty"`
	CutPlaceLower          bool             `json:"cut_place_lower,omitempty"`
	CutConnectors          string           `json:"cut_connectors,omitempty"`
	CutDovetail            string           `json:"cut_dovetail,omitempty"`
	MeshOp                 string           `json:"mesh_op,omitempty"`
	SimplifyRatio          float64          `json:"simplify_ratio,omitempty"`
	SimplifyTriangles      int              `json:"simplify_triangles,omitempty"`
	SimplifyMaxError       float64          `json:"simplify_max_error,omitempty"`
	BooleanOp              string           `json:"boolean_op,omitempty"`
	Text                   []string         `json:"text,omitempty"`
	TextSurface            []string         `json:"text_surface,omitempty"`
	ExportFormat           string           `json:"export_format,omitempty"`
	DracoBits              int              `json:"draco_bits,omitempty"`
	Vendor                 string           `json:"vendor"`
	Printer                string           `json:"printer"`
	Process                string           `json:"process"`
	Filaments              []string         `json:"filaments"`
	Overrides              [][2]string      `json:"overrides"`
	Events                 []string         `json:"events"`
	Files                  []string         `json:"files"`
	Plate                  bool             `json:"plate"`
	Output                 string           `json:"output"`
	Filename               string           `json:"filename,omitempty"`
	ProjectSettings        [][2]string      `json:"project_settings,omitempty"`
	ForcePlanar            bool             `json:"force_planar,omitempty"`
	Floating               bool             `json:"floating,omitempty"`
	Calib                  string           `json:"calib,omitempty"`
	CalibParams            string           `json:"calib_params,omitempty"`
	CalibHead              string           `json:"calib_head,omitempty"`
	Edits                  bool             `json:"edits,omitempty"`
	Priority               string           `json:"priority,omitempty"`
	Session                string           `json:"session,omitempty"`
	SliceGroup             string           `json:"slice_group,omitempty"`
	PreviewBudget          float64          `json:"preview_budget,omitempty"`
}

type Event struct {
	Seq            uint64   `json:"seq"`
	QueuePosition  *int     `json:"queue_position,omitempty"`
	ETA            *float64 `json:"eta_seconds,omitempty"`
	ReplayGap      bool     `json:"replay_gap,omitempty"`
	ID             int      `json:"id"`
	Type           string   `json:"type"`
	Stream         string   `json:"stream,omitempty"`
	Line           string   `json:"line,omitempty"`
	State          JobState `json:"state,omitempty"`
	Error          string   `json:"error,omitempty"`
	Stage          string   `json:"stage,omitempty"`
	Pct            *float64 `json:"pct,omitempty"`
	TimeoutWarning bool     `json:"timeout_warning,omitempty"`
}

const maxEvents = 20000
const maxEventBytes = 8 << 20

type Job struct {
	ID             string
	Client         string
	Dir            string
	Request        JobRequest
	IdempotencyKey string
	RetryOf        string
	Created        time.Time

	nextEvent      int
	queuePosition  int
	eta            *float64
	stageStarted   time.Time
	stageDurations map[string]float64
	stageHistory   *stageHistory
	persistenceErr error

	clearanceOnce sync.Once
	clearance     clearanceVerdict

	mu             sync.Mutex
	transitions    []JobTransition
	transitionSeq  uint64
	state          JobState
	started        time.Time
	finished       time.Time
	errMsg         string
	exitCode       *int
	slice          json.RawMessage
	events         []Event
	dropped        int
	eventBytes     int
	changed        chan struct{}
	cancel         context.CancelFunc
	stopReason     string
	stage          string
	progress       *float64
	send           *Transfer
	resultReady    bool
	timeoutWarning bool
}

type JobView struct {
	ID             string          `json:"id"`
	Stage          string          `json:"stage,omitempty"`
	Progress       *float64        `json:"progress,omitempty"`
	State          JobState        `json:"state"`
	Created        time.Time       `json:"created"`
	Started        *time.Time      `json:"started,omitempty"`
	Finished       *time.Time      `json:"finished,omitempty"`
	Error          string          `json:"error,omitempty"`
	ExitCode       *int            `json:"exit_code,omitempty"`
	Request        JobRequest      `json:"request"`
	DownloadURL    string          `json:"download_url,omitempty"`
	EventsURL      string          `json:"events_url"`
	Slice          json.RawMessage `json:"record,omitempty"`
	Filename       string          `json:"filename,omitempty"`
	RetryOf        string          `json:"retry_of,omitempty"`
	QueuePosition  int             `json:"queue_position"`
	ETA            *float64        `json:"eta_seconds,omitempty"`
	ResultReady    bool            `json:"result_ready,omitempty"`
	HasSurfaceGrid bool            `json:"has_surface_grid"`
	TimeoutWarning bool            `json:"timeout_warning,omitempty"`
}

func (j *Job) View() JobView {
	v := j.snapshotView()
	var record map[string]json.RawMessage
	if bytes.Contains(v.Slice, []byte(`"surface_grid"`)) && json.Unmarshal(v.Slice, &record) == nil && record["surface_grid"] != nil {
		delete(record, "surface_grid")
		v.Slice, _ = json.Marshal(record)
		v.HasSurfaceGrid = true
	}
	return v
}

func (j *Job) snapshotView() JobView {
	j.mu.Lock()
	defer j.mu.Unlock()
	v := JobView{ID: j.ID, RetryOf: j.RetryOf, State: j.state, Created: j.Created, Error: j.errMsg, ExitCode: j.exitCode,
		Stage: j.stage, Progress: j.progress, Request: j.Request, QueuePosition: j.queuePosition, ETA: j.eta, EventsURL: "/api/jobs/" + j.ID + "/events", Slice: j.slice,
		ResultReady: j.resultReady && !j.state.Finished(), TimeoutWarning: j.timeoutWarning}
	if !j.started.IsZero() {
		t := j.started
		v.Started = &t
	}
	if !j.finished.IsZero() {
		t := j.finished
		v.Finished = &t
	}
	if j.state == StateDone && !j.Request.Orient && !overlayOutput(j.Request.Output) {
		v.DownloadURL = "/api/jobs/" + j.ID + "/download"
		v.Filename = j.DownloadName()
	}
	return v
}

func (j *Job) Record() json.RawMessage {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.slice
}

func (j *Job) State() JobState {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.state
}

func (j *Job) Previewable() bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.state == StateDone || j.resultReady && !j.state.Finished()
}

func (j *Job) AwaitFinished(ctx context.Context) JobState {
	for {
		j.mu.Lock()
		state, ready, changed := j.state, j.resultReady, j.changed
		j.mu.Unlock()
		if state.Finished() || !ready {
			return state
		}
		select {
		case <-changed:
		case <-ctx.Done():
			return state
		}
	}
}

func (j *Job) notifyLocked() {
	close(j.changed)
	j.changed = make(chan struct{})
}

func (j *Job) appendLocked(e Event) {
	j.nextEvent++
	e.ID, e.Seq = j.nextEvent, uint64(j.nextEvent)
	if len(e.Line) > maxEventBytes {
		e.Line = e.Line[:maxEventBytes]
	}
	j.eventBytes += len(e.Line)
	j.events = append(j.events, e)
	trimmed := false
	for len(j.events) > maxEvents || j.eventBytes > maxEventBytes {
		j.eventBytes -= len(j.events[0].Line)
		j.events = j.events[1:]
		j.dropped++
		trimmed = true
	}
	j.writeEventLocked(e, trimmed && j.nextEvent%1024 == 0)
	j.notifyLocked()
}

func (j *Job) log(stream, line string) {
	j.mu.Lock()
	defer j.mu.Unlock()
	e := Event{Type: "log", Stream: stream, Line: line}
	fields := strings.Fields(line)
	if (stream == "stdout" || stream == "server") && len(fields) == 3 && fields[0] == "progress" && len(fields[1]) <= 64 {
		if pct, err := strconv.ParseFloat(fields[2], 64); err == nil && pct >= 0 && pct <= 100 && !math.IsNaN(pct) && !math.IsInf(pct, 0) {
			e.Type = "progress"
			e.Stage = fields[1]
			e.Pct = &pct
			j.progressLocked(&e)
		}
	}
	j.appendLocked(e)
	if e.Type == "progress" {
		j.persistLocked()
	}
}

func (j *Job) EventsSince(after int) ([]Event, <-chan struct{}, bool) {
	j.mu.Lock()
	defer j.mu.Unlock()
	var out []Event
	if len(j.events) > 0 && after < j.events[0].ID-1 {
		id := j.events[0].ID - 1
		pos := j.queuePosition
		out = append(out, Event{ID: id, Seq: uint64(id), Type: "reset", ReplayGap: true,
			State: j.state, Stage: j.stage, Pct: j.progress,
			ETA: j.eta, QueuePosition: &pos})
	}
	for _, e := range j.events {
		if e.ID > after {
			out = append(out, e)
		}
	}
	return out, j.changed, j.state.Finished()
}

func (j *Job) OutputPath(kind string) string {
	return filepath.Join(j.Dir, "out", outputBase(j.Request)+"."+kind)
}

func validPiece(side string) bool {
	if side == "upper" || side == "lower" {
		return true
	}
	n, err := strconv.Atoi(strings.TrimPrefix(side, "dowel-"))
	return strings.HasPrefix(side, "dowel-") && err == nil && n > 0 && strconv.Itoa(n) == side[len("dowel-"):]
}

func (j *Job) PiecePath(side string) string {
	return filepath.Join(j.Dir, "out", outputBase(j.Request)+"."+side+".stl")
}

func (j *Job) MovesPath() string { return filepath.Join(j.Dir, "moves.bin") }

func (j *Job) ExplodedDir() string { return filepath.Join(j.Dir, "exploded") }

func (j *Job) ExtendedMovesPath() string { return filepath.Join(j.Dir, "moves-extended.bin") }

func (j *Job) DownloadName() string {
	name := j.Request.Filename
	if name == "" && len(j.Request.Files) > 0 {
		name = j.Request.Files[0]
	}
	for _, ext := range []string{".gcode.3mf", ".gcode", ".3mf", ".stl", ".obj"} {
		if strings.HasSuffix(strings.ToLower(name), ext) {
			name = name[:len(name)-len(ext)]
			break
		}
	}
	if name == "" {
		name = "plate_1"
	}
	return name + "." + j.Request.Output
}

func meshExt(r JobRequest) string {
	switch {
	case r.MeshOp == "export" && r.ExportFormat == "drc":
		return "drc"
	case r.MeshOp == "export":
		return "3mf"
	}
	return "stl"
}

func outputBase(r JobRequest) string {
	if r.Plate || len(r.Files) == 0 {
		return "plate_1"
	}
	name := r.Files[0]
	for _, ext := range []string{".3mf", ".stl", ".obj", ".step", ".stp", ".amf"} {
		if strings.HasSuffix(strings.ToLower(name), ext) {
			return name[:len(name)-len(ext)]
		}
	}
	return name
}

type JobManager struct {
	served        map[string]uint64
	serveSeq      uint64
	cfg           Config
	pending       []*queuedJob
	active        int
	stages        *stageHistory
	wg            sync.WaitGroup
	mu            sync.Mutex
	jobs          map[string]*Job
	idempotency   map[string]*idempotencyEntry
	stop          chan struct{}
	reaped        chan struct{}
	closing       bool
	uploads       map[string]*Job
	buckets       map[string]tokenBucket
	durations     [8]uint64
	durationCount uint64
	durationSum   float64
	submitted     uint64
	completedJobs [3]uint64
	rejects       [200]uint64
	responseBytes uint64
	downloadBytes uint64
}

var jobIDPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

func NewJobManager(cfg Config) (*JobManager, error) {
	if err := os.MkdirAll(cfg.JobRoot, 0o755); err != nil {
		return nil, err
	}
	if cfg.MaxJobs < 1 {
		cfg.MaxJobs = 1
	}
	if cfg.MaxQueue < 0 {
		cfg.MaxQueue = 64
	}
	m := &JobManager{cfg: cfg, jobs: map[string]*Job{}, idempotency: map[string]*idempotencyEntry{}, stop: make(chan struct{}), reaped: make(chan struct{}), uploads: map[string]*Job{}, buckets: map[string]tokenBucket{}, stages: &stageHistory{durations: map[string]float64{}}}
	if err := m.restoreJobs(); err != nil {
		return nil, err
	}
	m.mu.Lock()
	m.dispatchLocked()
	m.mu.Unlock()
	go m.reap()
	if jobResultWarm != nil {
		go jobResultWarm(m)
	}
	return m, nil
}

func (m *JobManager) NewJob(client string) (*Job, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.newJobLocked(client)
}

func (m *JobManager) newJobLocked(client string) (*Job, error) {
	if err := m.admitLocked(client); err != nil {
		return nil, err
	}

	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return nil, err
	}
	id := hex.EncodeToString(b[:])
	dir := filepath.Join(m.cfg.JobRoot, id)
	for _, sub := range []string{"input", "out", "datadir", "tmp"} {
		if err := os.MkdirAll(filepath.Join(dir, sub), 0o755); err != nil {
			return nil, errors.Join(err, os.RemoveAll(dir))
		}
	}
	j := &Job{ID: id, Client: client, Dir: dir, Created: time.Now(), state: StateIdle, changed: make(chan struct{}), stageHistory: m.stages, stageDurations: map[string]float64{}}
	m.uploads[id] = j
	return j, nil
}

func (m *JobManager) Start(j *Job) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.jobs[j.ID] == j {
		return nil
	}
	if m.closing {
		return &admissionError{503, "service is shutting down", 1}
	}
	if err := m.quotaLocked(); err != nil {
		return err
	}
	ctx, cancel := context.WithCancel(context.Background())
	j.mu.Lock()
	j.cancel = cancel
	j.queuePosition = 0
	j.setStateLocked(StateQueued, "")
	err := j.persistenceErr
	j.mu.Unlock()
	if err != nil {
		cancel()
		return fmt.Errorf("persist job: %w", err)
	}
	delete(m.uploads, j.ID)
	m.jobs[j.ID] = j
	m.pending = append(m.pending, &queuedJob{job: j, ctx: ctx, cancel: cancel})
	if m.cfg.Metrics {
		m.submitted++
	}
	var superseded []*Job
	if g := j.Request.SliceGroup; g != "" && !batchJob(j) {
		for _, o := range m.jobs {
			if o != j && o.Request.SliceGroup == g && o.Request.Session == j.Request.Session && !batchJob(o) {
				if _, queued := m.cancelLocked(o, "superseded"); queued {
					superseded = append(superseded, o)
				}
			}
		}
	}
	m.dispatchLocked()
	if len(superseded) > 0 {
		go func() {
			for _, o := range superseded {
				m.completed(o)
			}
		}()
	}
	return nil
}

func (m *JobManager) Get(id string) *Job {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.jobs[id]
}

func (m *JobManager) Cancel(j *Job) bool {
	m.mu.Lock()
	ok, queued := m.cancelLocked(j, "cancelled")
	if queued {
		m.dispatchLocked()
	}
	m.mu.Unlock()
	if queued {
		m.completed(j)
	}
	return ok
}

func (m *JobManager) cancelLocked(j *Job, reason string) (ok, queued bool) {
	j.mu.Lock()
	if j.state.Finished() || j.state == StateCanceling {
		j.mu.Unlock()
		return false, false
	}
	queued = j.state == StateQueued
	j.stopReason = reason
	j.setStateLocked(StateCanceling, j.stopReason)
	if j.cancel != nil {
		j.cancel()
	}
	if queued {
		j.setStateLocked(StateCancelled, j.stopReason)
	}
	j.mu.Unlock()
	if queued {
		for i, q := range m.pending {
			if q.job == j {
				m.pending = append(m.pending[:i], m.pending[i+1:]...)
				break
			}
		}
	}
	return true, queued
}

func (m *JobManager) Remove(j *Job) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.removeLocked(j)
}

func (m *JobManager) removeLocked(j *Job) error {
	if err := os.RemoveAll(j.Dir); err != nil {
		return err
	}
	delete(m.jobs, j.ID)
	if e := m.idempotency[j.IdempotencyKey]; e != nil && e.job == j {
		delete(m.idempotency, j.IdempotencyKey)
	}
	return nil
}

func (m *JobManager) Counts() (queued, running int) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, j := range m.jobs {
		switch j.State() {
		case StateQueued:
			queued++
		case StatePreparing, StateSlicing, StateStoring, StateCanceling:
			running++
		}
	}
	return
}

func (m *JobManager) Shutdown() {
	m.mu.Lock()
	if m.closing {
		m.mu.Unlock()
		m.wg.Wait()
		return
	}
	m.closing = true
	close(m.stop)
	for _, j := range m.jobs {
		j.mu.Lock()
		if !j.state.Finished() && j.state != StateQueued {
			j.stopReason = "server shutting down"
			if j.cancel != nil {
				j.cancel()
			}
		}
		j.mu.Unlock()
	}
	m.pending = nil
	m.mu.Unlock()
	<-m.reaped
	m.wg.Wait()
}

func (m *JobManager) Cleanup() {}

func (m *JobManager) reap() {
	defer close(m.reaped)
	interval := time.Minute
	if m.cfg.DiskQuota > 0 {
		interval = 5 * time.Second
	}
	t := time.NewTicker(interval)
	defer t.Stop()
	for {
		select {
		case <-m.stop:
			return
		case now := <-t.C:
			m.mu.Lock()
			if err := m.quotaLocked(); err != nil {
				log.Printf("job quota: %v", err)
			}
			for client, bucket := range m.buckets {
				if now.Sub(bucket.at).Seconds()*m.cfg.SubmitRate >= float64(m.cfg.SubmitBurst) {
					delete(m.buckets, client)
				}
			}
			m.mu.Unlock()
			var old []*Job
			m.mu.Lock()
			for _, j := range m.jobs {
				j.mu.Lock()
				if m.cfg.JobTTL > 0 && j.state.Finished() && now.Sub(j.finished) > m.cfg.JobTTL {
					old = append(old, j)
				}
				j.mu.Unlock()
			}
			m.mu.Unlock()
			for _, j := range old {
				if err := m.Remove(j); err != nil {
					log.Printf("expire job %s: %v", j.ID, err)
				}
			}
		}
	}
}

func (m *JobManager) toolArgs(j *Job) []string {
	r := j.Request
	in := filepath.Join(j.Dir, "input")
	if r.Explode {
		return []string{"--explode-3mf", filepath.Join(in, r.Files[0]), j.ExplodedDir()}
	}
	if r.MeshOp == "export" {
		if r.ExportFormat == "drc" {
			args := []string{"--export-drc", filepath.Join(in, r.Files[0]), j.OutputPath(meshExt(r))}
			if r.DracoBits > 0 {
				args = append(args, "--bits", strconv.Itoa(r.DracoBits))
			}
			return args
		}
		return []string{"--export-3mf-generic", filepath.Join(in, r.Files[0]), j.OutputPath(meshExt(r))}
	}
	if r.MeshOp == "boolean" {
		return []string{"--mesh-boolean", r.BooleanOp, "--a", filepath.Join(in, r.Files[0]), "--b", filepath.Join(in, r.Files[1]), j.OutputPath("stl")}
	}
	if r.MeshOp == "svg" {
		args := append([]string{"--emboss-svg", filepath.Join(in, r.Files[0]), j.OutputPath("stl")}, r.Text...)
		if len(r.TextSurface) > 0 {
			args = append(append(args, "--use-surface", "--surface", filepath.Join(in, r.Files[1])), r.TextSurface...)
		}
		return args
	}
	if r.MeshOp == "text" {
		args := append([]string{"--emboss-text", j.OutputPath("stl"), "--profiles", filepath.Join(m.cfg.Resources, "profiles")}, r.Text...)
		if len(r.TextSurface) > 0 {
			args = append(append(args, "--use-surface", "--surface", filepath.Join(in, r.Files[0])), r.TextSurface...)
		}
		return args
	}
	if r.MeshOp != "" {
		args := []string{"--" + r.MeshOp + "-mesh", filepath.Join(in, r.Files[0]), j.OutputPath("stl")}
		if r.SimplifyMaxError > 0 {
			return append(args, "--max-error", strconv.FormatFloat(r.SimplifyMaxError, 'f', -1, 64))
		}
		if r.SimplifyTriangles > 0 {
			return append(args, "--triangles", strconv.Itoa(r.SimplifyTriangles))
		}
		if r.MeshOp == "simplify" {
			return append(args, "--ratio", strconv.FormatFloat(r.SimplifyRatio, 'f', -1, 64))
		}
		return args
	}
	args := []string{
		"--profiles", filepath.Join(m.cfg.Resources, "profiles"),
		"--datadir", filepath.Join(j.Dir, "datadir"),
		"--vendor", r.Vendor,
		"--printer", r.Printer,
		"--process", r.Process,
	}
	if dir := presetCacheDir(m); dir != "" {
		args = append(args, "--preset-cache", dir)
	}
	args = append(args, stageCacheArgs(m)...)
	for _, f := range r.Filaments {
		args = append(args, "--filament", f)
	}
	for _, kv := range r.Overrides {
		args = append(args, "--set", kv[0]+"="+kv[1])
	}
	for _, e := range r.Events {
		args = append(args, "--event", e)
	}
	if r.Orient {
		flag := "--orient-only"
		if r.OrientScores {
			flag = "--orient-scores"
		}
		return append(args, flag, filepath.Join(in, r.Files[0]))
	}
	if r.Cut != "" {
		args = append(args, "--cut-only", r.Cut, "--cut-keep", r.CutKeep, "-o", j.OutputPath("stl"))
		if r.CutPlaceLower {
			args = append(args, "--cut-place-lower")
		}
		if r.CutConnectors != "" {
			args = append(args, "--cut-connectors", r.CutConnectors)
		}
		if r.CutDovetail != "" {
			args = append(args, "--cut-mode", "dovetail", "--cut-dovetail", r.CutDovetail)
		}
		return append(args, filepath.Join(in, r.Files[0]))
	}
	if r.Arrange {
		args = append(args, "--arrange-only", j.OutputPath("json"))
		if r.ArrangeSpacing > 0 {
			args = append(args, "--arrange-spacing", strconv.FormatFloat(r.ArrangeSpacing, 'f', -1, 64))
		}
		if r.ArrangeNoRotation {
			args = append(args, "--arrange-no-rotation")
		}
		if r.ArrangeMaterials != "" {
			args = append(args, "--arrange-multi-materials", r.ArrangeMaterials)
		}
		return append(args, "--plate", filepath.Join(in, "plate.json"))
	}
	if r.WipeTower {
		return append(args, "--wipe-tower-only", j.OutputPath("json"), "--plate", filepath.Join(in, "plate.json"))
	}
	if r.Output == "plan-tops" {
		args = append(args, "--plan-tops", j.OutputPath("json"))
		if r.Plate {
			return append(args, "--plate", filepath.Join(in, "plate.json"))
		}
		return append(args, filepath.Join(in, r.Files[0]))
	}
	if r.Output == "patch-preview" {
		args = append(args, "--patch-preview", j.OutputPath("json"), "--patch-preview-budget", strconv.FormatFloat(r.PreviewBudget, 'f', -1, 64))
		if r.Plate {
			return append(args, "--plate", filepath.Join(in, "plate.json"))
		}
		return append(args, filepath.Join(in, r.Files[0]))
	}
	if m.cfg.Features.MovesOut {
		if m.cfg.Features.MovesExtended {
			args = append(args, "--moves-out", j.ExtendedMovesPath(), "--moves-extended")
		} else {
			args = append(args, "--moves-out", j.MovesPath())
		}
	}
	if m.cfg.Features.Progress {
		args = append(args, "--progress")
	}
	if r.Output == "3mf" {
		args = append(args, "--save-3mf", j.OutputPath("3mf"))
	} else {
		args = append(args, "--json", j.OutputPath("json"), "-o", j.OutputPath("gcode"))
		if m.cfg.Features.SurfaceGrid {
			args = append(args, "--surface-grid")
		}
	}
	if r.Output == "gcode.3mf" {
		args = append(args, "--gcode-3mf", j.OutputPath("gcode.3mf"))
	}
	if r.PlateIndex > 0 && r.Output != "3mf" {
		args = append(args, "--plate-index", fmt.Sprint(r.PlateIndex))
	}
	if r.ForcePlanar {
		args = append(args, "--force-planar")
	}
	if r.Floating {
		args = append(args, "--floating")
	}
	if r.AllowMixedTemperatures {
		args = append(args, "--allow-mixed-temperatures")
	}
	if r.Edits {
		args = append(args, "--edit", filepath.Join(in, "edits.json"))
	}
	if r.Calib != "" {
		args = append(args, "--calib", r.Calib)
		if r.CalibParams != "" {
			args = append(args, "--calib-params", r.CalibParams)
		}
		if r.CalibHead != "" {
			args = append(args, "--calib-head", r.CalibHead)
		}
	} else if r.Plate {
		args = append(args, "--plate", filepath.Join(in, "plate.json"))
	} else {
		args = append(args, filepath.Join(in, r.Files[0]))
	}
	return args
}

func (m *JobManager) run(ctx context.Context, j *Job) {
	cancel := func() {}
	if m.cfg.JobTimeout > 0 {
		ctx, cancel = context.WithTimeout(ctx, m.cfg.JobTimeout)
	}
	defer cancel()
	warningTimer := time.NewTimer(m.cfg.JobWarningTimeout)
	warningDone := make(chan struct{})
	defer func() {
		close(warningDone)
		if !warningTimer.Stop() {
			select {
			case <-warningTimer.C:
			default:
			}
		}
	}()
	go func() {
		select {
		case <-warningTimer.C:
			j.mu.Lock()
			if !j.state.Finished() {
				j.timeoutWarning = true
				j.appendLocked(Event{Type: "timeout_warning", State: j.state, Error: fmt.Sprintf("slicing has exceeded the %s warning threshold", m.cfg.JobWarningTimeout), TimeoutWarning: true})
				j.persistLocked()
			}
			j.mu.Unlock()
		case <-warningDone:
		}
	}()
	if ctx.Err() != nil {
		j.setState(StateInterrupted, "worker interrupted before preparation")
		return
	}
	j.log("server", "progress preparing 0")
	args := m.toolArgs(j)
	j.log("server", "nonplanar_slice_tool "+quoteArgs(args))
	if jobResultGet != nil && jobResultGet(m, j, args) {
		return
	}
	if overlayOutput(j.Request.Output) && m.previewCached(j, args) {
		return
	}

	if err := copyUserHeads(m.cfg, filepath.Join(j.Dir, "datadir")); err != nil {
		j.setState(StateFailed, "cannot copy the user heads: "+err.Error())
		return
	}
	cmd, commandErr := lineBufferedCommand(ctx, m.cfg.Tool, args)
	if commandErr != nil {
		j.setState(StateFailed, sanitizeToolError(commandErr))
		return
	}
	cmd.Dir = j.Dir
	cmd.Env = append(os.Environ(), "TMPDIR="+filepath.Join(j.Dir, "tmp"), "TMP="+filepath.Join(j.Dir, "tmp"), "TEMP="+filepath.Join(j.Dir, "tmp"))
	if err := limitJobMemory(cmd); err != nil {
		j.setState(StateFailed, err.Error())
		return
	}
	configureJobProcess(cmd)
	cmd.WaitDelay = 5 * time.Second
	stdout, stdoutWriter := io.Pipe()
	stderr, stderrWriter := io.Pipe()
	defer stdout.Close()
	defer stderr.Close()
	defer stdoutWriter.Close()
	defer stderrWriter.Close()
	cmd.Stdout = stdoutWriter
	cmd.Stderr = stderrWriter
	if !j.setState(StateSlicing, "") {
		return
	}
	if !m.cfg.Features.Progress {
		j.log("server", "progress slicing 0")
	}
	if err := cmd.Start(); err != nil {
		switch {
		case errors.Is(ctx.Err(), context.Canceled):
			j.setState(StateCancelled, "cancelled before the slicer started")
		case errors.Is(ctx.Err(), context.DeadlineExceeded):
			j.setState(StateFailed, "slicing exceeded the time limit")
		default:
			j.setState(StateFailed, "cannot start the slicer: "+sanitizeToolError(err))
		}
		return
	}
	var rotation []float64
	var scores []map[string]any
	var pieces []map[string]any
	var meshRecord []byte
	var lastErr lastLines
	var readers, published sync.WaitGroup
	readers.Add(2)
	go func() {
		defer readers.Done()
		scanLines(stdout, func(l string) {
			j.log("stdout", l)
			if l == "progress json 100" && previewableRequest(j.Request) {
				published.Add(1)
				go func() {
					defer published.Done()
					m.publishResult(j)
				}()
			}
			if j.Request.MeshOp != "" && strings.HasPrefix(l, "{") && json.Valid([]byte(l)) {
				meshRecord = []byte(l)
			}
			if j.Request.OrientScores {
				if score := orientScoreLine(l); score != nil {
					scores = append(scores, score)
				}
			}
			if j.Request.Orient && strings.HasPrefix(l, "auto_orient_rotation ") {
				fields := strings.Fields(l)[1:]
				if len(fields) == 9 {
					values := make([]float64, 9)
					for i, field := range fields {
						value, err := strconv.ParseFloat(field, 64)
						if err != nil || math.IsNaN(value) || math.IsInf(value, 0) {
							return
						}
						values[i] = value
					}
					rotation = values
				}
			}
			if f := strings.Fields(l); j.Request.Cut != "" && len(f) == 8 && f[0] == "cut_piece" && f[2] == "triangles" && f[4] == "open_edges" && f[6] == "volume_mm3" {
				triangles, e1 := strconv.Atoi(f[3])
				open, e2 := strconv.Atoi(f[5])
				volume, e3 := strconv.ParseFloat(f[7], 64)
				if e1 == nil && e2 == nil && e3 == nil && validPiece(f[1]) && !math.IsNaN(volume) && !math.IsInf(volume, 0) {
					pieces = append(pieces, map[string]any{"side": f[1], "triangles": triangles, "open_edges": open, "volume_mm3": volume})
				}
			}
		})
	}()
	go func() {
		defer readers.Done()
		scanLines(stderr, func(l string) { lastErr.add(l); j.log("stderr", l) })
	}()
	waitErr := cmd.Wait()
	if cmd.Cancel != nil {
		cmd.Cancel()
	}
	stdoutWriter.Close()
	stderrWriter.Close()
	readers.Wait()
	published.Wait()

	code := cmd.ProcessState.ExitCode()
	j.mu.Lock()
	j.exitCode = &code
	j.mu.Unlock()
	raw, recordErr := os.ReadFile(j.OutputPath("json"))
	if j.Request.Explode {
		raw, recordErr = os.ReadFile(filepath.Join(j.ExplodedDir(), "plate.json"))
	} else if j.Request.OrientScores {
		raw, recordErr = json.Marshal(map[string]any{"schema": "orca.orient-scores.v1", "scores": scores})
		if len(scores) == 0 {
			recordErr = errors.New("missing native orientation scores")
		}
	} else if j.Request.Orient {
		if len(rotation) == 9 {
			raw, recordErr = json.Marshal(map[string]any{"rotation": rotation})
		} else {
			recordErr = errors.New("missing native orientation rotation")
		}
	}
	if j.Request.MeshOp != "" {
		raw, recordErr = meshRecord, nil
		if meshRecord == nil {
			recordErr = errors.New("missing mesh operation report")
		}
	}
	if j.Request.Cut != "" {
		raw, recordErr = json.Marshal(map[string]any{"pieces": pieces})
		for _, p := range pieces {
			if _, err := os.Stat(j.PiecePath(p["side"].(string))); err != nil {
				recordErr = errors.New("missing cut piece " + p["side"].(string))
			}
		}
		if len(pieces) == 0 {
			recordErr = errors.New("missing cut pieces")
		}
	}
	if !j.Request.Orient && j.Request.Cut == "" && j.Request.Output == "3mf" {
		recordErr = nil
		raw = nil
	}
	if recordErr == nil && j.Request.Output != "3mf" && !json.Valid(raw) {
		recordErr = errors.New("invalid slice JSON")
	}
	if recordErr == nil {
		raw = bindNativeRecord(raw, m.cfg, j)
		j.mu.Lock()
		j.slice = raw
		j.mu.Unlock()
	}
	switch {
	case errors.Is(ctx.Err(), context.Canceled):
		j.setState(StateCancelled, "cancelled")
	case errors.Is(ctx.Err(), context.DeadlineExceeded):
		j.setState(StateFailed, fmt.Sprintf("slicing exceeded the %s time limit", m.cfg.JobTimeout))
	case waitErr != nil:
		j.setState(StateFailed, lastErr.message(waitErr))
	case recordErr != nil:
		j.setState(StateFailed, "the slicer output could not be read: "+sanitizeToolError(recordErr))
	default:
		kind := j.Request.Output
		if j.Request.Arrange || j.Request.WipeTower {
			kind = "json"
		}
		if j.Request.MeshOp != "" {
			kind = meshExt(j.Request)
		}
		if kind == "plan-tops" {
			mapped, err := m.mapPlanFacets(ctx, j, raw)
			if err != nil {
				j.setState(StateFailed, "the curved tops plan could not be read: "+sanitizeToolError(err))
				return
			}
			raw = mapped
			j.mu.Lock()
			j.slice = raw
			j.mu.Unlock()
		}
		if overlayOutput(kind) {
			kind = "json"
			m.keepPreview(j, args, raw)
		}
		if _, err := os.Stat(j.OutputPath(kind)); !j.Request.Orient && !j.Request.Explode && j.Request.Cut == "" && err != nil {
			j.setState(StateFailed, "the slicer finished without writing "+filepath.Base(j.OutputPath(kind)))
			return
		}
		if !j.setState(StateStoring, "") {
			return
		}
		j.mu.Lock()
		stripped := j.resultReady
		j.mu.Unlock()
		if _, err := os.Stat(j.ExtendedMovesPath()); err == nil && !stripped {
			if err := stripExtendedMoves(j.ExtendedMovesPath(), j.MovesPath()); err != nil {
				j.setState(StateFailed, "the preview stream could not be written: "+sanitizeToolError(err))
				return
			}
		}
		if !m.cfg.Features.Progress {
			j.log("server", "progress export 100")
		}
		if jobResultPut != nil && ctx.Err() == nil {
			jobResultPut(m, j, args, raw)
		}
		j.setState(StateDone, "")
	}
	log.Printf("job %s %s (exit %d)", j.ID, j.State(), code)
}

func overlayOutput(output string) bool { return output == "patch-preview" || output == "plan-tops" }

func previewableRequest(r JobRequest) bool {
	return (r.Output == "gcode" || r.Output == "gcode.3mf") && !r.Orient && !r.OrientScores && !r.Explode && r.Cut == "" &&
		r.MeshOp == "" && !r.Arrange && !r.WipeTower
}

func (m *JobManager) publishResult(j *Job) {
	raw, err := os.ReadFile(j.OutputPath("json"))
	if err != nil || !json.Valid(raw) {
		return
	}
	if _, err := os.Stat(j.ExtendedMovesPath()); err == nil {
		if stripExtendedMoves(j.ExtendedMovesPath(), j.MovesPath()) != nil {
			return
		}
	}
	raw = bindNativeRecord(raw, m.cfg, j)
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.state != StateSlicing {
		return
	}
	j.slice, j.resultReady = raw, true
	j.appendLocked(Event{Type: "result", State: j.state})
}

func scanLines(r io.Reader, fn func(string)) {
	s := bufio.NewScanner(r)
	s.Buffer(make([]byte, 64<<10), 4<<20)
	for s.Scan() {
		fn(s.Text())
	}
	io.Copy(io.Discard, r)
}

type lastLines struct{ lines []string }

func (l *lastLines) add(s string) {
	if strings.TrimSpace(s) == "" {
		return
	}
	l.lines = append(l.lines, s)
	if len(l.lines) > 20 {
		l.lines = l.lines[1:]
	}
}

func (l *lastLines) message(waitErr error) string {
	for i := len(l.lines) - 1; i >= 0; i-- {
		if strings.HasPrefix(strings.ToLower(strings.TrimSpace(l.lines[i])), "error") {
			return sanitizeToolError(errors.New(strings.TrimSpace(l.lines[i])))
		}
	}
	if len(l.lines) > 0 {
		return sanitizeToolError(errors.New(strings.TrimSpace(l.lines[len(l.lines)-1])))
	}
	return "slicer " + waitErr.Error()
}

func quoteArgs(args []string) string {
	q := make([]string, len(args))
	for i, a := range args {
		if a == "" || strings.ContainsAny(a, " \t\"'\\$`") {
			q[i] = "'" + strings.ReplaceAll(a, "'", `'\''`) + "'"
		} else {
			q[i] = a
		}
	}
	return strings.Join(q, " ")
}

func sanitizeToolError(err error) string {
	for {
		next := errors.Unwrap(err)
		if next == nil {
			break
		}
		err = next
	}
	return toolErrorPath.ReplaceAllStringFunc(err.Error(), func(token string) string {
		parts := toolErrorPath.FindStringSubmatch(token)
		return parts[1] + filepath.Base(strings.ReplaceAll(parts[2], "\\", "/"))
	})
}

var toolErrorPath = regexp.MustCompile(`(^|[\s"'‘(])((?:[A-Za-z]:[\\/]|/)[^\s"'’<>:]+)`)

func orientScoreLine(l string) map[string]any {
	if !strings.HasPrefix(l, "{") {
		return nil
	}
	var score map[string]any
	if json.Unmarshal([]byte(l), &score) != nil {
		return nil
	}
	if _, ok := score["original"].(bool); !ok {
		return nil
	}
	if axis, ok := score["axis"].([]any); !ok || len(axis) != 3 {
		return nil
	}
	if _, ok := score["angle_deg"].(float64); !ok {
		return nil
	}
	return score
}

type idempotencyEntry struct {
	done    chan struct{}
	job     *Job
	settled bool
}

func (m *JobManager) reserve(ctx context.Context, key string) (*Job, func(*Job), error) {
	if key == "" {
		return nil, func(*Job) {}, nil
	}
	for {
		m.mu.Lock()
		if e := m.idempotency[key]; e != nil {
			m.mu.Unlock()
			select {
			case <-ctx.Done():
				return nil, nil, ctx.Err()
			case <-e.done:
				if e.job != nil {
					return e.job, func(*Job) {}, nil
				}
				continue
			}
		}
		e := &idempotencyEntry{done: make(chan struct{})}
		m.idempotency[key] = e
		m.mu.Unlock()
		return nil, func(j *Job) {
			m.mu.Lock()
			defer m.mu.Unlock()
			if e.settled {
				log.Printf("idempotency key %q settled twice; keeping its first outcome", key)
				return
			}
			e.settled = true
			e.job = j
			if j == nil {
				delete(m.idempotency, key)
			}
			close(e.done)
		}, nil
	}
}

func submissionKey(endpoint, key string) (string, error) {
	if key == "" {
		return "", nil
	}
	if len(key) > 200 {
		return "", errors.New("Idempotency-Key exceeds 200 bytes")
	}
	for _, c := range key {
		if c < 33 || c > 126 {
			return "", errors.New("Idempotency-Key must contain visible ASCII")
		}
	}
	h := sha256.Sum256([]byte(endpoint + "\x00" + key))
	return hex.EncodeToString(h[:]), nil
}

func (m *JobManager) Retry(parent *Job, client string) (*Job, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.jobs[parent.ID] != parent {
		return nil, &admissionError{404, "job expired", 0}
	}
	if !parent.State().Finished() {
		return nil, &admissionError{409, "retry requires a finished job", 0}
	}
	required := append([]string{}, parent.Request.Files...)
	if parent.Request.Plate {
		required = append(required, "plate.json")
	}
	if parent.Request.Edits {
		required = append(required, "edits.json")
	}
	for _, name := range required {
		info, err := os.Stat(filepath.Join(parent.Dir, "input", name))
		if err != nil || !info.Mode().IsRegular() {
			return nil, &admissionError{410, "Original job inputs are unavailable. Reimport the original project before slicing again; retained outputs remain available.", 0}
		}
	}
	j, err := m.newJobLocked(client)
	if err != nil {
		return nil, err
	}
	j.Request, j.RetryOf = parent.Request, parent.ID
	entries, err := os.ReadDir(filepath.Join(parent.Dir, "input"))
	if err == nil {
		for _, e := range entries {
			if !e.Type().IsRegular() {
				err = errors.New("invalid retry input")
				break
			}
			var src, dst *os.File
			src, err = os.Open(filepath.Join(parent.Dir, "input", e.Name()))
			if err != nil {
				break
			}
			dst, err = os.OpenFile(filepath.Join(j.Dir, "input", e.Name()), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
			if err == nil {
				_, err = io.Copy(dst, src)
				err = errors.Join(err, dst.Close())
			}
			src.Close()
			if err != nil {
				break
			}
		}
	}
	if err != nil {
		delete(m.uploads, j.ID)
		os.RemoveAll(j.Dir)
		return nil, err
	}
	return j, nil
}
