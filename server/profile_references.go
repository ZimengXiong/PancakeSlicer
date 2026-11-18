package main

import (
	"encoding/json"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type droppedProfile struct {
	Name     string
	Archived bool
}

func droppedProfiles(previous *resolve.Library, doc map[string]json.RawMessage) map[string]droppedProfile {
	listed := map[string]bool{}
	for _, list := range []string{"printers", "processes", "filaments"} {
		var records []struct {
			ID string `json:"id"`
		}
		json.Unmarshal(doc[list], &records)
		for _, r := range records {
			listed[r.ID] = true
		}
	}
	out := map[string]droppedProfile{}
	for id, p := range previous.Printers {
		if !listed[id] {
			out[id] = droppedProfile{p.Name, p.Archived != nil}
		}
	}
	for id, p := range previous.Processes {
		if !listed[id] {
			out[id] = droppedProfile{p.Name, p.Archived != nil}
		}
	}
	for id, f := range previous.Filaments {
		if !listed[id] {
			out[id] = droppedProfile{f.Name, f.Archived != nil}
		}
	}
	for alias, id := range previous.Alias {
		if d, ok := out[id]; ok {
			out[alias] = d
		}
	}
	return out
}

func slotUser(doc map[string]json.RawMessage, dropped map[string]droppedProfile) (printer string, profile droppedProfile) {
	var printers []struct {
		Name  string `json:"name"`
		Slots []struct {
			Filament string `json:"filament"`
		} `json:"slots"`
	}
	json.Unmarshal(doc["printers"], &printers)
	for _, p := range printers {
		for _, slot := range p.Slots {
			if d, ok := dropped[slot.Filament]; ok {
				return p.Name, d
			}
		}
	}
	return "", droppedProfile{}
}

func dropCalibrations(doc map[string]json.RawMessage, dropped map[string]droppedProfile) {
	var calibrations []json.RawMessage
	if json.Unmarshal(doc["calibrations"], &calibrations) != nil {
		return
	}
	kept := []json.RawMessage{}
	for _, raw := range calibrations {
		var c struct {
			Filament string `json:"filament"`
			Printer  string `json:"printer"`
		}
		if json.Unmarshal(raw, &c) == nil && (dropped[c.Filament].Name != "" || dropped[c.Printer].Name != "") {
			continue
		}
		kept = append(kept, raw)
	}
	if len(kept) != len(calibrations) {
		doc["calibrations"], _ = json.Marshal(kept)
	}
}

func (st *workspaceStore) profileReference(ids map[string]droppedProfile) (owner string, profile droppedProfile, err error) {
	st.mu.Lock()
	defer st.mu.Unlock()
	d, err := st.read()
	if err != nil {
		return "", droppedProfile{}, err
	}
	type document struct {
		Name string                `json:"name"`
		Meta struct{ Name string } `json:"meta"`
		Doc  struct {
			Printer, Process any
			FilamentSlots    []any `json:"filamentSlots"`
		} `json:"doc"`
	}
	selects := func(raw json.RawMessage) (string, droppedProfile) {
		var doc document
		json.Unmarshal(raw, &doc)
		for _, key := range append([]any{doc.Doc.Printer, doc.Doc.Process}, doc.Doc.FilamentSlots...) {
			if id, ok := key.(string); ok && ids[id].Name != "" {
				return doc.Meta.Name + doc.Name, ids[id]
			}
		}
		return "", droppedProfile{}
	}
	for _, p := range d.Projects {
		if name, profile := selects(p.Record); profile.Name != "" {
			return name, profile, nil
		}
	}
	for _, v := range d.Drafts {
		if name, profile := selects(v.Document); profile.Name != "" {
			return name + " (draft)", profile, nil
		}
	}
	return "", droppedProfile{}, nil
}
