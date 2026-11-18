package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

const optionsWireSchema = "orca.options.v2"

func registerOptions(mux *http.ServeMux, dump *cachedResponse, catalog *resolve.Catalog, devReset bool) {
	var defs map[string]map[string]json.RawMessage
	if err := json.Unmarshal(dump.plain, &defs); err != nil {
		panic("option dictionary: " + err.Error())
	}
	tips := map[string]string{}
	for key, def := range defs {
		var tip string
		if json.Unmarshal(def["tooltip"], &tip) == nil && tip != "" {
			tips[key] = tip
		}
		delete(def, "tooltip")
		def["records"], _ = json.Marshal(catalog.RecordKinds(key))
	}
	tipsBody, _ := json.Marshal(tips)
	sum := sha256.Sum256(tipsBody)
	version := hex.EncodeToString(sum[:6])
	wire := map[string]any{"schema": optionsWireSchema, "tooltips": "/api/options/tooltips?v=" + version, "options": defs}
	if devReset {
		wire["devReset"] = true
	}
	body, err := cacheJSON(wire)
	if err != nil {
		panic("option dictionary: " + err.Error())
	}
	tooltips := cacheResponse(append(tipsBody, '\n'))
	mux.HandleFunc("GET /api/options", func(w http.ResponseWriter, r *http.Request) {
		body.serve(w, r, "options.json", "application/json", catalogCache, time.Time{})
	})
	mux.HandleFunc("GET /api/options/tooltips", func(w http.ResponseWriter, r *http.Request) {
		policy := catalogCache
		if r.URL.Query().Get("v") == version {
			policy = "public, max-age=31536000, immutable"
		}
		tooltips.serve(w, r, "tooltips.json", "application/json", policy, time.Time{})
	})
}
