package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"io/fs"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"sync"
	"time"
)

type blobStore struct {
	root  string
	limit int64
	mu    sync.Mutex
}

var blobDigest = regexp.MustCompile(`^[0-9a-f]{64}$`)

const blobCacheBytes = 8 << 30

func (b *blobStore) path(sum string) string { return filepath.Join(b.root, sum[:2], sum) }

func (b *blobStore) register(mux *http.ServeMux, maxUpload int64) {
	mux.HandleFunc("PUT /api/blobs/{sha256}", func(w http.ResponseWriter, r *http.Request) { b.put(w, r, maxUpload) })
	mux.HandleFunc("POST /api/blobs/missing", b.missing)
}

func (b *blobStore) has(sum string) bool {
	now := time.Now()
	return os.Chtimes(b.path(sum), now, now) == nil
}

func (b *blobStore) missing(w http.ResponseWriter, r *http.Request) {
	var body struct {
		SHA256 []string `json:"sha256"`
	}
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<20)).Decode(&body) != nil || len(body.SHA256) > 4096 {
		fail(w, 400, "missing_needs_sha256_most")
		return
	}
	missing := []string{}
	for _, sum := range body.SHA256 {
		if !blobDigest.MatchString(sum) {
			fail(w, 400, "digests_must_lowercase_hex")
			return
		}
		if !b.has(sum) {
			missing = append(missing, sum)
		}
	}
	writeJSON(w, 200, map[string]any{"missing": missing})
}

func (b *blobStore) put(w http.ResponseWriter, r *http.Request, maxUpload int64) {
	sum := r.PathValue("sha256")
	if !blobDigest.MatchString(sum) {
		fail(w, 400, "blob_name_must_lowercase")
		return
	}
	if b.has(sum) {
		io.Copy(io.Discard, io.LimitReader(r.Body, 1<<20))
		writeJSON(w, 200, map[string]string{"sha256": sum})
		return
	}
	dir := filepath.Dir(b.path(sum))
	if err := os.MkdirAll(dir, 0700); err != nil {
		fail(w, 507, "file_storage_server_isnt")
		return
	}
	f, err := os.CreateTemp(dir, ".blob-*")
	if err != nil {
		fail(w, 507, "file_storage_server_isnt")
		return
	}
	defer os.Remove(f.Name())
	hash := sha256.New()
	_, err = io.Copy(io.MultiWriter(f, hash), http.MaxBytesReader(w, r.Body, maxUpload))
	err = errors.Join(err, f.Close())
	var tooLarge *http.MaxBytesError
	var timeout net.Error
	switch {
	case errors.As(err, &tooLarge):
		fail(w, 413, "file_larger_than_servers")
		return
	case errors.As(err, &timeout) && timeout.Timeout():
		w.Header().Set("Connection", "close")
		fail(w, http.StatusRequestTimeout, "upload_took_too_long")
		return
	case err != nil:
		fail(w, 400, "file_upload_interrupted_before")
		return
	}
	if hex.EncodeToString(hash.Sum(nil)) != sum {
		fail(w, 400, "uploaded_files_checksum_didnt")
		return
	}
	if err := os.Chmod(f.Name(), 0400); err != nil || os.Rename(f.Name(), b.path(sum)) != nil {
		fail(w, 507, "server_couldnt_save_uploaded")
		return
	}
	go b.evict()
	writeJSON(w, 201, map[string]string{"sha256": sum})
}

func (b *blobStore) stage(sum, dst string, writable bool) error {
	src := b.path(sum)
	if !b.has(sum) {
		return fs.ErrNotExist
	}
	if !writable && os.Link(src, dst) == nil {
		return nil
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	return errors.Join(err, out.Close())
}

func (b *blobStore) evict() {
	if !b.mu.TryLock() {
		return
	}
	defer b.mu.Unlock()
	type entry struct {
		path string
		size int64
		used time.Time
	}
	var entries []entry
	var total int64
	filepath.WalkDir(b.root, func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || !blobDigest.MatchString(d.Name()) {
			return nil
		}
		if info, err := d.Info(); err == nil {
			entries = append(entries, entry{path, info.Size(), info.ModTime()})
			total += info.Size()
		}
		return nil
	})
	if total <= b.limit {
		return
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].used.Before(entries[j].used) })
	for _, e := range entries {
		if total <= b.limit*4/5 {
			break
		}
		if os.Remove(e.path) == nil {
			total -= e.size
		}
	}
}
