package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"maps"
	"math"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

type workspaceStore struct {
	mu             sync.Mutex
	root           string
	cached         *workspaceData
	stamp          [2]int64
	views          map[string]any
	leases         map[string]*projectLease
	watch          sync.Mutex
	sessionChanged chan struct{}
}

func (st *workspaceStore) sessionWritten() {
	st.watch.Lock()
	if st.sessionChanged != nil {
		close(st.sessionChanged)
	}
	st.sessionChanged = make(chan struct{})
	st.watch.Unlock()
}

func (st *workspaceStore) sessionWait() <-chan struct{} {
	st.watch.Lock()
	defer st.watch.Unlock()
	if st.sessionChanged == nil {
		st.sessionChanged = make(chan struct{})
	}
	return st.sessionChanged
}

func (st *workspaceStore) sessionRevision() uint64 {
	st.mu.Lock()
	defer st.mu.Unlock()
	d, err := st.read()
	if err != nil {
		return 0
	}
	return d.Values["session"].Revision
}

type workspaceProject struct {
	Record   json.RawMessage `json:"record"`
	Archived bool            `json:"archived,omitempty"`
	Revision uint64          `json:"revision"`
	Folder   string          `json:"folder,omitempty"`
	Versions *versionIndex   `json:"versions,omitempty"`
}
type workspaceDraft struct {
	ID        string          `json:"id"`
	Sequence  uint64          `json:"sequence"`
	Document  json.RawMessage `json:"document"`
	Metadata  json.RawMessage `json:"metadata,omitempty"`
	UpdatedAt int64           `json:"updatedAt"`
}
type workspaceValue struct {
	Revision uint64          `json:"revision"`
	Document json.RawMessage `json:"document"`
}
type workspaceReceipt struct {
	Digest   string          `json:"digest"`
	Response json.RawMessage `json:"response"`
	Seq      uint64          `json:"seq,omitempty"`
}
type workspaceData struct {
	Schema         int                         `json:"schema"`
	Projects       map[string]workspaceProject `json:"projects"`
	Drafts         map[string]workspaceDraft   `json:"drafts"`
	Values         map[string]workspaceValue   `json:"values"`
	Operations     map[string]workspaceReceipt `json:"operations"`
	Adoptions      map[string]json.RawMessage  `json:"adoptions"`
	Aliases        map[string]string           `json:"aliases"`
	Folders        map[string]workspaceFolder  `json:"folders,omitempty"`
	FolderRevision uint64                      `json:"folderRevision,omitempty"`
	Migrations     map[string]int64            `json:"migrations,omitempty"`
	Problems       map[string]workspaceProblem `json:"problems,omitempty"`
}

type workspaceProblem struct {
	Code    string `json:"code"`
	File    string `json:"file"`
	Message string `json:"message"`
}

func newWorkspaceData() *workspaceData {
	return &workspaceData{Schema: 1, Projects: map[string]workspaceProject{}, Drafts: map[string]workspaceDraft{}, Values: map[string]workspaceValue{}, Operations: map[string]workspaceReceipt{}, Adoptions: map[string]json.RawMessage{}, Aliases: map[string]string{}, Folders: map[string]workspaceFolder{}}
}
func (d *workspaceData) clone() *workspaceData {
	c := *d
	c.Projects, c.Drafts, c.Values, c.Operations = maps.Clone(d.Projects), maps.Clone(d.Drafts), maps.Clone(d.Values), maps.Clone(d.Operations)
	c.Adoptions, c.Aliases, c.Folders, c.Migrations = maps.Clone(d.Adoptions), maps.Clone(d.Aliases), maps.Clone(d.Folders), maps.Clone(d.Migrations)
	return &c
}
func fileStamp(info os.FileInfo) [2]int64 { return [2]int64{info.Size(), info.ModTime().UnixNano()} }

