package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"slices"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func placeRecordKeys(doc map[string]json.RawMessage, catalog *resolve.Catalog) ([]string, error) {
	var printers, processes, filaments, calibrations []map[string]json.RawMessage
	for collection, list := range map[string]*[]map[string]json.RawMessage{"printers": &printers, "processes": &processes, "filaments": &filaments, "calibrations": &calibrations} {
		if raw, ok := doc[collection]; ok {
			if err := json.Unmarshal(raw, list); err != nil {
				return nil, fmt.Errorf("%s: %w", collection, err)
			}
		}
	}
	var records []string
	note := func(format string, args ...any) {
		line := fmt.Sprintf(format, args...)
		records = append(records, line)
		log.Printf("profile library: setting placement: %s", line)
	}
	values := func(record map[string]json.RawMessage, field string) map[string]json.RawMessage {
		v := map[string]json.RawMessage{}
		json.Unmarshal(record[field], &v)
		return v
	}
	set := func(record map[string]json.RawMessage, field string, v map[string]json.RawMessage) {
		if len(v) == 0 {
			delete(record, field)
		} else {
			record[field] = must(json.Marshal(v))
		}
	}
	label := func(record map[string]json.RawMessage) string {
		return fmt.Sprintf("%s (%s)", stringValue(record["name"]), stringValue(record["id"]))
	}
	printerOwned := func(key string) bool { return slices.Contains(catalog.RecordKinds(key), "printer") }
	printKey := func(key string) bool { return catalog.PrintSettings()[key] }
	give := func(p map[string]json.RawMessage, field, key string, v json.RawMessage, from string) {
		allowed := field == "machine" && catalog.PrinterSettings(stringValue(p["model"]))[key] ||
			field == "printOverrides" && printKey(key) && key != "layer_height"
		if !allowed {
			note("%s %s = %s dropped: printer %s (%s) has no such setting", from, key, v, label(p), stringValue(p["model"]))
			return
		}
		target := values(p, field)
		if own, ok := target[key]; ok {
			if sameSetting(key, own, v) {
				note("%s %s = %s dropped: printer %s sets it already", from, key, v, label(p))
			} else {
				note("%s %s = %s dropped: printer %s sets its own %s", from, key, v, label(p), own)
			}
			return
		}
		if field == "machine" {
			if stock, ok := catalogMachineValue(catalog, p, key); ok && sameSetting(key, stock, v) {
				note("%s %s = %s dropped: printer %s has it from its catalog preset", from, key, v, label(p))
				return
			}
		}
		target[key] = v
		set(p, field, target)
		note("%s %s = %s moved to printer %s %s", from, key, v, label(p), field)
	}
	printerByID := map[string]map[string]json.RawMessage{}
	for _, p := range printers {
		printerByID[stringValue(p["id"])] = p
	}
	place := func(owners []map[string]json.RawMessage, key string, v json.RawMessage, from string) {
		switch {
		case !printerOwned(key):
			note("%s %s = %s dropped: not a setting it holds, and no printer setting", from, key, v)
		case len(owners) == 0:
			note("%s %s = %s dropped: no printer uses it", from, key, v)
		default:
			for _, p := range owners {
				give(p, "machine", key, v, from)
			}
		}
	}

	for _, pr := range processes {
		id, own := stringValue(pr["id"]), values(pr, "values")
		var users []map[string]json.RawMessage
		for _, p := range printers {
			if stringValue(p["process"]) == id {
				users = append(users, p)
			}
		}
		changed := false
		for _, key := range sortedKeys(own) {
			if !printKey(key) {
				place(users, key, own[key], "print profile "+label(pr))
				delete(own, key)
				changed = true
			}
		}
		if changed {
			set(pr, "values", own)
		}
	}
	for _, f := range filaments {
		own := values(f, "settings")
		var aliases []string
		json.Unmarshal(f["aliases"], &aliases)
		ids := append([]string{stringValue(f["id"])}, aliases...)
		var holders []map[string]json.RawMessage
		for _, p := range printers {
			var slots []resolve.Slot
			json.Unmarshal(p["slots"], &slots)
			if slices.ContainsFunc(slots, func(s resolve.Slot) bool { return slices.Contains(ids, s.Filament) }) {
				holders = append(holders, p)
			}
		}
		changed := false
		for _, key := range sortedKeys(own) {
			if !catalog.FilamentSettings()[key] && !slices.Contains(resolve.ColourKeys, key) {
				place(holders, key, own[key], "filament "+label(f))
				delete(own, key)
				changed = true
			}
		}
		if changed {
			set(f, "settings", own)
		}
	}
	for _, p := range printers {
		from := "printer " + label(p)
		overrides, changed := values(p, "printOverrides"), false
		for _, key := range sortedKeys(overrides) {
			if !printKey(key) || key == "layer_height" {
				v := overrides[key]
				delete(overrides, key)
				changed = true
				if printerOwned(key) {
					give(p, "machine", key, v, from+" printOverrides")
				} else {
					note("%s printOverrides %s = %s dropped: not a print setting a printer can set", from, key, v)
				}
			}
		}
		if changed {
			set(p, "printOverrides", overrides)
		}
		allowed := catalog.PrinterSettings(stringValue(p["model"]))
		if allowed == nil {
			continue
		}
		changed = false
		machine, moved := values(p, "machine"), map[string]json.RawMessage{}
		for _, key := range sortedKeys(machine) {
			v := machine[key]
			switch {
			case allowed[key]:
				continue
			case slices.Contains(resolve.HardwareKeys, key):
				note("%s machine %s = %s dropped: the printer's nozzles record holds it", from, key, v)
			case printKey(key) && key != "layer_height":
				moved[key] = v
			default:
				note("%s machine %s = %s dropped: not a setting of %s", from, key, v, stringValue(p["model"]))
			}
			delete(machine, key)
			changed = true
		}
		if changed {
			set(p, "machine", machine)
		}
		for _, key := range sortedKeys(moved) {
			give(p, "printOverrides", key, moved[key], from+" machine")
		}
	}
	for _, c := range calibrations {
		own, changed := values(c, "values"), false
		from := fmt.Sprintf("calibration %s (filament %s, printer %s)", stringValue(c["id"]), stringValue(c["filament"]), stringValue(c["printer"]))
		for _, key := range sortedKeys(own) {
			if !resolve.CalibrationKey(key) && !slices.Contains(resolve.ColourKeys, key) {
				var owners []map[string]json.RawMessage
				if p := printerByID[stringValue(c["printer"])]; p != nil {
					owners = append(owners, p)
				}
				place(owners, key, own[key], from)
				delete(own, key)
				changed = true
			}
		}
		if changed {
			c["values"] = must(json.Marshal(own))
		}
	}
	if len(records) > 0 {
		doc["printers"], doc["processes"], doc["filaments"] = must(json.Marshal(printers)), must(json.Marshal(processes)), must(json.Marshal(filaments))
		if calibrations != nil {
			doc["calibrations"] = must(json.Marshal(calibrations))
		}
	}
	if colourIn(filaments, "settings") || colourIn(calibrations, "values") {
		report, lines, err := moveFilamentColours(doc)
		if err != nil {
			return nil, err
		}
		note("colours moved off filaments and calibrations: %v", report)
		records = append(records, lines...)
	}
	return records, nil
}

