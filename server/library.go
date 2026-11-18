package main

import (
	"bytes"
	"cmp"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"maps"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

var librarySchema = schemas.Library

type libraryStore struct {
	dir string
	mu  sync.Mutex
	settled bool
	swap func(variant string) (string, []string)
	extruders func(variant string) int
	toolheads func(vendor, model string, nozzle float64) int
	catalog *resolve.Catalog
	parsed     *resolve.Library
	parsedSize int64
	parsedTime time.Time
	watch   sync.Mutex
	changed chan struct{}
}

func (l *libraryStore) written() {
	l.watch.Lock()
	if l.changed != nil {
		close(l.changed)
	}
	l.changed = make(chan struct{})
	l.watch.Unlock()
}

func (l *libraryStore) wait() <-chan struct{} {
	l.watch.Lock()
	defer l.watch.Unlock()
	if l.changed == nil {
		l.changed = make(chan struct{})
	}
	return l.changed
}

type libraryEnvelope struct {
	Document   map[string]json.RawMessage `json:"document"`
	Operations map[string]libraryReceipt  `json:"operations"`
	Migrations []libraryMigration         `json:"migrations,omitempty"`
	Writer *libraryWriter `json:"writer,omitempty"`
}

type libraryWriter struct {
	Tables  resolve.Tables `json:"tables"`
	Catalog string         `json:"catalog"`
	Build   string         `json:"build"`
}

func (l *libraryStore) stampWriter(e *libraryEnvelope) {
	if e.Writer == nil || !e.Writer.Tables.Newer(resolve.TableRevisions()) {
		_, build := buildVersion()
		e.Writer = &libraryWriter{Tables: resolve.TableRevisions(), Catalog: l.catalog.Version, Build: build}
	}
}

func newerWriter(e *libraryEnvelope) *resolve.Problem {
	if e.Writer == nil || !e.Writer.Tables.Newer(resolve.TableRevisions()) {
		return nil
	}
	_, build := buildVersion()
	return &resolve.Problem{Code: "newer_build", Message: fmt.Sprintf("Heads up — this library was saved by a newer PancakeSlicer (%s; you're on %s). Printers and settings it added might not work here, so give PancakeSlicer an update!", e.Writer.Build, build)}
}

func (l *libraryStore) libraryAnswer(e *libraryEnvelope, extra map[string]any) map[string]any {
	notices := []*resolve.Problem{}
	out := map[string]any{"document": e.Document, "problems": map[string]*resolve.Problem{}}
	lib, err := l.catalog.ReadLibrary(e.Document)
	if err == nil {
		out["problems"] = lib.Problems()
	}
	if notice := newerWriter(e); notice != nil {
		notices = append(notices, notice)
	}
	if err != nil {
		notices = append(notices, &resolve.Problem{Code: "library_invalid", Message: err.Error()})
	}
	out["notices"] = notices
	for k, v := range extra {
		out[k] = v
	}
	return out
}

type libraryMigration struct {
	From   int            `json:"from"`
	To     int            `json:"to"`
	At     time.Time      `json:"at"`
	Backup string         `json:"backup"`
	SlotBackup string `json:"slotBackup,omitempty"`
	Report map[string]int `json:"report"`
	Records []string `json:"records,omitempty"`
}

const libraryFile = "library.json"



func (l *libraryStore) stored(e *libraryEnvelope) *resolve.Library {
	lib, _ := l.catalog.ReadLibrary(e.Document)
	return lib
}

func (s *Server) libraryDocument() (*resolve.Library, error) {
	l := s.library
	l.mu.Lock()
	defer l.mu.Unlock()
	st, statErr := os.Stat(filepath.Join(l.dir, libraryFile))
	if statErr == nil && l.parsed != nil && l.parsedSize == st.Size() && l.parsedTime.Equal(st.ModTime()) {
		return l.parsed, nil
	}
	e, err := l.read()
	if err != nil {
		return nil, err
	}
	lib, err := l.catalog.ReadLibrary(e.Document)
	if err == nil && statErr == nil {
		l.parsed, l.parsedSize, l.parsedTime = lib, st.Size(), st.ModTime()
	}
	return lib, err
}

func newID(prefix string) string {
	b := make([]byte, 16)
	rand.Read(b)
	b[6], b[8] = b[6]&0x0f|0x40, b[8]&0x3f|0x80
	return fmt.Sprintf("%s:%x-%x-%x-%x-%x", prefix, b[0:4], b[4:6], b[6:8], b[8:10], b[10:])
}



func starterRecord(i int) map[string]any {
	s := starterProfiles[i]
	values := map[string]string{}
	for k, v := range starterValues {
		values[k] = v
	}
	for k, v := range s.values {
		values[k] = v
	}
	p := map[string]any{"id": "profile:starter-" + s.id, "name": s.name, "revision": 1, "values": values, "scales": resolve.ProfileRules().DefaultScales}
	speedSeed := map[string]string{"everyday": "standard", "strong": "strength", "fine-detail": "fine", "draft": "draft"}[s.id]
	p["speedModel"] = resolve.NewSpeedModel(speedSeed)
	for _, f := range resolve.ProfileRules().SpeedFeatures {
		delete(values, f.Key)
	}
	if s.heightRule != "" {
		p["heightRule"] = s.heightRule
	}
	return p
}

func emptyLibrary(epoch string, processes ...map[string]any) map[string]json.RawMessage {
	doc := map[string]json.RawMessage{"schema": json.RawMessage(fmt.Sprint(librarySchema)), "revision": json.RawMessage(`1`),
		"printers": json.RawMessage(`[]`), "filaments": json.RawMessage(`[]`), "calibrations": json.RawMessage(`[]`)}
	doc["processes"] = must(json.Marshal(append([]map[string]any{}, processes...)))
	if epoch != "" {
		doc["epoch"], _ = json.Marshal(epoch)
	}
	return doc
}

func printCatalog() []map[string]any {
	out := []map[string]any{}
	for i, s := range starterProfiles {
		out = append(out, map[string]any{"id": "profile:starter-" + s.id, "name": s.name, "description": s.description, "layers": s.layers, "record": starterRecord(i)})
	}
	return out
}

func (l *libraryStore) read() (*libraryEnvelope, error) {
	path := filepath.Join(l.dir, libraryFile)
	raw, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return &libraryEnvelope{Document: emptyLibrary(""), Operations: map[string]libraryReceipt{}}, nil
	}
	var e libraryEnvelope
	if err == nil {
		err = json.Unmarshal(raw, &e)
	}
	if err == nil && (e.Document == nil || e.Operations == nil) {
		err = errors.New("Oops! Stored profile library data is invalid.")
	}
	if err != nil {
		return nil, err
	}
	var schema int
	json.Unmarshal(e.Document["schema"], &schema)
	if !l.settled && schema != 4 {
		if err := l.finishSlotUpgrade(); err != nil {
			return nil, fmt.Errorf("Uh-oh, couldn't finish upgrading the profile library: %w", err)
		}
		l.settled = true
	}
	if schema < librarySchema {
		err = l.upgrade(&e, raw, schema)
	}
	if err != nil {
		return nil, fmt.Errorf("Uh-oh, we couldn't upgrade the profile library: %w", err)
	}
	return &e, nil
}

