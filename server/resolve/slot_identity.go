package resolve

import (
	"fmt"
	"strconv"
	"strings"
)

type Mix struct {
	ID         string    `json:"id"`
	Components []string  `json:"components"`
	Ratios     []float64 `json:"ratios"`
	Gradient   *int      `json:"gradient"`
	PerPart    bool      `json:"perPart"`
}

type TrayRef struct {
	External   bool
	Side       int
	Unit, Slot int
}

func ParseTray(s string) (TrayRef, bool) {
	switch s {
	case "ext":
		return TrayRef{External: true, Side: -1}, true
	case "ext.L":
		return TrayRef{External: true, Side: 0}, true
	case "ext.R":
		return TrayRef{External: true, Side: 1}, true
	}
	rest, ok := strings.CutPrefix(s, "ams")
	unit, slot, dot := strings.Cut(rest, ".")
	if !ok || !dot {
		return TrayRef{}, false
	}
	u, err1 := strconv.Atoi(unit)
	n, err2 := strconv.Atoi(slot)
	if err1 != nil || err2 != nil || u < 0 || u > 255 || n < 0 || n > 3 || strconv.Itoa(u) != unit || strconv.Itoa(n) != slot {
		return TrayRef{}, false
	}
	return TrayRef{Unit: u, Slot: n}, true
}

func (t TrayRef) String() string {
	if t.External {
		return [...]string{"ext", "ext.L", "ext.R"}[t.Side+1]
	}
	return fmt.Sprintf("ams%d.%d", t.Unit, t.Slot)
}

func (printer *Printer) SlotIndex(id string) int {
	for i, s := range printer.Slots {
		if s.ID == id {
			return i
		}
	}
	return -1
}

func (printer *Printer) MixByID(id string) *Mix {
	for i := range printer.Mixes {
		if printer.Mixes[i].ID == id {
			return &printer.Mixes[i]
		}
	}
	return nil
}

func (printer *Printer) StripSlots() []string {
	out := make([]string, 0, len(printer.Slots)+len(printer.Mixes))
	for _, s := range printer.Slots {
		out = append(out, s.ID)
	}
	for _, m := range printer.Mixes {
		out = append(out, m.ID)
	}
	return out
}

func mixesProblem(p *Printer) *Problem {
	seen := map[string]bool{}
	for _, m := range p.Mixes {
		if !strings.HasPrefix(m.ID, "mix:") || len(m.ID) <= len("mix:") || seen[m.ID] {
			return problem("mix_id", "%s: every mixed filament needs a unique id. Remove the mixed filament and add it again.", p.Name)
		}
		seen[m.ID] = true
		parts := map[string]bool{}
		for _, c := range m.Components {
			if p.SlotIndex(c) < 0 || parts[c] {
				return problem("mix_component", "%s: a mixed filament mixes a slot the printer doesn't have. Edit the mixed filament.", p.Name)
			}
			parts[c] = true
		}
		if len(m.Components) < 2 || len(m.Ratios) != len(m.Components) {
			return problem("mix_component", "%s: a mixed filament needs two or more slots, each with a ratio. Edit the mixed filament.", p.Name)
		}
	}
	return nil
}
