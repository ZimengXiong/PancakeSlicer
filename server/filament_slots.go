package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type slotTray struct {
	Type       string `json:"type"`
	Colour     string `json:"colour"`
	Name       string `json:"name"`
	FilamentID string `json:"filament_id"`
}

type slotReport struct {
	Slot      string   `json:"slot"`
	Label     string   `json:"label"`
	Tray      slotTray `json:"tray"`
	Remaining *int     `json:"remaining"`
	Spool     *string  `json:"spool"`
	Earlier   *string  `json:"earlier,omitempty"`
}

type printerSlots struct {
	Online  bool         `json:"online"`
	Updated time.Time    `json:"updated"`
	Slots   []slotReport `json:"slots"`
	Error   string       `json:"error,omitempty"`
}

type slotChoice struct {
	Filament string    `json:"filament"`
	Type     string    `json:"type"`
	Colour   string    `json:"colour"`
	At       time.Time `json:"at"`
}

type spoolChoice struct {
	Filament string    `json:"filament"`
	At       time.Time `json:"at"`
}

type slotDoc struct {
	Schema int                      `json:"schema"`
	Last   map[string]*printerSlots `json:"last"`
	Spools map[string]spoolChoice   `json:"spools"`
}

const slotDocSchema = 2

type legacySlotDoc struct {
	Chosen map[string]map[string]slotChoice `json:"chosen"`
}

type matchedSlotDoc struct {
	Last map[string]struct {
		Slots []struct {
			Slot     string  `json:"slot"`
			Filament *string `json:"filament"`
			Rule     string  `json:"rule"`
		} `json:"slots"`
	} `json:"last"`
}

type filamentSlotStore struct {
	mu   sync.Mutex
	path string
	doc  *slotDoc
}

func newFilamentSlotStore(dir string) *filamentSlotStore {
	return &filamentSlotStore{path: filepath.Join(dir, "printer-slots.json")}
}

