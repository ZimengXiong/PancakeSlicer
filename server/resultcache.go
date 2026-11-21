package main

import (
	"container/list"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

type ResultCache struct {
	mu                      sync.Mutex
	root, artifact          string
	limit, bytes            int64
	lru                     *list.List
	entries                 map[string]*list.Element
	hits, misses, evictions uint64
}
type cachedResult struct {
	key   string
	bytes int64
}
type ResultCacheStats struct {
	ArtifactSHA string `json:"artifact_sha"`
	Entries     int    `json:"entries"`
	Bytes       int64  `json:"bytes"`
	MaxBytes    int64  `json:"max_bytes"`
	Hits        uint64 `json:"hits"`
	Misses      uint64 `json:"misses"`
	Evictions   uint64 `json:"evictions"`
}

func NewResultCache(root string, maxBytes int64, artifactSHA string) (*ResultCache, error) {
	if artifactSHA == "" || maxBytes <= 0 {
		return nil, errors.New("result cache needs artifact SHA and positive byte limit")
	}
	if err := os.MkdirAll(root, 0700); err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil, err
	}
	for _, e := range entries {
		if err := os.RemoveAll(filepath.Join(root, e.Name())); err != nil {
			return nil, err
		}
	}
	return &ResultCache{root: root, artifact: artifactSHA, limit: maxBytes, lru: list.New(), entries: make(map[string]*list.Element)}, nil
}

func (c *ResultCache) Key(inputDir string, effectiveConfig []byte) (string, error) {
	h := sha256.New()
	field := func(b []byte) { _ = binary.Write(h, binary.LittleEndian, uint64(len(b))); _, _ = h.Write(b) }
	field([]byte("orca-result-v1"))
	field([]byte(c.artifact))
	field(effectiveConfig)
	err := filepath.WalkDir(inputDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("nonregular cache input: %s", path)
		}
		relative, err := filepath.Rel(inputDir, path)
		if err != nil {
			return err
		}
		field([]byte(filepath.ToSlash(relative)))
		f, err := os.Open(path)
		if err != nil {
			return err
		}
		sum := sha256.New()
		_, readErr := io.Copy(sum, f)
		closeErr := f.Close()
		if err := errors.Join(readErr, closeErr); err != nil {
			return err
		}
		field(sum.Sum(nil))
		return nil
	})
	if err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func (c *ResultCache) Stats() ResultCacheStats {
	c.mu.Lock()
	defer c.mu.Unlock()
	return ResultCacheStats{c.artifact, len(c.entries), c.bytes, c.limit, c.hits, c.misses, c.evictions}
}
func validResultKey(key string) bool {
	b, err := hex.DecodeString(key)
	return err == nil && len(b) == sha256.Size && hex.EncodeToString(b) == key
}
func (c *ResultCache) evict(e *list.Element) error {
	entry := e.Value.(cachedResult)
	if err := os.RemoveAll(filepath.Join(c.root, entry.key)); err != nil {
		return err
	}
	c.bytes -= entry.bytes
	delete(c.entries, entry.key)
	c.lru.Remove(e)
	c.evictions++
	return nil
}

func (c *ResultCache) Restore(key, jobDir string) (json.RawMessage, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e := c.entries[key]
	if e == nil {
		c.misses++
		return nil, false
	}
	dir := filepath.Join(c.root, key)
	raw, err := os.ReadFile(filepath.Join(dir, "record.json"))
	if err == nil && json.Valid(raw) {
		err = copyResultFiles(dir, jobDir)
	} else if err == nil {
		err = errors.New("invalid cached record")
	}
	if err != nil {
		for _, name := range resultPaths {
			_ = os.RemoveAll(filepath.Join(jobDir, name))
		}
		_ = os.MkdirAll(filepath.Join(jobDir, "out"), 0700)
		_ = c.evict(e)
		c.misses++
		return nil, false
	}
	c.lru.MoveToFront(e)
	c.hits++
	return json.RawMessage(raw), true
}

