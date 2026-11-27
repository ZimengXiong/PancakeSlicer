package resolve

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"
	"sort"
	"regexp"
	"slices"
	"strings"
	"unicode"
	"unicode/utf8"
)

type Feed struct {
	Unit     string   `json:"unit"`
	Slots    int      `json:"slots"`
	MaxUnits int      `json:"maxUnits"`
	External string   `json:"external"`
	Nozzles  []string `json:"nozzles,omitempty"`
}

var (
	printerFeeds  map[string]Feed
	librarySchema int
	settingKey    = regexp.MustCompile(`^[a-zA-Z][a-zA-Z0-9_]*$`)
	swapModes     = []string{"ams", "toolheads", "manual"}
)

var ColourKeys = []string{"filament_colour", "default_filament_colour"}

func PrinterFeed(model string) (Feed, bool) {
	f, ok := printerFeeds[model]
	return f, ok
}

type FeedPosition struct {
	Tray   string
	Nozzle int
}

func FeedPositions(feed Feed, units []Unit) []FeedPosition {
	fed := func(n int) bool { return slices.ContainsFunc(units, func(u Unit) bool { return u.Nozzle == n }) }
	order := make([]int, max(1, len(feed.Nozzles)))
	for n := range order {
		order[n] = n
	}
	sort.SliceStable(order, func(a, b int) bool { return fed(order[a]) && !fed(order[b]) })
	var out []FeedPosition
	for _, n := range order {
		for u, unit := range units {
			if unit.Nozzle == n {
				for s := 0; s < feed.Slots; s++ {
					out = append(out, FeedPosition{fmt.Sprintf("ams%d.%d", u, s), n})
				}
			}
		}
		out = append(out, FeedPosition{map[bool]string{true: "ext.L", false: "ext.R"}[n == 0], n})
	}
	return out
}

func colourKey(values map[string]json.RawMessage) string {
	for _, k := range ColourKeys {
		if _, ok := values[k]; ok {
			return k
		}
	}
	return ""
}

func CalibrationKey(k string) bool {
	if k == TemperatureOffset || slices.Contains(profileRules.CalibrationKeys, k) {
		return true
	}
	for _, p := range profileRules.CalibrationPrefixes {
		if strings.HasPrefix(k, p) {
			return true
		}
	}
	return false
}

func LibraryRevision(doc map[string]json.RawMessage) uint64 {
	var revision uint64
	json.Unmarshal(doc["revision"], &revision)
	return revision
}

func ValidProfileName(name string) error {
	switch {
	case strings.TrimSpace(name) == "":
		return errors.New("Enter a name.")
	case strings.TrimSpace(name) != name:
		return errors.New("Names can't start or end with a space!")
	case utf8.RuneCountInString(name) > 200:
		return errors.New("Keep the name under 200 characters!")
	}
	for _, r := range name {
		if unicode.IsControl(r) {
			return errors.New("Profile names can't contain control characters.")
		}
	}
	return nil
}

func decodeList[T any](doc map[string]json.RawMessage, key string) ([]*T, error) {
	var raws []json.RawMessage
	if json.Unmarshal(doc[key], &raws) != nil || raws == nil {
		return nil, fmt.Errorf("%s must be an array", key)
	}
	out := make([]*T, 0, len(raws))
	for _, raw := range raws {
		v := new(T)
		dec := json.NewDecoder(bytes.NewReader(raw))
		dec.DisallowUnknownFields()
		if err := dec.Decode(v); err != nil {
			return nil, fmt.Errorf("%s has an unrecognized or invalid entry: %v", key, err)
		}
		out = append(out, v)
	}
	return out, nil
}

