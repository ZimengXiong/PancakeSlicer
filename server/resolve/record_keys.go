package resolve

import (
	"sync"
)

var HardwareKeys = []string{"nozzle_diameter", "nozzle_volume_type", "extruder_type", "extruder_offset"}

type settingSets struct {
	filament map[string]bool
	printer  map[string]map[string]bool
}

var settingSetsOf sync.Map

func (c *Catalog) settingSets() *settingSets {
	if v, ok := settingSetsOf.Load(c); ok {
		return v.(*settingSets)
	}
	sets := &settingSets{filament: map[string]bool{}, printer: map[string]map[string]bool{}}
	for _, p := range c.Filaments {
		for key := range p.Base {
			sets.filament[key] = true
		}
		for _, t := range p.Tunings {
			for key := range t.Values {
				sets.filament[key] = true
			}
		}
	}
	for id, p := range c.Printers {
		keys := map[string]bool{}
		for key := range p.Base {
			keys[key] = true
		}
		for _, v := range p.Variants {
			for key := range v.Values {
				keys[key] = true
			}
		}
		for _, h := range HardwareKeys {
			delete(keys, h)
		}
		sets.printer[id] = keys
	}
	settingSetsOf.Store(c, sets)
	return sets
}

func (c *Catalog) PrintSettings() map[string]bool { return c.printSettings }

func (c *Catalog) FilamentSettings() map[string]bool { return c.settingSets().filament }

func (c *Catalog) PrinterSettings(model string) map[string]bool {
	return c.settingSets().printer[model]
}

func (c *Catalog) RecordKinds(key string) []string {
	kinds := []string{}
	for _, m := range c.Printers {
		if c.PrinterSettings(m.ID)[key] {
			kinds = append(kinds, "printer")
			break
		}
	}
	if c.FilamentSettings()[key] {
		kinds = append(kinds, "filament")
	}
	if c.PrintSettings()[key] {
		kinds = append(kinds, "print")
	}
	return kinds
}
