package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"time"
)

type upgradeStep struct {
	From, To int
	ID       string
	Run      func(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error)
}

var upgradeSteps = []upgradeStep{
	{4, 6, "devices", upgradeDevices},
	{5, 6, "matched-slots", upgradeMatchedSlots},
	{6, 7, "colours-on-slots", func(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
		return c.counted(func() ([]string, error) { return upgradeToV7(doc, c.report) })
	}},
	{7, 8, "nozzle-records", func(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
		return c.counted(func() ([]string, error) { return upgradeToV8(doc, c.report, c.store.toolheads) })
	}},
	{8, 9, "calibration-keys", func(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
		return upgradeToV9(doc, c.report)
	}},
	{9, 10, "catalog-records", func(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
		return c.counted(func() ([]string, error) { return upgradeToV10(doc, c.report, c.store.catalog) })
	}},
	{10, 11, "settings-slots-folds", func(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
		return c.store.upgradeToV11(c, doc)
	}},
	{11, 12, "starter-generations", upgradeStarterGenerations},
	{12, 13, "slot-identity", upgradeSlotIdentity},
}

func init() {
	for i, st := range upgradeSteps {
		if st.To <= st.From || st.To > librarySchema || i > 0 && upgradeSteps[i-1].From >= st.From {
			panic(fmt.Sprintf("library upgrade step %s (%d to %d) is out of order", st.ID, st.From, st.To))
		}
	}
	for k := upgradeSteps[0].From; k < librarySchema; k++ {
		if upgradePath(k) == nil {
			panic(fmt.Sprintf("library schema %d has no upgrade path to schema %d", k, librarySchema))
		}
	}
}

func upgradePath(from int) []upgradeStep {
	var path []upgradeStep
	for at := from; at < librarySchema; {
		i := slices.IndexFunc(upgradeSteps, func(s upgradeStep) bool { return s.From == at })
		if i < 0 {
			return nil
		}
		path = append(path, upgradeSteps[i])
		at = upgradeSteps[i].To
	}
	return path
}

type upgradeContext struct {
	store         *libraryStore
	report        map[string]int
	beforeCommit  []func(m *libraryMigration) error
	afterCommit   []func() error
	workspaceRaw  []byte
	workspaceErr  error
	workspaceRead bool
}

func (c *upgradeContext) workspace() ([]byte, error) {
	if !c.workspaceRead {
		c.workspaceRead = true
		c.workspaceRaw, c.workspaceErr = os.ReadFile(filepath.Join(c.store.dir, "workspace", "workspace.json"))
		if errors.Is(c.workspaceErr, os.ErrNotExist) {
			c.workspaceRaw, c.workspaceErr = nil, nil
		}
	}
	return c.workspaceRaw, c.workspaceErr
}

func (c *upgradeContext) counted(run func() ([]string, error)) ([]string, error) {
	before := maps.Clone(c.report)
	lines, err := run()
	if err != nil {
		return nil, err
	}
	for _, k := range sortedKeys(c.report) {
		if n := c.report[k] - before[k]; n != 0 {
			lines = append(lines, fmt.Sprintf("%s %d", k, n))
		}
	}
	return lines, nil
}

type upgradeRefused struct {
	Step     string
	From, To int
	Err      error
}

func (r *upgradeRefused) Error() string {
	return fmt.Sprintf("its upgrade step %s (schema %d to %d) failed, and the library was left as it is: %v", r.Step, r.From, r.To, r.Err)
}
func (r *upgradeRefused) Unwrap() error { return r.Err }

