package resolve

import (
	"bytes"
	"cmp"
	"encoding/json"
	"errors"
	"fmt"
	"maps"
	"math"
	"slices"
	"strconv"
	"strings"
)

type Request struct {
	Printer        string        `json:"printer"`
	Filaments      []string      `json:"filaments"`
	Slots          []string      `json:"slots"`
	Print          string        `json:"print"`
	LayerHeight    float64       `json:"layerHeight"`
	Project        ProjectValues `json:"project"`
	UsedSlots      []int         `json:"usedSlots,omitempty"`
	Saving         bool          `json:"-"`
	PlateNonplanar bool          `json:"-"`
	extruders      []int
	partial        bool
}

type ProjectValues struct {
	Machine   map[string]json.RawMessage   `json:"machine,omitempty"`
	Process   map[string]json.RawMessage   `json:"process,omitempty"`
	Filaments []map[string]json.RawMessage `json:"filaments,omitempty"`
	Slots     map[string]json.RawMessage   `json:"slots,omitempty"`
}

type ProfileValues struct {
	Values map[string]json.RawMessage            `json:"values"`
	Layers map[string]map[string]json.RawMessage `json:"layers"`
}

func newValues() *ProfileValues {
	return &ProfileValues{Values: map[string]json.RawMessage{}, Layers: map[string]map[string]json.RawMessage{}}
}
func (r *ProfileValues) put(k string, v json.RawMessage, layer string) {
	r.Values[k] = v
	if r.Layers[k] == nil {
		r.Layers[k] = map[string]json.RawMessage{}
	}
	r.Layers[k][layer] = v
}

type resolveError struct {
	profile string
	slot    int
	code    string
	message string
}

func (e *resolveError) Error() string { return e.message }

func ErrorCode(err error) string {
	var r *resolveError
	if errors.As(err, &r) {
		return r.code
	}
	return ""
}

type SliceConfig struct {
	Config  map[string]json.RawMessage `json:"config"`
	Sources map[string]string          `json:"sources"`
	Nozzles []resolvedNozzle           `json:"nozzles,omitempty"`
	Names   struct {
		Variant   string   `json:"variant"`
		Print     string   `json:"print"`
		Filaments []string `json:"filaments"`
	} `json:"names"`
	engineOverrides map[string]bool
	printer, print  *ProfileValues
	slots           []*ProfileValues
	target          *ProfileValues
	slotErrors      map[int]*resolveError
	heights         *resolvedHeights
	flushing        *resolvedFlushing
	Engine          struct {
		Printer   string   `json:"printer"`
		Process   string   `json:"process"`
		Filaments []string `json:"filaments"`
	} `json:"engine"`
}

func ConfigList(raw json.RawMessage) []string {
	var list []string
	if json.Unmarshal(raw, &list) == nil {
		if len(list) == 1 && strings.Contains(list[0], ",") {
			return strings.Split(list[0], ",")
		}
		return list
	}
	var text string
	if json.Unmarshal(raw, &text) == nil && text != "" {
		return strings.Split(text, ",")
	}
	return nil
}

func machineSwap(vendor string, m *Preset) (string, []string) {
	if len(ConfigList(m.Config["nozzle_diameter"])) > 1 && vendor != "BBL" {
		return "toolheads", []string{"toolheads"}
	}
	change := strings.TrimSpace(FirstConfigText(m.Config["change_filament_gcode"]))
	pause := strings.TrimSpace(FirstConfigText(m.Config["machine_pause_gcode"]))
	onlyPause := change == "" || change == pause || slices.Contains([]string{"M600", "M0", "M1", "M25", "PAUSE", "M601"}, strings.ToUpper(change))
	if vendor == "BBL" || FirstConfigText(m.Config["single_extruder_multi_material"]) == "1" && !onlyPause {
		return "ams", []string{"ams", "manual"}
	}
	return "manual", []string{"manual"}
}

func (c *Catalog) Swap(variant string) (string, []string) {
	m := c.Machine(variant)
	if m == nil {
		return "manual", []string{"manual"}
	}
	return machineSwap(m.Vendor, m)
}