func (c *ResultCache) Store(key, jobDir string, record json.RawMessage) error {
	if !validResultKey(key) || !json.Valid(record) {
		return errors.New("invalid result cache key or record")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if e := c.entries[key]; e != nil {
		c.lru.MoveToFront(e)
		return nil
	}
	size := int64(len(record))
	for _, name := range resultPaths {
		err := filepath.WalkDir(filepath.Join(jobDir, name), func(path string, d fs.DirEntry, err error) error {
			if errors.Is(err, os.ErrNotExist) && path == filepath.Join(jobDir, name) {
				return nil
			}
			if err != nil {
				return err
			}
			if d.IsDir() {
				return nil
			}
			st, err := d.Info()
			if err != nil {
				return err
			}
			if !st.Mode().IsRegular() {
				return errors.New("nonregular result file")
			}
			if st.Size() > c.limit-size {
				return errors.New("result exceeds cache byte limit")
			}
			size += st.Size()
			return nil
		})
		if err != nil {
			return err
		}
	}
	if size > c.limit {
		return errors.New("result exceeds cache byte limit")
	}
	for c.bytes > c.limit-size {
		if err := c.evict(c.lru.Back()); err != nil {
			return err
		}
	}
	tmp, err := os.MkdirTemp(c.root, ".pending-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	if err := copyResultFiles(jobDir, tmp); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(tmp, "record.json"), record, 0600); err != nil {
		return err
	}
	if err := os.Rename(tmp, filepath.Join(c.root, key)); err != nil {
		return err
	}
	c.entries[key] = c.lru.PushFront(cachedResult{key, size})
	c.bytes += size
	return nil
}

var resultPaths = []string{"out", "moves.bin", "moves-extended.bin", "exploded"}

func copyResultFiles(src, dst string) error {
	for _, name := range resultPaths {
		base := filepath.Join(src, name)
		err := filepath.WalkDir(base, func(path string, d fs.DirEntry, err error) error {
			if errors.Is(err, os.ErrNotExist) && path == base {
				return nil
			}
			if err != nil {
				return err
			}
			relative, err := filepath.Rel(src, path)
			if err != nil {
				return err
			}
			target := filepath.Join(dst, relative)
			if d.IsDir() {
				return os.MkdirAll(target, 0700)
			}
			st, err := d.Info()
			if err != nil {
				return err
			}
			if !st.Mode().IsRegular() {
				return errors.New("nonregular result file")
			}
			in, err := os.Open(path)
			if err != nil {
				return err
			}
			out, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
			if err != nil {
				_ = in.Close()
				return err
			}
			_, copyErr := io.Copy(out, in)
			return errors.Join(copyErr, out.Close(), in.Close())
		})
		if err != nil {
			return err
		}
	}
	return nil
}

type managerResultCache struct {
	cache     *ResultCache
	toolInfo  os.FileInfo
	resources string
}

var resultCaches = struct {
	sync.Mutex
	managers map[*JobManager]*managerResultCache
}{managers: make(map[*JobManager]*managerResultCache)}

func init() {
	jobResultGet = restoreJobResult
	jobResultPut = storeJobResult
	jobResultWarm = func(m *JobManager) {
		if _, err := cacheForManager(m); err != nil {
			log.Printf("result cache unavailable: %v", err)
		}
		_ = os.RemoveAll(filepath.Join(m.cfg.JobRoot, "preset-cache"))
		if current := stageCacheDir(m); current != "" {
			entries, _ := os.ReadDir(filepath.Dir(current))
			for _, e := range entries {
				if e.Name() != filepath.Base(current) {
					_ = os.RemoveAll(filepath.Join(filepath.Dir(current), e.Name()))
				}
			}
		}
	}
	jobResultStats = func(m *JobManager) any {
		resultCaches.Lock()
		state := resultCaches.managers[m]
		resultCaches.Unlock()
		if state == nil {
			return map[string]any{"enabled": envInt("ORCA_WEB_RESULT_CACHE_BYTES", 512<<20) > 0, "entries": 0, "hits": 0, "misses": 0}
		}
		return state.cache.Stats()
	}
}

func cacheForManager(m *JobManager) (*managerResultCache, error) {
	limit := int64(envInt("ORCA_WEB_RESULT_CACHE_BYTES", 512<<20))
	if limit <= 0 {
		return nil, nil
	}
	info, err := os.Stat(m.cfg.Tool)
	if err != nil {
		return nil, err
	}
	resultCaches.Lock()
	defer resultCaches.Unlock()
	state := resultCaches.managers[m]
	if state != nil && os.SameFile(info, state.toolInfo) && info.Size() == state.toolInfo.Size() && info.ModTime().Equal(state.toolInfo.ModTime()) {
		return state, nil
	}
	tool, err := os.Open(m.cfg.Tool)
	if err != nil {
		return nil, err
	}
	hash := sha256.New()
	_, readErr := io.Copy(hash, tool)
	closeErr := tool.Close()
	if err := errors.Join(readErr, closeErr); err != nil {
		return nil, err
	}
	artifact := hex.EncodeToString(hash.Sum(nil))
	resourceHasher := &ResultCache{artifact: "resources-v1"}
	resourceSHA, err := resourceHasher.Key(m.cfg.Resources, nil)
	if err != nil {
		return nil, err
	}
	cache, err := NewResultCache(filepath.Join(m.cfg.JobRoot, "result-cache"), limit, artifact)
	if err != nil {
		return nil, err
	}
	state = &managerResultCache{cache, info, resourceSHA}
	resultCaches.managers[m] = state
	return state, nil
}

func stageCacheDir(m *JobManager) string {
	state, err := cacheForManager(m)
	if err != nil || state == nil {
		return ""
	}
	return filepath.Join(m.cfg.JobRoot, "stage-cache", state.cache.artifact[:16])
}

func stageCacheArgs(m *JobManager) []string {
	dir := stageCacheDir(m)
	if dir == "" {
		return nil
	}
	return []string{"--stage-cache", dir, "--stage-cache-max-mb", strconv.FormatUint(stageCacheMaxMB(), 10)}
}

func stageCacheMaxMB() uint64 {
	if v, err := strconv.ParseUint(strings.TrimSpace(os.Getenv("PANCAKE_STAGE_CACHE_MAX_MB")), 10, 64); err == nil && v > 0 {
		return v
	}
	return 1024
}

func cacheableJob(m *JobManager, j *Job) bool {
	r := j.Request
	if r.SupportFins || r.Orient || r.Explode || r.Cut != "" || r.MeshOp != "" || r.Arrange || r.WipeTower || overlayOutput(r.Output) {
		return false
	}
	heads, err := os.ReadDir(userHeadDir(m.cfg))
	return (err == nil && len(heads) == 0) || errors.Is(err, os.ErrNotExist)
}

func restoreJobResult(m *JobManager, j *Job, args []string) bool {
	if !cacheableJob(m, j) {
		return false
	}
	state, err := cacheForManager(m)
	if err != nil {
		log.Printf("result cache unavailable: %v", err)
		return false
	}
	if state == nil {
		return false
	}
	args = keyArgs(j, args)
	normalized := make([]string, len(args))
	for i, arg := range args {
		normalized[i] = strings.ReplaceAll(arg, j.Dir, "<job>")
	}
	effective, err := json.Marshal(struct {
		Args      []string
		Resources string
	}{normalized, state.resources})
	if err != nil {
		return false
	}
	key, err := state.cache.Key(filepath.Join(j.Dir, "input"), effective)
	if err != nil {
		log.Printf("result cache hash: %v", err)
		return false
	}
	marker := state.cache.artifact + "\n" + key
	if err := os.WriteFile(filepath.Join(j.Dir, ".result-cache-key"), []byte(marker), 0600); err != nil {
		return false
	}
	raw, hit := state.cache.Restore(key, j.Dir)
	if !hit {
		return false
	}
	raw = bindNativeRecord(raw, m.cfg, j)
	j.mu.Lock()
	code := 0
	j.slice, j.exitCode = raw, &code
	j.mu.Unlock()
	j.log("server", "progress cached 100")
	j.setState(StateDone, "")
	return true
}

func storeJobResult(m *JobManager, j *Job, _ []string, raw []byte) {
	if !cacheableJob(m, j) {
		return
	}
	j.mu.Lock()
	cancelled := j.stopReason != "" || j.state != StateStoring
	j.mu.Unlock()
	if cancelled {
		return
	}
	state, err := cacheForManager(m)
	if err != nil || state == nil {
		return
	}
	marker, err := os.ReadFile(filepath.Join(j.Dir, ".result-cache-key"))
	prefix := state.cache.artifact + "\n"
	if err != nil || !strings.HasPrefix(string(marker), prefix) {
		return
	}
	if len(raw) == 0 {
		raw = []byte("null")
	}
	started := time.Now()
	if err := state.cache.Store(strings.TrimPrefix(string(marker), prefix), j.Dir, raw); err != nil {
		log.Printf("result cache store bypassed: %v", err)
	} else {
		log.Printf("result cache stored in %s", time.Since(started))
	}
}
