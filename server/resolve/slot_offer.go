package resolve

import (
	"cmp"
	"encoding/json"
	"fmt"
	"reflect"
	"slices"
	"strconv"
	"strings"
)

type SlotOfferRequest struct {
	Printer string        `json:"printer"`
	Strip   SlotStrip     `json:"strip"`
	Values  SlotValues    `json:"values"`
	Edits   FilamentEdits `json:"edits"`
}

func (catalog *Catalog) SlotOfferFor(lib *Library, in SlotOfferRequest) SlotOffer {
	var printer *Printer
	if p := lib.Printers[in.Printer]; p != nil && p.Live() {
		printer = p
	}
	o := catalog.placeStrip(lib, printer, in.Strip)
	catalog.offerValues(lib, printer, &o, in.Values, in.Edits)
	return o
}

type SlotStrip struct {
	Filaments []string                   `json:"filaments"`
	Colours   []*string                  `json:"colours"`
	Mixes     map[string]json.RawMessage `json:"mixes"`
}

type SlotOffer struct {
	Strip   SlotStrip                    `json:"strip"`
	Placed  []bool                       `json:"placed"`
	Adds    []string                     `json:"adds"`
	Differs bool                         `json:"differs"`
	Notes   []string                     `json:"notes"`
	Values  []map[string]json.RawMessage `json:"values"`
	Same    bool                         `json:"same"`
}

type SlotValues = []map[string]json.RawMessage
type FilamentEdits = map[string]map[string]json.RawMessage

const catalogSlot, unknownSlot = "catalog:", "unknown:"

func PrinterStrip(p *Printer) SlotStrip {
	if p == nil {
		return SlotStrip{Filaments: []string{}, Mixes: map[string]json.RawMessage{}}
	}
	var physical []string
	var colours []*string
	for _, s := range p.Slots {
		physical = append(physical, s.Filament)
		if s.Colour != "" {
			colours = append(colours, &s.Colour)
		} else {
			colours = append(colours, nil)
		}
	}
	if len(physical) == 0 {
		physical = []string{""}
	}
	out, mixes := slices.Clone(physical), map[string]json.RawMessage{}
	for k, m := range p.Mixes {
		components := make([]int, len(m.Components))
		for j, id := range m.Components {
			components[j] = p.SlotIndex(id) + 1
		}
		f := physical[0]
		if len(components) > 0 && components[0] >= 1 {
			f = physical[components[0]-1]
		}
		out = append(out, f)
		mixes[strconv.Itoa(len(physical)+k)] = must(json.Marshal(map[string]any{"components": components, "ratios": m.Ratios, "gradient": m.Gradient, "perPart": m.PerPart}))
	}
	all := make([]*string, len(out))
	copy(all, colours)
	return SlotStrip{Filaments: out, Colours: all, Mixes: mixes}
}

