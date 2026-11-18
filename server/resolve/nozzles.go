package resolve

import (
	"encoding/json"
	"fmt"
	"math"
	"slices"
	"strconv"
	"strings"
)

func nozzleName(printer *Printer, e int) string {
	if len(printer.Nozzles) == 1 {
		return "nozzle"
	}
	if printer.Vendor == "BBL" && len(printer.Nozzles) == 2 {
		return []string{"Left", "Right"}[e] + " nozzle"
	}
	return "extruder " + strconv.Itoa(e+1)
}

func slotNozzles(printer *Printer, slots []string) ([]int, []int, error) {
	each := make([]int, len(slots))
	used := []int{}
	for i, id := range slots {
		e, ok := -2, false
		if at := printer.SlotIndex(id); at >= 0 {
			e, ok = slotExtruder(printer, at)
		} else if m := printer.MixByID(id); m != nil {
			for _, c := range m.Components {
				n, routed := slotExtruder(printer, printer.SlotIndex(c))
				if !routed {
					e, ok = -2, false
					break
				}
				if e == -2 || e == n {
					e, ok = n, true
				} else {
					e = -1
				}
			}
		}
		if !ok || e >= len(printer.Nozzles) {
			return nil, nil, fmt.Errorf("Slot %d isn't on a nozzle of %s. Hop over to Device and lay out its slots again!", i+1, printer.Name)
		}
		if e < 0 {
			return nil, nil, fmt.Errorf("Mixed filament %d combines filaments on different nozzles of %s. Mix filaments on the same nozzle!", i+1, printer.Name)
		}
		each[i] = e
		if !slices.Contains(used, e) {
			used = append(used, e)
		}
	}
	if len(used) == 0 {
		used = append(used, 0)
	}
	slices.Sort(used)
	return each, used, nil
}

func nozzleColumn(config map[string]json.RawMessage, key string, e int) float64 {
	list := ConfigList(config[key])
	if e >= len(list) {
		return 0
	}
	n, _ := strconv.ParseFloat(strings.TrimSpace(list[e]), 64)
	return n
}

func nozzleMaxLayer(printer *Printer, sources []*Preset, e int) float64 {
	top := roundRule(printer.Nozzles[e].Diameter * profileRules.MaxLayerOfNozzle)
	if m := nozzleColumn(sources[e].Config, "max_layer_height", e); m > 0 && m < top && SameMm(sources[e].Nozzle, printer.Nozzles[e].Diameter) {
		top = m
	}
	return top
}

func smallestNozzleFor(lowest float64) float64 {
	d := math.Floor(lowest/profileRules.MaxLayerOfNozzle*1000)/1000 - 0.005
	for roundRule(d*profileRules.MaxLayerOfNozzle) < lowest-1e-9 {
		d = math.Round((d+0.001)*1000) / 1000
	}
	return d
}

type resolvedHeights struct {
	Chosen    float64       `json:"chosen"`
	Available []float64     `json:"available"`
	Limits    []heightLimit `json:"limits"`
}
type heightLimit struct {
	Extruder int     `json:"extruder"`
	Diameter float64 `json:"diameter"`
	Max      float64 `json:"max"`
}

func heightRange(printer *Printer, sources []*Preset, used []int) (lo, hi float64, limit int, err error) {
	hi = math.Inf(1)
	low := 0
	for _, e := range used {
		top := nozzleMaxLayer(printer, sources, e)
		if top < hi {
			hi, limit = top, e
		}
		if m := nozzleColumn(sources[e].Config, "min_layer_height", e); m > lo {
			lo, low = m, e
		}
	}
	if lo > hi+1e-9 && SameMm(printer.Nozzles[low].Diameter, printer.Nozzles[limit].Diameter) {
		return 0, 0, 0, fmt.Errorf("A %g mm nozzle prints layers up to %g mm, and this printer's profile starts at %g mm — no layer height fits! Pick a %g mm nozzle or larger.",
			printer.Nozzles[limit].Diameter, hi, lo, smallestNozzleFor(lo))
	}
	if lo > hi+1e-9 {
		return 0, 0, 0, fmt.Errorf("The %s (%g mm) prints layers from %g mm and the %s (%g mm) up to %g mm — they don't share any layer height! Put the filaments on closer nozzle sizes.",
			nozzleName(printer, low), printer.Nozzles[low].Diameter, lo, nozzleName(printer, limit), printer.Nozzles[limit].Diameter, hi)
	}
	return lo, hi, limit, nil
}

