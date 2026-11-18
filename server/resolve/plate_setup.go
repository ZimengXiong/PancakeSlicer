package resolve

import (
	"encoding/json"
	"fmt"
	"math"
	"slices"
	"strings"
	"sync"
)

var PlateSetupKeys = []string{"curr_bed_type", "flush_volumes_matrix", "flush_volumes_vector", "flush_multiplier"}

type Flushing struct {
	Multipliers []float64  `json:"multipliers,omitempty"`
	Volumes     []*float64 `json:"volumes,omitempty"`
}

type BuildPlate struct {
	Value string `json:"value"`
	Label string `json:"label"`
}

type Plates struct {
	Plates  []BuildPlate `json:"plates"`
	Default string       `json:"default,omitempty"`
}

var bedTypes = sync.OnceValue(func() []BuildPlate {
	var defs map[string]struct {
		Values []string `json:"enum_values"`
		Labels []string `json:"enum_labels"`
	}
	json.Unmarshal(optionDictionary, &defs)
	def := defs["curr_bed_type"]
	out := make([]BuildPlate, len(def.Values))
	for i, v := range def.Values {
		out[i] = BuildPlate{v, v}
		if i < len(def.Labels) && def.Labels[i] != "" {
			out[i].Label = def.Labels[i]
		}
	}
	return out
})

func (c *Catalog) PlatesOf(variantID string) *Plates {
	ref, ok := c.variants[variantID]
	if !ok {
		return nil
	}
	p := ref.printer
	value := func(key string) string {
		if v, ok := p.Variants[ref.index].Values[key]; ok {
			return v
		}
		return p.Base[key]
	}
	model := func(key string) string {
		var s string
		json.Unmarshal(p.Model[key], &s)
		return s
	}
	def := value("default_bed_type")
	if def == "" {
		def = model("default_bed_type")
	}
	out := &Plates{Plates: []BuildPlate{}}
	if p.Vendor != "BBL" && value("support_multi_bed_types") != "1" && def == "" {
		return out
	}
	unsupported := strings.Split(model("not_support_bed_type"), ";")
	for _, b := range bedTypes() {
		if !slices.Contains(unsupported, b.Value) && !slices.Contains(unsupported, b.Label) {
			out.Plates = append(out.Plates, b)
		}
	}
	if def == "" {
		switch model("model_id") {
		case "BL-P001", "BL-P002", "C13":
			def = "Cool Plate"
		default:
			def = "Textured PEI Plate"
		}
	}
	out.Default = def
	return out
}

func (c *Catalog) validatePlateSetup(p *Printer) error {
	if p.Plate != "" {
		plates := c.PlatesOf(p.Nozzles[0].Variant)
		if plates == nil || !slices.ContainsFunc(plates.Plates, func(b BuildPlate) bool { return b.Value == p.Plate }) {
			return fmt.Errorf("its build plate %s isn't one the printer takes", p.Plate)
		}
	}
	if f := p.Flushing; f != nil {
		if len(f.Multipliers) > 0 && len(f.Multipliers) != len(p.Nozzles) {
			return fmt.Errorf("its flushing has %d multipliers for %d nozzles", len(f.Multipliers), len(p.Nozzles))
		}
		for _, m := range f.Multipliers {
			if !(m >= 0 && m <= 3) {
				return fmt.Errorf("a flushing multiplier is 0 to 3")
			}
		}
		if len(f.Volumes)%len(p.Nozzles) != 0 {
			return fmt.Errorf("its flushing table has a table per nozzle")
		}
		for _, v := range f.Volumes {
			if v != nil && !(*v >= 0 && *v <= MaxFlushVolume) {
				return fmt.Errorf("a flushing volume is 0 to %d mm³", MaxFlushVolume)
			}
		}
	}
	return nil
}

type resolvedFlushing struct {
	Nozzles []flushNozzle `json:"nozzles"`
	Own     bool          `json:"own"`
	Stale   bool          `json:"stale,omitempty"`
}
type flushNozzle struct {
	Matrix     []float64 `json:"matrix"`
	Auto       []float64 `json:"auto"`
	Multiplier float64   `json:"multiplier"`
	MinVolume  float64   `json:"minVolume"`
}

