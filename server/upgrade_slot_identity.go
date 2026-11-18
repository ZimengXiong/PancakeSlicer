package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func upgradeSlotIdentity(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
	var printers []map[string]json.RawMessage
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return nil, fmt.Errorf("printers: %w", err)
	}
	chosen, err := c.v4Choices()
	if err != nil {
		return nil, err
	}
	aliases := filamentAliases(doc)
	var lines []string
	for _, p := range printers {
		name := fmt.Sprintf("%s (%s)", stringValue(p["name"]), stringValue(p["id"]))
		var slots []map[string]json.RawMessage
		if err := json.Unmarshal(p["slots"], &slots); err != nil {
			return nil, fmt.Errorf("%s slots: %w", name, err)
		}
		slots, mixes, mixLines := slotMixesByID(name, slots, p["mixes"])
		lines = append(lines, mixLines...)
		lines = append(lines, repairChoiceTrays(name, slots, chosen[stringValue(p["id"])], aliases)...)
		units, trayLines := singleNozzleTrays(name, p, slots)
		lines = append(lines, trayLines...)
		p["slots"] = must(json.Marshal(slots))
		if len(mixes) > 0 {
			p["mixes"] = must(json.Marshal(mixes))
		} else {
			delete(p, "mixes")
		}
		if units != nil {
			p["units"] = must(json.Marshal(units))
		}
	}
	doc["printers"] = must(json.Marshal(printers))
	c.report["slot_identity_lines"] += len(lines)
	return lines, nil
}

func slotMixesByID(name string, slots []map[string]json.RawMessage, raw json.RawMessage) ([]map[string]json.RawMessage, []resolve.Mix, []string) {
	type storedMix struct {
		Components []int     `json:"components"`
		Ratios     []float64 `json:"ratios"`
		Gradient   *int      `json:"gradient"`
		PerPart    bool      `json:"perPart"`
	}
	var lines []string
	var stored map[string]json.RawMessage
	if len(raw) == 0 || string(raw) == "null" {
		return slots, nil, nil
	}
	if err := json.Unmarshal(raw, &stored); err != nil {
		return slots, nil, []string{fmt.Sprintf("%s: mixes dropped, not a mix per strip index (%v): %s", name, err, raw)}
	}
	old := map[string]storedMix{}
	keys := []int{}
	for _, k := range sortedKeys(stored) {
		var m storedMix
		i, err := strconv.Atoi(k)
		if err == nil {
			err = json.Unmarshal(stored[k], &m)
		}
		if err != nil {
			lines = append(lines, fmt.Sprintf("%s: mix %q dropped, not a mixed filament at a strip index: %s", name, k, stored[k]))
			continue
		}
		old[k] = m
		keys = append(keys, i)
	}
	sort.Ints(keys)
	physical := []map[string]json.RawMessage{}
	for i, s := range slots {
		if slices.Contains(keys, i) {
			lines = append(lines, fmt.Sprintf("%s: slot %d (%s) was mixed filament %d's place in the slot list, not a filament; dropped: %s", name, i+1, stringValue(s["id"]), i+1, must(json.Marshal(s))))
			continue
		}
		physical = append(physical, s)
	}
	mixes := []resolve.Mix{}
	for _, k := range keys {
		m := old[strconv.Itoa(k)]
		components := []string{}
		for _, n := range m.Components {
			if n >= 1 && n <= len(physical) {
				components = append(components, stringValue(physical[n-1]["id"]))
			}
		}
		if len(components) != len(m.Components) || len(components) < 2 {
			lines = append(lines, fmt.Sprintf("%s: mixed filament %d mixes slots %v, which the printer doesn't have; dropped: %s", name, k+1, m.Components, stored[strconv.Itoa(k)]))
			continue
		}
		mix := resolve.Mix{ID: newID("mix"), Components: components, Ratios: m.Ratios, Gradient: m.Gradient, PerPart: m.PerPart}
		mixes = append(mixes, mix)
		lines = append(lines, fmt.Sprintf("%s: mixed filament %d is %s over slots %v", name, k+1, mix.ID, components))
	}
	return physical, mixes, lines
}