func slotsOfJob(lib *Library, printer *Printer, in Request) error {
	if len(in.Slots) != len(in.Filaments) {
		return &resolveError{"", 0, "slot_identity", fmt.Sprintf("This job names %d slots for %d filaments. Slice again.", len(in.Slots), len(in.Filaments))}
	}
	for i, id := range in.Slots {
		holds := ""
		if at := printer.SlotIndex(id); at >= 0 {
			holds = printer.Slots[at].Filament
		} else if m := printer.MixByID(id); m != nil && len(m.Components) > 0 {
			if at := printer.SlotIndex(m.Components[0]); at >= 0 {
				holds = printer.Slots[at].Filament
			}
		} else {
			return &resolveError{"filament", i, "slot_identity", fmt.Sprintf("Slot %d is no longer on %s. Slice again.", i+1, printer.Name)}
		}
		if in.Filaments[i] != "" && (lib.Filament(holds) == nil || lib.Filament(holds) != lib.Filament(in.Filaments[i])) {
			return &resolveError{"filament", i, "slot_identity", fmt.Sprintf("Slot %d on %s holds another filament now. Slice again.", i+1, printer.Name)}
		}
	}
	return nil
}

func slotAt(printer *Printer, slots []string, i int) int {
	if i >= len(slots) {
		return -1
	}
	return printer.SlotIndex(slots[i])
}

