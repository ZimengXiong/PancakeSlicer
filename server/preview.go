package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const maxKeptPreviews = 64

func (m *JobManager) previewDir() string { return filepath.Join(m.cfg.JobRoot, "patch-preview-cache") }

func (m *JobManager) previewKey(j *Job, args []string) string {
	h := sha256.New()
	if st, err := os.Stat(m.cfg.Tool); err == nil {
		h.Write([]byte(m.cfg.Tool + "\x00" + st.ModTime().String() + "\x00"))
	}
	for _, a := range keyArgs(j, args) {
		h.Write([]byte(strings.ReplaceAll(a, j.Dir, "") + "\x00"))
	}
	in := filepath.Join(j.Dir, "input")
	entries, _ := os.ReadDir(in)
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	for _, n := range names {
		b, err := os.ReadFile(filepath.Join(in, n))
		if err != nil {
			return ""
		}
		h.Write([]byte(n + "\x00"))
		sum := sha256.Sum256(b)
		h.Write(sum[:])
	}
	return hex.EncodeToString(h.Sum(nil))
}

func (m *JobManager) previewCached(j *Job, args []string) bool {
	key := m.previewKey(j, args)
	if key == "" {
		return false
	}
	raw, err := os.ReadFile(filepath.Join(m.previewDir(), key+".json"))
	if err != nil || !json.Valid(raw) || os.MkdirAll(filepath.Dir(j.OutputPath("json")), 0700) != nil || os.WriteFile(j.OutputPath("json"), raw, 0600) != nil {
		return false
	}
	j.log("server", "patch preview from cache")
	j.mu.Lock()
	j.slice = raw
	j.mu.Unlock()
	j.setState(StateDone, "")
	return true
}

func (m *JobManager) keepPreview(j *Job, args []string, raw []byte) {
	var head struct {
		Partial bool   `json:"partial"`
		Error   string `json:"error"`
	}
	if json.Unmarshal(raw, &head) != nil || head.Partial || head.Error != "" {
		return
	}
	key := m.previewKey(j, args)
	dir := m.previewDir()
	if key == "" || os.MkdirAll(dir, 0700) != nil {
		return
	}
	tmp := filepath.Join(dir, key+".tmp")
	if os.WriteFile(tmp, raw, 0600) != nil || os.Rename(tmp, filepath.Join(dir, key+".json")) != nil {
		os.Remove(tmp)
		return
	}
	entries, _ := os.ReadDir(dir)
	if len(entries) <= maxKeptPreviews {
		return
	}
	type kept struct {
		name string
		mod  int64
	}
	all := make([]kept, 0, len(entries))
	for _, e := range entries {
		if info, err := e.Info(); err == nil {
			all = append(all, kept{e.Name(), info.ModTime().UnixNano()})
		}
	}
	sort.Slice(all, func(a, b int) bool { return all[a].mod < all[b].mod })
	for _, k := range all[:len(all)-maxKeptPreviews] {
		os.Remove(filepath.Join(dir, k.name))
	}
}
