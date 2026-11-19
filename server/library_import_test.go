package main

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func exportedPreset(t *testing.T, typ, name string, record map[string]any) parsedPreset {
	t.Helper()
	raw, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	return parsedPreset{File: name + ".json", Name: name, Type: typ, Record: raw, Values: map[string]json.RawMessage{}, Meta: map[string]json.RawMessage{}}
}

func exportedPrinter(t *testing.T, filament, process string) parsedPreset {
	t.Helper()
	record := map[string]any{
		"id": "profile:old-printer", "name": "Voron 2.4 300", "revision": 0, "vendor": "Voron", "model": "Voron 2.4 300", "swap": "manual",
		"nozzles": []map[string]any{{"variant": "Voron 2.4 300 0.4 nozzle", "diameter": 0.4}},
		"slots":   []map[string]any{{"id": "slot:old-1", "filament": filament, "colour": "#ff0000"}},
	}
	if process != "" {
		record["process"] = process
	}
	return exportedPreset(t, "machine", "Voron 2.4 300", record)
}

func exportedFilament(t *testing.T, id string) parsedPreset {
	t.Helper()
	return exportedPreset(t, "filament", "PLA Red", map[string]any{"id": id, "name": "PLA Red", "revision": 0, "product": "Generic PLA"})
}

func blankLibrary() *resolve.Library {
	return &resolve.Library{Printers: map[string]*resolve.Printer{}, Filaments: map[string]*resolve.Filament{}, Alias: map[string]string{}, Processes: map[string]*resolve.Process{}}
}

func rowOf(t *testing.T, rows []importRow, kind string) importRow {
	t.Helper()
	for _, r := range rows {
		if r.Kind == kind {
			return r
		}
	}
	t.Fatalf("no %s row in %+v", kind, rows)
	return importRow{}
}

func recordFields(t *testing.T, raw json.RawMessage) (id string, aliases []string, slots []resolve.Slot, process string) {
	t.Helper()
	var rec struct {
		ID      string         `json:"id"`
		Aliases []string       `json:"aliases"`
		Slots   []resolve.Slot `json:"slots"`
		Process string         `json:"process"`
	}
	if err := json.Unmarshal(raw, &rec); err != nil {
		t.Fatal(err)
	}
	return rec.ID, rec.Aliases, rec.Slots, rec.Process
}

func TestImportedPrinterKeepsItsSlotFilamentAndPrintProfile(t *testing.T) {
	presets := []parsedPreset{
		exportedPrinter(t, "profile:old-filament", "profile:old-process"),
		exportedFilament(t, "profile:old-filament"),
		exportedPreset(t, "process", "Standard", map[string]any{"id": "profile:old-process", "name": "Standard", "revision": 0}),
	}
	rows := (&Server{}).planImport(presets, blankLibrary())

	filamentID, _, _, _ := recordFields(t, rowOf(t, rows, "filament").Record)
	processID, _, _, _ := recordFields(t, rowOf(t, rows, "process").Record)
	printer := rowOf(t, rows, "printer")
	_, _, slots, process := recordFields(t, printer.Record)

	if slots[0].Filament != filamentID {
		t.Errorf("slot holds %q, the imported filament is %q", slots[0].Filament, filamentID)
	}
	if process != processID {
		t.Errorf("printer's print profile is %q, the imported one is %q", process, processID)
	}
	if printer.Note != "" {
		t.Errorf("nothing was dropped, but the row says %q", printer.Note)
	}
	imported := blankLibrary()
	imported.Filaments[filamentID] = &resolve.Filament{ID: filamentID}
	if f := imported.Filament(slots[0].Filament); f == nil || !f.Live() {
		t.Errorf("the slot's filament %q is not in the library after the import", slots[0].Filament)
	}
}

func TestReplacedFilamentStaysReachableUnderItsExportedID(t *testing.T) {
	revision := uint64(1)
	lib := blankLibrary()
	lib.Filaments["profile:existing"] = &resolve.Filament{ID: "profile:existing", Name: "PLA Red", Revision: &revision, Product: "Generic PLA"}
	presets := []parsedPreset{
		exportedPrinter(t, "profile:old-filament", ""),
		exportedFilament(t, "profile:old-filament"),
	}
	rows := (&Server{}).planImport(presets, lib)

	filament := rowOf(t, rows, "filament")
	if len(filament.Matches) != 1 {
		t.Fatalf("expected the existing PLA Red as a match, got %+v", filament.Matches)
	}
	id, aliases, _, _ := recordFields(t, filament.Matches[0].Record)
	if id != "profile:existing" {
		t.Fatalf("replacing keeps the existing id, got %q", id)
	}
	if len(aliases) != 1 || aliases[0] != "profile:old-filament" {
		t.Fatalf("replaced filament should carry the exported id as alias, got %v", aliases)
	}

	_, _, slots, _ := recordFields(t, rowOf(t, rows, "printer").Record)
	replaced := blankLibrary()
	replaced.Filaments[id] = &resolve.Filament{ID: id}
	replaced.Alias[aliases[0]] = id
	if f := replaced.Filament(slots[0].Filament); f == nil || f.ID != id {
		t.Errorf("slot %q does not resolve to the replaced filament", slots[0].Filament)
	}
}

func TestImportedPrinterDropsReferencesToProfilesThatAreNowhere(t *testing.T) {
	rows := (&Server{}).planImport([]parsedPreset{exportedPrinter(t, "profile:old-filament", "profile:old-process")}, blankLibrary())

	printer := rowOf(t, rows, "printer")
	if printer.Status != "new" {
		t.Fatalf("the printer should still import, got %s: %s", printer.Status, printer.Note)
	}
	_, _, slots, process := recordFields(t, printer.Record)
	if len(slots) != 1 || slots[0].ID != "slot:old-1" || slots[0].Colour != "#ff0000" {
		t.Errorf("the slot itself must survive, got %+v", slots)
	}
	if slots[0].Filament != "" {
		t.Errorf("dangling slot filament kept: %q", slots[0].Filament)
	}
	if process != "" {
		t.Errorf("dangling print profile kept: %q", process)
	}
	if !strings.Contains(printer.Note, "pick them again") {
		t.Errorf("the row should tell the user to pick again, got %q", printer.Note)
	}
}
