package resolve

import (
	"encoding/json"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

func TestLibraryNozzlePresets(t *testing.T) {
	catalog := &Catalog{Printers: map[string]*CatalogPrinter{"dual": {ID: "dual"}}}
	for _, tc := range []struct {
		name, field, wantError string
		want                   []NozzlePreset
	}{
		{name: "legacy"},
		{name: "saved", field: `,"nozzlePresets":[{"diameter":0.1,"variants":["left","right"]},{"diameter":2,"variants":["right","left"]}]`,
			want: []NozzlePreset{{0.1, []string{"left", "right"}}, {2, []string{"right", "left"}}}},
		{name: "too small", field: `,"nozzlePresets":[{"diameter":0.099,"variants":["left","right"]}]`, wantError: "unique diameters"},
		{name: "too large", field: `,"nozzlePresets":[{"diameter":2.001,"variants":["left","right"]}]`, wantError: "unique diameters"},
		{name: "duplicate", field: `,"nozzlePresets":[{"diameter":0.5,"variants":["left","right"]},{"diameter":0.5,"variants":["right","left"]}]`, wantError: "unique diameters"},
		{name: "missing nozzle", field: `,"nozzlePresets":[{"diameter":0.5,"variants":["left"]}]`, wantError: "variant for every nozzle"},
		{name: "extra nozzle", field: `,"nozzlePresets":[{"diameter":0.5,"variants":["left","right","left"]}]`, wantError: "variant for every nozzle"},
		{name: "empty variant", field: `,"nozzlePresets":[{"diameter":0.5,"variants":["left",""]}]`, wantError: "variant for every nozzle"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var doc map[string]json.RawMessage
			err := json.Unmarshal(fmt.Appendf(nil, `{"schema":%d,"printers":[{"id":"profile:test","name":"Test","revision":0,"vendor":"Test","model":"dual","swap":"toolheads","slots":[{"id":"slot:a"}],"nozzles":[{"variant":"left","diameter":0.4},{"variant":"right","diameter":0.4}]%s}],"filaments":[],"processes":[],"calibrations":[]}`, librarySchema, tc.field), &doc)
			if err != nil {
				t.Fatal(err)
			}
			lib, err := catalog.ParseLibrary(doc, nil)
			if tc.wantError != "" {
				if err == nil || !strings.Contains(err.Error(), tc.wantError) {
					t.Fatalf("ParseLibrary: got %v, want %q", err, tc.wantError)
				}
				stored, err := catalog.ReadLibrary(doc)
				if err != nil {
					t.Fatalf("ReadLibrary failed the whole library: %v", err)
				}
				if p := stored.Printers["profile:test"]; p == nil || p.Problem == nil || p.Problem.Code != "nozzle_presets" {
					t.Fatalf("ReadLibrary: got printer %+v, want it kept with a nozzle_presets problem", p)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			doc["printers"], err = json.Marshal([]*Printer{lib.Printers["profile:test"]})
			if err != nil {
				t.Fatal(err)
			}
			loaded, err := catalog.ReadLibrary(doc)
			if err != nil {
				t.Fatal(err)
			}
			p := loaded.Printers["profile:test"]
			if !reflect.DeepEqual(p.NozzlePresets, tc.want) || p.Nozzles[0].Diameter != 0.4 || p.Nozzles[1].Diameter != 0.4 {
				t.Fatalf("reloaded presets = %+v, want %+v; active nozzles = %+v, want both 0.4 mm", p.NozzlePresets, tc.want, p.Nozzles)
			}
		})
	}
}
