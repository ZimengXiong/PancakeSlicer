package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func TestNozzlePresetsUpgradeKeepsRollbackBackup(t *testing.T) {
	if err := configureResolve([]byte(`{"layer_height":{"type":"Float","profile":"process","default":"0.2"}}`)); err != nil {
		t.Fatal(err)
	}
	store := &libraryStore{dir: t.TempDir(), catalog: &resolve.Catalog{Printers: map[string]*resolve.CatalogPrinter{"dual": {ID: "dual"}}}}
	doc := emptyLibrary("nozzle-upgrade")
	doc["schema"] = json.RawMessage(`13`)
	doc["printers"] = json.RawMessage(`[{"id":"profile:dual","name":"Dual","revision":3,"vendor":"Test","model":"dual","swap":"toolheads","slots":[{"id":"slot:left"}],"nozzles":[{"variant":"","diameter":0.55},{"variant":"","diameter":0.55}]}]`)
	original, err := json.Marshal(libraryEnvelope{Document: doc, Operations: map[string]libraryReceipt{}})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(store.dir, libraryFile), original, 0600); err != nil {
		t.Fatal(err)
	}
	upgraded, err := store.read()
	if err != nil {
		t.Fatal(err)
	}
	if string(upgraded.Document["schema"]) != "14" || len(upgraded.Migrations) != 1 {
		t.Fatalf("got schema %s and migrations %+v, want schema 14 and one migration", upgraded.Document["schema"], upgraded.Migrations)
	}
	backup, err := os.ReadFile(filepath.Join(store.dir, upgraded.Migrations[0].Backup))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(backup, original) {
		t.Fatal("the rollback backup differs from the original schema 13 library")
	}
	lib, err := store.catalog.ReadLibrary(upgraded.Document)
	if err != nil {
		t.Fatal(err)
	}
	p := lib.Printers["profile:dual"]
	if p == nil || len(p.Nozzles) != 2 || p.Nozzles[1].Diameter != 0.55 || len(p.NozzlePresets) != 0 {
		t.Fatalf("upgrade changed the legacy printer: %+v", p)
	}
}