func (st *filamentSlotStore) load() (*slotDoc, error) {
	if st.doc != nil {
		return st.doc, nil
	}
	doc := &slotDoc{Schema: slotDocSchema, Last: map[string]*printerSlots{}, Spools: map[string]spoolChoice{}}
	b, err := os.ReadFile(st.path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	if err == nil {
		var head struct {
			Schema int                    `json:"schema"`
			Spools map[string]spoolChoice `json:"spools"`
		}
		if json.Unmarshal(b, &head) != nil || head.Schema != 1 && head.Schema != slotDocSchema {
			return nil, fmt.Errorf("%s: couldn't read printer slot configuration.", st.path)
		}
		if head.Schema == slotDocSchema && json.Unmarshal(b, doc) != nil {
			return nil, fmt.Errorf("%s: couldn't read printer slot configuration.", st.path)
		}
		if head.Spools != nil {
			doc.Spools = head.Spools
		}
		if doc.Last == nil {
			doc.Last = map[string]*printerSlots{}
		}
	}
	st.doc = doc
	return doc, nil
}

func (st *filamentSlotStore) save() error {
	b, err := json.MarshalIndent(st.doc, "", " ")
	if err != nil {
		return err
	}
	return writeFileAtomic(st.path, b)
}

func slotID(t AMSTray) string {
	if t.Kind == "external" {
		if t.Extruder != nil {
			return map[bool]string{true: "ext.L", false: "ext.R"}[*t.Extruder == 0]
		}
		return "ext"
	}
	return fmt.Sprintf("ams%d.%d", t.AMS, t.Slot)
}

func slotLabel(t AMSTray, units int) string {
	switch {
	case t.Kind == "external" && t.Extruder != nil:
		return map[bool]string{true: "Left external spool", false: "Right external spool"}[*t.Extruder == 0]
	case t.Kind == "external":
		return "External spool"
	case t.Kind == "ams-ht":
		return fmt.Sprintf("AMS HT %d", t.AMS-127)
	case units > 1:
		return fmt.Sprintf("AMS %d slot %d", t.AMS+1, t.Slot+1)
	}
	return fmt.Sprintf("Slot %d", t.Slot+1)
}

func earlierChoice(lib *resolve.Library, spools map[string]spoolChoice, spool *string) *string {
	if spool == nil {
		return nil
	}
	if c, ok := spools[*spool]; ok {
		if f := lib.Filament(c.Filament); f != nil && f.Live() {
			id := f.ID
			return &id
		}
	}
	return nil
}

func (s *Server) printerSlotsFor(key string, lib *resolve.Library) (*printerSlots, error) {
	st, ok := s.sends.hub.current(key)
	s.filamentSlots.mu.Lock()
	defer s.filamentSlots.mu.Unlock()
	doc, err := s.filamentSlots.load()
	if err != nil {
		return nil, err
	}
	var out *printerSlots
	if !ok || !st.Online || st.Trays == nil {
		out = &printerSlots{Slots: []slotReport{}}
		if last := doc.Last[key]; last != nil {
			out.Updated, out.Slots = last.Updated, append([]slotReport{}, last.Slots...)
		}
	} else {
		units := map[int]bool{}
		for _, t := range st.Trays {
			if t.Kind != "external" && t.Kind != "ams-ht" {
				units[t.AMS] = true
			}
		}
		out = &printerSlots{Online: true, Slots: []slotReport{}}
		for _, t := range st.Trays {
			r := slotReport{Slot: slotID(t), Label: slotLabel(t, len(units)), Tray: slotTray{Type: t.Type, Colour: t.Color, Name: t.Name, FilamentID: t.FilamentID}, Remaining: t.Remaining}
			if t.Spool != "" {
				spool := t.Spool
				r.Spool = &spool
			}
			if r.Remaining != nil && *r.Remaining < 0 {
				r.Remaining = nil
			}
			out.Slots = append(out.Slots, r)
		}
		prev := doc.Last[key]
		a, _ := json.Marshal(out.Slots)
		var b []byte
		if prev != nil {
			b, _ = json.Marshal(prev.Slots)
		}
		if prev == nil || string(a) != string(b) {
			out.Updated = time.Now().UTC()
			doc.Last[key] = &printerSlots{Updated: out.Updated, Slots: out.Slots}
			if err := s.filamentSlots.save(); err != nil {
				return nil, err
			}
		} else {
			out.Updated = prev.Updated
		}
		out.Slots = append([]slotReport{}, out.Slots...)
	}
	for i := range out.Slots {
		out.Slots[i].Earlier = earlierChoice(lib, doc.Spools, out.Slots[i].Spool)
	}
	return out, nil
}

func (s *Server) printerSlotRoutes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/printer-slots", func(w http.ResponseWriter, r *http.Request) {
		lib, err := s.libraryDocument()
		if err != nil {
			fail(w, 503, "profile_library_isnt_available", storageCause(err))
			return
		}
		out := map[string]*printerSlots{}
		for _, p := range s.sends.connections() {
			if p.Printer == "" || out[p.Printer] != nil {
				continue
			}
			slots, err := s.printerSlotsFor(p.Printer, lib)
			if err != nil {
				slots = &printerSlots{Slots: []slotReport{}, Error: err.Error()}
			}
			out[p.Printer] = slots
		}
		writeJSON(w, 200, map[string]any{"printers": out})
	})
	mux.HandleFunc("GET /api/printer-slots/{printer}", func(w http.ResponseWriter, r *http.Request) {
		lib, err := s.libraryDocument()
		if err != nil {
			fail(w, 503, "profile_library_isnt_available", storageCause(err))
			return
		}
		slots, err := s.printerSlotsFor(r.PathValue("printer"), lib)
		if err != nil {
			fail(w, 409, "filament_slots_conflict", err.Error())
			return
		}
		writeJSON(w, 200, slots)
	})
	choose := func(w http.ResponseWriter, r *http.Request, filament string) {
		key, slot := r.PathValue("printer"), r.PathValue("slot")
		lib, err := s.libraryDocument()
		if err != nil {
			fail(w, 503, "profile_library_isnt_available", storageCause(err))
			return
		}
		if f := lib.Filament(filament); filament != "" && (f == nil || !f.Live()) {
			fail(w, 400, "pick_filament_library")
			return
		}
		current, err := s.printerSlotsFor(key, lib)
		if err != nil {
			fail(w, 409, "filament_slots_conflict", err.Error())
			return
		}
		var tray *slotReport
		for i := range current.Slots {
			if current.Slots[i].Slot == slot {
				tray = &current.Slots[i]
			}
		}
		if tray != nil && tray.Spool != nil {
			s.filamentSlots.mu.Lock()
			doc, err := s.filamentSlots.load()
			if err == nil {
				if filament == "" {
					delete(doc.Spools, *tray.Spool)
				} else {
					doc.Spools[*tray.Spool] = spoolChoice{Filament: filament, At: time.Now().UTC()}
				}
				err = s.filamentSlots.save()
			}
			s.filamentSlots.mu.Unlock()
			if err != nil {
				fail(w, 500, "filament_slots_internal", err.Error())
				return
			}
			if lib, err = s.libraryDocument(); err == nil {
				current, err = s.printerSlotsFor(key, lib)
			}
			if err != nil {
				fail(w, 409, "filament_slots_conflict", err.Error())
				return
			}
		}
		writeJSON(w, 200, current)
	}
	mux.HandleFunc("PUT /api/printer-slots/{printer}/{slot}", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Filament string `json:"filament"`
		}
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&body); err != nil || body.Filament == "" {
			fail(w, 400, "send_filament")
			return
		}
		choose(w, r, body.Filament)
	})
	mux.HandleFunc("DELETE /api/printer-slots/{printer}/{slot}", func(w http.ResponseWriter, r *http.Request) { choose(w, r, "") })
}