func (c *Catalog) validatePrintOverrides(values map[string]json.RawMessage) error {
	if len(values) > 256 {
		return errors.New("printOverrides holds at most 256 settings")
	}
	for key := range values {
		if key != "layer_height" && settingKey.MatchString(key) && !c.printSettings[key] && slices.Contains(c.RecordKinds(key), "printer") {
			return errors.New("printOverrides." + key + " is a printer setting — set it on the printer itself instead of as a print override.")
		}
		if key == "layer_height" || !settingKey.MatchString(key) || !c.printSettings[key] {
			return errors.New("printOverrides." + key + " is not a print setting a printer can set")
		}
	}
	return nil
}

func (c *Catalog) ReadLibrary(doc map[string]json.RawMessage) (*Library, error) {
	return c.parseLibrary(doc, nil, true)
}

func (c *Catalog) ParseLibrary(doc map[string]json.RawMessage, stored *Library) (*Library, error) {
	return c.parseLibrary(doc, stored, false)
}

func (c *Catalog) parseLibrary(doc map[string]json.RawMessage, stored *Library, read bool) (*Library, error) {
	var schema int
	if json.Unmarshal(doc["schema"], &schema) != nil || schema != librarySchema {
		return nil, fmt.Errorf("Profile library schema version must be %d.", librarySchema)
	}
	for key := range doc {
		if !slices.Contains([]string{"schema", "revision", "epoch", "printers", "filaments", "calibrations", "processes"}, key) {
			return nil, fmt.Errorf("The library has an unknown section %q.", key)
		}
	}
	l := &Library{Revision: LibraryRevision(doc), Printers: map[string]*Printer{}, Filaments: map[string]*Filament{}, Alias: map[string]string{}, Processes: map[string]*Process{}}
	seen := map[string]bool{}
	unique := func(id, prefix string) bool {
		ok := strings.HasPrefix(id, prefix) && len(id) > len(prefix) && !seen[id]
		seen[id] = true
		return ok
	}
	problem := func(found *Problem, record any, before storedRecord) (*Problem, error) {
		if found == nil || read || before.problem != nil && before.problem.Code == found.Code && sameRecord(record, before.record) {
			return found, nil
		}
		return nil, found
	}
	processes, err := decodeList[Process](doc, "processes")
	if err != nil {
		return nil, err
	}
	for _, p := range processes {
		if !unique(p.ID, "profile:") || p.Revision == nil {
			return nil, errors.New("Every process needs a unique id, a name without an \"@\" suffix and a revision.")
		}
		if p.Problem, err = problem(c.processProblem(p), p, stored.process(p.ID)); err != nil {
			return nil, err
		}
		l.Processes[p.ID] = p
	}
	filaments, err := decodeList[Filament](doc, "filaments")
	if err != nil {
		return nil, err
	}
	for _, f := range filaments {
		if !unique(f.ID, "profile:") || f.Revision == nil || f.Product == "" {
			return nil, fmt.Errorf("%s: a filament needs a unique id, a name, a revision and a product", f.Name)
		}
		if f.Problem, err = problem(c.filamentProblem(f), f, stored.filament(f.ID)); err != nil {
			return nil, err
		}
		l.Filaments[f.ID] = f
	}
	for _, f := range filaments {
		for _, a := range f.Aliases {
			if l.Filaments[a] != nil || l.Alias[a] != "" || !strings.HasPrefix(a, "profile:") {
				return nil, fmt.Errorf("%s: alias %s is not unique", f.Name, a)
			}
			l.Alias[a] = f.ID
		}
	}
	printers, err := decodeList[Printer](doc, "printers")
	if err != nil {
		return nil, err
	}
	for _, p := range printers {
		bad := func(why string) error { return fmt.Errorf("%s: %s", p.Name, why) }
		if !unique(p.ID, "profile:") || p.Revision == nil {
			return nil, bad("a printer needs a unique id and a revision")
		}
		if p.Vendor == "" || p.Model == "" || len(p.Nozzles) == 0 {
			return nil, bad("a printer is {vendor, model, nozzles}")
		}
		if len(p.Slots) == 0 {
			return nil, bad("a printer has at least one slot")
		}
		for _, s := range p.Slots {
			if !unique(s.ID, "slot:") {
				return nil, bad("every slot needs a unique id")
			}
		}
		if p.Problem, err = problem(c.printerProblem(l, p), p, stored.printer(p.ID)); err != nil {
			return nil, err
		}
		l.Printers[p.ID] = p
	}
	calibrations, err := decodeList[Calibration](doc, "calibrations")
	if err != nil {
		return nil, err
	}
	keyed := map[string]bool{}
	for _, cal := range calibrations {
		f, p := l.Filament(cal.Filament), l.Printers[cal.Printer]
		if !unique(cal.ID, "calibration:") || cal.Revision == nil || f == nil || f.ID != cal.Filament || p == nil || !(cal.Nozzle > 0) || cal.Values == nil {
			return nil, errors.New("A calibration needs a unique id, a revision, a library filament, a printer, a nozzle and values.")
		}
		key := fmt.Sprintf("%s|%s|%d|%g|%s", cal.Filament, cal.Printer, cal.Extruder, cal.Nozzle, cal.Volume)
		if keyed[key] {
			return nil, fmt.Errorf("%s on %s has two calibrations for one %g mm nozzle.", f.Name, p.Name, cal.Nozzle)
		}
		keyed[key] = true
		if cal.Problem, err = problem(calibrationProblem(cal, f, p), cal, stored.calibration(cal.ID)); err != nil {
			return nil, err
		}
		l.Calibrations = append(l.Calibrations, cal)
	}
	return l, nil
}