func (c *Catalog) Job(lib *Library, in Request) (*SliceConfig, error) {
	if len(in.Project.Filaments) > len(in.Filaments) {
		return nil, &resolveError{"", 0, "unresolved", fmt.Sprintf("This project has filament values for %d slots, while the job has %d filaments.", len(in.Project.Filaments), len(in.Filaments))}
	}
	printer, print := lib.Printers[in.Printer], lib.Processes[in.Print]
	if printer == nil || !printer.Live() {
		return nil, &resolveError{"printer", 0, "unknown_record", "Please choose a printer."}
	}
	if printer.Problem != nil {
		return nil, &resolveError{"printer", 0, "invalid_record", printer.Problem.Message}
	}
	if in.extruders == nil {
		if err := slotsOfJob(lib, printer, in); err != nil {
			return nil, err
		}
	}
	machine, sources, err := c.printerHardware(printer)
	if err != nil {
		return nil, &resolveError{"printer", 0, "unresolved", err.Error()}
	}
	var process *Preset
	var printErr *resolveError
	switch {
	case print == nil || !print.Live():
		printErr = &resolveError{"print", 0, "unknown_record", "Please choose a print profile."}
	case print.Problem != nil:
		printErr = &resolveError{"print", 0, "invalid_record", print.Problem.Message}
	case print.Product != "":
		if process = c.Resolve("prints", print.Product, machine.Name); process == nil {
			printErr = &resolveError{"print", 0, "unresolved", print.Name + " builds on " + print.Product + ", which isn't in the catalog. Pick its print settings again."}
		}
	default:
		if process = c.standardPrint(machine); process == nil {
			printErr = &resolveError{"print", 0, "no_default_print", fmt.Sprintf("%s for %s.", errNoDefaultPrint, machine.Name)}
		}
	}
	if printErr != nil {
		if !in.partial {
			return nil, printErr
		}
		process, print = &Preset{Config: map[string]json.RawMessage{}, Layers: map[string]string{}}, &Process{}
	}
	out := newSliceConfig()
	out.target = out.printer
	out.Engine.Printer, out.Engine.Process = machine.Name, process.Product
	out.Names.Variant, out.Names.Print, out.Names.Filaments = machine.Name, process.Name, []string{}
	layer := func(values map[string]json.RawMessage, source func(key string) string) {
		for k, v := range values {
			from := source(k)
			if to := CanonicalKey(k); to != k {
				if _, both := values[to]; both {
					continue
				}
				k = to
			}
			out.put(k, v, from)
		}
	}
	set := func(values map[string]json.RawMessage, source string) {
		layer(values, func(string) string { return source })
	}
	catalog := func(p *Preset) { layer(p.Config, func(k string) string { return p.Layers[k] }) }
	text := func(values map[string]string) map[string]json.RawMessage {
		m := map[string]json.RawMessage{}
		for k, v := range values {
			m[k], _ = json.Marshal(v)
		}
		return m
	}
	catalog(machine)
	if v, ok := machine.Config["default_nozzle_volume_type"]; ok {
		if _, set := machine.Config["nozzle_volume_type"]; !set {
			out.put("nozzle_volume_type", v, machine.Layers["default_nozzle_volume_type"])
			out.engineOverrides["nozzle_volume_type"] = true
		}
	}
	for k, v := range nozzleValues(machine, sources, printer.Nozzles) {
		out.put(k, v, "record")
		out.engineOverrides[k] = true
	}
	set(printer.Machine, "record")
	set(withoutPlateSetup(in.Project.Machine), "project")
	if printer.Plate != "" {
		out.put("curr_bed_type", must(json.Marshal(printer.Plate)), "record")
	}
	out.target = out.print
	catalog(process)
	for _, k := range []string{"nonplanar_mode", "nonplanar_wave"} {
		out.engineOverrides[k] = true
	}
	set(print.Values, "record")
	var slotNozzle, used []int
	if in.extruders == nil {
		slotNozzle, used, err = slotNozzles(printer, in.Slots)
	} else {
		slotNozzle, used, err = in.extruders, []int{}, nil
		for _, e := range in.extruders {
			if e < 0 || e >= len(printer.Nozzles) {
				return nil, fmt.Errorf("Hmm, %s doesn't have a %d nozzle.", printer.Name, e+1)
			}
			if !slices.Contains(used, e) {
				used = append(used, e)
			}
		}
		slices.Sort(used)
	}
	if err != nil {
		return nil, err
	}
	lo, hi, limit, err := heightRange(printer, sources, used)
	if err != nil {
		return nil, err
	}
	smallest := slices.MinFunc(used, func(a, b int) int { return cmp.Compare(printer.Nozzles[a].Diameter, printer.Nozzles[b].Diameter) })
	height := in.LayerHeight
	if height > hi+1e-9 {
		return nil, fmt.Errorf("%g mm layers are taller than the %g mm max for %s (%g mm). Pick a smaller layer height!", height, hi, nozzleName(printer, limit), printer.Nozzles[limit].Diameter)
	}
	if !(height > 0) && print.PreferredHeight != nil {
		height = *print.PreferredHeight
	}
	if !(height > 0) && print.HeightRule != "" {
		height = ruleHeight(print.HeightRule, sources[smallest].LayerHeights, printer.Nozzles[smallest].Diameter)
	}
	if !(height > 0) {
		height, _ = strconv.ParseFloat(FirstConfigText(process.Config["layer_height"]), 64)
	}
	height = math.Min(height, hi)
	out.heights = &resolvedHeights{Chosen: height, Available: []float64{}, Limits: []heightLimit{}}
	for _, h := range sources[smallest].LayerHeights {
		if h >= lo-1e-9 && h <= hi+1e-9 {
			out.heights.Available = append(out.heights.Available, h)
		}
	}
	for _, e := range used {
		out.heights.Limits = append(out.heights.Limits, heightLimit{e, printer.Nozzles[e].Diameter, nozzleMaxLayer(printer, sources, e)})
	}
	scaled, err := scaleAcross(print.Scales, printer, used, height)
	if err != nil {
		return nil, err
	}
	set(text(scaled), "auto")
	heightLayer := "auto"
	if stored, err := strconv.ParseFloat(FirstConfigText(print.Values["layer_height"]), 64); err == nil && math.Abs(stored-height) < 1e-9 {
		heightLayer = "record"
	}
	set(text(map[string]string{"layer_height": strconv.FormatFloat(height, 'f', -1, 64)}), heightLayer)
	set(printer.PrintOverrides, "printer")
	set(withoutPlateSetup(in.Project.Process), "project")
	filamentConfigs := make([]map[string]json.RawMessage, 0, len(in.Filaments))
	filamentSource := map[string]string{}
	var keptNozzle []int
	var keptSlot []int
	failSlot := func(e *resolveError) error {
		if !in.partial {
			return e
		}
		out.slotErrors[e.slot] = e
		out.slots = append(out.slots, nil)
		return nil
	}
	printed, standIn := printUsedSlots(in.UsedSlots, out.Config, len(in.Filaments)), ""
	for _, id := range in.Filaments {
		if f := lib.Filament(id); standIn == "" && f != nil && f.Live() {
			standIn = id
		}
	}
	if standIn == "" && in.Saving {
		for id, f := range lib.Filaments {
			if f.Live() && (standIn == "" || id < standIn) {
				standIn = id
			}
		}
	}
	for i, id := range in.Filaments {
		if id == "" && !in.partial && (in.Saving || !printed[i]) {
			id = standIn
		}
		f := lib.Filament(id)
		if f == nil || !f.Live() {
			code := "unknown_record"
			if id == "" && !in.partial {
				code = "slot_unpicked"
			}
			if err := failSlot(&resolveError{"filament", i, code, "Please pick a filament for slot " + strconv.Itoa(i+1) + "."}); err != nil {
				return nil, err
			}
			continue
		}
		extruder := slotNozzle[i]
		cal := lib.Calibration(f.ID, printer, extruder)
		problem := f.Problem
		if cal != nil && problem == nil {
			problem = cal.Problem
		}
		if problem != nil {
			if err := failSlot(&resolveError{"filament", i, "invalid_record", problem.Message}); err != nil {
				return nil, err
			}
			continue
		}
		preset := c.Resolve("filaments", f.Product, sources[extruder].Name)
		if preset == nil {
			if err := failSlot(&resolveError{"filament", i, "unresolved", f.Name + " names " + f.Product + ", which isn't in the catalog. Pick its product again."}); err != nil {
				return nil, err
			}
			continue
		}
		keptNozzle, keptSlot = append(keptNozzle, extruder), append(keptSlot, i)
		out.Engine.Filaments = append(out.Engine.Filaments, preset.Product)
		out.Names.Filaments = append(out.Names.Filaments, preset.Name)
		config := map[string]json.RawMessage{}
		slot := newValues()
		out.slots = append(out.slots, slot)
		claim := func(k string, v json.RawMessage, source string) {
			config[k] = v
			slot.put(k, v, source)
			if !catalogLayer(source) || filamentSource[k] == "" {
				filamentSource[k] = source
			}
		}
		for k, v := range preset.Config {
			claim(k, v, preset.Layers[k])
		}
		for k, v := range f.Settings {
			claim(k, v, "record")
		}
		claim("filament_settings_id", must(json.Marshal([]string{f.Name})), "record")
		if at := slotAt(printer, in.Slots, i); at >= 0 && printer.Slots[at].Colour != "" {
			claim("filament_colour", must(json.Marshal([]string{printer.Slots[at].Colour})), "record")
		}
		if cal != nil {
			for k, v := range cal.Values {
				if k == TemperatureOffset {
					var offset float64
					if json.Unmarshal(v, &offset) != nil {
						fmt.Sscan(FirstConfigText(v), &offset)
					}
					for _, t := range []string{"nozzle_temperature", "nozzle_temperature_initial_layer"} {
						base, _ := strconv.ParseFloat(FirstConfigText(config[t]), 64)
						if base > 0 {
							claim(t, must(json.Marshal([]string{strconv.FormatFloat(base+offset, 'f', -1, 64)})), "calibration")
						}
					}
					continue
				}
				claim(k, v, "calibration")
			}
		}
		for k, v := range in.Project.Slots {
			claim(k, v, "project")
		}
		if i < len(in.Project.Filaments) {
			for k, v := range in.Project.Filaments[i] {
				claim(k, v, "project")
			}
		}
		filamentConfigs = append(filamentConfigs, config)
	}
	filamentConfig := map[string]json.RawMessage{}
	if len(filamentConfigs) > 0 {
		if filamentConfig, err = assembleFilamentConfig(filamentConfigs, optionDictionary); err != nil {
			return nil, err
		}
	}
	for k, v := range filamentConfig {
		source := filamentSource[k]
		if source == "" {
			source = "base"
		}
		out.Config[k], out.Sources[k] = v, source
		if len(v) > 0 && v[0] == '[' {
			out.engineOverrides[k] = true
		}
	}
	if out.flushing = flushingOf(printer, out.Config, filamentConfigs); out.flushing != nil {
		out.target = nil
		for k, v := range out.flushing.engineFlushValues() {
			out.put(k, v, "record")
		}
	}
	out.target = out.printer
	swapConfig(out, printer, machine, len(keptNozzle))
	if err := mapSlotsToNozzles(out, printer, keptNozzle); err != nil {
		return nil, &resolveError{"printer", 0, "unresolved", err.Error()}
	}
	out.target = out.print
	if len(print.Scales) > 0 || print.SpeedModel != nil {
		model := print.SpeedModel
		if model == nil {
			return nil, errors.New("This dynamic profile is missing its speed multipliers—try reloading your profiles.")
		}
		standard := c.standardPrint(machine)
		if standard == nil {
			return nil, fmt.Errorf("Dynamic speeds take their reference from the printer's Standard print settings: %w for %s.", errNoDefaultPrint, machine.Name)
		}
		base := maps.Clone(standard.Config)
		for _, f := range profileRules.SpeedFeatures {
			if v, ok := printer.PrintOverrides[f.Key]; ok {
				base[f.Key] = v
			}
			if _, own := print.Values[f.Acceleration]; !own {
				if _, project := in.Project.Process[f.Acceleration]; !project {
					if v, printerOwn := printer.PrintOverrides[f.Acceleration]; printerOwn {
						out.Config[f.Acceleration] = v
					} else if v, ok := base[f.Acceleration]; ok {
						out.Config[f.Acceleration] = v
					}
				}
			}
		}
		perNozzle, flows := map[int]map[string]json.RawMessage{}, slotFlows(out.Config, filamentConfigs, keptNozzle)
		usedSlots := printUsedSlots(in.UsedSlots, out.Config, len(in.Filaments))
		for _, e := range used {
			d := printer.Nozzles[e].Diameter
			overrides, overrideSources := map[string]json.RawMessage{}, map[string]string{}
			for _, f := range profileRules.SpeedFeatures {
				for _, row := range print.Scales {
					if row.Key == f.Key {
						overrides[f.Key], overrideSources[f.Key] = must(json.Marshal(scaleValues([]Scale{row}, d, height)[f.Key])), "legacy formula override"
					}
				}
				if v, ok := print.Values[f.Key]; ok {
					overrides[f.Key], overrideSources[f.Key] = v, "library"
				}
				if v, ok := in.Project.Process[f.Key]; ok {
					overrides[f.Key], overrideSources[f.Key] = v, "project"
				}
			}
			nozzle := resolvedNozzle{Extruder: e, Name: nozzleName(printer, e), Diameter: d, Variant: installedVariant(out.Config, e), Slots: []int{}}
			flow := 0.
			for j, on := range keptNozzle {
				if on == e {
					nozzle.Slots = append(nozzle.Slots, keptSlot[j])
					if flows[j] > 0 && usedSlots[keptSlot[j]] {
						flow = math.Max(flow, flows[j])
					}
				}
			}
			if flow == 0 {
				flow = math.Inf(1)
			}
			nozzle.Flow = finiteSpeed(flow)
			perNozzle[e], nozzle.Speeds = resolveDynamicSpeeds(model, base, out.Config, overrides, overrideSources, d, speedNumber(out.Config, "layer_height", height), flow)
			out.Nozzles = append(out.Nozzles, nozzle)
		}
		values := mergeNozzleSpeeds(perNozzle, used, process)
		layer(values, func(k string) string {
			if _, ok := print.Values[k]; ok {
				return "record"
			}
			return "auto"
		})
		for k := range values {
			out.engineOverrides[k] = true
		}
	}
	if FirstConfigText(out.Config["nonplanar_mode"]) == FastCurvedTops {
		out.Config["nonplanar_mode"] = json.RawMessage(`"` + CurvedTops + `"`)
		out.Config["nonplanar_curved_top_layers"] = json.RawMessage(`1`)
		out.Sources["nonplanar_curved_top_layers"] = out.Sources["nonplanar_mode"]
		out.engineOverrides["nonplanar_curved_top_layers"] = true
	}
	if (nonplanarMode(FirstConfigText(out.Config["nonplanar_mode"])) || in.PlateNonplanar) && strings.TrimSpace(FirstConfigText(out.Config["nonplanar_head"])) == "" {
		out.target = out.printer
		out.put("nonplanar_head", must(json.Marshal(GenericHead)), "default")
		out.engineOverrides["nonplanar_head"] = true
		out.target = out.print
	}
	out.Config["printer_settings_id"], out.Config["print_settings_id"] = must(json.Marshal(machine.Name)), must(json.Marshal(process.Name))
	if printErr != nil {
		return out, printErr
	}
	return out, nil
}