func flushingOf(printer *Printer, config map[string]json.RawMessage, slots []map[string]json.RawMessage) *resolvedFlushing {
	n, nozzles := len(slots), len(printer.Nozzles)
	if n < 2 {
		return nil
	}
	colours := make([]string, n)
	for i := range colours {
		colours[i] = configAt(config, "filament_colour", i)
	}
	out := &resolvedFlushing{Nozzles: make([]flushNozzle, nozzles)}
	own := printer.Flushing
	if own != nil && len(own.Volumes) > 0 {
		out.Own, out.Stale = len(own.Volumes) == nozzles*n*n, len(own.Volumes) != nozzles*n*n
	}
	engineMultiplier := 0.
	json.Unmarshal(profileDefaults["process"]["flush_multiplier"], &engineMultiplier)
	if engineMultiplier == 0 {
		fmt.Sscan(strings.Trim(FirstConfigText(profileDefaults["process"]["flush_multiplier"]), `"`), &engineMultiplier)
	}
	for e := range out.Nozzles {
		variant := installedVariantName(printer.Nozzles[e])
		filaments := make([]map[string]json.RawMessage, n)
		for i, f := range slots {
			filaments[i] = filamentAtVariant(f, variant)
		}
		machine := map[string]json.RawMessage{}
		for _, k := range []string{"nozzle_volume", "nozzle_flush_dataset", "long_retractions_when_cut", "retraction_distances_when_cut"} {
			if v := configAt(config, k, e); v != "" {
				machine[k] = must(json.Marshal(v))
			}
		}
		machine["enable_long_retraction_when_cut"] = config["enable_long_retraction_when_cut"]
		nozzle := flushNozzle{Auto: FlushTable(colours, machine, filaments, 0), MinVolume: MinFlushVolume(machine, filaments, 0), Multiplier: engineMultiplier}
		nozzle.Matrix = nozzle.Auto
		if out.Own {
			nozzle.Matrix = make([]float64, n*n)
			for k, v := range own.Volumes[e*n*n : (e+1)*n*n] {
				if nozzle.Matrix[k] = nozzle.Auto[k]; v != nil {
					nozzle.Matrix[k] = *v
				}
			}
		}
		if own != nil && len(own.Multipliers) == nozzles {
			nozzle.Multiplier = own.Multipliers[e]
		}
		if math.IsInf(nozzle.MinVolume, 0) {
			nozzle.MinVolume = 0
		}
		out.Nozzles[e] = nozzle
	}
	return out
}

func installedVariantName(n Nozzle) string {
	kind, volume := n.Type, n.Volume
	if kind == "" {
		kind = "Direct Drive"
	}
	if volume == "" {
		volume = "Standard"
	}
	return kind + " " + volume
}

func filamentAtVariant(config map[string]json.RawMessage, variant string) map[string]json.RawMessage {
	names := ConfigList(config["filament_extruder_variant"])
	column := max(0, slices.Index(names, variant))
	out := map[string]json.RawMessage{}
	for k, v := range config {
		out[k] = v
		if filamentVariantKeys[k] {
			if list := ConfigList(v); column < len(list) {
				out[k] = must(json.Marshal(list[column]))
			}
		}
	}
	return out
}

func (f *resolvedFlushing) engineFlushValues() map[string]json.RawMessage {
	matrix, multipliers := []string{}, []string{}
	for _, n := range f.Nozzles {
		for _, v := range n.Matrix {
			matrix = append(matrix, fmt.Sprint(v))
		}
		multipliers = append(multipliers, fmt.Sprint(n.Multiplier))
	}
	return map[string]json.RawMessage{"flush_volumes_matrix": must(json.Marshal(strings.Join(matrix, ","))),
		"flush_multiplier": must(json.Marshal(strings.Join(multipliers, ",")))}
}