func (st *workspaceStore) read() (*workspaceData, error) {
	path := filepath.Join(st.root, "workspace.json")
	info, err := os.Stat(path)
	if errors.Is(err, os.ErrNotExist) {
		return newWorkspaceData(), nil
	}
	if err != nil {
		return nil, err
	}
	if st.cached != nil && st.stamp == fileStamp(info) {
		return st.cached.clone(), nil
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	d, err := decodeWorkspace(raw)
	if err != nil {
		return nil, err
	}
	before := len(d.Migrations)
	ran, err := migrateWorkspace(d, time.Now(), st.root)
	if err != nil {
		return nil, fmt.Errorf("workspace migration: %w (nothing was recorded; it runs again on the next read)", err)
	}
	if len(ran) > 0 {
		stamp := time.Now().UTC().Format("20060102T150405Z")
		backup := fmt.Sprintf("workspace.v%d.%s.json", d.Schema, stamp)
		if err := writeBackup(filepath.Join(st.root, backup), raw); err != nil {
			return nil, fmt.Errorf("workspace migration: the backup %s could not be written, nothing changed: %w", backup, err)
		}
		encoded, err := d.encode()
		if err == nil {
			_, err = decodeWorkspace(encoded)
		}
		if err != nil {
			return nil, fmt.Errorf("workspace migration %v: the result does not read back, nothing changed (backup %s): %w", ran, backup, err)
		}
		if err := st.save(d); err != nil {
			return nil, err
		}
		log.Printf("workspace: ran migrations %v (%d recorded before), backup %s", ran, before, backup)
		return d, nil
	}
	st.cached, st.stamp, st.views = d.clone(), fileStamp(info), nil
	return d, nil
}

func decodeWorkspace(raw []byte) (*workspaceData, error) {
	var d workspaceData
	if err := json.Unmarshal(raw, &d); err != nil {
		return nil, err
	}
	if d.Schema != 1 || d.Projects == nil || d.Drafts == nil || d.Values == nil || d.Operations == nil || d.Adoptions == nil || d.Aliases == nil {
		return nil, errors.New("Invalid workspace data format.")
	}
	if d.Folders == nil {
		d.Folders = map[string]workspaceFolder{}
	}
	for id, project := range d.Projects {
		if bytes.Equal(bytes.TrimSpace(project.Record), []byte("null")) {
			project.Record = nil
			d.Projects[id] = project
		}
		if project.Record == nil {
			if !project.Archived {
				return nil, fmt.Errorf("Active project %s couldn't be found in storage.", id)
			}
			continue
		}
		if _, err := projectRecord(project.Record, id); err != nil {
			return nil, fmt.Errorf("Saved project %s has corrupted data: %w", id, err)
		}
	}
	return &d, nil
}
func durableJSON(path string, value any) error {
	raw, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return durableWrite(path, raw)
}

var durableWrites sync.Map

func durableWrite(path string, raw []byte) error { return durableWriteVia(path, path+".tmp", raw) }

func writeBackup(path string, raw []byte) error {
	kind := strings.TrimSuffix(path, ".json")
	kind = kind[:strings.LastIndexByte(kind, '.')]
	return durableWriteVia(path, kind+".backup.tmp", raw)
}

func durableWriteVia(path, tmp string, raw []byte) error {
	lock, _ := durableWrites.LoadOrStore(tmp, &sync.Mutex{})
	lock.(*sync.Mutex).Lock()
	defer lock.(*sync.Mutex).Unlock()
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return err
	}
	_, err = f.Write(raw)
	if err == nil {
		err = f.Sync()
	}
	err = errors.Join(err, f.Close())
	if err == nil {
		err = os.Rename(tmp, path)
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	return syncWorkspaceDirectory(filepath.Dir(path))
}

const legacyTempPrefix = ".workspace-"

func removeLegacyTemps(dataDir string) error {
	workspaceRoot := filepath.Join(dataDir, "workspace")
	dirs := []string{dataDir, filepath.Join(dataDir, "printers"), filepath.Join(dataDir, "nonplanar", "heads"), workspaceRoot}
	versions, err := os.ReadDir(filepath.Join(workspaceRoot, "versions"))
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	for _, v := range versions {
		if v.IsDir() {
			dir := filepath.Join(workspaceRoot, "versions", v.Name())
			dirs = append(dirs, dir, filepath.Join(dir, "records"))
		}
	}
	removed := 0
	for _, dir := range dirs {
		entries, err := os.ReadDir(dir)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasPrefix(entry.Name(), legacyTempPrefix) {
				continue
			}
			path := filepath.Join(dir, entry.Name())
			info, err := entry.Info()
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			if err != nil {
				return err
			}
			if err := os.Remove(path); errors.Is(err, os.ErrNotExist) {
				continue
			} else if err != nil {
				return err
			}
			removed++
			log.Printf("storage migration: removed %s (%d bytes, %s), a temporary file an earlier version's write left when killed before its rename", path, info.Size(), info.ModTime().UTC().Format(time.RFC3339))
		}
	}
	log.Printf("storage migration: removed %d leftover temporary files from %d directories under %s", removed, len(dirs), dataDir)
	return nil
}