func foldFilaments(doc map[string]json.RawMessage, catalog *resolve.Catalog) ([]string, error) {
	var filaments, calibrations []map[string]json.RawMessage
	var printers []struct {
		ID, Name string
		Nozzles  []struct {
			Variant, Volume string
			Diameter        float64
		}
	}
	if err := json.Unmarshal(doc["filaments"], &filaments); err != nil {
		return nil, fmt.Errorf("filaments: %w", err)
	}
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return nil, fmt.Errorf("printers: %w", err)
	}
	if raw := doc["calibrations"]; len(raw) > 0 {
		if err := json.Unmarshal(raw, &calibrations); err != nil {
			return nil, fmt.Errorf("calibrations: %w", err)
		}
	}
	var records []string
	record := func(format string, args ...any) {
		line := fmt.Sprintf(format, args...)
		records = append(records, line)
		log.Printf("profile library: %s", line)
	}
	settingsOf := func(raw json.RawMessage) map[string]json.RawMessage {
		m := map[string]json.RawMessage{}
		json.Unmarshal(raw, &m)
		return m
	}
	moved, kept := false, false
	for _, f := range filaments {
		product := stringValue(f["product"])
		into := catalog.FoldedInto(product)
		if product == "" || into == "" && !catalog.FoldTarget(product) {
			continue
		}
		id, name, own := stringValue(f["id"]), stringValue(f["name"]), settingsOf(f["settings"])
		for _, p := range printers {
			for e, n := range p.Nozzles {
				if n.Variant == "" {
					continue
				}
				diff, err := catalog.FoldDiff(product, n.Variant)
				if err != nil {
					return nil, fmt.Errorf("%s on %s: %w", name, p.Name, err)
				}
				var cal map[string]json.RawMessage
				for _, c := range calibrations {
					var extruder int
					var nozzle float64
					json.Unmarshal(c["extruder"], &extruder)
					json.Unmarshal(c["nozzle"], &nozzle)
					if stringValue(c["filament"]) == id && stringValue(c["printer"]) == p.ID && extruder == e && resolve.SameMm(nozzle, n.Diameter) && stringValue(c["volume"]) == n.Volume {
						cal = c
					}
				}
				values, keys := settingsOf(cal["values"]), []string{}
				for k, v := range diff {
					if _, set := own[k]; set {
						continue
					}
					if _, set := values[k]; set {
						continue
					}
					if !resolve.CalibrationKey(k) {
						return nil, fmt.Errorf("%s on %s: %s %s would change and is not a setting a printer's filament calibration holds", name, p.Name, product, k)
					}
					values[k] = v
					keys = append(keys, k)
				}
				if len(keys) == 0 {
					continue
				}
				sort.Strings(keys)
				if cal == nil {
					cal = map[string]json.RawMessage{"id": must(json.Marshal(newID("calibration"))), "revision": json.RawMessage(`1`), "filament": must(json.Marshal(id)),
						"printer": must(json.Marshal(p.ID)), "extruder": must(json.Marshal(e)), "nozzle": must(json.Marshal(n.Diameter))}
					if n.Volume != "" {
						cal["volume"] = must(json.Marshal(n.Volume))
					}
					calibrations = append(calibrations, cal)
				} else {
					var revision uint64
					json.Unmarshal(cal["revision"], &revision)
					cal["revision"] = must(json.Marshal(revision + 1))
				}
				cal["values"] = must(json.Marshal(values))
				kept = true
				record("%s (%s) on %s, nozzle %d (%s): keeps the %d values %s resolved to before the fold as its calibration there: %s", name, id, p.Name, e+1, n.Variant, len(keys), product, strings.Join(keys, ", "))
			}
		}
		if into != "" {
			f["product"] = must(json.Marshal(into))
			moved = true
			record("%s (%s): product %s -> %s (%s is %s's profile for other printers)", name, id, product, into, product, into)
		}
	}
	if moved {
		doc["filaments"] = must(json.Marshal(filaments))
	}
	if kept {
		doc["calibrations"] = must(json.Marshal(calibrations))
	}
	return records, nil
}

func renameLibraryValues(doc map[string]json.RawMessage) []string {
	var records []string
	for _, collection := range []string{"printers", "processes", "filaments", "calibrations"} {
		var list []json.RawMessage
		if json.Unmarshal(doc[collection], &list) != nil {
			continue
		}
		changed := false
		for i, record := range list {
			out, changes := renameStoredValues(record)
			if len(changes) == 0 {
				continue
			}
			var r map[string]json.RawMessage
			json.Unmarshal(record, &r)
			list[i], changed = out, true
			line := fmt.Sprintf("%s %s (%s): %s", collection, stringValue(r["name"]), stringValue(r["id"]), strings.Join(changes, ", "))
			records = append(records, line)
			log.Printf("profile library: renamed values: %s", line)
		}
		if changed {
			doc[collection] = must(json.Marshal(list))
		}
	}
	return records
}

func (l *libraryStore) upgradeToV11(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
	placed, err := placeRecordKeys(doc, l.catalog)
	if err != nil {
		return nil, err
	}
	laid, err := layoutTraylessSlots(doc, l.catalog, func() (map[string][]map[int]int, error) {
		raw, err := c.workspace()
		if err != nil || raw == nil {
			return nil, err
		}
		return projectNozzleVotes(raw)
	})
	if err != nil {
		return nil, err
	}
	folded, err := foldFilaments(doc, l.catalog)
	if err != nil {
		return nil, err
	}
	renamed := renameLibraryValues(doc)
	for key, lines := range map[string][]string{"settings_placed": placed, "slot_lines": laid, "fold_records": folded, "records_with_renamed_values": renamed} {
		if len(lines) > 0 {
			c.report[key] = len(lines)
		}
	}
	return slices.Concat(placed, laid, folded, renamed), nil
}

const slotUpgradeMarker = "printer-slots.upgrade.json"

const legacySlotMarker = "printer-slots.json.v4"

func rewrittenSlots(original []byte) []byte {
	var spools struct {
		Spools map[string]spoolChoice `json:"spools"`
	}
	json.Unmarshal(original, &spools)
	if spools.Spools == nil {
		spools.Spools = map[string]spoolChoice{}
	}
	return must(json.Marshal(slotDoc{Schema: slotDocSchema, Last: map[string]*printerSlots{}, Spools: spools.Spools}))
}

func upgradeSource(path string, live []byte, liveErr error) ([]byte, bool, error) {
	kept, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	if errors.Is(liveErr, os.ErrNotExist) || bytes.Equal(live, kept) || bytes.Equal(live, rewrittenSlots(kept)) {
		return kept, true, nil
	}
	log.Printf("profile library: %s is stale (printers/printer-slots.json was written after it), the upgrade reads printers/printer-slots.json", path)
	return nil, false, nil
}

func (l *libraryStore) retireLegacySlotMarker() error {
	path := filepath.Join(l.dir, "printers", legacySlotMarker)
	info, err := os.Stat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	backup := filepath.Join(l.dir, "printers", fmt.Sprintf("printer-slots.v4.%s.json", info.ModTime().UTC().Format("20060102T150405Z")))
	if err := os.Rename(path, backup); err != nil {
		return err
	}
	log.Printf("profile library: kept printers/%s as %s, the slot store backup of an earlier schema 4 upgrade", legacySlotMarker, backup)
	return syncWorkspaceDirectory(filepath.Dir(path))
}