func (l *libraryStore) upgrade(e *libraryEnvelope, original []byte, from int) error {
	path := upgradePath(from)
	if path == nil {
		return fmt.Errorf("this build can't upgrade a library at schema %d", from)
	}
	c := &upgradeContext{store: l, report: map[string]int{}}
	next := maps.Clone(e.Document)
	var records []string
	for _, s := range path {
		lines, err := s.Run(c, next)
		if err != nil {
			log.Printf("profile library: upgrade refused at step %s (%d to %d): %v", s.ID, s.From, s.To, err)
			return &upgradeRefused{s.ID, s.From, s.To, err}
		}
		next["schema"] = json.RawMessage(fmt.Sprint(s.To))
		for _, line := range lines {
			records = append(records, s.ID+": "+line)
		}
		log.Printf("profile library: upgrade step %s (%d to %d): %d lines", s.ID, s.From, s.To, len(lines))
	}
	upgraded, err := l.catalog.ReadLibrary(next)
	if err != nil {
		return &upgradeRefused{"validate", from, librarySchema, fmt.Errorf("the upgraded library is invalid: %w", err)}
	}
	for id, p := range upgraded.Problems() {
		log.Printf("profile library: upgraded record %s is invalid (%s): %s", id, p.Code, p.Message)
	}
	stamp := time.Now().UTC()
	backup := fmt.Sprintf("library.v%d.%s.json", from, stamp.Format("20060102T150405Z"))
	if err := writeBackup(filepath.Join(l.dir, backup), original); err != nil {
		return err
	}
	m := libraryMigration{From: from, To: librarySchema, At: stamp, Backup: backup, Report: c.report, Records: records}
	for _, f := range c.beforeCommit {
		if err := f(&m); err != nil {
			return err
		}
	}
	e.Document = next
	e.Migrations = append(e.Migrations, m)
	if err := l.write(e); err != nil {
		return err
	}
	for _, f := range c.afterCommit {
		if err := f(); err != nil {
			return err
		}
	}
	log.Printf("profile library: upgraded schema %d to %d (backup %s): %v", from, librarySchema, backup, c.report)
	return nil
}

func upgradeDevices(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
	l := c.store
	printers := filepath.Join(l.dir, "printers")
	slotsPath := filepath.Join(printers, "printer-slots.json")
	marker := filepath.Join(printers, slotUpgradeMarker)
	live, liveErr := os.ReadFile(slotsPath)
	if liveErr != nil && !errors.Is(liveErr, os.ErrNotExist) {
		return nil, liveErr
	}
	slotsRaw, resumed, err := upgradeSource(marker, live, liveErr)
	if err == nil && !resumed {
		slotsRaw, resumed, err = upgradeSource(filepath.Join(printers, legacySlotMarker), live, liveErr)
	}
	if err != nil {
		return nil, err
	}
	if !resumed && liveErr == nil {
		slotsRaw = live
	}
	var slotDocument legacySlotDoc
	if slotsRaw != nil {
		if err := json.Unmarshal(slotsRaw, &slotDocument); err != nil {
			return nil, fmt.Errorf("printer-slots.json is unreadable: %w", err)
		}
	}
	next, report, lines, err := migrateLibraryV4(doc, slotDocument.Chosen, func(vendor, model string, nozzle float64) (string, []string) {
		return l.swap(l.catalog.Variant(model, nozzle))
	})
	if err != nil {
		return nil, err
	}
	maps.Copy(c.report, report)
	clear(doc)
	maps.Copy(doc, next)
	lines = append(lines, fmt.Sprintf("AMS slot choices read from %s", map[bool]string{true: "an interrupted upgrade's copy", false: "printers/printer-slots.json"}[resumed]))
	if slotsRaw != nil {
		c.beforeCommit = append(c.beforeCommit, func(m *libraryMigration) error {
			m.SlotBackup = fmt.Sprintf("printers/printer-slots.v4.%s.json", m.At.Format("20060102T150405Z"))
			if err := writeBackup(filepath.Join(l.dir, m.SlotBackup), slotsRaw); err != nil {
				return err
			}
			return writeFileAtomic(marker, slotsRaw)
		})
		c.afterCommit = append(c.afterCommit, func() error {
			if err := writeFileAtomic(slotsPath, rewrittenSlots(slotsRaw)); err != nil {
				return err
			}
			if err := os.Remove(marker); err != nil {
				return err
			}
			return syncWorkspaceDirectory(printers)
		})
	}
	c.afterCommit = append(c.afterCommit, func() error {
		if err := l.retireLegacySlotMarker(); err != nil {
			return err
		}
		l.settled = true
		return nil
	})
	return lines, nil
}

