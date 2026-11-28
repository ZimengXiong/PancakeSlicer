package main

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"io"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode"
)

type shareEntry struct {
	Name     string `json:"name"`
	Dir      bool   `json:"dir,omitempty"`
	Size     int64  `json:"size"`
	Modified int64  `json:"modified"`
}

const shareListLimit = 5000

var shareExtensions = []string{".stl", ".3mf", ".obj", ".step", ".stp", ".gcode"}

var errShareOutside = errors.New("path leads outside the shared folder")

func shareHidden(name string) bool {
	return strings.HasPrefix(name, ".") || name == "@eaDir" || name == "#recycle" || name == "#snapshot"
}

func shareFileName(name string) bool {
	lower := strings.ToLower(name)
	for _, ext := range shareExtensions {
		if strings.HasSuffix(lower, ext) && len(lower) > len(ext) {
			return true
		}
	}
	return false
}

func shareFolderName(name string) bool {
	return len(name) <= 120 && strings.Trim(name, " .") != "" && !strings.ContainsFunc(name, unicode.IsControl)
}

func sharePath(p string) (string, bool) {
	if p == "" {
		return ".", true
	}
	if !filepath.IsLocal(p) || strings.ContainsRune(p, '\\') {
		return "", false
	}
	for _, part := range strings.Split(filepath.ToSlash(p), "/") {
		if shareHidden(part) {
			return "", false
		}
	}
	return filepath.Clean(p), true
}

func (s *Server) shareRoutes(mux *http.ServeMux) {
	if s.cfg.ShareDir == "" {
		return
	}
	mux.HandleFunc("GET /api/share", s.listShare)
	mux.HandleFunc("GET /api/share/file", s.readShare)
	mux.HandleFunc("PUT /api/share/file", s.writeShare)
	mux.HandleFunc("POST /api/share/folder", s.makeShareFolder)
}

func (s *Server) shareBase(w http.ResponseWriter, r *http.Request) string {
	if s.cfg.BearerToken != "" && !s.authorized(r) {
		w.Header().Set("WWW-Authenticate", "Bearer")
		fail(w, 401, "access_token_required")
		return ""
	}
	base, err := filepath.EvalSymlinks(s.cfg.ShareDir)
	if err != nil {
		fail(w, 503, "share_unavailable", err.Error())
		return ""
	}
	return base
}

func shareResolve(base, rel string) (string, error) {
	real, err := filepath.EvalSymlinks(filepath.Join(base, rel))
	if err != nil {
		return "", err
	}
	if real != base && !strings.HasPrefix(real, base+string(filepath.Separator)) {
		return "", errShareOutside
	}
	return real, nil
}

func shareFailure(w http.ResponseWriter, err error) {
	if errors.Is(err, os.ErrNotExist) {
		fail(w, 404, "share_not_found")
	} else {
		fail(w, 403, "share_path_refused")
	}
}

func shareInfo(name string, st os.FileInfo) shareEntry {
	return shareEntry{Name: name, Dir: st.IsDir(), Size: st.Size(), Modified: st.ModTime().UnixMilli()}
}

func (s *Server) listShare(w http.ResponseWriter, r *http.Request) {
	dir, ok := sharePath(r.URL.Query().Get("path"))
	if !ok {
		fail(w, 400, "share_path_invalid")
		return
	}
	base := s.shareBase(w, r)
	if base == "" {
		return
	}
	real, err := shareResolve(base, dir)
	if err != nil {
		shareFailure(w, err)
		return
	}
	entries, err := os.ReadDir(real)
	if err != nil {
		fail(w, 400, "share_not_folder")
		return
	}
	out := []shareEntry{}
	for _, e := range entries {
		if shareHidden(e.Name()) || len(out) >= shareListLimit {
			continue
		}
		target, err := shareResolve(base, filepath.Join(dir, e.Name()))
		if err != nil {
			continue
		}
		st, err := os.Stat(target)
		if err != nil || !st.IsDir() && (!st.Mode().IsRegular() || !shareFileName(e.Name())) {
			continue
		}
		out = append(out, shareInfo(e.Name(), st))
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Dir != out[j].Dir {
			return out[i].Dir
		}
		return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name)
	})
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, map[string]any{"path": filepath.ToSlash(dir), "entries": out})
}