func withoutPlateSetup(values map[string]json.RawMessage) map[string]json.RawMessage {
	out := maps.Clone(values)
	for _, k := range PlateSetupKeys {
		delete(out, k)
	}
	return out
}

func slotExtruder(printer *Printer, i int) (int, bool) { return printer.SlotExtruder(i) }

func (printer *Printer) SlotExtruder(i int) (int, bool) {
	switch {
	case len(printer.Nozzles) == 1:
		return 0, true
	case printer.Swap == "toolheads":
		return i, i < len(printer.Nozzles)
	case i >= len(printer.Slots):
		return 0, false
	}
	tray := printer.Slots[i].Tray
	switch {
	case tray == "ext" || tray == "ext.L":
		return 0, true
	case tray == "ext.R":
		return 1, true
	case strings.HasPrefix(tray, "ams"):
		unit, err := strconv.Atoi(strings.SplitN(strings.TrimPrefix(tray, "ams"), ".", 2)[0])
		if err != nil || unit < 0 || unit >= len(printer.Units) {
			return 0, false
		}
		return printer.Units[unit].Nozzle, true
	}
	return 0, false
}

func mapSlotsToNozzles(out *SliceConfig, printer *Printer, slotNozzle []int) error {
	extruders := len(ConfigList(out.Config["nozzle_diameter"]))
	if extruders < 2 || printer.Swap != "ams" {
		return nil
	}
	mapping := make([]string, len(slotNozzle))
	for i, e := range slotNozzle {
		if e >= extruders {
			return fmt.Errorf("Slot %d isn't on a nozzle of %s. Hop over to Device and lay out its slots again!", i+1, printer.Name)
		}
		mapping[i] = strconv.Itoa(e + 1)
	}
	for k, v := range map[string]any{"filament_map_mode": "Manual", "filament_map": mapping} {
		out.put(k, must(json.Marshal(v)), "record")
		out.engineOverrides[k] = true
	}
	return nil
}

