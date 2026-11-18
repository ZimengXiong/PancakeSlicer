package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func loadCatalog(dir string) (*resolve.Catalog, error) {
	files := make([]io.Reader, 3)
	for i, kind := range []string{"printers", "filaments", "prints"} {
		f, err := os.Open(filepath.Join(dir, kind+".jsonl"))
		if err != nil {
			return nil, fmt.Errorf("the flattened catalog is missing (%w); build it with nonplanar_slice_tool --flatten-catalog", err)
		}
		defer f.Close()
		files[i] = f
	}
	return resolve.LoadCatalog(files[0], files[1], files[2])
}

func stringValue(raw json.RawMessage) string { var s string; json.Unmarshal(raw, &s); return s }
func stringList(raw json.RawMessage) []string {
	var list []string
	if json.Unmarshal(raw, &list) == nil && list != nil {
		return list
	}
	if s := stringValue(raw); s != "" {
		return strings.Split(s, ";")
	}
	return []string{}
}

func (s *Server) catalogRoutes(mux *http.ServeMux) {
	lists := map[string]*cachedResponse{}
	for _, kind := range []string{"printers", "filaments", "prints"} {
		var entries any = s.catalog.List(kind)
		if kind == "printers" {
			entries = s.catalogPrinters()
		}
		list, err := cacheJSON(map[string]any{"version": s.catalog.Version, kind: entries})
		if err != nil {
			log.Fatalf("catalog %s: %v", kind, err)
		}
		lists[kind] = list
	}
	mux.HandleFunc("GET /api/catalog/{kind}", func(w http.ResponseWriter, r *http.Request) {
		kind := r.PathValue("kind")
		if lists[kind] == nil {
			fail(w, 404, "unknown_catalog_kind")
			return
		}
		lists[kind].serve(w, r, kind+".json", "application/json", catalogCache, time.Time{})
	})
	mux.HandleFunc("GET /api/catalog/covers/{vendor}/{model}", s.catalogCover)
	mux.HandleFunc("GET /api/catalog/{kind}/{id}", func(w http.ResponseWriter, r *http.Request) {
		line := s.catalog.Line(r.PathValue("kind"), r.PathValue("id"))
		if line == nil {
			fail(w, 404, "unknown_catalog_product")
			return
		}
		etag := `"` + s.catalog.Version + `"`
		w.Header().Set("ETag", etag)
		w.Header().Set("Cache-Control", catalogCache)
		if match := r.Header.Get("If-None-Match"); match == etag {
			w.WriteHeader(http.StatusNotModified)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write(line)
	})
}
