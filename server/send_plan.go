package main

import (
	"fmt"
	"strings"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type sendMaterial struct {
	Type  string `json:"type,omitempty"`
	Color string `json:"color,omitempty"`
}

type sendSlot struct {
	Filament int           `json:"filament"`
	Slot     string        `json:"slot"`
	Tray     string        `json:"tray,omitempty"`
	Used     bool          `json:"used"`
	State    string        `json:"state,omitempty"`
	Expected sendMaterial  `json:"expected"`
	Reported *sendMaterial `json:"reported,omitempty"`
	tray     *AMSTray
}

type sendPlan struct {
	Slots []sendSlot `json:"slots"`
}

func sendPlanOf(lib *resolve.Library, req *resolve.Request, sliced *BambuJobMapping, trays []AMSTray) (sendPlan, error) {
	if req == nil {
		return sendPlan{}, fmt.Errorf("This job names no library printer. Slice again.")
	}
	printer := lib.Printers[req.Printer]
	if printer == nil || len(req.Slots) != len(req.Filaments) {
		return sendPlan{}, fmt.Errorf("This job's printer or slots are no longer in your library. Slice again.")
	}
	plan := sendPlan{Slots: []sendSlot{}}
	for i, id := range req.Slots {
		row := sendSlot{Filament: i + 1, Slot: id}
		at := printer.SlotIndex(id)
		if m := printer.MixByID(id); at < 0 && m != nil && len(m.Components) > 0 {
			at = printer.SlotIndex(m.Components[0])
		}
		if at < 0 {
			return sendPlan{}, fmt.Errorf("Filament %d's slot is no longer on %s. Slice again.", i+1, printer.Name)
		}
		row.Tray = printer.Slots[at].Tray
		for _, m := range sliced.Materials {
			if m.Slot == i+1 {
				row.Used = m.Object || m.Support
				row.Expected = sendMaterial{m.Type, m.Color}
			}
		}
		if row.Used {
			row.State = "assigned"
			ref, ok := resolve.ParseTray(row.Tray)
			switch {
			case !ok:
				row.State = "unassigned"
			case trays != nil:
				row.State = "mismatch"
				for k := range trays {
					if slotID(trays[k]) == ref.String() {
						row.tray = &trays[k]
						row.Reported = &sendMaterial{trays[k].Type, trays[k].Color}
						if trays[k].Type != "" && strings.EqualFold(trays[k].Type, row.Expected.Type) {
							row.State = "confirmed"
						}
					}
				}
			}
		}
		plan.Slots = append(plan.Slots, row)
	}
	return plan, nil
}

func (plan sendPlan) mapping(override []string) ([]int, []bambuTrayAddress, *sendSlot) {
	v0, v1 := make([]int, len(plan.Slots)), make([]bambuTrayAddress, len(plan.Slots))
	for i, row := range plan.Slots {
		if !row.Used {
			v0[i], v1[i] = -1, bambuTrayAddress{255, 255}
			continue
		}
		overridden := row.State == "mismatch" && row.tray != nil && row.tray.Type != "" && containsString(override, row.Slot)
		if row.tray == nil || row.State != "confirmed" && !overridden {
			return nil, nil, &plan.Slots[i]
		}
		t := row.tray
		switch {
		case t.Kind == "external":
			v0[i], v1[i] = -1, bambuTrayAddress{t.ID, 0}
		case t.AMS >= 128:
			v0[i], v1[i] = t.ID, bambuTrayAddress{t.AMS, 0}
		default:
			v0[i], v1[i] = t.ID, bambuTrayAddress{t.AMS, t.Slot}
		}
	}
	return v0, v1, nil
}

func containsString(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}