func upgradeMatchedSlots(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
	var matched matchedSlotDoc
	slotsRaw, err := os.ReadFile(filepath.Join(c.store.dir, "printers", "printer-slots.json"))
	if err == nil {
		var head struct {
			Schema int `json:"schema"`
		}
		if json.Unmarshal(slotsRaw, &head) == nil && head.Schema == 1 {
			if err := json.Unmarshal(slotsRaw, &matched); err != nil {
				return nil, fmt.Errorf("printer-slots.json is unreadable: %w", err)
			}
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	var printers, filaments []map[string]json.RawMessage
	if err := json.Unmarshal(doc["printers"], &printers); err != nil {
		return nil, fmt.Errorf("printers: %w", err)
	}
	if err := json.Unmarshal(doc["filaments"], &filaments); err != nil {
		return nil, fmt.Errorf("filaments: %w", err)
	}
	live := map[string]bool{}
	for _, f := range filaments {
		var aliases []string
		json.Unmarshal(f["aliases"], &aliases)
		archived := len(f["archived"]) > 0 && string(f["archived"]) != "null"
		for _, id := range append([]string{stringValue(f["id"])}, aliases...) {
			live[id] = !archived
		}
	}
	for _, k := range []string{"cleared_automatic", "cleared_removed", "kept"} {
		c.report[k] = c.report[k]
	}
	var lines []string
	for _, p := range printers {
		id, name := stringValue(p["id"]), stringValue(p["name"])
		automatic := map[string]bool{}
		for _, m := range matched.Last[id].Slots {
			if m.Filament != nil && (m.Rule == "exact" || m.Rule == "type" || m.Rule == "generic" || m.Rule == "remembered") {
				automatic[m.Slot+"\n"+*m.Filament] = true
			}
		}
		var slots []map[string]json.RawMessage
		if json.Unmarshal(p["slots"], &slots) != nil {
			continue
		}
		for _, s := range slots {
			filament, tray := stringValue(s["filament"]), stringValue(s["tray"])
			switch {
			case filament == "":
				continue
			case !live[filament]:
				lines = append(lines, fmt.Sprintf("printers %s (%s) slot %s: removed filament %s cleared", name, id, stringValue(s["id"]), filament))
				c.report["cleared_removed"]++
			case tray != "" && automatic[tray+"\n"+filament]:
				lines = append(lines, fmt.Sprintf("printers %s (%s) slot %s: filament %s the AMS matching chose for %s cleared", name, id, stringValue(s["id"]), filament, tray))
				c.report["cleared_automatic"]++
			default:
				c.report["kept"]++
				continue
			}
			s["filament"] = json.RawMessage(`""`)
		}
		p["slots"] = must(json.Marshal(slots))
	}
	doc["printers"] = must(json.Marshal(printers))
	return lines, nil
}

func upgradeStarterGenerations(c *upgradeContext, doc map[string]json.RawMessage) ([]string, error) {
	var list []map[string]json.RawMessage
	if raw, ok := doc["processes"]; ok {
		if err := json.Unmarshal(raw, &list); err != nil {
			return nil, fmt.Errorf("processes: %w", err)
		}
	}
	var lines []string
	for _, p := range list {
		if _, ok := p["starterGeneration"]; !ok {
			continue
		}
		delete(p, "starterGeneration")
		lines = append(lines, fmt.Sprintf("processes %s (%s): starter generation removed", stringValue(p["name"]), stringValue(p["id"])))
	}
	if len(lines) > 0 {
		c.report["starter_generations_removed"] = len(lines)
		doc["processes"] = must(json.Marshal(list))
	}
	return lines, nil
}