func (l *libraryStore) finishSlotUpgrade() error {
	slotsPath := filepath.Join(l.dir, "printers", "printer-slots.json")
	marker := filepath.Join(l.dir, "printers", slotUpgradeMarker)
	kept, err := os.ReadFile(marker)
	if err == nil {
		live, liveErr := os.ReadFile(slotsPath)
		rewritten := rewrittenSlots(kept)
		switch {
		case liveErr == nil && bytes.Equal(live, rewritten):
		case errors.Is(liveErr, os.ErrNotExist) || liveErr == nil && bytes.Equal(live, kept):
			if err := writeFileAtomic(slotsPath, rewritten); err != nil {
				return err
			}
			log.Printf("profile library: finished the schema 4 upgrade killed after its library commit: printers/printer-slots.json rewritten from printers/%s", slotUpgradeMarker)
		case liveErr != nil:
			return liveErr
		default:
			return fmt.Errorf("printers/%s is left from an interrupted schema 4 upgrade, but printers/printer-slots.json changed since; compare them and remove the one that is wrong", slotUpgradeMarker)
		}
		if err := os.Remove(marker); err != nil {
			return err
		}
		if err := syncWorkspaceDirectory(filepath.Dir(marker)); err != nil {
			return err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return l.retireLegacySlotMarker()
}

func upgradeToV7(doc map[string]json.RawMessage, report map[string]int) ([]string, error) {
	coloured, lines, err := moveFilamentColours(doc)
	if err != nil {
		return nil, err
	}
	maps.Copy(report, coloured)
	removed, err := dropUnscalableRows(doc)
	if err != nil {
		return nil, err
	}
	report["scale_rows_removed"] = len(removed)
	return append(lines, removed...), nil
}



func moveFilamentColours(doc map[string]json.RawMessage) (map[string]int, []string, error) {
	report := map[string]int{"slots_coloured": 0, "slot_colours_kept": 0, "filament_colours_moved": 0, "calibration_colours_dropped": 0}
	var filaments, printers, calibrations []map[string]json.RawMessage
	if err := json.Unmarshal(doc["filaments"], &filaments); err != nil {
		return nil, nil, fmt.Errorf("filaments: %w", err)
	}
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return nil, nil, fmt.Errorf("printers: %w", err)
	}
	if raw, ok := doc["calibrations"]; ok {
		if err := json.Unmarshal(raw, &calibrations); err != nil {
			return nil, nil, fmt.Errorf("calibrations: %w", err)
		}
	}
	var lines []string
	stripColour := func(values map[string]json.RawMessage) (string, map[string]json.RawMessage) {
		colour, removed := "", map[string]json.RawMessage{}
		for _, k := range resolve.ColourKeys {
			if v, ok := values[k]; ok {
				if colour == "" && k == "filament_colour" {
					colour = resolve.FirstConfigText(v)
				}
				delete(values, k)
				removed[k] = v
			}
		}
		return colour, removed
	}
	colourOf := map[string]string{}
	type colourSource struct {
		id, name, colour string
		removed          map[string]json.RawMessage
	}
	var sources []*colourSource
	sourceOf := map[string]*colourSource{}
	for _, f := range filaments {
		colour := stringValue(f["colour"])
		delete(f, "colour")
		src := &colourSource{id: stringValue(f["id"]), name: stringValue(f["name"]), removed: map[string]json.RawMessage{}}
		if colour != "" {
			src.removed["colour"] = must(json.Marshal(colour))
		}
		var settings map[string]json.RawMessage
		if json.Unmarshal(f["settings"], &settings) == nil && settings != nil {
			if c, removed := stripColour(settings); len(removed) > 0 {
				maps.Copy(src.removed, removed)
				if colour == "" {
					colour = c
				}
				if len(settings) == 0 {
					delete(f, "settings")
				} else {
					f["settings"] = must(json.Marshal(settings))
				}
			}
		}
		if len(src.removed) > 0 {
			src.colour = colour
			sources = append(sources, src)
		}
		if colour == "" {
			continue
		}
		report["filament_colours_moved"]++
		var aliases []string
		json.Unmarshal(f["aliases"], &aliases)
		for _, id := range append([]string{stringValue(f["id"])}, aliases...) {
			colourOf[id] = colour
			sourceOf[id] = src
		}
	}
	took := map[*colourSource]bool{}
	for _, p := range printers {
		var slots []map[string]json.RawMessage
		if json.Unmarshal(p["slots"], &slots) != nil {
			continue
		}
		for _, slot := range slots {
			colour := colourOf[stringValue(slot["filament"])]
			switch {
			case colour == "":
			case stringValue(slot["colour"]) != "":
				report["slot_colours_kept"]++
			default:
				slot["colour"] = must(json.Marshal(colour))
				report["slots_coloured"]++
				took[sourceOf[stringValue(slot["filament"])]] = true
			}
		}
		p["slots"] = must(json.Marshal(slots))
	}
	for _, src := range sources {
		switch {
		case !took[src]:
			lines = append(lines, fmt.Sprintf("filaments %s (%s): colour values %s dropped, no slot without a colour of its own holds it", src.name, src.id, must(json.Marshal(src.removed))))
		case len(src.removed) > 1:
			lines = append(lines, fmt.Sprintf("filaments %s (%s): colour %s moved to its slots, colour values %s removed", src.name, src.id, src.colour, must(json.Marshal(src.removed))))
		}
	}
	for _, c := range calibrations {
		var values map[string]json.RawMessage
		if json.Unmarshal(c["values"], &values) == nil && values != nil {
			if _, removed := stripColour(values); len(removed) > 0 {
				c["values"] = must(json.Marshal(values))
				report["calibration_colours_dropped"]++
				lines = append(lines, fmt.Sprintf("calibrations %s (filament %s, printer %s): colour values %s dropped", stringValue(c["id"]), stringValue(c["filament"]), stringValue(c["printer"]), must(json.Marshal(removed))))
			}
		}
	}
	doc["filaments"], doc["printers"] = must(json.Marshal(filaments)), must(json.Marshal(printers))
	if calibrations != nil {
		doc["calibrations"] = must(json.Marshal(calibrations))
	}
	return report, lines, nil
}

type v4Doc struct {
	Revision json.RawMessage              `json:"revision"`
	Epoch    json.RawMessage              `json:"epoch"`
	Profiles []map[string]json.RawMessage `json:"profiles"`
	Setups   map[string]struct {
		Slots []struct {
			ID         string `json:"id"`
			MaterialID string `json:"materialId"`
			Colour     string `json:"colour"`
		} `json:"slots"`
		MaterialIDs []string                              `json:"materialIds"`
		Tuning      map[string]map[string]json.RawMessage `json:"tuning"`
		Mixes       json.RawMessage                       `json:"mixes"`
		ProcessID   string                                `json:"processId"`
	} `json:"setups"`
}

func migrateLibraryV4(doc map[string]json.RawMessage, chosen map[string]map[string]slotChoice,
	swap func(vendor, model string, nozzle float64) (string, []string)) (map[string]json.RawMessage, map[string]int, []string, error) {
	var old v4Doc
	raw, _ := json.Marshal(doc)
	if err := json.Unmarshal(raw, &old); err != nil {
		return nil, nil, nil, err
	}
	var lines []string
	report := map[string]int{}
	dropped := func(count, format string, args ...any) {
		lines = append(lines, fmt.Sprintf(format, args...))
		report[count]++
	}
	for _, k := range sortedKeys(doc) {
		if !slices.Contains([]string{"schema", "revision", "epoch", "profiles", "setups"}, k) {
			dropped("library fields dropped", "library field %s dropped, schema 6 has no such field: %s", k, doc[k])
		}
	}
	byType := map[string][]map[string]json.RawMessage{}
	for _, p := range old.Profiles {
		t := stringValue(p["type"])
		if t != "printer" && t != "filament" && t != "print" {
			dropped("profiles dropped", "profile %s dropped, schema 6 has no %q records: %s", stringValue(p["id"]), t, must(json.Marshal(p)))
			continue
		}
		byType[t] = append(byType[t], p)
	}
	var rawSetups map[string]map[string]json.RawMessage
	json.Unmarshal(doc["setups"], &rawSetups)
	users := map[string][]string{}
	addUser := func(filament, printer string) {
		if filament != "" && !slices.Contains(users[filament], printer) {
			users[filament] = append(users[filament], printer)
		}
	}
	printerIDs := []string{}
	for _, p := range byType["printer"] {
		printerIDs = append(printerIDs, stringValue(p["id"]))
	}
	for _, id := range printerIDs {
		s := old.Setups[id]
		for _, m := range s.MaterialIDs {
			addUser(m, id)
		}
		for _, slot := range s.Slots {
			addUser(slot.MaterialID, id)
		}
		for m := range s.Tuning {
			addUser(m, id)
		}
		for _, c := range chosen[id] {
			addUser(c.Filament, id)
		}
	}
	for _, id := range sortedKeys(rawSetups) {
		if !slices.Contains(printerIDs, id) {
			dropped("setups dropped", "setup %s dropped, no printer profile %s: %s", id, id, must(json.Marshal(rawSetups[id])))
		}
	}
	for _, id := range sortedKeys(chosen) {
		if !slices.Contains(printerIDs, id) && len(chosen[id]) > 0 {
			dropped("AMS slot choices dropped", "AMS slot choices of %s dropped, no printer profile %s: %s", id, id, must(json.Marshal(chosen[id])))
		}
	}
	type copyInfo struct {
		raw      map[string]json.RawMessage
		id       string
		material map[string]json.RawMessage
		calib    map[string]json.RawMessage
		revision uint64
	}
	groups := map[string][]*copyInfo{}
	order := []string{}
	for _, p := range byType["filament"] {
		c := &copyInfo{raw: p, id: stringValue(p["id"]), material: map[string]json.RawMessage{}, calib: map[string]json.RawMessage{}}
		json.Unmarshal(p["revision"], &c.revision)
		var settings map[string]json.RawMessage
		json.Unmarshal(p["settings"], &settings)
		for k, v := range settings {
			if slices.Contains(resolve.ProfileRules().CalibrationKeys, k) {
				c.calib[k] = v
			} else {
				c.material[k] = v
			}
		}
		if len(users[c.id]) == 0 && len(c.calib) > 0 {
			for k, v := range c.calib {
				c.material[k] = v
			}
			c.calib = map[string]json.RawMessage{}
		}
		key := strings.Join([]string{stringValue(p["family"]), strings.TrimSpace(stringValue(p["name"])), strings.ToUpper(stringValue(p["colour"])),
			stringValue(p["brand"]), stringValue(p["material"]), string(must(json.Marshal(c.material))), string(p["fits"]), string(p["archived"])}, "\x00")
		if groups[key] == nil {
			order = append(order, key)
		}
		groups[key] = append(groups[key], c)
	}
	target := map[string]string{}
	filaments := []map[string]any{}
	type calibKey struct {
		filament, printer string
	}
	calibs := map[calibKey]map[string]json.RawMessage{}
	calibFrom := map[calibKey]map[string]string{}
	calibRevision := map[calibKey]uint64{}
	calibOrder := []calibKey{}
	addCalib := func(filament, printer string, values map[string]json.RawMessage, revision uint64, from string) {
		if len(values) == 0 {
			return
		}
		k := calibKey{filament, printer}
		if calibs[k] == nil {
			calibs[k] = map[string]json.RawMessage{}
			calibFrom[k] = map[string]string{}
			calibOrder = append(calibOrder, k)
		}
		for _, key := range sortedKeys(values) {
			v := values[key]
			if was, ok := calibs[k][key]; ok && !sameSetting(key, was, v) {
				lines = append(lines, fmt.Sprintf("calibration of %s on printer %s: %s = %s from %s replaces %s from %s", filament, printer, key, v, from, was, calibFrom[k][key]))
			}
			calibs[k][key] = v
			calibFrom[k][key] = from
		}
		calibRevision[k] = max(calibRevision[k], revision)
	}
	for _, key := range order {
		group := groups[key]
		survivor := group[0]
		entry := map[string]any{}
		for k, v := range survivor.raw {
			if k != "type" && k != "settings" {
				entry[k] = v
			}
		}
		entry["family"] = vendorlessFamily(stringValue(survivor.raw["family"]))
		if len(survivor.material) > 0 {
			entry["settings"] = survivor.material
		}
		aliases := []string{}
		for _, c := range group {
			target[c.id] = survivor.id
			if c != survivor {
				aliases = append(aliases, c.id)
				report["filament copies merged"]++
				for _, k := range sortedKeys(c.raw) {
					if !slices.Contains([]string{"id", "revision", "type", "settings"}, k) && !bytes.Equal(c.raw[k], survivor.raw[k]) {
						dropped("filament copy fields dropped", "filament copy %s merged into %s: %s = %s dropped, %s keeps %s", c.id, survivor.id, k, c.raw[k], survivor.id, cmp.Or(string(survivor.raw[k]), "none"))
					}
				}
			}
			for _, printer := range users[c.id] {
				addCalib(survivor.id, printer, c.calib, c.revision, fmt.Sprintf("filament copy %s (%s)", c.id, strings.TrimSpace(stringValue(c.raw["name"]))))
			}
		}
		if len(aliases) > 0 {
			entry["aliases"] = aliases
		}
		filaments = append(filaments, entry)
		report["library filaments"]++
	}
	printers := []map[string]any{}
	nozzles := map[string]float64{}
	for _, p := range byType["printer"] {
		id := stringValue(p["id"])
		s := old.Setups[id]
		device := map[string]any{}
		for k, v := range p {
			if k != "type" {
				device[k] = v
			}
		}
		var vendor, model string
		var nozzle float64
		json.Unmarshal(p["vendor"], &vendor)
		json.Unmarshal(p["model"], &model)
		json.Unmarshal(p["nozzle"], &nozzle)
		nozzles[id] = nozzle
		mode := "manual"
		if swap != nil {
			mode, _ = swap(vendor, model, nozzle)
		}
		device["swap"] = mode
		setup := rawSetups[id]
		for _, k := range sortedKeys(setup) {
			switch k {
			case "schema", "printerId", "revision", "materialIds", "slots", "tuning", "mixes", "processId":
			case "assignments":
				if a := string(setup[k]); a != "{}" && a != "null" {
					dropped("slot assignments dropped", "printer %s: slot assignments dropped, schema 6 has no such field: %s", id, setup[k])
				}
			default:
				dropped("setup fields dropped", "printer %s: setup field %s dropped, schema 6 has no such field: %s", id, k, setup[k])
			}
		}
		var rawSlots []map[string]json.RawMessage
		json.Unmarshal(setup["slots"], &rawSlots)
		slots := []resolve.Slot{}
		for i, slot := range s.Slots {
			sid := slot.ID
			if !strings.HasPrefix(sid, "slot:") {
				sid = newID("slot")
				lines = append(lines, fmt.Sprintf("printer %s: slot %q is now %s", id, slot.ID, sid))
			}
			if i < len(rawSlots) {
				for _, k := range sortedKeys(rawSlots[i]) {
					if k != "id" && k != "materialId" && k != "colour" {
						dropped("slot fields dropped", "printer %s slot %s: %s dropped, schema 6 has no such slot field: %s", id, sid, k, rawSlots[i][k])
					}
				}
			}
			if slot.MaterialID != "" && target[slot.MaterialID] == "" {
				dropped("slot filaments dropped", "printer %s slot %s: materialId %s dropped, it is not in the library", id, sid, slot.MaterialID)
			}
			slots = append(slots, resolve.Slot{ID: sid, Filament: target[slot.MaterialID], Colour: slot.Colour})
			report["slots carried over"]++
		}
		trays := []string{}
		for tray := range chosen[id] {
			if _, ok := resolve.ParseTray(tray); ok {
				trays = append(trays, tray)
			} else {
				dropped("AMS slot choices dropped", "printer %s: AMS choice %q of filament %s dropped, it is no tray", id, tray, chosen[id][tray].Filament)
			}
		}
		sort.Slice(trays, func(a, b int) bool { return trayBefore(trays[a], trays[b]) })
		filamentOf := map[string]string{}
		for _, tray := range trays {
			f := target[chosen[id][tray].Filament]
			if m := chosen[id][tray].Filament; m != "" && f == "" {
				dropped("slot filaments dropped", "printer %s tray %s: AMS choice of filament %s dropped, it is not in the library", id, tray, m)
			}
			filamentOf[tray] = f
		}
		placed := map[string]bool{}
		for _, tray := range trays {
			f := filamentOf[tray]
			if at := slices.IndexFunc(slots, func(s resolve.Slot) bool { return s.Tray == "" && f != "" && s.Filament == f }); at >= 0 {
				slots[at].Tray, placed[tray] = tray, true
				lines = append(lines, fmt.Sprintf("printer %s slot %s: slot %d (filament %s) takes its AMS choice, tray %s", id, slots[at].ID, at+1, f, tray))
				report["AMS slot choices carried over"]++
			}
		}
		for _, tray := range trays {
			if placed[tray] {
				continue
			}
			f := filamentOf[tray]
			if at := slices.IndexFunc(slots, func(s resolve.Slot) bool { return s.Tray == "" }); at >= 0 && f != "" {
				if was := slots[at].Filament; was != f {
					lines = append(lines, fmt.Sprintf("printer %s slot %s: slot %d takes its AMS choice, tray %s, filament %s replacing %s", id, slots[at].ID, at+1, tray, f, cmp.Or(was, "none")))
					report["AMS slot choices replaced a slot's filament"]++
				} else {
					lines = append(lines, fmt.Sprintf("printer %s slot %s: slot %d (filament %s) takes its AMS choice, tray %s", id, slots[at].ID, at+1, f, tray))
				}
				slots[at].Filament, slots[at].Tray = f, tray
			} else {
				slots = append(slots, resolve.Slot{ID: newID("slot"), Filament: f, Tray: tray})
				lines = append(lines, fmt.Sprintf("printer %s tray %s: AMS choice of filament %s is new slot %d (%s), no slot without a tray was left", id, tray, f, len(slots), slots[len(slots)-1].ID))
			}
			report["AMS slot choices carried over"]++
		}
		if len(slots) == 0 {
			slots = append(slots, resolve.Slot{ID: newID("slot")})
		}
		device["slots"] = slots
		held := map[string]bool{}
		for _, slot := range slots {
			held[slot.Filament] = true
		}
		for _, m := range s.MaterialIDs {
			switch {
			case m == "":
			case target[m] == "":
				dropped("printer filament links dropped", "printer %s: materialIds entry %s (its filament list) dropped, it is not in the library", id, m)
			case held[target[m]]:
			default:
				dropped("printer filament links dropped", "printer %s: materialIds entry %s (its filament list) is in no slot; it stays in the library as %s", id, m, target[m])
			}
		}
		if s.ProcessID != "" && slices.ContainsFunc(byType["print"], func(r map[string]json.RawMessage) bool { return stringValue(r["id"]) == s.ProcessID }) {
			device["process"] = s.ProcessID
		} else if s.ProcessID != "" {
			dropped("printer processes dropped", "printer %s: processId %s dropped, it is not in the library", id, s.ProcessID)
		}
		if len(s.Mixes) > 0 && string(s.Mixes) != "{}" && string(s.Mixes) != "null" {
			device["mixes"] = s.Mixes
		}
		for m, tuning := range s.Tuning {
			if target[m] != "" {
				var revision uint64
				for _, c := range groups {
					for _, x := range c {
						if x.id == m {
							revision = x.revision
						}
					}
				}
				addCalib(target[m], id, tuning, revision, "printer tuning of "+m)
			} else if len(tuning) > 0 {
				return nil, nil, nil, fmt.Errorf("printer %s has settings for filament %s, which is not in the library", id, m)
			}
		}
		printers = append(printers, device)
		report["devices"]++
	}
	calibrations := []resolve.Calibration{}
	for _, k := range calibOrder {
		revision := calibRevision[k]
		calibrations = append(calibrations, resolve.Calibration{ID: newID("calibration"), Revision: &revision, Filament: k.filament, Printer: k.printer, Nozzle: nozzles[k.printer], Values: calibs[k]})
		report["calibration rows"]++
	}
	processes := []map[string]json.RawMessage{}
	for _, p := range byType["print"] {
		delete(p, "type")
		processes = append(processes, p)
		report["processes"]++
	}
	next := map[string]json.RawMessage{"schema": json.RawMessage(fmt.Sprint(librarySchema)), "revision": old.Revision}
	if len(old.Epoch) > 0 {
		next["epoch"] = old.Epoch
	}
	next["printers"] = must(json.Marshal(printers))
	next["filaments"] = must(json.Marshal(filaments))
	next["calibrations"] = must(json.Marshal(calibrations))
	next["processes"] = must(json.Marshal(processes))
	return next, report, lines, nil
}

func vendorlessFamily(family string) string {
	if _, name, ok := strings.Cut(family, "/"); ok {
		return name
	}
	return family
}

func dropUnscalableRows(doc map[string]json.RawMessage) ([]string, error) {
	var profiles []map[string]json.RawMessage
	if err := json.Unmarshal(doc["processes"], &profiles); err != nil {
		return nil, fmt.Errorf("processes: %w", err)
	}
	var removed []string
	for _, p := range profiles {
		var rows []json.RawMessage
		json.Unmarshal(p["scales"], &rows)
		kept := rows[:0:0]
		for _, row := range rows {
			var r resolve.Scale
			json.Unmarshal(row, &r)
			if slices.Contains(resolve.ProfileRules().NotScalable, r.Key) {
				removed = append(removed, fmt.Sprintf("processes %s (%s): Dynamic row dropped, %s cannot scale: %s", stringValue(p["name"]), stringValue(p["id"]), r.Key, row))
			} else {
				kept = append(kept, row)
			}
		}
		if len(kept) != len(rows) {
			p["scales"] = must(json.Marshal(kept))
		}
	}
	if len(removed) > 0 {
		doc["processes"] = must(json.Marshal(profiles))
	}
	return removed, nil
}

func (l *libraryStore) write(e *libraryEnvelope) error {
	l.stampWriter(e)
	raw, err := json.Marshal(e)
	if err != nil {
		return err
	}
	if err := writeFileAtomic(filepath.Join(l.dir, libraryFile), append(raw, '\n')); err != nil {
		return err
	}
	l.written()
	return nil
}

func (l *libraryStore) reset(epoch string) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.write(&libraryEnvelope{Document: emptyLibrary(epoch), Operations: map[string]libraryReceipt{}})
}

