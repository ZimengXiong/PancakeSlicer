package resolve

import (
	"encoding/json"
	"errors"
	"math"
	"strconv"
	"strings"
)

type SpeedModel struct {
	Schema           int                `json:"schema"`
	Multipliers      map[string]float64 `json:"multipliers"`
	OuterWallCeiling float64            `json:"outerWallCeiling"`
}

type SpeedSeed struct {
	Multipliers      map[string]float64 `json:"multipliers"`
	OuterWallCeiling float64            `json:"outerWallCeiling"`
}

type SpeedFeature struct {
	Key          string `json:"key"`
	Label        string `json:"label"`
	Width        string `json:"width"`
	Acceleration string `json:"acceleration"`
	FirstLayer   bool   `json:"firstLayer"`
	RelativeTo   string `json:"relativeTo,omitempty"`
}
type resolvedSpeed struct {
	Base         float64  `json:"base"`
	Multiplier   float64  `json:"multiplier"`
	Requested    float64  `json:"requested"`
	Speed        float64  `json:"speed"`
	Limit        string   `json:"limit"`
	Override     string   `json:"override,omitempty"`
	Unset        float64  `json:"unset"`
	FlowCap      *float64 `json:"flowCap,omitempty"`
	PrinterMax   *float64 `json:"printerMax,omitempty"`
	Width        float64  `json:"width,omitempty"`
	Height       float64  `json:"height,omitempty"`
	Acceleration float64  `json:"acceleration"`
}

func NewSpeedModel(seed string) *SpeedModel {
	preset, ok := profileRules.SpeedSeeds[seed]
	if !ok {
		panic("unknown profile speed seed: " + seed)
	}
	multipliers := map[string]float64{}
	for _, f := range profileRules.SpeedFeatures {
		multipliers[f.Key] = 1
		if v, ok := preset.Multipliers[f.Key]; ok {
			multipliers[f.Key] = v
		}
	}
	return &SpeedModel{Schema: 2, Multipliers: multipliers, OuterWallCeiling: preset.OuterWallCeiling}
}
func validateSpeedModel(m *SpeedModel) error {
	if m == nil {
		return nil
	}
	if m.Schema != 2 || len(m.Multipliers) != len(profileRules.SpeedFeatures) || !(m.OuterWallCeiling > 0) || m.OuterWallCeiling > 2000 || math.IsNaN(m.OuterWallCeiling) {
		return errors.New("speedModel needs schema 2, a multiplier for every feature and an outer-wall ceiling above 0 and at most 2000 mm/s")
	}
	for _, f := range profileRules.SpeedFeatures {
		value := m.Multipliers[f.Key]
		if !(value > 0) || value > 10 || math.IsInf(value, 0) {
			return errors.New("speed multipliers need a value above 0 and at most 10 for every feature")
		}
	}
	return nil
}
func speedNumber(config map[string]json.RawMessage, key string, relative float64) float64 {
	t := FirstConfigText(config[key])
	if strings.HasSuffix(t, "%") {
		n, _ := strconv.ParseFloat(strings.TrimSuffix(t, "%"), 64)
		return n * relative / 100
	}
	n, _ := strconv.ParseFloat(t, 64)
	return n
}
func speedMinimum(config map[string]json.RawMessage, keys ...string) float64 {
	min := math.Inf(1)
	for _, key := range keys {
		if n := speedNumber(config, key, 0); n > 0 {
			min = math.Min(min, n)
		}
	}
	return min
}
func finiteSpeed(n float64) *float64 {
	if math.IsInf(n, 0) || math.IsNaN(n) {
		return nil
	}
	return &n
}

