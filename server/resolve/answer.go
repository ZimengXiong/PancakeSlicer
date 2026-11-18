package resolve

import (
	"cmp"
	"encoding/json"
	"errors"
	"maps"
	"slices"
	"strconv"
	"strings"
)

type Answer struct {
	Printer   resolvedProfile   `json:"printer"`
	Print     resolvedProfile   `json:"print"`
	Filaments []resolvedProfile `json:"filaments"`
	Profiles  []askedProfile    `json:"profiles,omitempty"`
	Objects   []resolvedObject  `json:"objects,omitempty"`
	Catalog   string            `json:"catalog"`
	Revision  uint64            `json:"revision"`
	Setup     *answerSetup      `json:"setup,omitempty"`
}

type answerSetup struct {
	Plates
	Plate    string            `json:"plate,omitempty"`
	Flushing *resolvedFlushing `json:"flushing,omitempty"`
}

type Query struct {
	Request
	Drafts   *resolveDrafts `json:"drafts,omitempty"`
	Profiles []Ask          `json:"profiles,omitempty"`
	Objects  []struct {
		ID       string                     `json:"id"`
		Settings map[string]json.RawMessage `json:"settings"`
	} `json:"objects,omitempty"`
}

type resolveCalibrationDraft struct {
	Filament string                     `json:"filament"`
	Printer  string                     `json:"printer"`
	Extruder int                        `json:"extruder"`
	Values   map[string]json.RawMessage `json:"values"`
}

type draftRecord struct {
	Values          map[string]json.RawMessage `json:"values"`
	Scales          []Scale                    `json:"scales,omitempty"`
	SpeedModel      *SpeedModel                `json:"speedModel,omitempty"`
	PreferredHeight *float64                   `json:"preferredHeight,omitempty"`
}
type resolveDrafts struct {
	Records      map[string]draftRecord    `json:"records,omitempty"`
	Nozzles      map[string][]Nozzle       `json:"nozzles,omitempty"`
	Calibrations []resolveCalibrationDraft `json:"calibrations,omitempty"`
}

type Ask struct {
	Type     string `json:"type"`
	ID       string `json:"id"`
	Printer  string `json:"printer,omitempty"`
	Extruder *int   `json:"extruder,omitempty"`
}
type resolveStatus struct {
	OK      bool   `json:"ok"`
	Code    string `json:"code,omitempty"`
	Message string `json:"message,omitempty"`
}
type resolvedProfile struct {
	*ProfileValues
	Status  resolveStatus    `json:"status"`
	Heights *resolvedHeights `json:"heights,omitempty"`
	Nozzles []resolvedNozzle `json:"nozzles,omitempty"`
}

type resolvedObject struct {
	ID string `json:"id"`
	*ProfileValues
	Status resolveStatus `json:"status"`
}
type askedProfile struct {
	resolvedProfile
	Ask
	Setup *answerSetup `json:"setup,omitempty"`
}

func (c *Catalog) setupOf(printer *Printer, flushing *resolvedFlushing) *answerSetup {
	plates := c.PlatesOf(printer.Nozzles[0].Variant)
	if plates == nil {
		plates = &Plates{Plates: []BuildPlate{}}
	}
	if len(plates.Plates) == 0 && flushing == nil {
		return nil
	}
	setup := &answerSetup{Plates: *plates, Flushing: flushing}
	if len(plates.Plates) > 0 {
		setup.Plate = cmp.Or(printer.Plate, plates.Default)
	}
	return setup
}

func statusOf(err error) resolveStatus {
	var r *resolveError
	if errors.As(err, &r) {
		return resolveStatus{Code: r.code, Message: r.message}
	}
	return resolveStatus{Code: "unresolved", Message: err.Error()}
}