func singleNozzleTrays(name string, p map[string]json.RawMessage, slots []map[string]json.RawMessage) ([]resolve.Unit, []string) {
	var nozzles []resolve.Nozzle
	json.Unmarshal(p["nozzles"], &nozzles)
	feed, ok := resolve.PrinterFeed(stringValue(p["model"]))
	if !ok || len(nozzles) != 1 || stringValue(p["swap"]) == "toolheads" || !slices.ContainsFunc(slots, func(s map[string]json.RawMessage) bool { return stringValue(s["tray"]) == "" }) {
		return nil, nil
	}
	var units []resolve.Unit
	derived := p["units"] == nil || json.Unmarshal(p["units"], &units) != nil
	if derived {
		units = nil
		extra := len(slots) - 1
		if feed.External != "perNozzle" {
			extra = map[bool]int{true: len(slots), false: 0}[len(slots) > 1]
		}
		for range min(feed.MaxUnits, (max(0, extra)+feed.Slots-1)/feed.Slots) {
			units = append(units, resolve.Unit{Nozzle: 0})
		}
	}
	var positions []string
	if feed.External == "perNozzle" {
		for _, pos := range resolve.FeedPositions(feed, units) {
			positions = append(positions, pos.Tray)
		}
	} else if len(units) == 0 {
		positions = []string{"ext"}
	} else {
		for u := range units {
			for s := 0; s < feed.Slots; s++ {
				positions = append(positions, fmt.Sprintf("ams%d.%d", u, s))
			}
		}
	}
	held := map[string]bool{}
	for _, s := range slots {
		if t := stringValue(s["tray"]); slices.Contains(positions, t) {
			held[t] = true
		}
	}
	var lines []string
	for i, s := range slots {
		if t := stringValue(s["tray"]); slices.Contains(positions, t) {
			continue
		}
		free := slices.IndexFunc(positions, func(t string) bool { return !held[t] })
		if free < 0 {
			lines = append(lines, fmt.Sprintf("%s slot %d (%s): no free tray among %v; left without one", name, i+1, stringValue(s["id"]), positions))
			continue
		}
		held[positions[free]] = true
		s["tray"] = must(json.Marshal(positions[free]))
		lines = append(lines, fmt.Sprintf("%s slot %d (%s, filament %s): tray %s", name, i+1, stringValue(s["id"]), stringValue(s["filament"]), positions[free]))
	}
	if !derived {
		units = nil
	}
	return units, lines
}

func repairChoiceTrays(name string, slots []map[string]json.RawMessage, chosen map[string]string, aliases map[string]string) []string {
	if len(chosen) == 0 {
		return nil
	}
	trays := make([]string, 0, len(chosen))
	for t := range chosen {
		trays = append(trays, t)
	}
	sort.Strings(trays)
	k := min(len(trays), len(slots))
	for i := range k {
		if stringValue(slots[i]["tray"]) != trays[i] {
			return nil
		}
	}
	next, taken := make([]string, k), make([]bool, k)
	for _, t := range trays[:k] {
		want, found := aliases[chosen[t]], -1
		for i := range k {
			if !taken[i] && want != "" && aliases[stringValue(slots[i]["filament"])] == want {
				found = i
				break
			}
		}
		if found < 0 {
			return []string{fmt.Sprintf("%s: v0.4.9 AMS choice %s (filament %s) is on no slot that took a choice; trays left as they are", name, t, chosen[t])}
		}
		taken[found], next[found] = true, t
	}
	var lines []string
	for i := range k {
		if next[i] != trays[i] {
			lines = append(lines, fmt.Sprintf("%s slot %d (%s, filament %s): tray %s -> %s, the tray its filament was chosen for in v0.4.9", name, i+1, stringValue(slots[i]["id"]), stringValue(slots[i]["filament"]), trays[i], next[i]))
			slots[i]["tray"] = must(json.Marshal(next[i]))
		}
	}
	return lines
}

func filamentAliases(doc map[string]json.RawMessage) map[string]string {
	var filaments []struct {
		ID      string   `json:"id"`
		Aliases []string `json:"aliases"`
	}
	json.Unmarshal(doc["filaments"], &filaments)
	out := map[string]string{}
	for _, f := range filaments {
		out[f.ID] = f.ID
		for _, a := range f.Aliases {
			out[a] = f.ID
		}
	}
	return out
}

func (c *upgradeContext) v4Choices() (map[string]map[string]string, error) {
	raw, err := os.ReadFile(filepath.Join(c.store.dir, libraryFile))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var e struct {
		Migrations []libraryMigration `json:"migrations"`
	}
	json.Unmarshal(raw, &e)
	out := map[string]map[string]string{}
	for _, m := range e.Migrations {
		if m.From != 4 || m.SlotBackup == "" {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(c.store.dir, m.SlotBackup))
		if err != nil {
			return nil, fmt.Errorf("the v0.4.9 slot store %s the schema 4 upgrade kept: %w", m.SlotBackup, err)
		}
		var store legacySlotDoc
		if err := json.Unmarshal(raw, &store); err != nil {
			return nil, fmt.Errorf("%s: %w", m.SlotBackup, err)
		}
		for printer, trays := range store.Chosen {
			out[printer] = map[string]string{}
			for t, choice := range trays {
				out[printer][t] = choice.Filament
			}
		}
	}
	return out, nil
}
