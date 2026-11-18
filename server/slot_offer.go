package main

import (
	"cmp"
	"encoding/json"
	"fmt"
	"net/http"
	"reflect"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func documentStrip(doc map[string]json.RawMessage) *resolve.SlotStrip {
	var slots []any
	if json.Unmarshal(doc["filamentSlots"], &slots) == nil && slots != nil {
		out := resolve.SlotStrip{Filaments: make([]string, len(slots)), Mixes: map[string]json.RawMessage{}}
		for i, v := range slots {
			out.Filaments[i] = fmt.Sprint(v)
			if v == nil {
				out.Filaments[i] = "null"
			}
		}
		json.Unmarshal(doc["slotColours"], &out.Colours)
		json.Unmarshal(doc["mixes"], &out.Mixes)
		return &out
	}
	var printer string
	var snapshots struct {
		Printer *resolve.Printer `json:"printer"`
		Slots   []resolve.Slot   `json:"slots"`
	}
	json.Unmarshal(doc["printer"], &printer)
	if json.Unmarshal(doc["profileSnapshots"], &snapshots) != nil || snapshots.Printer == nil || snapshots.Printer.ID != printer {
		return nil
	}
	record := *snapshots.Printer
	if snapshots.Slots != nil {
		record.Slots = snapshots.Slots
	}
	s := resolve.PrinterStrip(&record)
	return &s
}

func projectDocument(d *workspaceData, project string) map[string]json.RawMessage {
	p, ok := d.Projects[project]
	if !ok || p.Archived || p.Record == nil {
		return nil
	}
	var record struct {
		Doc map[string]json.RawMessage `json:"doc"`
	}
	if json.Unmarshal(p.Record, &record) != nil {
		return nil
	}
	return record.Doc
}

func openedDocument(d *workspaceData, project string) (doc map[string]json.RawMessage, recovered string) {
	doc = projectDocument(d, project)
	if doc == nil {
		return nil, ""
	}
	type draftDoc struct {
		ProjectID string                     `json:"projectId"`
		Base      uint64                     `json:"baseRevision"`
		Failure   *struct {
			Kind string `json:"kind"`
		} `json:"failure"`
		Kind      string                     `json:"kind"`
		EditedAt  float64                    `json:"editedAt"`
		UpdatedAt float64                    `json:"updatedAt"`
		Doc       map[string]json.RawMessage `json:"doc"`
	}
	var newest *draftDoc
	var newestID string
	var newestSeq uint64
	for _, dr := range d.Drafts {
		var x draftDoc
		if json.Unmarshal(dr.Document, &x) != nil || x.ProjectID != project || x.Kind != "dirty" || x.Doc == nil {
			continue
		}
		edited := cmp.Or(x.EditedAt, x.UpdatedAt)
		if newest == nil || edited > cmp.Or(newest.EditedAt, newest.UpdatedAt) {
			newest, newestID, newestSeq = &x, dr.ID, dr.Sequence
		}
	}
	if newest == nil {
		return doc, ""
	}
	var checkpoint struct {
		ID       string `json:"id"`
		Sequence uint64 `json:"sequence"`
	}
	if json.Unmarshal(doc["recoveryCheckpoint"], &checkpoint) == nil && checkpoint.ID == newestID && checkpoint.Sequence >= newestSeq {
		return doc, ""
	}
	if sameDocument(newest.Doc, doc) {
		return doc, ""
	}
	if d.Projects[project].Revision != newest.Base || newest.Failure != nil && newest.Failure.Kind == "conflict" {
		return newest.Doc, "conflict"
	}
	return newest.Doc, "interrupted"
}

func documentPrinter(doc map[string]json.RawMessage, selected string) string {
	var printer string
	if json.Unmarshal(doc["printer"], &printer) != nil || printer == "" {
		return selected
	}
	return printer
}

func (s *Server) projectOffer(lib *resolve.Library, doc map[string]json.RawMessage, printer string) *resolve.SlotOffer {
	strip := documentStrip(doc)
	if strip == nil {
		return nil
	}
	in := resolve.SlotOfferRequest{Printer: printer, Strip: *strip}
	json.Unmarshal(doc["projectSlotValues"], &in.Values)
	json.Unmarshal(doc["filamentEdits"], &in.Edits)
	o := s.library.catalog.SlotOfferFor(lib, in)
	if !o.Shown() {
		return nil
	}
	return &o
}

func (s *Server) postSlotOffer(w http.ResponseWriter, r *http.Request) {
	var in resolve.SlotOfferRequest
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<20)).Decode(&in) != nil || len(in.Strip.Filaments) > 256 || len(in.Values) > 256 {
		fail(w, 400, "expected_printer_strip_filaments")
		return
	}
	lib, err := s.libraryDocument()
	if err != nil {
		fail(w, 503, "profile_library_isnt_available", storageCause(err))
		return
	}
	writeJSON(w, 200, s.library.catalog.SlotOfferFor(lib, in))
}

func sameDocument(a, b map[string]json.RawMessage) bool {
	value := func(m map[string]json.RawMessage) any {
		out := map[string]any{}
		for k, v := range m {
			if k == "recoveryCheckpoint" {
				continue
			}
			var x any
			json.Unmarshal(v, &x)
			out[k] = x
		}
		return out
	}
	return reflect.DeepEqual(value(a), value(b))
}