func (c *Catalog) Answer(lib *Library, in Query) Answer {
	view := lib.withDrafts(in.Drafts)
	listed := func(v *ProfileValues, kind string) *ProfileValues {
		for k, raw := range profileDefaults[kind] {
			if _, set := v.Values[k]; !set && !slices.Contains(PlateSetupKeys, k) {
				v.put(k, raw, "default")
			}
		}
		for k, raw := range v.Values {
			v.Values[k] = listedValue(vectors, k, raw)
		}
		for k, layers := range v.Layers {
			for layer, raw := range layers {
				layers[layer] = listedValue(vectors, k, raw)
			}
		}
		return v
	}
	ok := resolveStatus{OK: true}
	response := Answer{Catalog: c.Version, Revision: lib.Revision, Filaments: []resolvedProfile{}}
	req := in.Request
	req.partial = true
	job, err := c.Job(view, req)
	if job != nil {
		if printer := view.Printers[req.Printer]; printer != nil {
			response.Setup = c.setupOf(printer, job.flushing)
		}
		response.Printer = resolvedProfile{ProfileValues: listed(job.printer, "machine"), Status: ok}
		response.Print = resolvedProfile{ProfileValues: listed(job.print, "process"), Status: ok, Heights: job.heights, Nozzles: job.Nozzles}
		if err != nil {
			response.Print = resolvedProfile{ProfileValues: newValues(), Status: statusOf(err)}
		}
		for i, slot := range job.slots {
			if e := job.slotErrors[i]; e != nil {
				response.Filaments = append(response.Filaments, resolvedProfile{ProfileValues: newValues(), Status: statusOf(e)})
			} else {
				response.Filaments = append(response.Filaments, resolvedProfile{ProfileValues: listed(slot, "filament"), Status: ok})
			}
		}
	} else {
		failed := statusOf(err)
		var r *resolveError
		errors.As(err, &r)
		blocked := resolveStatus{Code: "unresolved", Message: failed.Message}
		pick := func(profile string, slot int) resolveStatus {
			if r != nil && r.profile == profile && (profile != "filament" || r.slot == slot) {
				return failed
			}
			return blocked
		}
		empty := func() *ProfileValues { return newValues() }
		response.Printer, response.Print = resolvedProfile{ProfileValues: empty(), Status: pick("printer", 0)}, resolvedProfile{ProfileValues: empty(), Status: pick("print", 0)}
		for i := range in.Filaments {
			response.Filaments = append(response.Filaments, resolvedProfile{ProfileValues: empty(), Status: pick("filament", i)})
		}
	}
	for _, object := range in.Objects {
		answer := resolvedObject{ID: object.ID, ProfileValues: newValues(), Status: response.Print.Status}
		if response.Print.Status.OK {
			for k, v := range response.Print.Values {
				answer.Values[k] = v
			}
			for k, layers := range response.Print.Layers {
				answer.Layers[k] = maps.Clone(layers)
			}
			for k, v := range object.Settings {
				answer.put(CanonicalKey(k), listedValue(vectors, CanonicalKey(k), v), "object")
			}
		}
		response.Objects = append(response.Objects, answer)
	}
	for _, ask := range in.Profiles {
		printer := cmp.Or(ask.Printer, in.Printer)
		values, err := c.Profile(view, printer, ask)
		if err != nil {
			response.Profiles = append(response.Profiles, askedProfile{resolvedProfile{ProfileValues: newValues(), Status: statusOf(err)}, ask, nil})
			continue
		}
		answer := askedProfile{resolvedProfile{ProfileValues: listed(values, ask.Type), Status: ok}, ask, nil}
		if p := view.Printers[ask.ID]; ask.Type == "machine" && p != nil {
			answer.Setup = c.setupOf(p, nil)
		}
		if ask.Type == "process" {
			if lib, req := c.askSelection(view, printer, ask.ID, in.Request); lib != nil {
				if job, err := c.Job(lib, req); err == nil {
					answer.ProfileValues, answer.Heights, answer.Nozzles = listed(job.print, "process"), job.heights, job.Nozzles
				}
			}
		}
		response.Profiles = append(response.Profiles, answer)
	}
	return response
}

func listedValue(vectors map[string]bool, k string, v json.RawMessage) json.RawMessage {
	var text string
	if vectors[k] && json.Unmarshal(v, &text) == nil {
		return must(json.Marshal(strings.Split(text, ",")))
	}
	return v
}