func sameRecord(a, b any) bool {
	canonical := func(v any) any {
		var out any
		json.Unmarshal(must(json.Marshal(v)), &out)
		return out
	}
	return reflect.DeepEqual(canonical(a), canonical(b))
}

type storedRecord struct {
	record  any
	problem *Problem
}

func (l *Library) process(id string) storedRecord {
	if l == nil || l.Processes[id] == nil {
		return storedRecord{}
	}
	return storedRecord{l.Processes[id], l.Processes[id].Problem}
}
func (l *Library) filament(id string) storedRecord {
	if l == nil || l.Filaments[id] == nil {
		return storedRecord{}
	}
	return storedRecord{l.Filaments[id], l.Filaments[id].Problem}
}
func (l *Library) printer(id string) storedRecord {
	if l == nil || l.Printers[id] == nil {
		return storedRecord{}
	}
	return storedRecord{l.Printers[id], l.Printers[id].Problem}
}
func (l *Library) calibration(id string) storedRecord {
	if l != nil {
		for _, c := range l.Calibrations {
			if c.ID == id {
				return storedRecord{c, c.Problem}
			}
		}
	}
	return storedRecord{}
}

func problem(code, format string, a ...any) *Problem {
	return &Problem{Code: code, Message: fmt.Sprintf(format, a...)}
}

func nameProblem(name string) *Problem {
	if err := ValidProfileName(name); err != nil {
		return problem("name_invalid", "%s Rename this profile.", err.Error())
	}
	return nil
}

func (c *Catalog) keysProblem(record string, values map[string]json.RawMessage, allowed map[string]bool, what string) *Problem {
	for _, key := range sortedKeys(values) {
		if allowed[key] {
			continue
		}
		if what != "printer" && slices.Contains(c.RecordKinds(key), "printer") {
			return problem("printer_setting", "%s: %s is a printer setting; set it on the printer, not in a %s profile", record, key, what)
		}
		return problem("unknown_setting", "%s: %s is not a %s setting", record, key, what)
	}
	return nil
}

func renamedProblem(record string, sets ...map[string]json.RawMessage) *Problem {
	for _, values := range sets {
		for _, key := range sortedKeys(values) {
			if to := CanonicalValue(key, values[key]); !bytes.Equal(to, values[key]) {
				return problem("renamed_value", "%s: %s: %s -> %s; the old value was renamed, reload profiles and save again", record, key, values[key], to)
			}
		}
	}
	return nil
}