func (s *Server) libraryRoutes(mux *http.ServeMux) {
	l := s.library
	mux.HandleFunc("GET /api/print-catalog", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, printCatalog())
	})
	mux.HandleFunc("GET /api/library", func(w http.ResponseWriter, r *http.Request) {
		l.mu.Lock()
		e, err := l.read()
		l.mu.Unlock()
		if err != nil {
			libraryFailure(w, 503, "library_unavailable", "Oops! Couldn't read profiles: "+storageCause(err), "retry_storage", 0)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, 200, l.libraryAnswer(e, nil))
	})

	mux.HandleFunc("PUT /api/library", func(w http.ResponseWriter, r *http.Request) {
		raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 8<<20))
		var in struct {
			OperationID      string                     `json:"operationId"`
			ExpectedRevision uint64                     `json:"expectedRevision"`
			Document         map[string]json.RawMessage `json:"document"`
		}
		if err != nil || json.Unmarshal(raw, &in) != nil || in.OperationID == "" || len(in.OperationID) > 200 || in.Document == nil {
			libraryFailure(w, 400, "library_invalid", "operationId, expectedRevision and a complete document are required.", "reload", 0)
			return
		}
		digest := workspaceDigest([]any{in.ExpectedRevision, in.Document})
		l.mu.Lock()
		defer l.mu.Unlock()
		e, err := l.read()
		if err != nil {
			libraryFailure(w, 503, "library_unavailable", "Oops! Couldn't read the stored library, so nothing was changed.", "retry_storage", 0)
			return
		}
		revision := resolve.LibraryRevision(e.Document)
		if receipt, ok := e.Operations[in.OperationID]; ok {
			if receipt.Digest != digest {
				libraryFailure(w, 409, "operation_conflict", "Heads up — this operation ID was already used for different changes.", "reload", revision)
				return
			}
			writeJSON(w, 200, l.libraryAnswer(e, map[string]any{"revision": receipt.Revision, "operationId": in.OperationID}))
			return
		}
		if stringValue(in.Document["epoch"]) != stringValue(e.Document["epoch"]) {
			libraryFailure(w, 409, "library_reset", "Profiles were reset! Give them a reload.", "reload", revision)
			return
		}
		if in.ExpectedRevision != revision || resolve.LibraryRevision(in.Document) != revision {
			libraryFailure(w, 409, "revision_conflict", "The library changed elsewhere — reload it and reapply your change.", "reload", revision)
			return
		}
		previous := l.stored(e)
		if previous != nil {
			if dropped := droppedProfiles(previous, in.Document); len(dropped) > 0 {
				owner, profile := slotUser(in.Document, dropped)
				if profile.Name == "" {
					if owner, profile, err = s.workspace.profileReference(dropped); err != nil {
						libraryFailure(w, 503, "workspace_unavailable", "Oops! Couldn't read your saved projects, so we didn't delete any profiles.", "retry_storage", revision)
						return
					}
				}
				if profile.Name != "" {
					message := owner + " uses " + profile.Name + ". Archive it instead."
					if profile.Archived {
						message = owner + " uses " + profile.Name + ", so it stays archived."
					}
					libraryFailure(w, 409, "profile_referenced", message, "archive_profile", revision)
					return
				}
				dropCalibrations(in.Document, dropped)
			}
		}
		stampEdited(in.Document, e.Document, time.Now().UTC())
		if err := l.deviceDefaults(in.Document); err != nil {
			libraryFailure(w, 400, "library_invalid", err.Error(), "repair_profiles", revision)
			return
		}
		next, err := l.catalog.ParseLibrary(in.Document, previous)
		if err != nil {
			libraryFailure(w, 400, "library_invalid", err.Error(), "repair_profiles", revision)
			return
		}
		if previous != nil {
			if livePrinters(previous) > 0 && livePrinters(next) == 0 {
				libraryFailure(w, 409, "last_printer", "Keep at least one printer!", "reload", revision)
				return
			}
		}
		if name := editedWhileRemoved(e.Document, in.Document); name != "" {
			libraryFailure(w, 409, "removed", name+" was removed in another window, so this change wasn't saved.", "reload", revision)
			return
		}
		if kind, taken := takenName(previous, next); taken != "" {
			libraryFailure(w, 409, "name_taken", "Oops! There's already a "+kind+" named "+taken+".", "rename", revision)
			return
		}
		if sameLibraryDocument(in.Document, e.Document) {
			writeJSON(w, 200, l.libraryAnswer(e, map[string]any{"revision": revision, "operationId": in.OperationID}))
			return
		}
		in.Document["revision"] = json.RawMessage(fmt.Sprint(revision + 1))
		e.Document = in.Document
		putReceipt(e.Operations, in.OperationID, libraryReceipt{Digest: digest, Revision: revision + 1}, func(r libraryReceipt) uint64 { return r.Seq },
			func(r libraryReceipt, seq uint64) libraryReceipt { r.Seq = seq; return r })
		if err := l.write(e); err != nil {
			libraryFailure(w, 507, "library_write_failed", "Uh-oh, couldn't save profiles! Free up a little storage and try again.", "retry_storage", revision)
			return
		}
		s.reconcileConnections(in.Document, "library save")
		writeJSON(w, 200, l.libraryAnswer(e, map[string]any{"revision": revision + 1, "operationId": in.OperationID}))
	})
}