func (l *Library) withDrafts(d *resolveDrafts) *Library {
	if d == nil {
		return l
	}
	view := &Library{Printers: maps.Clone(l.Printers), Filaments: maps.Clone(l.Filaments), Alias: l.Alias, Processes: maps.Clone(l.Processes),
		Calibrations: slices.Clone(l.Calibrations), Revision: l.Revision}
	for id, draft := range d.Records {
		if p := view.Printers[id]; p != nil {
			copy := *p
			copy.Machine = draft.Values
			view.Printers[id] = &copy
		} else if p := view.Processes[id]; p != nil {
			copy := *p
			copy.Values, copy.Scales, copy.SpeedModel, copy.PreferredHeight = draft.Values, draft.Scales, draft.SpeedModel, draft.PreferredHeight
			view.Processes[id] = &copy
		} else if f := view.Filament(id); f != nil {
			copy := *f
			copy.Settings = draft.Values
			view.Filaments[f.ID] = &copy
		}
	}
	for id, nozzles := range d.Nozzles {
		if p := view.Printers[id]; p != nil && len(nozzles) == len(p.Nozzles) {
			copy := *p
			copy.Nozzles = nozzles
			view.Printers[id] = &copy
		}
	}
	for _, c := range d.Calibrations {
		printer, f := view.Printers[c.Printer], view.Filament(c.Filament)
		if printer == nil || f == nil || c.Extruder < 0 || c.Extruder >= len(printer.Nozzles) {
			continue
		}
		n := printer.Nozzles[c.Extruder]
		row := &Calibration{ID: "draft", Revision: new(uint64), Filament: f.ID, Printer: printer.ID, Extruder: c.Extruder, Nozzle: n.Diameter, Volume: n.Volume, Values: c.Values}
		i := slices.IndexFunc(view.Calibrations, func(o *Calibration) bool {
			return o.Filament == row.Filament && o.Printer == row.Printer && o.Extruder == row.Extruder && SameMm(o.Nozzle, row.Nozzle) && o.Volume == row.Volume
		})
		if i >= 0 {
			view.Calibrations[i] = row
		} else {
			view.Calibrations = append(view.Calibrations, row)
		}
	}
	return view
}

