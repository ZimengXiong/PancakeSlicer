package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strconv"
)

var jobResultGet func(*JobManager, *Job, []string) bool
var jobResultPut func(*JobManager, *Job, []string, []byte)
var jobResultStats func(*JobManager) any

var jobResultWarm func(*JobManager)

type queuedJob struct {
	job    *Job
	ctx    context.Context
	cancel context.CancelFunc
}

func batchJob(j *Job) bool { return j.Request.Priority == "low" || j.Request.Priority == "batch" }

func (m *JobManager) dispatchLocked() {
	m.queueOrderLocked()
	for !m.closing && m.active < m.cfg.MaxJobs && len(m.pending) > 0 {
		q := m.pending[0]
		m.pending = m.pending[1:]
		j := q.job
		j.mu.Lock()
		j.queuePosition = 0
		ok := j.setStateLocked(StatePreparing, "")
		j.mu.Unlock()
		if !ok {
			q.cancel()
			continue
		}
		if m.served == nil {
			m.served = map[string]uint64{}
		}
		m.serveSeq++
		m.served[queueSession(j)] = m.serveSeq
		m.active++
		m.wg.Add(1)
		go func() {
			defer m.wg.Done()
			defer q.cancel()
			m.run(q.ctx, j)
			j.mu.Lock()
			if !j.state.Finished() {
				j.setStateLocked(StateCancelled, "worker cancelled")
			}
			j.mu.Unlock()
			m.completed(j)
			m.mu.Lock()
			m.active--
			m.dispatchLocked()
			m.mu.Unlock()
		}()
	}
	for i, q := range m.pending {
		j := q.job
		j.mu.Lock()
		if j.queuePosition != i+1 {
			j.queuePosition = i + 1
			pos := i + 1
			j.appendLocked(Event{Type: "queue", State: j.state.legacy(), LifecycleState: j.state, QueuePosition: &pos})
			j.persistLocked()
		}
		j.mu.Unlock()
	}
}

func queueSession(j *Job) string {
	if j.Request.Session != "" {
		return "s:" + j.Request.Session
	}
	return "ip:" + j.Client
}

func (m *JobManager) queueOrderLocked() {
	key := queueSession
	running := map[string]int{}
	for _, j := range m.jobs {
		j.mu.Lock()
		st := j.state
		j.mu.Unlock()
		if st == StatePreparing || st == StateSlicing || st == StateStoring || st == StateCanceling {
			running[key(j)]++
		}
	}
	sort.SliceStable(m.pending, func(a, b int) bool { return m.pending[a].job.Created.Before(m.pending[b].job.Created) })
	rank, seen := map[*queuedJob]int{}, map[string]int{}
	for _, q := range m.pending {
		k := key(q.job)
		rank[q] = running[k] + seen[k]
		seen[k]++
	}
	sort.SliceStable(m.pending, func(a, b int) bool {
		qa, qb := m.pending[a], m.pending[b]
		if batchJob(qa.job) != batchJob(qb.job) {
			return !batchJob(qa.job)
		}
		if rank[qa] != rank[qb] {
			return rank[qa] < rank[qb]
		}
		return m.served[key(qa.job)] < m.served[key(qb.job)]
	})
}

var jobMemoryLimit func(*exec.Cmd) error

func limitJobMemory(cmd *exec.Cmd) error {
	if jobMemoryLimit != nil {
		return jobMemoryLimit(cmd)
	}
	raw := os.Getenv("ORCA_WEB_JOB_MEMORY_MB")
	if raw == "" {
		return nil
	}
	mb, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || mb < 64 || mb > 1<<30 {
		return fmt.Errorf("invalid ORCA_WEB_JOB_MEMORY_MB")
	}
	if runtime.GOOS != "linux" {
		return fmt.Errorf("job memory cap requires a platform memory hook")
	}
	wrapper, err := exec.LookPath("prlimit")
	if err != nil {
		return fmt.Errorf("job memory cap requires prlimit")
	}
	cmd.Args = append([]string{wrapper, "--as=" + strconv.FormatInt(mb<<20, 10), "--", cmd.Path}, cmd.Args[1:]...)
	cmd.Path = wrapper
	return nil
}

func configureJobProcess(cmd *exec.Cmd) {
	configureKill(cmd)
	if runtime.GOOS == "windows" {
		cmd.Cancel = func() error {
			return exec.Command("taskkill", "/PID", strconv.Itoa(cmd.Process.Pid), "/T", "/F").Run()
		}
	}
}
