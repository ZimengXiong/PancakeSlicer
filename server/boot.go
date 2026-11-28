package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/url"
	"strings"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type bootShape struct {
	Network   bool        `json:"network"`
	Project   string      `json:"project,omitempty"`
	Recovered string      `json:"recovered,omitempty"`
	Printer   string      `json:"printer"`
	Connected bool        `json:"connected"`
	Slots     []bootSlot  `json:"slots"`
	RailWidth json.Number `json:"railWidth,omitempty"`
	Notice    *bootNotice `json:"notice,omitempty"`
	Picker *bootPicker `json:"picker,omitempty"`
	Projects int `json:"projects"`
	Folders []string `json:"folders,omitempty"`
	Share bool `json:"share,omitempty"`
}

type bootPicker struct {
	Vendor  string  `json:"vendor"`
	Model   string  `json:"model"`
	Variant string  `json:"variant"`
	Nozzle  float64 `json:"nozzle,omitempty"`
	Nozzles []float64 `json:"nozzles,omitempty"`
}

type bootNotice struct {
	Project string            `json:"project"`
	Offer   resolve.SlotOffer `json:"offer"`
}

func pageProject(path string) string {
	rest, ok := strings.CutPrefix(path, "/p/")
	if !ok {
		return ""
	}
	id, err := url.PathUnescape(strings.TrimSuffix(rest, "/"))
	if err != nil || id == "" || len(id) > 160 || strings.Contains(id, "/") {
		return ""
	}
	return id
}

func slotChoiceMade(d *workspaceData, project string) bool {
	v, ok := d.Values["session"]
	if !ok {
		return false
	}
	var session struct {
		Notices map[string]struct {
			Dismissed bool `json:"dismissed"`
		} `json:"projectSlotNotices"`
	}
	return json.Unmarshal(v.Document, &session) == nil && session.Notices[project].Dismissed
}

type bootSlot struct {
	Group  bool      `json:"group,omitempty"`
	Report bool      `json:"report,omitempty"`
	Note   *bootNote `json:"note,omitempty"`
}
type bootNote struct {
	Tray string `json:"tray"`
	Name string `json:"name"`
	Type string `json:"type"`
}

func bootText(s string) string {
	if r := []rune(s); len(r) > 80 {
		return string(r[:80])
	}
	return s
}

const bootSlotLimit = 64

func (s *Server) bootShape(project string) *bootShape {
	st := s.workspace
	st.mu.Lock()
	d, err := st.read()
	st.mu.Unlock()
	if err != nil {
		return nil
	}
	var prefs struct {
		Printer   string      `json:"orca.printer"`
		RailWidth json.Number `json:"orca.web.railWidth"`
	}
	out := &bootShape{Network: s.sends.store != nil, Slots: []bootSlot{}, Share: s.cfg.ShareDir != ""}
	for _, p := range d.Projects {
		if !p.Archived && p.Record != nil {
			out.Projects++
		}
	}
	for _, f := range d.Folders {
		if f.ParentID == "" && len(out.Folders) < 48 {
			out.Folders = append(out.Folders, bootText(f.Name))
		}
	}
	if v, ok := d.Values["preferences"]; ok && json.Unmarshal(v.Document, &prefs) == nil {
		if _, err := prefs.RailWidth.Float64(); err == nil {
			out.RailWidth = prefs.RailWidth
		}
	}
	lib, err := s.libraryDocument()
	if err != nil {
		return out
	}
	printer := prefs.Printer
	if doc, recovered := openedDocument(d, project); doc != nil {
		printer, out.Project, out.Recovered = documentPrinter(doc, prefs.Printer), project, recovered
		if !slotChoiceMade(d, project) {
			if o := s.projectOffer(lib, doc, printer); o != nil {
				out.Notice = &bootNotice{Project: project, Offer: *o}
			}
		}
	}
	p := lib.Printers[printer]
	if printer == "" || p == nil || !p.Live() || len(p.Slots) > bootSlotLimit {
		return out
	}
	out.Printer, out.Slots = p.ID, make([]bootSlot, len(p.Slots))
	if len(p.Nozzles) > 0 {
		out.Picker = &bootPicker{Vendor: p.Vendor, Model: p.Model, Variant: p.Nozzles[0].Variant, Nozzle: p.Nozzles[0].Diameter}
		if len(p.Nozzles) > 1 {
			for _, n := range p.Nozzles {
				out.Picker.Nozzles = append(out.Picker.Nozzles, n.Diameter)
			}
		}
	}
	if len(p.Nozzles) > 1 && p.Swap != "toolheads" {
		last := -1
		for i := range p.Slots {
			if e, ok := p.SlotExtruder(i); ok && e != last {
				out.Slots[i].Group, last = true, e
			}
		}
	}
	if c := s.sends.printer(p.ID); c != nil && c.Kind == "bambu-lan" {
		out.Connected = true
		reports, err := s.printerSlotsFor(p.ID, lib)
		if err != nil {
			return out
		}
		for i, slot := range p.Slots {
			for _, r := range reports.Slots {
				if r.Slot != slot.Tray {
					continue
				}
				out.Slots[i].Report = true
				if f := lib.Filament(slot.Filament); slot.Filament != "" && f != nil && r.Tray.Type != "" {
					if t := s.filamentType(lib, p.ID, slot.Filament); materialDiffers(r.Tray.Type, t) {
						out.Slots[i].Note = &bootNote{Tray: bootText(r.Tray.Type), Name: bootText(f.Name), Type: bootText(t)}
					}
				}
			}
		}
	}
	return out
}

func (s *Server) filamentType(lib *resolve.Library, printer, filament string) string {
	v, err := s.library.catalog.Profile(lib, printer, resolve.Ask{Type: "filament", ID: filament})
	if err != nil {
		return ""
	}
	raw := v.Values["filament_type"]
	var list []string
	if json.Unmarshal(raw, &list) == nil && len(list) > 0 {
		return list[0]
	}
	var one string
	json.Unmarshal(raw, &one)
	return one
}

func materialDiffers(tray, filament string) bool {
	head := func(t string) string {
		f := strings.FieldsFunc(strings.ToUpper(strings.TrimSpace(t)), func(r rune) bool { return r == '-' || r == ' ' || r == '\t' || r == '\n' })
		if len(f) == 0 {
			return ""
		}
		return f[0]
	}
	return filament == "" || head(tray) != head(filament)
}

func (s *Server) bootScript(r *http.Request) []byte {
	shape := s.bootShape(pageProject(r.URL.Path))
	if shape == nil {
		return nil
	}
	b, err := json.Marshal(shape)
	if err != nil {
		return nil
	}
	return append(append([]byte(`<script id="pancake-boot" type="application/json">`), b...), "</script>"...)
}

func withBoot(page, script []byte) []byte {
	return bytes.Replace(page, []byte("</head>"), append(script, "</head>"...), 1)
}

func (s *Server) getBoot(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, s.bootShape(pageProject(r.URL.Query().Get("page"))))
}
