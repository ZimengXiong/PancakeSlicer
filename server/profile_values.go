package main

import "encoding/json"

func must(b []byte, _ error) []byte { return b }

func optionDefaults(dictionary []byte) map[string]json.RawMessage {
	var defs map[string]struct {
		Default json.RawMessage `json:"default"`
	}
	json.Unmarshal(dictionary, &defs)
	out := map[string]json.RawMessage{}
	for key, def := range defs {
		out[key] = def.Default
	}
	return out
}