func resolveDynamicSpeeds(model *SpeedModel, base, config, overrides map[string]json.RawMessage, overrideSources map[string]string, nozzle, height, flow float64) (map[string]json.RawMessage, map[string]resolvedSpeed) {
	values, details := map[string]json.RawMessage{}, map[string]resolvedSpeed{}
	if model == nil {
		return values, details
	}
	maxSpeed := speedMinimum(config, "machine_max_speed_x", "machine_max_speed_y")
	printAccel := speedMinimum(config, "machine_max_acceleration_x", "machine_max_acceleration_y", "machine_max_acceleration_extruding")
	travelAccel := speedMinimum(config, "machine_max_acceleration_x", "machine_max_acceleration_y", "machine_max_acceleration_travel")
	set := func(k string, n float64) { values[k], _ = json.Marshal(strconv.FormatFloat(n, 'f', -1, 64)) }
	for _, f := range profileRules.SpeedFeatures {
		baseSpeed := speedNumber(base, f.Key, 0)
		if strings.HasSuffix(FirstConfigText(base[f.Key]), "%") {
			parent := "sparse_infill_speed"
			if f.Key == "support_interface_speed" {
				parent = "support_speed"
			}
			if f.RelativeTo != "" {
				parent = f.RelativeTo
			}
			baseSpeed = speedNumber(base, f.Key, speedNumber(base, parent, 0))
		}
		if !(baseSpeed > 0) {
			continue
		}
		mult := model.Multipliers[f.Key]
		requested, override := baseSpeed*mult, ""
		if t := strings.TrimSpace(FirstConfigText(overrides[f.Key])); t != "" && t != "nil" {
			relative := baseSpeed
			if f.RelativeTo != "" {
				relative = speedNumber(values, f.RelativeTo, 0)
				if !(relative > 0) {
					relative = speedNumber(base, f.RelativeTo, 0)
				}
			}
			requested = speedNumber(overrides, f.Key, relative)
			override = overrideSources[f.Key]
		}
		w, h, cap := 0.0, height, math.Inf(1)
		if f.Width != "" {
			if f.FirstLayer {
				h = speedNumber(config, "initial_layer_print_height", height)
				if !(h > 0) {
					h = height
				}
			}
			w = speedNumber(config, f.Width, nozzle)
			if f.Width == "bridge" {
				ratio := speedNumber(config, "bridge_flow", 1)
				if !(ratio > 0) {
					ratio = 1
				}
				w = nozzle * math.Sqrt(ratio)
				h = w
			}
			if !(w > 0) {
				w = speedNumber(config, "line_width", nozzle)
			}
			if !(w > 0) {
				w = nozzle
			}
			if w > 0 && h > 0 {
				cap = flow / (w * h)
			}
		}
		capped := func(requested float64) (float64, string) {
			bound, limit := requested, "multiplier"
			for _, c := range []struct {
				value float64
				name  string
			}{{cap, "flow cap"}, {maxSpeed, "printer max"}} {
				if c.value < bound {
					bound, limit = c.value, c.name
				}
			}
			if f.Key == "outer_wall_speed" && model.OuterWallCeiling < bound {
				bound, limit = model.OuterWallCeiling, "outer-wall ceiling"
			}
			return math.Floor(bound*1000+1e-7) / 1000, limit
		}
		bound, limit := capped(requested)
		unset, _ := capped(baseSpeed * mult)
		accel := speedNumber(config, f.Acceleration, speedNumber(config, "default_acceleration", 0))
		if !(accel > 0) {
			accel = speedNumber(config, "default_acceleration", 0)
		}
		maxAccel := printAccel
		if f.Width == "" {
			maxAccel = travelAccel
		}
		if accel > 0 {
			accel = math.Min(accel, maxAccel)
			set(f.Acceleration, accel)
		}
		set(f.Key, bound)
		details[f.Key] = resolvedSpeed{Base: baseSpeed, Multiplier: mult, Requested: requested, Speed: bound, Limit: limit, Override: override, Unset: unset, FlowCap: finiteSpeed(cap), PrinterMax: finiteSpeed(maxSpeed), Width: w, Height: h, Acceleration: accel}
	}
	return values, details
}