func editedWhileRemoved(previous, next map[string]json.RawMessage) string {
	for _, list := range []string{"printers", "filaments", "processes"} {
		var before, after []json.RawMessage
		json.Unmarshal(previous[list], &before)
		json.Unmarshal(next[list], &after)
		stored := map[string]json.RawMessage{}
		for _, r := range before {
			var probe struct {
				ID       string          `json:"id"`
				Archived json.RawMessage `json:"archived"`
			}
			if json.Unmarshal(r, &probe) == nil && len(probe.Archived) > 0 && string(probe.Archived) != "null" {
				stored[probe.ID] = r
			}
		}
		for _, r := range after {
			var probe struct {
				ID       string          `json:"id"`
				Name     string          `json:"name"`
				Archived json.RawMessage `json:"archived"`
			}
			if json.Unmarshal(r, &probe) != nil || len(probe.Archived) == 0 || string(probe.Archived) == "null" {
				continue
			}
			if old, ok := stored[probe.ID]; ok && !sameRecordJSON(old, r) {
				return probe.Name
			}
		}
	}
	return ""
}

func takenName(previous, next *resolve.Library) (string, string) {
	type rec struct {
		name string
		live bool
	}
	check := func(before, after map[string]rec) string {
		names := map[string]int{}
		for _, r := range after {
			if r.live {
				names[r.name]++
			}
		}
		ids := make([]string, 0, len(after))
		for id := range after {
			ids = append(ids, id)
		}
		sort.Strings(ids)
		for _, id := range ids {
			r, was := after[id], before[id]
			if r.live && names[r.name] > 1 && (was.name != r.name || !was.live) {
				return r.name
			}
		}
		return ""
	}
	recs := func(l *resolve.Library) [3]map[string]rec {
		out := [3]map[string]rec{{}, {}, {}}
		if l == nil {
			return out
		}
		for id, r := range l.Printers {
			out[0][id] = rec{r.Name, r.Live()}
		}
		for id, r := range l.Filaments {
			out[1][id] = rec{r.Name, r.Live()}
		}
		for id, r := range l.Processes {
			out[2][id] = rec{r.Name, r.Live()}
		}
		return out
	}
	b, a := recs(previous), recs(next)
	for i, kind := range []string{"printer", "filament", "print profile"} {
		if n := check(b[i], a[i]); n != "" {
			return kind, n
		}
	}
	return "", ""
}

