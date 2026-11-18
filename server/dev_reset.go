package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"
)

func devResetEnabled() bool { return os.Getenv("PANCAKE_DEV_RESET") == "1" }

var lastReset struct {
	sync.Mutex
	operationID string
	response    []byte
}

func (s *Server) devReset(w http.ResponseWriter, r *http.Request) {
	var in struct {
		OperationID string `json:"operationId"`
	}
	if !workspaceDecode(w, r, &in) {
		return
	}
	lastReset.Lock()
	defer lastReset.Unlock()
	if in.OperationID != "" && in.OperationID == lastReset.operationID {
		w.Header().Set("Content-Type", "application/json")
		w.Write(lastReset.response)
		return
	}
	var token [8]byte
	rand.Read(token[:])
	epoch := hex.EncodeToString(token[:])
	if err := s.resetAll(epoch); err != nil {
		fail(w, 500, "reset_failed", err.Error())
		return
	}
	audit(map[string]any{"event": "dev_reset", "client": s.clientIP(r), "epoch": epoch})
	body, _ := json.Marshal(map[string]any{"epoch": epoch, "revision": 1})
	lastReset.operationID, lastReset.response = in.OperationID, append(body, '\n')
	w.Header().Set("Content-Type", "application/json")
	w.Write(lastReset.response)
}

func clearDir(dir string) error {
	entries, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, e := range entries {
		if err := os.RemoveAll(filepath.Join(dir, e.Name())); err != nil {
			return err
		}
	}
	return nil
}

func (s *Server) resetAll(epoch string) error {
	s.jobs.removeAll()
	if m := s.sends; m != nil && m.store != nil {
		m.mu.Lock()
		err := clearDir(m.store.dir)
		m.store.printers, m.conns = nil, nil
		m.mu.Unlock()
		if err != nil {
			return err
		}
		m.hub.changed()
	}
	st := s.workspace
	st.mu.Lock()
	err := clearDir(st.root)
	st.cached, st.views = nil, nil
	st.mu.Unlock()
	if err != nil {
		return err
	}
	thumbnails.Lock()
	thumbnails.m = map[string]thumbnailEntry{}
	thumbnails.Unlock()
	s.blobs.mu.Lock()
	err = clearDir(s.blobs.root)
	s.blobs.mu.Unlock()
	if err != nil {
		return err
	}
	for _, dir := range []string{"profile-backups", "user-presets", "nonplanar", "profile-library.json"} {
		if err := os.RemoveAll(filepath.Join(s.cfg.DataDir, dir)); err != nil {
			return err
		}
	}
	if err := s.library.reset(epoch); err != nil {
		return err
	}
	s.reconcileStoredConnections("profile reset")
	return nil
}

func (m *JobManager) removeAll() {
	m.mu.Lock()
	all := make([]*Job, 0, len(m.jobs)+len(m.uploads))
	for _, j := range m.jobs {
		all = append(all, j)
	}
	for _, j := range m.uploads {
		all = append(all, j)
	}
	m.mu.Unlock()
	for _, j := range all {
		m.Cancel(j)
	}
	deadline := time.Now().Add(5 * time.Second)
	for _, j := range all {
		for !j.State().Finished() && j.State() != StateIdle && time.Now().Before(deadline) {
			time.Sleep(50 * time.Millisecond)
		}
		m.Remove(j)
	}
	m.mu.Lock()
	m.uploads = map[string]*Job{}
	m.mu.Unlock()
}