func catalogMachineValue(catalog *resolve.Catalog, p map[string]json.RawMessage, key string) (json.RawMessage, bool) {
	var nozzles []resolve.Nozzle
	json.Unmarshal(p["nozzles"], &nozzles)
	if len(nozzles) > 0 {
		if v := catalog.VariantByID(nozzles[0].Variant); v != nil {
			if value, ok := v.Values[key]; ok {
				return must(json.Marshal(value)), true
			}
		}
	}
	if m := catalog.Printers[stringValue(p["model"])]; m != nil {
		if value, ok := m.Base[key]; ok {
			return must(json.Marshal(value)), true
		}
	}
	return nil, false
}

func colourIn(list []map[string]json.RawMessage, field string) bool {
	for _, r := range list {
		var v map[string]json.RawMessage
		json.Unmarshal(r[field], &v)
		for _, k := range resolve.ColourKeys {
			if _, ok := v[k]; ok {
				return true
			}
		}
	}
	return false
}

func sameSetting(key string, a, b json.RawMessage) bool {
	norm := func(raw json.RawMessage) []byte {
		var v any
		if json.Unmarshal(resolve.CanonicalValue(key, raw), &v) != nil {
			return raw
		}
		if list, ok := v.([]any); ok && len(list) == 1 {
			v = list[0]
		}
		return must(json.Marshal(v))
	}
	return bytes.Equal(norm(a), norm(b))
}