func livePrinters(l *resolve.Library) int {
	n := 0
	for _, p := range l.Printers {
		if p.Live() {
			n++
		}
	}
	return n
}

func (l *libraryStore) deviceDefaults(doc map[string]json.RawMessage) error {
	var printers []map[string]json.RawMessage
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return errors.New("printers must be an array")
	}
	changed := false
	for _, p := range printers {
		var nozzles []resolve.Nozzle
		json.Unmarshal(p["nozzles"], &nozzles)
		variant := ""
		if len(nozzles) > 0 {
			variant = nozzles[0].Variant
		}
		if n := l.extruders(variant); variant != "" && len(nozzles) != n {
			return fmt.Errorf("%s: the printer has %d nozzles, its record lists %d", stringValue(p["name"]), n, len(nozzles))
		}
		if stringValue(p["swap"]) == "" {
			mode, _ := l.swap(variant)
			p["swap"], changed = must(json.Marshal(mode)), true
		}
		var slots []json.RawMessage
		if json.Unmarshal(p["slots"], &slots); len(slots) == 0 {
			n := 1
			if stringValue(p["swap"]) == "toolheads" {
				n = len(nozzles)
			}
			fresh := []resolve.Slot{}
			for range max(n, 1) {
				fresh = append(fresh, resolve.Slot{ID: newID("slot")})
			}
			p["slots"], changed = must(json.Marshal(fresh)), true
		}
	}
	if changed {
		doc["printers"] = must(json.Marshal(printers))
	}
	return nil
}