func syncWorkspaceDirectory(path string) error {
	if runtime.GOOS == "windows" {
		return nil
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Sync()
}
func (st *workspaceStore) save(d *workspaceData) error {
	path := filepath.Join(st.root, "workspace.json")
	st.cached, st.views = nil, nil
	raw, err := d.encode()
	if err != nil {
		return err
	}
	if err := durableWrite(path, raw); err != nil {
		return err
	}
	if info, err := os.Stat(path); err == nil {
		st.cached, st.stamp = d.clone(), fileStamp(info)
	}
	return nil
}

func (d *workspaceData) encode() ([]byte, error) {
	rest := *d
	rest.Projects, rest.Drafts = nil, nil
	head, err := json.Marshal(rest)
	if err != nil {
		return nil, err
	}
	var projects, drafts bytes.Buffer
	if err = encodeVerbatim(&projects, d.Projects, `{"record":null`, func(p workspaceProject) (workspaceProject, json.RawMessage) {
		raw := p.Record
		p.Record = nil
		return p, raw
	}); err != nil {
		return nil, err
	}
	if err = encodeVerbatim(&drafts, d.Drafts, `,"document":null`, func(dr workspaceDraft) (workspaceDraft, json.RawMessage) {
		raw := dr.Document
		dr.Document = nil
		return dr, raw
	}); err != nil {
		return nil, err
	}
	return bytes.Replace(head, []byte(`"projects":null,"drafts":null`), []byte(`"projects":`+projects.String()+`,"drafts":`+drafts.String()), 1), nil
}

func encodeVerbatim[V any](b *bytes.Buffer, m map[string]V, marker string, split func(V) (V, json.RawMessage)) error {
	keys := make([]string, 0, len(m))
	for key := range m {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	b.WriteByte('{')
	for i, key := range keys {
		value, raw := split(m[key])
		entry, err := json.Marshal(value)
		if err != nil {
			return err
		}
		if i > 0 {
			b.WriteByte(',')
		}
		k, _ := json.Marshal(key)
		b.Write(k)
		b.WriteByte(':')
		if at := bytes.Index(entry, []byte(marker)); at >= 0 && raw != nil {
			at += len(marker) - len("null")
			b.Write(entry[:at])
			b.Write(raw)
			b.Write(entry[at+len("null"):])
		} else {
			b.Write(entry)
		}
	}
	b.WriteByte('}')
	return nil
}

func (st *workspaceStore) derived(key string, build func() any) any {
	if v, ok := st.views[key]; ok {
		return v
	}
	v := build()
	if st.cached != nil {
		if st.views == nil {
			st.views = map[string]any{}
		}
		st.views[key] = v
	}
	return v
}
func workspaceID(id string) bool {
	return id != "" && len(id) <= 200 && strings.Trim(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == "" && id != "." && id != ".."
}

func canonicalWorkspaceProject(d *workspaceData, id string) (string, error) {
	seen := map[string]bool{}
	for {
		next, alias := d.Aliases[id]
		if !alias {
			break
		}
		if seen[id] {
			return "", errors.New("Oops! A loop was found in your project shortcuts. Keep your existing files and repair the project links.")
		}
		seen[id] = true
		if !workspaceID(next) {
			return "", errors.New("Oops! A project shortcut points to an invalid ID. Keep your existing files and repair the project links.")
		}
		id = next
	}
	if !workspaceID(id) {
		return "", errors.New("Invalid project ID.")
	}
	if len(seen) > 0 {
		if _, exists := d.Projects[id]; !exists {
			return "", errors.New("Oops! A project shortcut points to a missing project. Keep your existing files and repair the project links.")
		}
	}
	return id, nil
}
func workspaceFailure(w http.ResponseWriter, status int, code, message, recovery string, revision uint64) {
	writeJSON(w, status, workspaceError(code, message, recovery, revision))
}
func workspaceError(code, message, recovery string, revision uint64) map[string]any {
	name := "WorkspaceError"
	if code == "revision_conflict" {
		name = "ProjectConflictError"
	}
	if code == "project_archived" {
		name = "ProjectDeletedError"
	}
	return map[string]any{"error": message, "name": name, "code": code, "recovery": recovery, "revision": revision}
}
func workspaceDecode(w http.ResponseWriter, r *http.Request, v any) bool {
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<20))
	if err := dec.Decode(v); err != nil {
		workspaceFailure(w, 400, "invalid_document", "Oops, the saved document is invalid or over 64 MiB.", "export_and_repair", 0)
		return false
	}
	var extra any
	if dec.Decode(&extra) != io.EOF {
		workspaceFailure(w, 400, "invalid_document", "Expected one JSON document here.", "export_and_repair", 0)
		return false
	}
	return true
}
func workspaceDigest(v any) string {
	raw, _ := json.Marshal(v)
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])
}
func (st *workspaceStore) locked(w http.ResponseWriter, fn func(*workspaceData)) {
	w.Header().Set("Cache-Control", "no-store")
	st.mu.Lock()
	defer st.mu.Unlock()
	d, err := st.read()
	if err != nil {
		workspaceFailure(w, 503, "workspace_unavailable", "Oops! Couldn't read saved work ("+storageCause(err)+"). Hang on to your current document and try the service again.", "retry_storage", 0)
		return
	}
	fn(d)
}

func storageCause(err error) string {
	log.Printf("storage: %v", err)
	switch {
	case errors.Is(err, syscall.ENOSPC):
		return "the disk is full: no space left on device"
	case errors.Is(err, syscall.EROFS):
		return "the storage is read-only"
	case errors.Is(err, fs.ErrPermission):
		return "permission denied"
	case errors.Is(err, fs.ErrNotExist):
		return "a stored file is missing"
	}
	return "a storage error; the service log has the details"
}

