package main

import (
	"bufio"
	"bytes"
	"io"
	"net/http"
	"os"
	"strconv"
	"sync"
)

const gcodeBlockLines = 1024

type gcodeIndex struct {
	size   int64
	starts []int64
	total  int
}

var gcodeIndexes = struct {
	sync.Mutex
	m map[string]*gcodeIndex
}{m: map[string]*gcodeIndex{}}

func indexGcode(path string, f *os.File, size int64) (*gcodeIndex, error) {
	gcodeIndexes.Lock()
	ix := gcodeIndexes.m[path]
	gcodeIndexes.Unlock()
	if ix != nil && ix.size == size {
		return ix, nil
	}
	ix = &gcodeIndex{size: size, starts: []int64{0}, total: 1}
	buf := make([]byte, 256<<10)
	var base int64
	for {
		n, err := f.Read(buf)
		for off := 0; ; {
			i := bytes.IndexByte(buf[off:n], '\n')
			if i < 0 {
				break
			}
			off += i + 1
			if ix.total%gcodeBlockLines == 0 {
				ix.starts = append(ix.starts, base+int64(off))
			}
			ix.total++
		}
		base += int64(n)
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
	}
	gcodeIndexes.Lock()
	if len(gcodeIndexes.m) >= 64 {
		clear(gcodeIndexes.m)
	}
	gcodeIndexes.m[path] = ix
	gcodeIndexes.Unlock()
	return ix, nil
}

func (s *Server) gcodeLines(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	if (j.Request.Output != "gcode" && j.Request.Output != "gcode.3mf") || j.AwaitFinished(r.Context()) != StateDone {
		fail(w, 409, "no_finished_g_code")
		return
	}
	path := j.OutputPath("gcode")
	f, err := os.Open(path)
	if err != nil {
		fail(w, 410, "sliced_file_has_expired")
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		fail(w, 500, "gcode_lines_internal", err.Error())
		return
	}
	ix, err := indexGcode(path, f, st.Size())
	if err != nil {
		fail(w, 500, "gcode_lines_internal", err.Error())
		return
	}
	k, err := strconv.Atoi(r.URL.Query().Get("block"))
	if err != nil || k < 0 || k >= len(ix.starts) {
		fail(w, 404, "couldnt_find_g_code")
		return
	}
	if _, err := f.Seek(ix.starts[k], io.SeekStart); err != nil {
		fail(w, 500, "gcode_lines_internal", err.Error())
		return
	}
	lines := make([]string, 0, min(gcodeBlockLines, ix.total-k*gcodeBlockLines))
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64<<10), 16<<20)
	for len(lines) < cap(lines) && sc.Scan() {
		lines = append(lines, sc.Text())
	}
	for len(lines) < cap(lines) {
		lines = append(lines, "")
	}
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	writeJSON(w, 200, map[string]any{"total": ix.total, "lines": lines})
}
