package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"math"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"runtime/debug"
	"sort"
	"strconv"
	"strings"
	"time"
)

type admissionError struct {
	status  int
	message string
	retry   int
}

func (e *admissionError) Error() string { return e.message }
func admissionFailure(w http.ResponseWriter, err error) {
	var e *admissionError
	if errors.As(err, &e) {
		if e.retry > 0 {
			w.Header().Set("Retry-After", strconv.Itoa(e.retry))
		}
		fail(w, e.status, "admission_refused", e.message)
	} else {
		fail(w, 500, "controls_internal", err.Error())
	}
}

type tokenBucket struct {
	tokens float64
	at     time.Time
}

func (m *JobManager) admitLocked(client string) error {
	if m.closing {
		return &admissionError{503, "service is shutting down", 1}
	}
	if m.cfg.SubmitRate > 0 {
		now := time.Now()
		b, ok := m.buckets[client]
		if !ok {
			b = tokenBucket{float64(m.cfg.SubmitBurst), now}
		}
		b.tokens = math.Min(float64(m.cfg.SubmitBurst), b.tokens+now.Sub(b.at).Seconds()*m.cfg.SubmitRate)
		b.at = now
		if b.tokens < 1 {
			m.buckets[client] = b
			return &admissionError{429, "submission rate exceeded", int(math.Min(86400, math.Max(1, math.Ceil((1-b.tokens)/m.cfg.SubmitRate))))}
		}
		b.tokens--
		m.buckets[client] = b
	}
	if m.cfg.MaxClientJobs > 0 || m.cfg.MaxQueue >= 0 {
		active, own := len(m.uploads), 0
		for _, j := range m.uploads {
			if j.Client == client {
				own++
			}
		}
		for _, j := range m.jobs {
			if !j.State().Finished() {
				active++
				if j.Client == client {
					own++
				}
			}
		}
		if m.cfg.MaxClientJobs > 0 && own >= m.cfg.MaxClientJobs {
			return &admissionError{429, "client active-job limit reached", 1}
		}
		if m.cfg.MaxQueue >= 0 && active >= m.cfg.MaxJobs && active-m.cfg.MaxJobs >= m.cfg.MaxQueue {
			return &admissionError{503, "job queue is full", 1}
		}
	}
	return m.quotaLocked()
}

func (m *JobManager) releaseUpload(j *Job) {
	m.mu.Lock()
	delete(m.uploads, j.ID)
	m.mu.Unlock()
}

func directoryBytes(dir string) (int64, error) {
	var size int64
	err := filepath.WalkDir(dir, func(_ string, d fs.DirEntry, err error) error {
		if os.IsNotExist(err) {
			return nil
		}
		if err != nil {
			return err
		}
		if d.Type().IsRegular() {
			st, err := d.Info()
			if os.IsNotExist(err) {
				return nil
			}
			if err != nil {
				return err
			}
			size += st.Size()
		}
		return nil
	})
	return size, err
}

func (m *JobManager) quotaLocked() error {
	if m.closing || m.cfg.DiskQuota == 0 {
		return nil
	}
	entries, err := os.ReadDir(m.cfg.JobRoot)
	if err != nil {
		return err
	}
	var total int64
	sizes := map[string]int64{}
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		size, err := directoryBytes(filepath.Join(m.cfg.JobRoot, entry.Name()))
		if err != nil {
			return err
		}
		sizes[entry.Name()] = size
		total += size
	}
	var finished []*Job
	for _, j := range m.jobs {
		if j.State().Finished() {
			finished = append(finished, j)
		}
	}
	sort.Slice(finished, func(i, k int) bool {
		finished[i].mu.Lock()
		a := finished[i].finished
		finished[i].mu.Unlock()
		finished[k].mu.Lock()
		b := finished[k].finished
		finished[k].mu.Unlock()
		return a.Before(b)
	})
	for _, j := range finished {
		if total <= m.cfg.DiskQuota {
			break
		}
		if err := m.removeLocked(j); err != nil {
			return err
		}
		total -= sizes[j.ID]
	}
	if total > m.cfg.DiskQuota {
		return &admissionError{507, "job disk quota exceeded", 0}
	}
	return nil
}

var durationBounds = [...]float64{1, 5, 15, 30, 60, 300, 900, 3600}

func (m *JobManager) completed(j *Job) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.cfg.Metrics {
		j.mu.Lock()
		for i, state := range []JobState{StateDone, StateFailed, StateCancelled} {
			if j.state == state {
				m.completedJobs[i]++
			}
		}
		if !j.started.IsZero() {
			seconds := j.finished.Sub(j.started).Seconds()
			m.durationCount++
			m.durationSum += seconds
			for i, bound := range durationBounds {
				if seconds <= bound {
					m.durations[i]++
				}
			}
		}
		j.mu.Unlock()
	}
	if err := m.quotaLocked(); err != nil {
		log.Printf("job quota: %v", err)
	}
}