func (st *workspaceStore) view(w http.ResponseWriter, fn func(*workspaceData)) {
	st.locked(w, func(d *workspaceData) {
		w.Header().Set("Cache-Control", privateRevalidate)
		fn(d)
	})
}
func workspaceReplay(w http.ResponseWriter, d *workspaceData, op, digest string) bool {
	if op == "" || len(op) > 200 {
		workspaceFailure(w, 400, "operation_required", "We need a stable operationId to retry safely.", "retry_same_operation", 0)
		return true
	}
	if old, ok := d.Operations[op]; ok {
		if old.Digest != digest {
			workspaceFailure(w, 409, "operation_conflict", "Heads up — this operationId was already used for different data.", "reload_and_reapply", 0)
		} else {
			writeJSON(w, 200, json.RawMessage(old.Response))
		}
		return true
	}
	return false
}
func (st *workspaceStore) commit(w http.ResponseWriter, d *workspaceData, op, digest string, response any) bool {
	raw, _ := json.Marshal(response)
	putReceipt(d.Operations, op, workspaceReceipt{Digest: digest, Response: raw}, func(r workspaceReceipt) uint64 { return r.Seq },
		func(r workspaceReceipt, seq uint64) workspaceReceipt { r.Seq = seq; return r })
	if err := st.save(d); err != nil {
		workspaceFailure(w, 507, "workspace_write_failed", "Uh-oh, couldn't save your work ("+storageCause(err)+"). Keep your draft safe, free up a bit of storage, and retry!", "free_storage_and_retry", 0)
		return false
	}
	writeJSON(w, 200, response)
	return true
}

func withRecord(p workspaceProject, id string, raw json.RawMessage, revision uint64) (workspaceProject, error) {
	record, err := projectRecord(raw, id)
	if err != nil {
		return p, err
	}
	p.Record, p.Revision = record, revision
	return p, nil
}

func projectRecord(raw json.RawMessage, id string) (json.RawMessage, error) {
	var record map[string]json.RawMessage
	if json.Unmarshal(raw, &record) != nil || record == nil {
		return nil, errors.New("record is required")
	}
	var meta map[string]json.RawMessage
	var doc map[string]json.RawMessage
	if json.Unmarshal(record["meta"], &meta) != nil || meta == nil || json.Unmarshal(record["doc"], &doc) != nil || doc == nil {
		return nil, errors.New("meta and doc must be objects")
	}
	var name string
	var objects, savedAt *float64
	var thumbnail *string
	if json.Unmarshal(meta["name"], &name) != nil || strings.TrimSpace(name) == "" || len(name) > 500 || json.Unmarshal(meta["objects"], &objects) != nil || objects == nil || *objects < 0 || math.Trunc(*objects) != *objects || json.Unmarshal(meta["savedAt"], &savedAt) != nil || savedAt == nil || *savedAt < 0 {
		return nil, errors.New("project name, nonnegative savedAt and integer object count are required")
	}
	if raw := meta["thumbnail"]; len(raw) != 0 && json.Unmarshal(raw, &thumbnail) != nil || thumbnail != nil && strings.HasPrefix(*thumbnail, thumbnailRoute) {
		return nil, errors.New("project thumbnail must be an image data URL, another string or null")
	}
	meta["id"], _ = json.Marshal(id)
	delete(meta, "revision")
	delete(meta, "folderId")
	delete(meta, "change")
	record["meta"], _ = json.Marshal(meta)
	return json.Marshal(record)
}
func (st *workspaceStore) checkAssets(record json.RawMessage) error {
	var v struct {
		Doc struct {
			Files map[string]string `json:"files"`
		} `json:"doc"`
	}
	if err := json.Unmarshal(record, &v); err != nil {
		return err
	}
	for key := range v.Doc.Files {
		if _, err := os.Stat(st.assetPath(key)); err != nil {
			return fmt.Errorf("Model file %s is missing on the server—please re-upload it before saving.", key)
		}
	}
	return nil
}

func servedRecord(p workspaceProject) json.RawMessage {
	var record map[string]json.RawMessage
	var meta map[string]json.RawMessage
	if json.Unmarshal(p.Record, &record) != nil || json.Unmarshal(record["meta"], &meta) != nil || meta == nil {
		return p.Record
	}
	meta["revision"], _ = json.Marshal(p.Revision)
	record["meta"], _ = json.Marshal(meta)
	var b bytes.Buffer
	b.WriteByte('{')
	for i, key := range sortedKeys(record) {
		if i > 0 {
			b.WriteByte(',')
		}
		k, _ := json.Marshal(key)
		b.Write(k)
		b.WriteByte(':')
		b.Write(record[key])
	}
	b.WriteByte('}')
	return b.Bytes()
}