func upgradeToV10(doc map[string]json.RawMessage, report map[string]int, catalog *resolve.Catalog) ([]string, error) {
	var lines []string
	var printers, filaments []map[string]json.RawMessage
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return nil, fmt.Errorf("printers: %w", err)
	}
	for _, p := range printers {
		var nozzles []map[string]json.RawMessage
		if err := json.Unmarshal(p["nozzles"], &nozzles); err != nil {
			return nil, fmt.Errorf("%s nozzles: %w", stringValue(p["name"]), err)
		}
		for i, n := range nozzles {
			var diameter float64
			json.Unmarshal(n["diameter"], &diameter)
			variant := catalog.Variant(stringValue(p["model"]), diameter)
			n["variant"] = must(json.Marshal(variant))
			if variant == "" {
				report["nozzles_without_variant"]++
				lines = append(lines, fmt.Sprintf("printers %s (%s) nozzle %d (%g mm) has no %s catalog variant; pick one", stringValue(p["name"]), stringValue(p["id"]), i+1, diameter, stringValue(p["model"])))
			} else {
				report["nozzles_with_variant"]++
				lines = append(lines, fmt.Sprintf("printers %s (%s) nozzle %d (%g mm) is variant %q", stringValue(p["name"]), stringValue(p["id"]), i+1, diameter, variant))
			}
		}
		p["nozzles"] = must(json.Marshal(nozzles))
	}
	doc["printers"] = must(json.Marshal(printers))
	if err := json.Unmarshal(doc["filaments"], &filaments); err != nil {
		return nil, fmt.Errorf("filaments: %w", err)
	}
	for _, f := range filaments {
		name := stringValue(f["name"])
		id := vendorlessFamily(stringValue(f["family"]))
		f["product"] = must(json.Marshal(id))
		delete(f, "family")
		product := catalog.Filaments[id]
		if into := catalog.FoldedInto(id); product == nil && into != "" {
			product = catalog.Filaments[into]
		}
		if product == nil {
			report["filaments_without_product"]++
			lines = append(lines, fmt.Sprintf("filaments %s (%s): family %q is no catalog product; kept as its product, pick one", name, stringValue(f["id"]), id))
		} else {
			report["filaments_renamed"]++
			lines = append(lines, fmt.Sprintf("filaments %s (%s): family → product %q", name, stringValue(f["id"]), id))
		}
		settings := map[string]json.RawMessage{}
		json.Unmarshal(f["settings"], &settings)
		for _, field := range []string{"brand", "material"} {
			key := map[string]string{"brand": "filament_vendor", "material": "filament_type"}[field]
			raw, ok := f[field]
			if !ok {
				continue
			}
			delete(f, field)
			value := stringValue(raw)
			own := ""
			if product != nil {
				own = map[string]string{"brand": product.Brand, "material": product.Type}[field]
			}
			if _, set := settings[key]; value != "" && value != own && !set {
				settings[key] = must(json.Marshal([]string{value}))
				report["filament_"+field+"_to_settings"]++
				lines = append(lines, fmt.Sprintf("filaments %s (%s): %s %q differs from the product's %q; now its %s setting", name, stringValue(f["id"]), field, value, own, key))
			} else {
				report["filament_"+field+"_dropped"]++
				lines = append(lines, fmt.Sprintf("filaments %s (%s): %s %q dropped (product's %q, %s setting %s)", name, stringValue(f["id"]), field, value, own, key, cmp.Or(string(settings[key]), "unset")))
			}
		}
		if len(settings) > 0 {
			f["settings"] = must(json.Marshal(settings))
		}
		if fits, ok := f["fits"]; ok {
			delete(f, "fits")
			report["filament_fits_dropped"]++
			lines = append(lines, fmt.Sprintf("filaments %s (%s): Works with rule (fits) %s dropped", name, stringValue(f["id"]), fits))
		}
	}
	doc["filaments"] = must(json.Marshal(filaments))
	return lines, nil
}