func (c *Catalog) Profile(lib *Library, printerID string, ask Ask) (*ProfileValues, error) {
	extruder := 0
	if ask.Extruder != nil {
		extruder = *ask.Extruder
	}
	if ask.Type == "machine" {
		printerID = ask.ID
	}
	owner := lib.Printers[printerID]
	variant := printerID
	if owner != nil {
		if extruder < 0 || extruder >= len(owner.Nozzles) {
			return nil, &resolveError{"", 0, "unresolved", owner.Name + " has no nozzle " + strconv.Itoa(extruder+1) + "."}
		}
		variant = owner.Nozzles[extruder].Variant
	}
	machine := c.Machine(variant)
	if machine == nil && printerID == "" {
		what := map[string]string{"filament": "filament", "process": "print profile"}[ask.Type]
		return nil, &resolveError{"", 0, "unresolved", "Pick a printer to see this " + what + "'s values for it!"}
	}
	if machine == nil {
		if ask.Type == "machine" {
			return nil, &resolveError{"", 0, "unknown_record", "Hmm, that printer isn't in your library or the catalog anymore."}
		}
		return nil, &resolveError{"", 0, "unresolved", "This profile's printer isn't in your library or the catalog anymore — pick another printer!"}
	}
	out := newValues()
	catalogLayers := func(p *Preset) {
		for k, v := range p.Config {
			out.put(k, v, p.Layers[k])
		}
	}
	record := func(values map[string]json.RawMessage, layer string) {
		for k, v := range values {
			out.put(CanonicalKey(k), v, layer)
		}
	}
	switch ask.Type {
	case "machine":
		if p := lib.Printers[ask.ID]; p != nil {
			if p.Problem != nil {
				return nil, &resolveError{"", 0, "invalid_record", p.Problem.Message}
			}
			if m := c.Machine(p.Nozzles[0].Variant); m != nil {
				catalogLayers(m)
				record(p.Machine, "record")
				return out, nil
			}
			return nil, &resolveError{"", 0, "unresolved", p.Name + " has no catalog variant. Pick its nozzle again."}
		}
		m := c.Machine(ask.ID)
		if m == nil {
			return nil, &resolveError{"", 0, "unknown_record", "Hmm, that printer isn't in your library or the catalog anymore."}
		}
		catalogLayers(m)
	case "process":
		product, values := ask.ID, map[string]json.RawMessage(nil)
		if p := lib.Processes[ask.ID]; p != nil && p.Problem != nil {
			return nil, &resolveError{"", 0, "invalid_record", p.Problem.Message}
		} else if p != nil {
			product, values = p.Product, p.Values
		} else if c.Prints[ask.ID] == nil {
			return nil, &resolveError{"", 0, "unknown_record", "Hmm, no print profile or print product " + ask.ID + "."}
		}
		preset := c.standardPrint(machine)
		if product != "" {
			preset = c.Resolve("prints", product, machine.Name)
		}
		if preset == nil && product == "" {
			return nil, &resolveError{"", 0, "no_default_print", errNoDefaultPrint.Error() + " for " + machine.Name + "."}
		}
		if preset == nil {
			return nil, &resolveError{"", 0, "unresolved", product + " isn't in the catalog."}
		}
		catalogLayers(preset)
		record(values, "record")
	case "filament":
		product, values := ask.ID, map[string]json.RawMessage(nil)
		f := lib.Filament(ask.ID)
		if f != nil && f.Problem != nil {
			return nil, &resolveError{"", 0, "invalid_record", f.Problem.Message}
		} else if f != nil {
			product, values = f.Product, f.Settings
		} else if c.Filaments[ask.ID] == nil {
			return nil, &resolveError{"", 0, "unknown_record", "Hmm, no filament or filament product " + ask.ID + "."}
		}
		preset := c.Resolve("filaments", product, machine.Name)
		if preset == nil {
			return nil, &resolveError{"", 0, "unresolved", product + " isn't in the catalog."}
		}
		catalogLayers(preset)
		record(values, "record")
		if f != nil && owner != nil {
			if cal := lib.Calibration(f.ID, owner, extruder); cal != nil && cal.Problem != nil {
				return nil, &resolveError{"", 0, "invalid_record", cal.Problem.Message}
			} else if cal != nil {
				record(cal.Values, "calibration")
			}
		}
	default:
		return nil, &resolveError{"", 0, "unresolved", "A profile is a machine, process or filament."}
	}
	return out, nil
}

func (c *Catalog) askSelection(lib *Library, printerID, printID string, selection Request) (*Library, Request) {
	view := &Library{Revision: lib.Revision, Printers: maps.Clone(lib.Printers), Filaments: lib.Filaments, Alias: lib.Alias, Processes: maps.Clone(lib.Processes), Calibrations: lib.Calibrations}
	project := selection.Project
	project.Process = nil
	req := Request{Printer: printerID, Print: printID, Filaments: selection.Filaments, Slots: selection.Slots, LayerHeight: selection.LayerHeight, Project: project, UsedSlots: selection.UsedSlots, partial: true}
	if view.Printers[printerID] == nil {
		machine := c.Machine(printerID)
		if machine == nil {
			return nil, req
		}
		p := &Printer{ID: "catalog:" + printerID, Name: printerID, Revision: new(uint64), Vendor: machine.Vendor, Model: machine.Product, Slots: []Slot{}}
		for range max(len(ConfigList(machine.Config["nozzle_diameter"])), 1) {
			p.Nozzles = append(p.Nozzles, Nozzle{Variant: machine.Name, Diameter: machine.Nozzle})
		}
		p.Swap, _ = c.Swap(machine.Name)
		view.Printers[p.ID], req.Printer = p, p.ID
		req.extruders = make([]int, len(req.Filaments))
	}
	if req.Printer != selection.Printer && req.extruders == nil {
		req.Slots, req.extruders = nil, make([]int, len(req.Filaments))
	}
	if view.Processes[printID] == nil {
		p := &Process{ID: "catalog:" + printID, Name: printID, Revision: new(uint64), Product: printID}
		view.Processes[p.ID], req.Print = p, p.ID
	}
	return view, req
}