func (s *Server) readShare(w http.ResponseWriter, r *http.Request) {
	name, ok := sharePath(r.URL.Query().Get("path"))
	if !ok || name == "." {
		fail(w, 400, "share_path_invalid")
		return
	}
	if !shareFileName(name) {
		fail(w, 400, "share_type_not_allowed")
		return
	}
	base := s.shareBase(w, r)
	if base == "" {
		return
	}
	real, err := shareResolve(base, name)
	if err != nil {
		shareFailure(w, err)
		return
	}
	f, err := os.Open(real)
	if err != nil {
		shareFailure(w, err)
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || !st.Mode().IsRegular() {
		fail(w, 400, "share_not_file")
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": filepath.Base(name)}))
	http.ServeContent(w, r, "", st.ModTime(), f)
}

func (s *Server) writeShare(w http.ResponseWriter, r *http.Request) {
	name, ok := sharePath(r.URL.Query().Get("path"))
	if !ok || name == "." {
		fail(w, 400, "share_path_invalid")
		return
	}
	if !shareFileName(name) {
		fail(w, 400, "share_type_not_allowed")
		return
	}
	if r.ContentLength > s.cfg.MaxUploadSize {
		fail(w, http.StatusRequestEntityTooLarge, "upload_larger_than_servers")
		return
	}
	base := s.shareBase(w, r)
	if base == "" {
		return
	}
	dir, err := shareResolve(base, filepath.Dir(name))
	if err != nil {
		shareFailure(w, err)
		return
	}
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		fail(w, 400, "share_not_folder")
		return
	}
	target := filepath.Join(dir, filepath.Base(name))
	overwrite := r.URL.Query().Get("overwrite") == "1"
	exists := func() bool {
		st, err := os.Lstat(target)
		return err == nil && (!overwrite || st.IsDir())
	}
	if exists() {
		fail(w, 409, "share_file_exists")
		return
	}
	suffix := make([]byte, 8)
	rand.Read(suffix)
	tmp := filepath.Join(dir, "."+filepath.Base(name)+"."+hex.EncodeToString(suffix)+".tmp")
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		fail(w, 500, "share_write_failed", err.Error())
		return
	}
	_, err = io.Copy(f, http.MaxBytesReader(w, r.Body, s.cfg.MaxUploadSize))
	if err == nil {
		err = f.Sync()
	}
	err = errors.Join(err, f.Close())
	if err == nil && exists() {
		os.Remove(tmp)
		fail(w, 409, "share_file_exists")
		return
	}
	if err == nil {
		err = os.Rename(tmp, target)
	}
	if err != nil {
		os.Remove(tmp)
		var tooLarge *http.MaxBytesError
		if errors.As(err, &tooLarge) {
			fail(w, http.StatusRequestEntityTooLarge, "upload_larger_than_servers")
		} else {
			fail(w, 500, "share_write_failed", err.Error())
		}
		return
	}
	st, err := os.Stat(target)
	if err != nil {
		fail(w, 500, "share_write_failed", err.Error())
		return
	}
	writeJSON(w, 200, shareInfo(filepath.Base(name), st))
}

func (s *Server) makeShareFolder(w http.ResponseWriter, r *http.Request) {
	name, ok := sharePath(r.URL.Query().Get("path"))
	if !ok || name == "." {
		fail(w, 400, "share_path_invalid")
		return
	}
	if !shareFolderName(filepath.Base(name)) {
		fail(w, 400, "share_folder_name_invalid")
		return
	}
	base := s.shareBase(w, r)
	if base == "" {
		return
	}
	dir, err := shareResolve(base, filepath.Dir(name))
	if err != nil {
		shareFailure(w, err)
		return
	}
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		fail(w, 400, "share_not_folder")
		return
	}
	target := filepath.Join(dir, filepath.Base(name))
	if err := os.Mkdir(target, 0o755); err != nil {
		if errors.Is(err, os.ErrExist) {
			fail(w, 409, "share_folder_exists")
		} else {
			fail(w, 500, "share_write_failed", err.Error())
		}
		return
	}
	st, err := os.Stat(target)
	if err != nil {
		fail(w, 500, "share_write_failed", err.Error())
		return
	}
	writeJSON(w, 200, shareInfo(filepath.Base(name), st))
}