func projectMeta(p workspaceProject) json.RawMessage {
	var record map[string]json.RawMessage
	json.Unmarshal(p.Record, &record)
	var meta map[string]json.RawMessage
	if json.Unmarshal(record["meta"], &meta) != nil || meta == nil {
		return record["meta"]
	}
	var id, thumbnail string
	if json.Unmarshal(meta["id"], &id) == nil && json.Unmarshal(meta["thumbnail"], &thumbnail) == nil && strings.HasPrefix(thumbnail, "data:image/") {
		meta["thumbnail"], _ = json.Marshal(thumbnailURL(id, thumbnail))
		rememberThumbnail(id, thumbnail)
	}
	meta["revision"], _ = json.Marshal(p.Revision)
	meta["folderId"] = json.RawMessage("null")
	if p.Folder != "" {
		meta["folderId"], _ = json.Marshal(p.Folder)
	}
	raw, _ := json.Marshal(meta)
	return raw
}
func (st *workspaceStore) register(mux *http.ServeMux) {
	st.leaseRoutes(mux)
	st.folderRoutes(mux)
	st.thumbnailRoutes(mux)
	mux.HandleFunc("GET /api/projects", func(w http.ResponseWriter, r *http.Request) {
		st.view(w, func(d *workspaceData) {
			archived := r.URL.Query().Get("archived") == "1"
			writeJSON(w, 200, st.derived(fmt.Sprint("projects:", archived), func() any {
				type listed struct {
					meta    json.RawMessage
					SavedAt float64 `json:"savedAt"`
				}
				var rows []listed
				for _, p := range d.Projects {
					if p.Archived == archived && p.Record != nil {
						row := listed{meta: projectMeta(p)}
						json.Unmarshal(row.meta, &row)
						rows = append(rows, row)
					}
				}
				sort.Slice(rows, func(i, j int) bool { return rows[i].SavedAt > rows[j].SavedAt })
				out := make([]json.RawMessage, len(rows))
				for i, row := range rows {
					out[i] = row.meta
				}
				return out
			}))
		})
	})
	mux.HandleFunc("GET /api/projects/references", func(w http.ResponseWriter, r *http.Request) {
		st.view(w, func(d *workspaceData) {
			writeJSON(w, 200, st.derived("references", func() any {
				type reference struct {
					ID      string `json:"id"`
					Name    string `json:"name"`
					Trashed bool   `json:"trashed"`
					Doc     struct {
						Printer       json.RawMessage `json:"printer,omitempty"`
						Process       json.RawMessage `json:"process,omitempty"`
						FilamentSlots json.RawMessage `json:"filamentSlots,omitempty"`
					} `json:"doc"`
				}
				documents := []reference{}
				for id, p := range d.Projects {
					if p.Record == nil {
						continue
					}
					var record struct {
						Meta struct {
							Name string `json:"name"`
						} `json:"meta"`
						Doc json.RawMessage `json:"doc"`
					}
					json.Unmarshal(p.Record, &record)
					ref := reference{ID: id, Name: record.Meta.Name, Trashed: p.Archived}
					json.Unmarshal(record.Doc, &ref.Doc)
					documents = append(documents, ref)
				}
				sort.Slice(documents, func(i, j int) bool { return documents[i].ID < documents[j].ID })
				return map[string]any{"complete": true, "documents": documents}
			}))
		})
	})
	mux.HandleFunc("GET /api/projects/{id}", func(w http.ResponseWriter, r *http.Request) {
		st.view(w, func(d *workspaceData) {
			id, err := canonicalWorkspaceProject(d, r.PathValue("id"))
			if err != nil {
				workspaceFailure(w, 409, "project_alias_invalid", err.Error(), "repair_aliases", 0)
				return
			}
			p, ok := d.Projects[id]
			if !ok {
				workspaceFailure(w, 404, "project_missing", "Project not found.", "open_projects", 0)
				return
			}
			if p.Record == nil {
				workspaceFailure(w, 410, "project_archived", "This project was deleted and doesn't have a saved document left — its deletion marker keeps stale writers from bringing it back.", "open_projects", p.Revision)
				return
			}
			if p.Archived && r.URL.Query().Get("archived") != "1" {
				workspaceFailure(w, 410, "project_archived", "This project is in the Trash! Restore it first before editing.", "restore_project", p.Revision)
				return
			}
			writeJSON(w, 200, servedRecord(p))
		})
	})
	mux.HandleFunc("PUT /api/projects/{id}", func(w http.ResponseWriter, r *http.Request) { st.projectWrite(w, r, "save") })
	for _, action := range []string{"rename", "duplicate", "archive", "restore"} {
		mux.HandleFunc("POST /api/projects/{id}/"+action, func(w http.ResponseWriter, r *http.Request) { st.projectWrite(w, r, action) })
	}
	mux.HandleFunc("GET /api/drafts", func(w http.ResponseWriter, r *http.Request) {
		st.view(w, func(d *workspaceData) {
			out := []workspaceDraft{}
			for _, v := range d.Drafts {
				v.Document = nil
				out = append(out, v)
			}
			sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
			writeJSON(w, 200, out)
		})
	})
	mux.HandleFunc("GET /api/drafts/{id}", func(w http.ResponseWriter, r *http.Request) {
		st.view(w, func(d *workspaceData) {
			v, ok := d.Drafts[r.PathValue("id")]
			if !ok {
				workspaceFailure(w, 404, "draft_missing", "Hmm, couldn't find that recovery draft.", "open_projects", 0)
				return
			}
			writeJSON(w, 200, v)
		})
	})
	mux.HandleFunc("PUT /api/drafts/{id}", st.putDraft)
	mux.HandleFunc("DELETE /api/drafts/{id}", st.deleteDraft)
	for _, value := range []struct{ kind, method string }{{"session", "PATCH"}, {"preferences", "PUT"}} {
		kind := value.kind
		mux.HandleFunc("GET /api/"+value.kind, func(w http.ResponseWriter, r *http.Request) {
			st.view(w, func(d *workspaceData) {
				v := d.Values[kind]
				if v.Document == nil {
					v.Document = json.RawMessage(`{}`)
				}
				writeJSON(w, 200, v)
			})
		})
		mux.HandleFunc(value.method+" /api/"+value.kind, func(w http.ResponseWriter, r *http.Request) { st.putValue(w, r, kind) })
	}
	mux.HandleFunc("PUT /api/assets/{key}", st.putAsset)
	mux.HandleFunc("GET /api/assets/{key}", st.getAsset)
}