func swapConfig(out *SliceConfig, printer *Printer, machine *Preset, filaments int) {
	put := func(k string, v any) {
		out.put(k, must(json.Marshal(v)), "record")
		out.engineOverrides[k] = true
	}
	switch printer.Swap {
	case "ams":
		put("manual_filament_change", "0")
	case "manual":
		put("manual_filament_change", "1")
		if filaments > 1 {
			put("single_extruder_multi_material", "1")
			pause := strings.TrimSpace(FirstConfigText(machine.Config["machine_pause_gcode"]))
			if pause == "" {
				pause = "M600"
			}
			put("change_filament_gcode", pause)
		}

	}
}

func newSliceConfig() *SliceConfig {
	return &SliceConfig{Config: map[string]json.RawMessage{}, Sources: map[string]string{}, engineOverrides: map[string]bool{},
		printer: newValues(), print: newValues(), slotErrors: map[int]*resolveError{}}
}

func (out *SliceConfig) put(k string, v json.RawMessage, layer string) {
	out.Config[k], out.Sources[k] = v, layer
	if out.target != nil {
		out.target.put(k, v, layer)
	}
}

func (out *SliceConfig) EngineValues() map[string]json.RawMessage {
	values := map[string]json.RawMessage{}
	for k, v := range out.Config {
		presence := strings.HasPrefix(k, "wipe_tower_") || k == "filament_colour"
		if !HiddenSetting(k) && (!presence || !catalogLayer(out.Sources[k]) || out.engineOverrides[k]) {
			values[k] = v
		}
	}
	return values
}

