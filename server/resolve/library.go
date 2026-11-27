package resolve

import "encoding/json"

const TemperatureOffset = "temperature_offset"

type ArchivedMark struct {
	At          float64 `json:"at"`
	OperationID string  `json:"operationId"`
}

type Problem struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (p *Problem) Error() string { return p.Message }

func (l *Library) Problems() map[string]*Problem {
	out := map[string]*Problem{}
	for id, p := range l.Printers {
		if p.Problem != nil {
			out[id] = p.Problem
		}
	}
	for id, p := range l.Processes {
		if p.Problem != nil {
			out[id] = p.Problem
		}
	}
	for id, f := range l.Filaments {
		if f.Problem != nil {
			out[id] = f.Problem
		}
	}
	for _, c := range l.Calibrations {
		if c.Problem != nil {
			out[c.ID] = c.Problem
		}
	}
	return out
}

type Printer struct {
	ID       string        `json:"id"`
	Name     string        `json:"name"`
	Revision *uint64       `json:"revision"`
	Archived *ArchivedMark `json:"archived,omitempty"`
	Edited         string                     `json:"edited,omitempty"`
	Vendor         string                     `json:"vendor"`
	Model          string                     `json:"model"`
	Nozzles        []Nozzle                   `json:"nozzles"`
	NozzlePresets  []NozzlePreset             `json:"nozzlePresets,omitempty"`
	Machine        map[string]json.RawMessage `json:"machine,omitempty"`
	PrintOverrides map[string]json.RawMessage `json:"printOverrides,omitempty"`
	Process        string                     `json:"process,omitempty"`
	Swap           string                     `json:"swap"`
	Slots          []Slot                     `json:"slots"`
	Units          []Unit                     `json:"units,omitempty"`
	Mixes []Mix `json:"mixes,omitempty"`
	Plate    string    `json:"plate,omitempty"`
	Flushing *Flushing `json:"flushing,omitempty"`
	Problem *Problem `json:"-"`
}

type Nozzle struct {
	Variant  string      `json:"variant"`
	Diameter float64     `json:"diameter"`
	Volume   string      `json:"volume,omitempty"`
	Type     string      `json:"type,omitempty"`
	Offset   *[2]float64 `json:"offset,omitempty"`
}

type NozzlePreset struct {
	Diameter float64   `json:"diameter"`
	Variants []string `json:"variants"`
}

type Unit struct {
	Nozzle int `json:"nozzle"`
}

type Slot struct {
	ID       string `json:"id"`
	Filament string `json:"filament"`
	Colour   string `json:"colour,omitempty"`
	Tray     string `json:"tray,omitempty"`
}

type Filament struct {
	ID       string        `json:"id"`
	Name     string        `json:"name"`
	Revision *uint64       `json:"revision"`
	Archived *ArchivedMark `json:"archived,omitempty"`
	Edited string `json:"edited,omitempty"`
	Product  string                     `json:"product"`
	Settings map[string]json.RawMessage `json:"settings,omitempty"`
	Aliases []string `json:"aliases,omitempty"`
	Problem *Problem `json:"-"`
}

type Calibration struct {
	ID       string  `json:"id"`
	Revision *uint64 `json:"revision"`
	Filament string  `json:"filament"`
	Printer  string  `json:"printer"`
	Extruder int                        `json:"extruder"`
	Nozzle   float64                    `json:"nozzle"`
	Volume   string                     `json:"volume,omitempty"`
	Values   map[string]json.RawMessage `json:"values"`
	Problem  *Problem                   `json:"-"`
}

type Process struct {
	ID       string        `json:"id"`
	Name     string        `json:"name"`
	Revision *uint64       `json:"revision"`
	Archived *ArchivedMark `json:"archived,omitempty"`
	Edited          string                     `json:"edited,omitempty"`
	Values          map[string]json.RawMessage `json:"values,omitempty"`
	Scales          []Scale                    `json:"scales,omitempty"`
	PreferredHeight *float64                   `json:"preferredHeight,omitempty"`
	HeightRule      string                     `json:"heightRule,omitempty"`
	SpeedModel      *SpeedModel                `json:"speedModel,omitempty"`
	Product string `json:"product,omitempty"`
	Problem           *Problem `json:"-"`
}

type Library struct {
	Revision     uint64
	Printers     map[string]*Printer
	Filaments    map[string]*Filament
	Alias        map[string]string
	Calibrations []*Calibration
	Processes    map[string]*Process
}

func (p *Printer) Live() bool  { return p.Archived == nil }
func (f *Filament) Live() bool { return f.Archived == nil }
func (p *Process) Live() bool  { return p.Archived == nil }

func (l *Library) Filament(id string) *Filament {
	if f := l.Filaments[id]; f != nil {
		return f
	}
	return l.Filaments[l.Alias[id]]
}

func (l *Library) Calibration(filament string, printer *Printer, extruder int) *Calibration {
	f := l.Filament(filament)
	if f == nil || extruder >= len(printer.Nozzles) {
		return nil
	}
	n := printer.Nozzles[extruder]
	for _, c := range l.Calibrations {
		if c.Filament == f.ID && c.Printer == printer.ID && c.Extruder == extruder && SameMm(c.Nozzle, n.Diameter) && c.Volume == n.Volume {
			return c
		}
	}
	return nil
}