type workspaceProjectState string

const (
	projectAbsent    workspaceProjectState = "absent"
	projectLive      workspaceProjectState = "live"
	projectTrashed   workspaceProjectState = "trashed"
	projectTombstone workspaceProjectState = "tombstone"
)

var workspaceProjectActions = map[workspaceProjectState]map[string]bool{
	projectAbsent:    {"save": true},
	projectLive:      {"save": true, "rename": true, "duplicate": true, "archive": true},
	projectTrashed:   {"restore": true},
	projectTombstone: {},
}

func workspaceProjectStateOf(p workspaceProject, exists bool) workspaceProjectState {
	switch {
	case !exists:
		return projectAbsent
	case p.Archived && p.Record == nil:
		return projectTombstone
	case p.Archived:
		return projectTrashed
	default:
		return projectLive
	}
}

func (st *workspaceStore) projectWrite(w http.ResponseWriter, r *http.Request, action string) {
	var in struct {
		OperationID      string          `json:"operationId"`
		ExpectedRevision uint64          `json:"expectedRevision"`
		Record           json.RawMessage `json:"record"`
		Name             string          `json:"name"`
		NewID            string          `json:"newId"`
	}
	if !workspaceDecode(w, r, &in) {
		return
	}
	st.locked(w, func(d *workspaceData) {
		id, aliasErr := canonicalWorkspaceProject(d, r.PathValue("id"))
		if aliasErr != nil {
			workspaceFailure(w, 409, "project_alias_invalid", aliasErr.Error(), "repair_aliases", 0)
			return
		}
		digest := workspaceDigest([]any{action, id, in})
		if workspaceReplay(w, d, in.OperationID, digest) {
			return
		}
		p, exists := d.Projects[id]
		if action != "duplicate" && st.leaseConflict(w, id, r.Header.Get(sessionHeader), p.Revision) {
			return
		}
		if state := workspaceProjectStateOf(p, exists); !workspaceProjectActions[state][action] {
			switch state {
			case projectAbsent:
				workspaceFailure(w, 404, "project_missing", "Project not found.", "open_projects", 0)
			case projectTombstone:
				workspaceFailure(w, 410, "project_archived", "This deleted project doesn't have a saved document to restore. Keep its deletion marker and open another project!", "open_projects", p.Revision)
			case projectTrashed:
				workspaceFailure(w, 410, "project_archived", "This project is in the Trash! Restore it first before saving.", "restore_project", p.Revision)
			default:
				workspaceFailure(w, 409, "project_not_archived", "This project isn't in the Trash!", "open_project", p.Revision)
			}
			return
		}
		if p.Revision != in.ExpectedRevision {
			workspaceFailure(w, 409, "revision_conflict", "Heads up — someone else changed this project! Reload it or keep a separate copy.", "reload_and_reapply", p.Revision)
			return
		}
		next, revision := p, p.Revision+1
		var err error
		switch action {
		case "save":
			next, err = withRecord(p, id, in.Record, revision)
			if err == nil {
				err = st.checkAssets(next.Record)
			}
		case "rename", "duplicate":
			var record map[string]json.RawMessage
			json.Unmarshal(p.Record, &record)
			var meta map[string]json.RawMessage
			json.Unmarshal(record["meta"], &meta)
			meta["name"], _ = json.Marshal(in.Name)
			meta["savedAt"], _ = json.Marshal(time.Now().UnixMilli())
			record["meta"], _ = json.Marshal(meta)
			raw, _ := json.Marshal(record)
			if action == "duplicate" {
				if !workspaceID(in.NewID) {
					err = errors.New("Please provide a new project ID.")
				} else if _, ok := d.Projects[in.NewID]; ok {
					err = errors.New("A project with that duplicate ID already exists.")
				} else if _, alias := d.Aliases[in.NewID]; alias {
					err = errors.New("That project ID is already in use.")
				} else {
					id, revision = in.NewID, 1
				}
			}
			if err == nil {
				next, err = withRecord(p, id, raw, revision)
			}
		case "archive":
			next, err = withRecord(p, id, p.Record, revision)
			next.Archived = true
		case "restore":
			next, err = withRecord(p, id, p.Record, revision)
			next.Archived = false
		}
		if err != nil {
			workspaceFailure(w, 400, "project_invalid", err.Error(), "export_and_repair", p.Revision)
			return
		}
		sha, err := st.keepRecord(id, next.Record)
		if err != nil {
			workspaceFailure(w, 507, "workspace_write_failed", "Uh-oh, couldn't save your work ("+storageCause(err)+"). Keep your draft safe, free up a bit of storage, and retry!", "free_storage_and_retry", p.Revision)
			return
		}
		change := map[string][]versionChange{"save": recordChange(in.Record), "rename": {{Key: "name", Label: "Renamed to " + in.Name}}, "duplicate": {{Label: "Copied"}}}[action]
		if action == "duplicate" {
			next.Versions = nil
		}
		next.Versions = withVersion(next, next.Revision, sha, next.Record, change, action, time.Now().UnixMilli())
		d.Projects[id] = next
		response := any(map[string]any{"revision": next.Revision, "operationId": in.OperationID})
		if action == "duplicate" {
			response = json.RawMessage(projectMeta(next))
		}
		if !st.commit(w, d, in.OperationID, digest, response) {
			return
		}
		if action == "save" {
			st.claimLease(id, r.Header.Get(sessionHeader))
		}
		st.pruneVersionFiles(id, next.Versions)
	})
}