func (catalog *Catalog) placeStrip(lib *Library, printer *Printer, project SlotStrip) SlotOffer {
	own := PrinterStrip(printer)
	fixed := printer != nil && (printer.Swap == "toolheads" || len(printer.Slots) > 0 && !slices.ContainsFunc(printer.Slots, func(s Slot) bool { return s.Tray == "" }))
	count := len(own.Filaments)
	if !fixed {
		count = max(count, len(project.Filaments))
	}
	products := map[string]bool{}
	for _, e := range catalog.List("filaments") {
		products[e.ID] = true
	}
	out := SlotOffer{Strip: SlotStrip{Filaments: []string{}, Colours: []*string{}, Mixes: map[string]json.RawMessage{}}, Placed: []bool{}, Adds: []string{}, Notes: []string{}}
	at := func(list []*string, i int) *string {
		if i < len(list) {
			return list[i]
		}
		return nil
	}
	unknown := 0
	for i := range count {
		want := ""
		if i < len(project.Filaments) {
			want = project.Filaments[i]
		}
		id := ""
		if want != "" {
			if f := lib.Filament(want); f != nil && f.Live() {
				id = f.ID
			} else if product, ok := strings.CutPrefix(want, unknownSlot); ok && product != "" && products[cmp.Or(catalog.FoldedInto(product), product)] {
				product = cmp.Or(catalog.FoldedInto(product), product)
				id = catalogSlot + product
				if !slices.Contains(out.Adds, product) {
					out.Adds = append(out.Adds, product)
				}
			} else {
				unknown++
			}
		}
		filament, colour := id, at(project.Colours, i)
		if id == "" {
			filament, colour = "", at(own.Colours, i)
			if i < len(own.Filaments) {
				filament = own.Filaments[i]
			}
		} else if colour == nil {
			colour = at(own.Colours, i)
		}
		out.Strip.Filaments = append(out.Strip.Filaments, filament)
		out.Strip.Colours = append(out.Strip.Colours, colour)
		out.Placed = append(out.Placed, id != "")
	}
	for k, raw := range project.Mixes {
		var m struct {
			Components []int `json:"components"`
		}
		i, err := strconv.Atoi(k)
		if err != nil || i >= count || json.Unmarshal(raw, &m) != nil || slices.ContainsFunc(m.Components, func(c int) bool { return c > count }) {
			continue
		}
		out.Strip.Mixes[k] = raw
	}
	out.Differs = !slices.Equal(out.Strip.Filaments, own.Filaments) || !sameMixes(out.Strip.Mixes, own.Mixes)
	if unknown == 1 {
		out.Notes = append(out.Notes, "One of the project's filaments isn't in your library, so its slot is keeping the printer's filament.")
	} else if unknown > 1 {
		out.Notes = append(out.Notes, fmt.Sprintf("%d of the project's filaments aren't in your library, so their slots keep the printer's filament.", unknown))
	}
	if n := len(project.Filaments); n > count {
		s, rest, verb := "s", strconv.Itoa(count+1), "aren't"
		if count == 1 {
			s = ""
		}
		if n > count+1 {
			rest += "–" + strconv.Itoa(n)
		}
		plural := "s"
		if n-count == 1 {
			plural, verb = "", "isn't"
		}
		out.Notes = append(out.Notes, fmt.Sprintf("This printer has %d slot%s, so the project's filament%s %s %s placed.", count, s, plural, rest, verb))
	}
	return out
}

func sameMixes(a, b map[string]json.RawMessage) bool {
	decode := func(m map[string]json.RawMessage) map[string]any {
		out := map[string]any{}
		for k, v := range m {
			var x any
			json.Unmarshal(v, &x)
			out[k] = x
		}
		return out
	}
	return reflect.DeepEqual(decode(a), decode(b))
}

func (o SlotOffer) Shown() bool { return o.Differs || len(o.Notes) > 0 || o.Same }

func (catalog *Catalog) offerValues(lib *Library, printer *Printer, o *SlotOffer, values SlotValues, edits FilamentEdits) {
	o.Values = make([]map[string]json.RawMessage, len(o.Placed))
	offered := false
	for i, placed := range o.Placed {
		if !placed || i >= len(values) || len(values[i]) == 0 {
			continue
		}
		id := o.Strip.Filaments[i]
		if printer == nil || strings.HasPrefix(id, catalogSlot) {
			o.Values[i], offered = values[i], true
			continue
		}
		ask := Ask{Type: "filament", ID: id}
		if e, ok := printer.SlotExtruder(i); ok {
			ask.Extruder = &e
		}
		now, err := catalog.Profile(lib, printer.ID, ask)
		if err != nil {
			o.Values[i], offered = values[i], true
			continue
		}
		for k, v := range values[i] {
			key := CanonicalKey(k)
			current, edited := edits[id][k]
			if !edited {
				current, edited = edits[id][key]
			}
			if !edited {
				current = now.Values[key]
			}
			if sameValue(v, current) {
				continue
			}
			o.Values[i], offered = values[i], true
			break
		}
	}
	o.Same = offered && !o.Differs
}

func sameValue(a, b json.RawMessage) bool {
	norm := func(raw json.RawMessage) string {
		var v any
		if json.Unmarshal(raw, &v) != nil {
			return string(raw)
		}
		var text func(any) string
		text = func(v any) string {
			switch x := v.(type) {
			case []any:
				parts := make([]string, len(x))
				for i, p := range x {
					parts[i] = text(p)
				}
				return strings.Join(parts, ",")
			case float64:
				return strconv.FormatFloat(x, 'g', -1, 64)
			case string:
				if f, err := strconv.ParseFloat(strings.TrimSpace(x), 64); err == nil {
					return strconv.FormatFloat(f, 'g', -1, 64)
				}
				return strings.TrimSpace(x)
			case bool:
				return map[bool]string{true: "1", false: "0"}[x]
			case nil:
				return ""
			}
			return fmt.Sprint(v)
		}
		return text(v)
	}
	return norm(a) == norm(b)
}
