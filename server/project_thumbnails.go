package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"strings"
	"sync"
	"time"
)

const thumbnailRoute = "/api/project-thumbnails/"

func thumbnailVersion(data string) string {
	sum := sha256.Sum256([]byte(data))
	return hex.EncodeToString(sum[:6])
}

func thumbnailURL(id, data string) string {
	return thumbnailRoute + id + "?v=" + thumbnailVersion(data)
}

func thumbnailImage(record json.RawMessage) (data, contentType string, image []byte, ok bool) {
	var r struct {
		Meta struct {
			Thumbnail *string `json:"thumbnail"`
		} `json:"meta"`
	}
	if json.Unmarshal(record, &r) != nil || r.Meta.Thumbnail == nil {
		return "", "", nil, false
	}
	contentType, image, ok = decodeThumbnail(*r.Meta.Thumbnail)
	return *r.Meta.Thumbnail, contentType, image, ok
}

func decodeThumbnail(data string) (contentType string, image []byte, ok bool) {
	header, payload, found := strings.Cut(data, ",")
	contentType, isBase64 := strings.CutSuffix(strings.TrimPrefix(header, "data:"), ";base64")
	if !found || !strings.HasPrefix(header, "data:image/") || !isBase64 {
		return "", nil, false
	}
	image, err := base64.StdEncoding.DecodeString(payload)
	return contentType, image, err == nil
}

func rememberThumbnail(id, data string) {
	key := id + "\x00" + thumbnailVersion(data)
	thumbnails.Lock()
	_, ok := thumbnails.m[key]
	thumbnails.Unlock()
	if ok {
		return
	}
	if contentType, image, decoded := decodeThumbnail(data); decoded {
		storeThumbnail(key, thumbnailEntry{contentType, image})
	}
}

func storeThumbnail(key string, e thumbnailEntry) {
	thumbnails.Lock()
	defer thumbnails.Unlock()
	if len(thumbnails.m) >= 1024 {
		thumbnails.m = map[string]thumbnailEntry{}
	}
	thumbnails.m[key] = e
}

type thumbnailEntry struct {
	contentType string
	image       []byte
}

var thumbnails = struct {
	sync.Mutex
	m map[string]thumbnailEntry
}{m: map[string]thumbnailEntry{}}

func serveThumbnail(w http.ResponseWriter, r *http.Request, version string, e thumbnailEntry) {
	if r.URL.Query().Get("v") == version {
		w.Header().Set("Cache-Control", "private, max-age=31536000, immutable")
	} else {
		w.Header().Set("Cache-Control", "private, no-cache")
	}
	w.Header().Set("Content-Type", e.contentType)
	w.Header().Set("ETag", `"`+version+`"`)
	http.ServeContent(w, r, "", time.Time{}, bytes.NewReader(e.image))
}

func (st *workspaceStore) thumbnailRoutes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/project-thumbnails/{id}", func(w http.ResponseWriter, r *http.Request) {
		if v := r.URL.Query().Get("v"); v != "" {
			thumbnails.Lock()
			e, ok := thumbnails.m[r.PathValue("id")+"\x00"+v]
			thumbnails.Unlock()
			if ok {
				serveThumbnail(w, r, v, e)
				return
			}
		}
		var data, contentType string
		var image []byte
		found, read := false, false
		st.locked(w, func(d *workspaceData) {
			read = true
			id, err := canonicalWorkspaceProject(d, r.PathValue("id"))
			if p, ok := d.Projects[id]; err == nil && ok && p.Record != nil {
				data, contentType, image, found = thumbnailImage(p.Record)
			}
		})
		if !read {
			return
		}
		if !found {
			workspaceFailure(w, 404, "thumbnail_missing", "This project doesn't have a thumbnail yet.", "open_projects", 0)
			return
		}
		version := thumbnailVersion(data)
		e := thumbnailEntry{contentType, image}
		storeThumbnail(r.PathValue("id")+"\x00"+version, e)
		serveThumbnail(w, r, version, e)
	})
}