func (s *Server) metrics(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	m := s.jobs
	m.mu.Lock()
	defer m.mu.Unlock()
	counts := map[JobState]int{}
	for _, j := range m.jobs {
		counts[j.State()]++
	}
	fmt.Fprintln(w, "# HELP orca_jobs_submitted_total Accepted jobs, excluding rejected uploads.\n# TYPE orca_jobs_submitted_total counter")
	fmt.Fprintf(w, "orca_jobs_submitted_total %d\n", m.submitted)
	fmt.Fprintln(w, "# HELP orca_jobs_completed_total Terminal jobs, including cancellations before starting.\n# TYPE orca_jobs_completed_total counter")
	for i, state := range []JobState{StateDone, StateFailed, StateCancelled} {
		fmt.Fprintf(w, "orca_jobs_completed_total{state=%q} %d\n", state, m.completedJobs[i])
	}
	fmt.Fprintln(w, "# HELP orca_http_rejections_total Completed HTTP responses with status 400 through 599.\n# TYPE orca_http_rejections_total counter")
	for i, count := range m.rejects {
		if count > 0 {
			fmt.Fprintf(w, "orca_http_rejections_total{status=\"%d\"} %d\n", i+400, count)
		}
	}
	fmt.Fprintln(w, "# HELP orca_http_response_bytes_total Response body bytes written for completed requests.\n# TYPE orca_http_response_bytes_total counter")
	fmt.Fprintf(w, "orca_http_response_bytes_total %d\n", m.responseBytes)
	fmt.Fprintln(w, "# HELP orca_download_bytes_total Response body bytes written for successful job downloads.\n# TYPE orca_download_bytes_total counter")
	fmt.Fprintf(w, "orca_download_bytes_total %d\n", m.downloadBytes)
	fmt.Fprintln(w, "# HELP orca_jobs Retained jobs by state.\n# TYPE orca_jobs gauge")
	for _, state := range []JobState{StateQueued, StateRunning, StateDone, StateFailed, StateCancelled} {
		fmt.Fprintf(w, "orca_jobs{state=%q} %d\n", state, counts[state])
	}
	fmt.Fprintln(w, "# HELP orca_slice_duration_seconds Running job duration, including failed and canceled slices.\n# TYPE orca_slice_duration_seconds histogram")
	for i, bound := range durationBounds {
		fmt.Fprintf(w, "orca_slice_duration_seconds_bucket{le=%q} %d\n", strconv.FormatFloat(bound, 'f', -1, 64), m.durations[i])
	}
	fmt.Fprintf(w, "orca_slice_duration_seconds_bucket{le=\"+Inf\"} %d\norca_slice_duration_seconds_sum %g\norca_slice_duration_seconds_count %d\n", m.durationCount, m.durationSum, m.durationCount)
}

func (s *Server) trusted(ip netip.Addr) bool {
	for _, prefix := range s.cfg.TrustedProxies {
		if prefix.Contains(ip) {
			return true
		}
	}
	return false
}
func (s *Server) clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	peer, err := netip.ParseAddr(host)
	if err != nil {
		return host
	}
	peer = peer.Unmap()
	socketPeer := peer
	if !s.trusted(peer) {
		return peer.String()
	}
	chain := strings.Split(strings.Join(r.Header.Values("X-Forwarded-For"), ","), ",")
	for i := len(chain) - 1; i >= 0; i-- {
		ip, err := netip.ParseAddr(strings.TrimSpace(chain[i]))
		if err != nil {
			return socketPeer.String()
		}
		peer = ip.Unmap()
		if !s.trusted(peer) {
			break
		}
	}
	return peer.String()
}

type requestWriter struct {
	http.ResponseWriter
	status int
	bytes  uint64
	failed []byte
}

func (w *requestWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }
func (w *requestWriter) WriteHeader(code int) {
	if code >= 100 && code < 200 {
		w.ResponseWriter.WriteHeader(code)
		return
	}
	if w.status == 0 {
		w.status = code
		w.ResponseWriter.WriteHeader(code)
	}
}
func (w *requestWriter) Write(b []byte) (int, error) {
	if w.status == 0 {
		w.WriteHeader(200)
	}
	n, err := w.ResponseWriter.Write(b)
	w.bytes += uint64(n)
	if w.status >= 400 && len(w.failed) < 1024 {
		w.failed = append(w.failed, b[:min(n, 1024-len(w.failed))]...)
	}
	return n, err
}
func (w *requestWriter) Flush() { _ = w.FlushError() }
func (w *requestWriter) FlushError() error {
	if w.status == 0 {
		w.WriteHeader(200)
	}
	return http.NewResponseController(w.ResponseWriter).Flush()
}