type draftRef struct {
	ID       string `json:"id"`
	Sequence uint64 `json:"sequence"`
}

func (st *workspaceStore) putDraft(w http.ResponseWriter, r *http.Request) {
	var in struct {
		OperationID string          `json:"operationId"`
		Sequence    uint64          `json:"sequence"`
		Document    json.RawMessage `json:"document"`
		Metadata    json.RawMessage `json:"metadata"`
		Supersedes  []draftRef      `json:"supersedes"`
	}
	if !workspaceDecode(w, r, &in) {
		return
	}
	id := r.PathValue("id")
	if !workspaceID(id) || len(in.Document) == 0 || slices.ContainsFunc(in.Supersedes, func(s draftRef) bool { return s.ID == id || !workspaceID(s.ID) }) {
		workspaceFailure(w, 400, "draft_invalid", "We need a stable draft ID and document here.", "keep_draft", 0)
		return
	}
	if err := st.checkAssets(in.Document); err != nil {
		workspaceFailure(w, 400, "asset_missing", err.Error(), "reimport_asset", 0)
		return
	}
	digest := workspaceDigest([]any{id, in})
	st.locked(w, func(d *workspaceData) {
		if workspaceReplay(w, d, in.OperationID, digest) {
			return
		}
		if old, ok := d.Drafts[id]; ok && in.Sequence <= old.Sequence {
			workspaceFailure(w, 409, "draft_conflict", "There's a newer recovery draft! Keep both versions before sorting it out.", "recover_draft", old.Sequence)
			return
		}
		d.Drafts[id] = workspaceDraft{id, in.Sequence, in.Document, in.Metadata, time.Now().UnixMilli()}
		for _, s := range in.Supersedes {
			if old, ok := d.Drafts[s.ID]; ok && old.Sequence <= s.Sequence {
				delete(d.Drafts, s.ID)
				log.Printf("workspace: draft %s retired, taken over by %s", s.ID, id)
			}
		}
		st.commit(w, d, in.OperationID, digest, map[string]any{"sequence": in.Sequence, "operationId": in.OperationID})
	})
}
func (st *workspaceStore) deleteDraft(w http.ResponseWriter, r *http.Request) {
	var through uint64
	if _, err := fmt.Sscan(r.URL.Query().Get("throughSequence"), &through); err != nil {
		workspaceFailure(w, 400, "sequence_required", "throughSequence is required to avoid deleting a newer draft.", "recover_draft", 0)
		return
	}
	st.locked(w, func(d *workspaceData) {
		id := r.PathValue("id")
		if draft, ok := d.Drafts[id]; !ok || draft.Sequence > through {
			w.WriteHeader(204)
			return
		}
		delete(d.Drafts, id)
		if err := st.save(d); err != nil {
			workspaceFailure(w, 507, "workspace_write_failed", "Oops! Couldn't remove the recovery draft.", "retry_storage", 0)
			return
		}
		w.WriteHeader(204)
	})
}
func (st *workspaceStore) putValue(w http.ResponseWriter, r *http.Request, kind string) {
	var in struct {
		OperationID      string          `json:"operationId"`
		ExpectedRevision uint64          `json:"expectedRevision"`
		Document         json.RawMessage `json:"document"`
	}
	if !workspaceDecode(w, r, &in) {
		return
	}
	if len(in.Document) == 0 {
		workspaceFailure(w, 400, "invalid_document", "document is required.", "keep_draft", 0)
		return
	}
	digest := workspaceDigest([]any{kind, in})
	st.locked(w, func(d *workspaceData) {
		if workspaceReplay(w, d, in.OperationID, digest) {
			return
		}
		v := d.Values[kind]
		if v.Revision != in.ExpectedRevision {
			failure := workspaceError("revision_conflict", "Heads up — someone else changed saved "+kind+". Reload before applying your changes!", "reload_and_reapply", v.Revision)
			failure["document"] = v.Document
			writeJSON(w, 409, failure)
			return
		}
		v = workspaceValue{v.Revision + 1, in.Document}
		d.Values[kind] = v
		if st.commit(w, d, in.OperationID, digest, map[string]any{"revision": v.Revision, "operationId": in.OperationID}) && kind == "session" {
			st.sessionWritten()
		}
	})
}
func (st *workspaceStore) assetPath(key string) string {
	return filepath.Join(st.root, "assets", workspaceDigest(key))
}
func (st *workspaceStore) putAsset(w http.ResponseWriter, r *http.Request) {
	key := r.PathValue("key")
	if key == "" || len(key) > 1024 {
		workspaceFailure(w, 400, "asset_invalid", "We need a valid asset key here.", "reimport_asset", 0)
		return
	}
	dir := filepath.Join(st.root, "assets")
	if err := os.MkdirAll(dir, 0700); err != nil {
		workspaceFailure(w, 507, "asset_write_failed", "Oops, asset storage is unavailable right now.", "free_storage_and_retry", 0)
		return
	}
	f, err := os.CreateTemp(dir, ".asset-*")
	if err != nil {
		workspaceFailure(w, 507, "asset_write_failed", "Oops, couldn't store that file ("+storageCause(err)+").", "free_storage_and_retry", 0)
		return
	}
	defer os.Remove(f.Name())
	hash := sha256.New()
	_, err = io.Copy(io.MultiWriter(f, hash), http.MaxBytesReader(w, r.Body, 512<<20))
	if err == nil {
		err = f.Sync()
	}
	err = errors.Join(err, f.Close())
	var tooLarge *http.MaxBytesError
	var timeout net.Error
	switch {
	case errors.As(err, &tooLarge):
		workspaceFailure(w, 413, "asset_too_large", "Whoa, that model is over 512 MiB!", "reimport_asset", 0)
		return
	case errors.As(err, &timeout) && timeout.Timeout():
		w.Header().Set("Connection", "close")
		workspaceFailure(w, 408, "asset_upload_timeout", "Oops, the model upload stalled out — give it another try!", "retry_same_asset", 0)
		return
	case err != nil:
		workspaceFailure(w, 400, "asset_upload_failed", "Oops, the model upload didn't finish — give it another try!", "retry_same_asset", 0)
		return
	}
	digest := hex.EncodeToString(hash.Sum(nil))
	st.mu.Lock()
	defer st.mu.Unlock()
	path := st.assetPath(key)
	if old, err := os.Open(path); err == nil {
		h := sha256.New()
		_, copyErr := io.Copy(h, old)
		old.Close()
		if copyErr != nil || hex.EncodeToString(h.Sum(nil)) != digest {
			workspaceFailure(w, 409, "asset_conflict", "Heads up — this asset key already holds different bytes. Keep the original key and use a new one for the edited asset!", "reimport_asset", 0)
			return
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		workspaceFailure(w, 503, "asset_unavailable", "Oops! Couldn't check the existing asset.", "retry_storage", 0)
		return
	} else if err = os.Rename(f.Name(), path); err != nil {
		workspaceFailure(w, 507, "asset_write_failed", "Uh-oh, couldn't save that asset.", "free_storage_and_retry", 0)
		return
	}
	if err := syncWorkspaceDirectory(dir); err != nil {
		workspaceFailure(w, 507, "asset_write_failed", "Uh-oh, couldn't sync the asset folder — retry with the exact same bytes!", "retry_same_asset", 0)
		return
	}
	w.Header().Set("X-Content-SHA256", digest)
	writeJSON(w, 200, map[string]string{"key": key, "sha256": digest})
}
func (st *workspaceStore) getAsset(w http.ResponseWriter, r *http.Request) {
	path := st.assetPath(r.PathValue("key"))
	f, err := os.Open(path)
	if err != nil {
		workspaceFailure(w, 404, "asset_missing", "Oops, the original model asset is missing.", "reimport_asset", 0)
		return
	}
	defer f.Close()
	h := sha256.New()
	if _, err = io.Copy(h, f); err != nil {
		workspaceFailure(w, 503, "asset_unavailable", "Oops! Couldn't read the model asset.", "retry_storage", 0)
		return
	}
	sum := hex.EncodeToString(h.Sum(nil))
	w.Header().Set("X-Content-SHA256", sum)
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", privateRevalidate)
	w.Header().Set("ETag", `"`+sum+`"`)
	f.Seek(0, io.SeekStart)
	stat, err := f.Stat()
	if err != nil {
		workspaceFailure(w, 503, "asset_unavailable", "Oops! Couldn't read the model asset.", "retry_storage", 0)
		return
	}
	if serveAssetGzip(w, r, f, stat.Size(), sum) {
		return
	}
	http.ServeContent(w, r, "model", stat.ModTime(), f)
}

func workspaceHasReference(d *workspaceData, id string) bool {
	if d == nil {
		return false
	}
	var contains func(any) bool
	contains = func(v any) bool {
		switch x := v.(type) {
		case string:
			return x == id
		case []any:
			for _, item := range x {
				if contains(item) {
					return true
				}
			}
		case map[string]any:
			for key, item := range x {
				if key == id || contains(item) {
					return true
				}
			}
		}
		return false
	}
	check := func(raw json.RawMessage) bool { var v any; return json.Unmarshal(raw, &v) == nil && contains(v) }
	for _, p := range d.Projects {
		if check(p.Record) {
			return true
		}
	}
	for _, draft := range d.Drafts {
		if check(draft.Document) || check(draft.Metadata) {
			return true
		}
	}
	for _, v := range d.Values {
		if check(v.Document) {
			return true
		}
	}
	for _, raw := range d.Adoptions {
		if check(raw) {
			return true
		}
	}
	return false
}