func upgradeToV8(doc map[string]json.RawMessage, report map[string]int, toolheads func(vendor, model string, nozzle float64) int) ([]string, error) {
	var lines []string
	var printers, calibrations []map[string]json.RawMessage
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return nil, fmt.Errorf("printers: %w", err)
	}
	if raw, ok := doc["calibrations"]; ok {
		if err := json.Unmarshal(raw, &calibrations); err != nil {
			return nil, fmt.Errorf("calibrations: %w", err)
		}
	}
	installed := map[string][]resolve.Nozzle{}
	for _, p := range printers {
		var scalar float64
		json.Unmarshal(p["nozzle"], &scalar)
		delete(p, "nozzle")
		machine := map[string]json.RawMessage{}
		json.Unmarshal(p["machine"], &machine)
		var slots []map[string]json.RawMessage
		json.Unmarshal(p["slots"], &slots)
		count := 1
		if toolheads != nil {
			count = max(toolheads(stringValue(p["vendor"]), stringValue(p["model"]), scalar), 1)
		}
		at := func(key string, i int) string {
			list := resolve.ConfigList(machine[key])
			if len(list) == 0 {
				return ""
			}
			return strings.TrimSpace(list[min(i, len(list)-1)])
		}
		nozzles := make([]resolve.Nozzle, count)
		for i := range nozzles {
			n := &nozzles[i]
			n.Diameter = scalar
			if d, err := strconv.ParseFloat(at("nozzle_diameter", i), 64); err == nil && d > 0 {
				n.Diameter = d
			}
			n.Volume, n.Type = at("nozzle_volume_type", i), at("extruder_type", i)
			if o := at("extruder_offset", i); o != "" {
				var x, y float64
				if _, err := fmt.Sscanf(o, "%gx%g", &x, &y); err == nil {
					n.Offset = &[2]float64{x, y}
				}
			}
			if stringValue(p["swap"]) == "toolheads" && i < len(slots) {
				var d float64
				if json.Unmarshal(slots[i]["nozzle"], &d) == nil && d > 0 {
					n.Diameter = d
				}
				var o [2]float64
				if json.Unmarshal(slots[i]["offset"], &o) == nil {
					n.Offset = &o
				}
			}
		}
		if slices.ContainsFunc(nozzles, func(n resolve.Nozzle) bool { return n.Offset != nil }) {
			for i := range nozzles {
				if nozzles[i].Offset == nil {
					nozzles[i].Offset = &[2]float64{}
				}
			}
		}
		record := fmt.Sprintf("printers %s (%s)", stringValue(p["name"]), stringValue(p["id"]))
		for _, k := range []string{"nozzle_diameter", "nozzle_volume_type", "extruder_type", "extruder_offset"} {
			if v, ok := machine[k]; ok {
				delete(machine, k)
				report["machine_values_moved"]++
				lines = append(lines, fmt.Sprintf("%s: machine %s = %s removed, its nozzles are %s", record, k, v, must(json.Marshal(nozzles))))
			}
		}
		if len(machine) == 0 {
			delete(p, "machine")
		} else {
			p["machine"] = must(json.Marshal(machine))
		}
		for _, s := range slots {
			for _, k := range []string{"nozzle", "offset"} {
				if v, ok := s[k]; ok {
					delete(s, k)
					lines = append(lines, fmt.Sprintf("%s slot %s: %s = %s removed, its nozzles are %s", record, stringValue(s["id"]), k, v, must(json.Marshal(nozzles))))
				}
			}
		}
		if slots != nil {
			p["slots"] = must(json.Marshal(slots))
		}
		p["nozzles"] = must(json.Marshal(nozzles))
		installed[stringValue(p["id"])] = nozzles
		report["printers_with_nozzles"]++
	}
	sided := []map[string]json.RawMessage{}
	for _, c := range calibrations {
		var size float64
		json.Unmarshal(c["nozzle"], &size)
		nozzles := installed[stringValue(c["printer"])]
		sides := []int{}
		for i, n := range nozzles {
			if resolve.SameMm(n.Diameter, size) {
				sides = append(sides, i)
			}
		}
		if len(sides) == 0 {
			for i := range nozzles {
				sides = append(sides, i)
			}
		}
		for j, side := range sides {
			row := maps.Clone(c)
			if j > 0 {
				row["id"] = must(json.Marshal(newID("calibration")))
			}
			row["extruder"] = must(json.Marshal(side))
			if v := nozzles[side].Volume; v != "" {
				row["volume"] = must(json.Marshal(v))
			}
			sided = append(sided, row)
		}
		report["calibrations_sided"] += len(sides)
	}
	doc["printers"] = must(json.Marshal(printers))
	if _, ok := doc["calibrations"]; ok {
		doc["calibrations"] = must(json.Marshal(sided))
	}
	return lines, nil
}

func upgradeToV9(doc map[string]json.RawMessage, report map[string]int) ([]string, error) {
	raw, ok := doc["calibrations"]
	if !ok {
		return nil, nil
	}
	var calibrations []map[string]json.RawMessage
	if err := json.Unmarshal(raw, &calibrations); err != nil {
		return nil, fmt.Errorf("calibrations: %w", err)
	}
	var lines []string
	for _, c := range calibrations {
		values := map[string]json.RawMessage{}
		if err := json.Unmarshal(c["values"], &values); err != nil {
			return nil, fmt.Errorf("calibration %s values: %w", stringValue(c["id"]), err)
		}
		for _, k := range sortedKeys(values) {
			if !resolve.CalibrationKey(k) {
				lines = append(lines, fmt.Sprintf("calibrations %s (filament %s, printer %s): %s = %s dropped, not a calibration setting", stringValue(c["id"]), stringValue(c["filament"]), stringValue(c["printer"]), k, values[k]))
				delete(values, k)
				report["calibration_keys_removed"]++
			}
		}
		c["values"] = must(json.Marshal(values))
	}
	doc["calibrations"] = must(json.Marshal(calibrations))
	return lines, nil
}

func sameLibraryDocument(a, b map[string]json.RawMessage) bool {
	canonical := func(doc map[string]json.RawMessage) string {
		var v any
		if json.Unmarshal(must(json.Marshal(doc)), &v) != nil {
			return ""
		}
		return string(must(json.Marshal(v)))
	}
	ca := canonical(a)
	return ca != "" && ca == canonical(b)
}

func stampEdited(doc, previous map[string]json.RawMessage, now time.Time) {
	for _, list := range []string{"printers", "filaments", "processes"} {
		var records, before []map[string]any
		if json.Unmarshal(doc[list], &records) != nil {
			continue
		}
		json.Unmarshal(previous[list], &before)
		old := map[any]map[string]any{}
		for _, r := range before {
			old[r["id"]] = r
		}
		content := func(r map[string]any) string {
			c := maps.Clone(r)
			delete(c, "revision")
			delete(c, "edited")
			return string(must(json.Marshal(c)))
		}
		for _, r := range records {
			if o := old[r["id"]]; o != nil && content(o) == content(r) {
				if stamp, ok := o["edited"]; ok {
					r["edited"] = stamp
				} else {
					delete(r, "edited")
				}
				continue
			}
			r["edited"] = now.Format(time.RFC3339)
		}
		doc[list] = must(json.Marshal(records))
	}
}

func (l *libraryStore) reportLoad() {
	l.mu.Lock()
	e, err := l.read()
	l.mu.Unlock()
	if err != nil {
		return
	}
	if notice := newerWriter(e); notice != nil {
		log.Printf("profile library: %s", notice.Message)
	}
	if lib, err := l.catalog.ReadLibrary(e.Document); err == nil {
		for id, p := range lib.Problems() {
			log.Printf("profile library: %s is invalid (%s): %s", id, p.Code, p.Message)
		}
	}
}

func trayBefore(a, b string) bool {
	x, _ := resolve.ParseTray(a)
	y, _ := resolve.ParseTray(b)
	if x.External != y.External {
		return !x.External
	}
	if x.External {
		return x.Side < y.Side
	}
	return x.Unit < y.Unit || x.Unit == y.Unit && x.Slot < y.Slot
}