var errNoDefaultPrint = errors.New("Oops, the catalog doesn't have any print settings.")

func catalogLayer(source string) bool { return source == "base" || source == "tuning" }

func vectorOptions(dictionary []byte) map[string]bool {
	var defs map[string]struct {
		Type string `json:"type"`
	}
	json.Unmarshal(dictionary, &defs)
	out := map[string]bool{}
	for key, def := range defs {
		switch strings.TrimPrefix(def.Type, "co") {
		case "Floats", "Ints", "Bools", "Percents", "FloatsOrPercents", "Points":
			out[key] = true
		}
	}
	return out
}

func (c *Catalog) Meshing(lib *Library, in Request) (*SliceConfig, error) {
	printer := lib.Printers[in.Printer]
	if printer == nil || !printer.Live() {
		return nil, errors.New("Please choose a printer.")
	}
	if printer.Problem != nil {
		return nil, printer.Problem
	}
	machine := c.Machine(printer.Nozzles[0].Variant)
	if machine == nil {
		return nil, errors.New("Hmm, the catalog doesn't have the printer variant " + printer.Nozzles[0].Variant + ".")
	}
	process, filament := c.standardPrint(machine), filamentFamilyOf(FirstConfigText(machine.Config["default_filament_profile"]))
	if process == nil || c.Filaments[filament] == nil {
		return nil, errors.New("Oops, the catalog can't mesh models for " + printer.Model + ".")
	}
	out := newSliceConfig()
	out.Engine.Printer, out.Engine.Process, out.Engine.Filaments = machine.Name, process.Product, []string{filament}
	return out, nil
}