func logFailure(r *http.Request, recorder *requestWriter) {
	var body struct {
		Code  string `json:"code"`
		Error string `json:"error"`
	}
	json.Unmarshal(recorder.failed, &body)
	detail := body.Code
	if detail == "" {
		detail = logPrefix(body.Error, 160)
	}
	log.Printf("%s %s -> %d %s", logPrefix(r.Method, 32), logPrefix(r.URL.Path, 256), recorder.status, detail)
}

func (s *Server) middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		recorder := &requestWriter{ResponseWriter: w}
		w = recorder
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					panic(v)
				}
				log.Printf("%s %s panic: %v\n%s", logPrefix(r.Method, 32), logPrefix(r.URL.Path, 256), v, debug.Stack())
				if recorder.status == 0 {
					fail(recorder, 500, "Oops! Something went wrong on the server.")
				}
			}
			if recorder.status >= 400 && !s.cfg.RequestLog {
				logFailure(r, recorder)
			}
		}()
		if s.cfg.RequestLog || s.cfg.Metrics {
			started := time.Now()
			defer func() {
				status := recorder.status
				if status == 0 {
					status = 200
				}
				if s.cfg.Metrics {
					if r.Method == "HEAD" {
						recorder.bytes = 0
					}
					m := s.jobs
					m.mu.Lock()
					m.responseBytes += recorder.bytes
					if status >= 400 && status <= 599 {
						m.rejects[status-400]++
					}
					if (r.Method == "GET" || r.Method == "HEAD") && r.PathValue("id") != "" && r.URL.Path == "/api/jobs/"+r.PathValue("id")+"/download" && status >= 200 && status < 300 {
						m.downloadBytes += recorder.bytes
					}
					m.mu.Unlock()
				}
				if !s.cfg.RequestLog {
					return
				}
				id := r.PathValue("id")
				if id == "" && strings.HasPrefix(r.URL.Path, "/api/jobs/") {
					id, _, _ = strings.Cut(strings.TrimPrefix(r.URL.Path, "/api/jobs/"), "/")
				}
				if id == "" {
					id = strings.TrimPrefix(w.Header().Get("Location"), "/api/jobs/")
				}
				b, _ := json.Marshal(map[string]any{"method": logPrefix(r.Method, 32), "path": logPrefix(r.URL.Path, 1024), "status": status, "duration_ms": float64(time.Since(started).Microseconds()) / 1000, "client": s.clientIP(r), "job_id": logPrefix(id, 128)})
				log.Writer().Write(append(b, '\n'))
			}()
		}
		protected := r.URL.Path != "/api/access" && r.Method == "DELETE" || (r.Method == "POST" && (r.URL.Path == "/api/jobs" || strings.HasPrefix(r.URL.Path, "/api/jobs/"))) || ((r.Method == "POST" || r.Method == "PUT" || r.Method == "PATCH") && (r.URL.Path == "/api/library" || r.URL.Path == "/api/dev/reset" || r.URL.Path == "/api/session" || r.URL.Path == "/api/preferences" || strings.HasPrefix(r.URL.Path, "/api/projects/") || strings.HasPrefix(r.URL.Path, "/api/folders") || strings.HasPrefix(r.URL.Path, "/api/drafts/") || strings.HasPrefix(r.URL.Path, "/api/assets/") || strings.HasPrefix(r.URL.Path, "/api/blobs/") || r.URL.Path == "/api/user-heads" || strings.HasPrefix(r.URL.Path, "/api/printer-connections")))
		if s.cfg.BearerToken != "" && protected {
			if !s.authorized(r) {
				w.Header().Set("WWW-Authenticate", "Bearer")
				fail(w, 401, "access_token_required")
				return
			}
		}
		headerBytes := len(r.Method) + len(r.RequestURI) + len(r.Host) + 32
		for name, values := range r.Header {
			for _, value := range values {
				headerBytes += len(name) + len(value) + 4
			}
		}
		if headerBytes > 1<<20 {
			fail(w, http.StatusRequestHeaderFieldsTooLarge, "request_headers_exceed_mib")
			return
		}
		if (r.Method == "PUT" || r.Method == "POST" || r.Method == "PATCH") && r.URL.Path != "/api/jobs" && r.URL.Path != "/api/patch-preview" && r.URL.Path != "/api/top-surfaces/plan" {
			controller := http.NewResponseController(w)
			deadline := time.Now().Add(uploadIdleTimeout)
			if controller.SetReadDeadline(deadline) == nil {
				defer controller.SetReadDeadline(time.Time{})
				r.Body = &uploadBody{ReadCloser: r.Body, controller: controller, rate: s.cfg.UploadMinRate, rateDeadline: deadline}
			}
		}
		next.ServeHTTP(w, r)
	})
}

func logPrefix(value string, limit int) string {
	if len(value) > limit {
		return value[:limit] + "..."
	}
	return value
}