func scaleAcross(rows []Scale, printer *Printer, used []int, height float64) (map[string]string, error) {
	out := scaleValues(rows, printer.Nozzles[used[0]].Diameter, height)
	for _, e := range used[1:] {
		d := printer.Nozzles[e].Diameter
		other := scaleValues(rows, d, height)
		for k, v := range out {
			if other[k] == v || strings.HasSuffix(v, "%") {
				continue
			}
			a, _ := strconv.ParseFloat(v, 64)
			b, _ := strconv.ParseFloat(other[k], 64)
			first := printer.Nozzles[used[0]].Diameter
			if k == "initial_layer_print_height" {
				out[k] = strconv.FormatFloat(math.Min(a, b), 'f', -1, 64)
				continue
			}
			if isLineWidth(k) && math.Abs(a/first-b/d) < 1e-9 {
				out[k] = strconv.FormatFloat(roundRule(a/first*100), 'f', -1, 64) + "%"
				continue
			}
			return nil, fmt.Errorf("%s comes out to %g mm on the %s and %g mm on the %s, and one setting can't be both! Base its formula on the nozzle diameter, or give it a fixed value.",
				k, a, nozzleName(printer, used[0]), b, nozzleName(printer, e))
		}
	}
	return out, nil
}

func slotFlows(config map[string]json.RawMessage, filaments []map[string]json.RawMessage, slotNozzle []int) []float64 {
	flows := ConfigList(config["filament_max_volumetric_speed"])
	number := func(t string) float64 { n, _ := strconv.ParseFloat(strings.TrimSpace(t), 64); return n }
	lowest := func(list []string) float64 {
		low := 0.0
		for _, t := range list {
			if n := number(t); n > 0 && (low == 0 || n < low) {
				low = n
			}
		}
		return low
	}
	variants, total := make([][]string, len(filaments)), 0
	for i, f := range filaments {
		if variants[i] = ConfigList(f["filament_extruder_variant"]); len(variants[i]) == 0 {
			variants[i] = []string{""}
		}
		total += len(variants[i])
	}
	out, offset := make([]float64, len(filaments)), 0
	for i := range filaments {
		column := slices.Index(variants[i], installedVariant(config, slotNozzle[i]))
		switch {
		case len(flows) == total && column >= 0:
			out[i] = number(flows[offset+column])
		case len(flows) == total:
			out[i] = lowest(flows[offset : offset+len(variants[i])])
		case len(flows) == len(filaments):
			out[i] = number(flows[i])
		default:
			out[i] = lowest(flows)
		}
		offset += len(variants[i])
	}
	return out
}

func installedVariant(config map[string]json.RawMessage, e int) string {
	at := func(key, fallback string) string {
		list := ConfigList(config[key])
		if len(list) == 0 {
			return fallback
		}
		return strings.TrimSpace(list[min(e, len(list)-1)])
	}
	return at("extruder_type", "Direct Drive") + " " + at("nozzle_volume_type", "Standard")
}

type resolvedNozzle struct {
	Extruder int                      `json:"extruder"`
	Name     string                   `json:"name"`
	Diameter float64                  `json:"diameter"`
	Variant  string                   `json:"variant"`
	Slots    []int                    `json:"slots"`
	Flow     *float64                 `json:"flow,omitempty"`
	Speeds   map[string]resolvedSpeed `json:"speeds"`
}

func mergeNozzleSpeeds(per map[int]map[string]json.RawMessage, used []int, process *Preset) map[string]json.RawMessage {
	out := map[string]json.RawMessage{}
	ids := ConfigList(process.Config["print_extruder_id"])
	for k, first := range per[used[0]] {
		same, low := true, math.Inf(1)
		for _, e := range used {
			v := per[e][k]
			if string(v) != string(first) {
				same = false
			}
			var t string
			json.Unmarshal(v, &t)
			if n, err := strconv.ParseFloat(t, 64); err == nil {
				low = math.Min(low, n)
			}
		}
		switch {
		case same:
			out[k] = first
		case len(ids) > 0:
			list := make([]string, len(ids))
			for col, id := range ids {
				e, _ := strconv.Atoi(strings.TrimSpace(id))
				list[col] = strconv.FormatFloat(low, 'f', -1, 64)
				if v, ok := per[e-1][k]; ok {
					json.Unmarshal(v, &list[col])
				}
			}
			out[k] = must(json.Marshal(list))
		default:
			out[k] = must(json.Marshal(strconv.FormatFloat(low, 'f', -1, 64)))
		}
	}
	return out
}