func filamentFamilyOf(name string) string {
	family, _, _ := strings.Cut(name, " @")
	return strings.TrimSpace(family)
}

func (c *Catalog) printerHardware(printer *Printer) (*Preset, []*Preset, error) {
	sources := make([]*Preset, len(printer.Nozzles))
	for i, n := range printer.Nozzles {
		if sources[i] = c.Machine(n.Variant); sources[i] == nil || c.variants[n.Variant].printer.ID != printer.Model {
			return nil, nil, fmt.Errorf("Hmm, the catalog doesn't have the %s variant %q. Open the printer and pick its nozzle again!", printer.Model, n.Variant)
		}
	}
	if e := max(len(ConfigList(sources[0].Config["nozzle_diameter"])), 1); e != len(sources) {
		return nil, nil, fmt.Errorf("%s has %d nozzles, while its record lists %d. Open the printer and set its nozzles again!", printer.Name, e, len(sources))
	}
	return sources[0], sources, nil
}

func nozzleValues(machine *Preset, sources []*Preset, nozzles []Nozzle) map[string]json.RawMessage {
	out := map[string]json.RawMessage{}
	ids := ConfigList(machine.Config["printer_extruder_id"])
	for k, raw := range machine.Config {
		list := ConfigList(raw)
		if len(list) < 2 || (len(list) != len(nozzles) && len(list) != len(ids)) {
			continue
		}
		next, changed := slices.Clone(list), false
		for col := range list {
			e := col
			if len(list) != len(nozzles) {
				e, _ = strconv.Atoi(ids[col])
				e--
			}
			if e < 0 || e >= len(sources) || sources[e] == machine {
				continue
			}
			if own := ConfigList(sources[e].Config[k]); len(own) == len(list) && own[col] != list[col] {
				next[col], changed = own[col], true
			}
		}
		if changed {
			out[k] = must(json.Marshal(next))
		}
	}
	each := func(key string, value func(n Nozzle) string) {
		list := make([]string, len(nozzles))
		for i, n := range nozzles {
			list[i] = value(n)
		}
		out[key] = must(json.Marshal(list))
	}
	each("nozzle_diameter", func(n Nozzle) string { return strconv.FormatFloat(n.Diameter, 'f', -1, 64) })
	if list := ConfigList(machine.Config["max_layer_height"]); len(list) == len(nozzles) {
		next, changed := slices.Clone(list), false
		for i, n := range nozzles {
			if !SameMm(n.Diameter, sources[i].Nozzle) {
				next[i], changed = strconv.FormatFloat(roundRule(n.Diameter*profileRules.MaxLayerOfNozzle), 'f', -1, 64), true
			}
		}
		if changed {
			out["max_layer_height"] = must(json.Marshal(next))
		}
	}
	if nozzles[0].Volume != "" {
		each("nozzle_volume_type", func(n Nozzle) string { return n.Volume })
	}
	if nozzles[0].Type != "" {
		each("extruder_type", func(n Nozzle) string { return n.Type })
	}
	if nozzles[0].Offset != nil {
		each("extruder_offset", func(n Nozzle) string {
			return strconv.FormatFloat(n.Offset[0], 'f', -1, 64) + "x" + strconv.FormatFloat(n.Offset[1], 'f', -1, 64)
		})
	}
	return out
}