func (c *Catalog) processProblem(p *Process) *Problem {
	if pr := nameProblem(p.Name); pr != nil {
		return pr
	}
	if strings.Contains(p.Name, " @") {
		return problem("name_suffix", "%s: a print profile's name can't end in an \"@\" suffix. Rename it.", p.Name)
	}
	for _, r := range p.Scales {
		if slices.Contains(profileRules.NotScalable, r.Key) {
			return problem("scale_not_scalable", "%s: %s can't scale with the nozzle or layer height; set it as a fixed value", p.Name, r.Key)
		}
	}
	auto, _ := json.Marshal(AutoProfile{Schema: 3, Scales: append([]Scale{}, p.Scales...), PreferredHeight: p.PreferredHeight, HeightRule: p.HeightRule, SpeedModel: p.SpeedModel})
	if err := ValidateAuto(map[string]json.RawMessage{"auto": auto}, "process"); err != nil {
		return problem("auto_invalid", "%s: %s", p.Name, strings.TrimPrefix(err.Error(), "auto."))
	}
	if pr := c.keysProblem(p.Name, p.Values, c.PrintSettings(), "print"); pr != nil {
		return pr
	}
	return renamedProblem(p.Name, p.Values)
}

func (c *Catalog) filamentProblem(f *Filament) *Problem {
	if pr := nameProblem(f.Name); pr != nil {
		return pr
	}
	if into := c.FoldedInto(f.Product); into != "" {
		return problem("folded_product", "%s: its product %s is now %s; reload profiles and save again", f.Name, f.Product, into)
	}
	if c.Filaments[f.Product] == nil {
		return problem("unknown_product", "%s: %s is not a filament product of the catalog; pick its product", f.Name, f.Product)
	}
	if colourKey(f.Settings) != "" {
		return problem("filament_colour", "%s: colour belongs to the printer slot, not the filament", f.Name)
	}
	if pr := c.keysProblem(f.Name, f.Settings, c.FilamentSettings(), "filament"); pr != nil {
		return pr
	}
	return renamedProblem(f.Name, f.Settings)
}