func nonplanarMode(mode string) bool { return mode != "" && mode != "off" }

func PlateNonplanar(plate []byte) bool {
	type settings map[string]json.RawMessage
	var doc struct {
		Objects []struct {
			Settings settings `json:"settings"`
			Volumes  []struct {
				Settings settings `json:"settings"`
			} `json:"volumes"`
			Ranges []struct {
				Settings settings `json:"settings"`
			} `json:"ranges"`
		} `json:"objects"`
	}
	if json.Unmarshal(plate, &doc) != nil {
		return false
	}
	for _, o := range doc.Objects {
		all := []settings{o.Settings}
		for _, v := range o.Volumes {
			all = append(all, v.Settings)
		}
		for _, r := range o.Ranges {
			all = append(all, r.Settings)
		}
		for _, s := range all {
			if nonplanarMode(FirstConfigText(s["nonplanar_mode"])) {
				return true
			}
		}
	}
	return false
}

const GenericHead = "generic-conservative"

var printFilamentKeys = []string{"wall_filament", "sparse_infill_filament", "solid_infill_filament", "support_filament", "support_interface_filament"}

func printUsedSlots(plate []int, config map[string]json.RawMessage, slots int) map[int]bool {
	used := map[int]bool{}
	add := func(slot int) {
		if slot >= 1 && slot <= slots {
			used[slot-1] = true
		}
	}
	for _, slot := range plate {
		add(slot)
	}
	if len(plate) == 0 {
		add(1)
	}
	for _, key := range printFilamentKeys {
		n, _ := strconv.Atoi(FirstConfigText(config[key]))
		add(n)
	}
	return used
}

func PlateUsedSlots(plate []byte, slots int) ([]int, error) {
	type settings map[string]json.RawMessage
	var doc struct {
		Objects []struct {
			Extruder int             `json:"extruder"`
			Settings settings        `json:"settings"`
			Paint    json.RawMessage `json:"paint"`
			Volumes  []struct {
				Extruder int             `json:"extruder"`
				Settings settings        `json:"settings"`
				Paint    json.RawMessage `json:"paint"`
			} `json:"volumes"`
			Ranges []struct {
				Extruder int      `json:"extruder"`
				Settings settings `json:"settings"`
			} `json:"ranges"`
		} `json:"objects"`
	}
	if err := json.Unmarshal(plate, &doc); err != nil {
		return nil, err
	}
	used := map[int]bool{}
	all := false
	fromSettings := func(s settings) {
		for _, key := range append([]string{"extruder"}, printFilamentKeys...) {
			if n, _ := strconv.Atoi(FirstConfigText(s[key])); n > 0 {
				used[n] = true
			}
		}
	}
	painted := func(raw json.RawMessage) bool { return bytes.Contains(raw, []byte(`"color"`)) }
	for _, o := range doc.Objects {
		object := max(o.Extruder, 1)
		used[object] = true
		fromSettings(o.Settings)
		all = all || painted(o.Paint)
		for _, v := range o.Volumes {
			used[cmp.Or(v.Extruder, object)] = true
			fromSettings(v.Settings)
			all = all || painted(v.Paint)
		}
		for _, r := range o.Ranges {
			used[cmp.Or(r.Extruder, object)] = true
			fromSettings(r.Settings)
		}
	}
	out := []int{}
	for slot := 1; slot <= slots; slot++ {
		if all || used[slot] {
			out = append(out, slot)
		}
	}
	return out, nil
}