func (c *Catalog) printerProblem(l *Library, p *Printer) *Problem {
	if pr := nameProblem(p.Name); pr != nil {
		return pr
	}
	allowed := c.PrinterSettings(p.Model)
	if allowed == nil {
		return problem("unknown_model", "%s: the catalog has no printer %s. It may be from a newer PancakeSlicer: update PancakeSlicer.", p.Name, p.Model)
	}
	for _, h := range HardwareKeys {
		if _, set := p.Machine[h]; set {
			return problem("hardware_setting", "%s: %s is the printer's nozzles, set on them, not a machine value", p.Name, h)
		}
	}
	if pr := c.keysProblem(p.Name, p.Machine, allowed, "printer"); pr != nil {
		return pr
	}
	if pr := renamedProblem(p.Name, p.Machine, p.PrintOverrides); pr != nil {
		return pr
	}
	for _, n := range p.Nozzles {
		if !(n.Diameter >= 0.1 && n.Diameter <= 2) {
			return problem("nozzle_diameter", "%s: a nozzle's diameter is 0.1 to 2 mm. Set its nozzle size again.", p.Name)
		}
		if (n.Volume == "") != (p.Nozzles[0].Volume == "") || (n.Type == "") != (p.Nozzles[0].Type == "") || (n.Offset == nil) != (p.Nozzles[0].Offset == nil) {
			return problem("nozzle_fields", "%s: a volume, type or offset is set on every nozzle or on none", p.Name)
		}
	}
	presetSizes := map[float64]bool{}
	for _, preset := range p.NozzlePresets {
		if !(preset.Diameter >= 0.1 && preset.Diameter <= 2) || presetSizes[preset.Diameter] {
			return problem("nozzle_presets", "%s: saved nozzle sizes need unique diameters from 0.1 to 2 mm. Set its nozzle size again.", p.Name)
		}
		if len(preset.Variants) != len(p.Nozzles) || slices.Contains(preset.Variants, "") {
			return problem("nozzle_presets", "%s: each saved nozzle size needs a base variant for every nozzle. Set its nozzle size again.", p.Name)
		}
		presetSizes[preset.Diameter] = true
	}
	if p.PrintOverrides != nil {
		if err := c.validatePrintOverrides(p.PrintOverrides); err != nil {
			return problem("print_overrides", "%s: %s", p.Name, err.Error())
		}
	}
	if !slices.Contains(swapModes, p.Swap) {
		return problem("swap_mode", "%s: swap is ams, toolheads or manual", p.Name)
	}
	if feed, ok := printerFeeds[p.Model]; len(p.Units) > 0 && !ok {
		return problem("unknown_feed", "%s: this PancakeSlicer doesn't know the %s's AMS, so this printer can't slice. It was likely added in a newer PancakeSlicer: update PancakeSlicer, or remove its AMS units.", p.Name, p.Model)
	} else if len(p.Units) > feed.MaxUnits {
		return problem("too_many_units", "%s: the %s takes at most %d AMS units. Remove the extra units.", p.Name, p.Model, feed.MaxUnits)
	} else {
		for _, u := range p.Units {
			if u.Nozzle < 0 || u.Nozzle >= max(1, len(feed.Nozzles)) {
				return problem("unit_nozzle", "%s: an AMS unit feeds a nozzle the printer does not have. Attach it to one of its nozzles.", p.Name)
			}
		}
	}
	for _, s := range p.Slots {
		if f := l.Filament(s.Filament); s.Filament != "" && (f == nil || !f.Live()) {
			return problem("slot_filament", "%s: a slot holds a filament that is not in your library. Pick that slot's filament again.", p.Name)
		}
	}
	if pr := slotsProblem(p); pr != nil {
		return pr
	}
	if err := c.validatePlateSetup(p); err != nil {
		return problem("plate_setup", "%s: %s", p.Name, err.Error())
	}
	if p.Process != "" && l.Processes[p.Process] == nil {
		return problem("process_missing", "%s: its print profile is not in the library. Pick its print profile again.", p.Name)
	}
	return nil
}

func slotsProblem(p *Printer) *Problem {
	held := map[string]bool{}
	for i, s := range p.Slots {
		if s.Tray == "" {
			continue
		}
		if _, ok := ParseTray(s.Tray); !ok || held[s.Tray] {
			return problem("slot_tray", "%s: slot %d's tray %q is not a tray of its own. Lay its slots out on Device.", p.Name, i+1, s.Tray)
		}
		held[s.Tray] = true
	}
	if pr := mixesProblem(p); pr != nil {
		return pr
	}
	feed, ok := printerFeeds[p.Model]
	if len(p.Nozzles) < 2 || p.Swap == "toolheads" || !ok || feed.External != "perNozzle" {
		return nil
	}
	for i, s := range p.Slots {
		if _, ok := p.SlotExtruder(i); !ok {
			return problem("slot_unrouted", "%s: slot %d is on no nozzle (tray %q). Lay its slots out on Device.", p.Name, i+1, s.Tray)
		}
	}
	return nil
}

func calibrationProblem(cal *Calibration, f *Filament, p *Printer) *Problem {
	if cal.Extruder < 0 || cal.Extruder >= len(p.Nozzles) {
		return problem("calibration_nozzle", "%s on %s: the calibration is for a nozzle the printer does not have.", f.Name, p.Name)
	}
	if colourKey(cal.Values) != "" {
		return problem("calibration_colour", "%s on %s: colour belongs to the printer slot, not a calibration", f.Name, p.Name)
	}
	for k := range cal.Values {
		if !CalibrationKey(k) {
			return problem("calibration_key", "%s on %s: %s is not a filament setting", f.Name, p.Name, k)
		}
	}
	return renamedProblem(f.Name+" on "+p.Name, cal.Values)
}

func sortedKeys(values map[string]json.RawMessage) []string {
	keys := make([]string, 0, len(values))
	for k := range values {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
