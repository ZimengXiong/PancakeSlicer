package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

var trayColour = regexp.MustCompile(`^#[0-9A-Fa-f]{6}$`)

type trayWanted struct {
	colour, kind, id string
}

func (w trayWanted) shownBy(tr AMSTray) bool {
	return strings.EqualFold(tr.Color, w.colour) && strings.EqualFold(tr.Type, w.kind) && (w.id == "" || tr.FilamentID == w.id)
}

func (w trayWanted) judge(slot, name string) func(bambuHeard, bool) (CommandState, string, []any) {
	return func(r bambuHeard, accepted bool) (CommandState, string, []any) {
		if !r.trays {
			return CommandPending, "", nil
		}
		for _, tr := range r.state {
			if slotID(tr) != slot {
				continue
			}
			if w.shownBy(tr) {
				return CommandConfirmed, "", nil
			}
			if accepted || r.full {
				return CommandRejected, "tray_contradicted", []any{name, cmpOr(tr.Type, "empty"), tr.Color, w.kind, w.colour}
			}
			break
		}
		return CommandPending, "", nil
	}
}

func (s *Server) setTray(w http.ResponseWriter, r *http.Request) {
	p := s.sends.printer(r.PathValue("printer"))
	if p == nil || p.Kind != "bambu-lan" {
		fail(w, 404, "printer_doesnt_have_bambu")
		return
	}
	var body struct {
		Slot     string `json:"slot"`
		Colour   string `json:"colour"`
		Filament string `json:"filament"`
	}
	if raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<10)); err != nil || json.Unmarshal(raw, &body) != nil || !trayColour.MatchString(body.Colour) || body.Slot == "" {
		fail(w, 400, "expected_slot_ams_unit")
		return
	}
	status, ok := s.sends.hub.current(p.Printer)
	if !ok || !status.Online {
		fail(w, 409, "isnt_reporting_cant_swap", p.Name)
		return
	}
	var tray *AMSTray
	for i := range status.Trays {
		if slotID(status.Trays[i]) == body.Slot {
			tray = &status.Trays[i]
		}
	}
	if tray == nil || tray.Kind == "external" {
		fail(w, 409, "only_ams_tray_reported")
		return
	}
	settings, err := s.trayFilament(p.Printer, body.Filament)
	if err != nil {
		fail(w, 422, "tray_filament_invalid", err.Error())
		return
	}
	secret, err := p.secret()
	if err != nil {
		fail(w, 409, "bambu_tray_conflict", err.Error())
		return
	}
	colour := strings.ToUpper(strings.TrimPrefix(body.Colour, "#"))
	want := trayWanted{colour: "#" + colour, kind: settings.kind, id: settings.id}
	command := map[string]any{"command": "ams_filament_setting", "ams_id": tray.AMS, "tray_id": tray.Slot, "slot_id": tray.Slot,
		"tray_info_idx": settings.id, "setting_id": "", "tray_color": colour + "FF", "tray_type": settings.kind,
		"nozzle_temp_min": settings.low, "nozzle_temp_max": settings.high}
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()
	audit(map[string]any{"event": "ams_tray_setting", "printer": p.Printer, "slot": body.Slot, "colour": body.Colour, "type": settings.kind, "client": s.clientIP(r)})
	result := s.runBambuCommand(ctx, p, secret, bambuCommand{scope: "print", body: command, codes: "tray", what: "the tray change",
		judge: want.judge(body.Slot, p.Name)})
	if st, ok := s.sends.hub.current(p.Printer); ok {
		result.Trays = st.Trays
	}
	writeJSON(w, result.status(), result)
}

type trayFilamentSettings struct {
	kind, id  string
	low, high int
}

func (s *Server) trayFilament(printerID, filamentID string) (trayFilamentSettings, error) {
	lib, err := s.libraryDocument()
	if err != nil {
		return trayFilamentSettings{}, err
	}
	printer, f := lib.Printers[printerID], lib.Filament(filamentID)
	if printer == nil || f == nil || !f.Live() {
		return trayFilamentSettings{}, fmt.Errorf("Pick a filament for this slot first so the printer's tray knows what it's holding!")
	}
	resolved, err := s.catalog.Profile(lib, printerID, resolve.Ask{Type: "filament", ID: f.ID})
	if err != nil {
		return trayFilamentSettings{}, fmt.Errorf("Oops, couldn't send %s to the printer: %w", f.Name, err)
	}
	value := func(key string) string { return strings.TrimSpace(resolve.FirstConfigText(resolved.Values[key])) }
	number := func(key, label string) (int, error) {
		text := value(key)
		n, err := strconv.ParseFloat(text, 64)
		if err != nil || math.IsNaN(n) || n <= 0 || n > 500 {
			return 0, fmt.Errorf("%s's %s is %q, which isn't a nozzle temperature the printer can take. Fix it in the filament's settings first.", f.Name, label, text)
		}
		return int(math.Round(n)), nil
	}
	id := value("filament_id")
	if id == "" {
		id = s.catalog.Resolve("filaments", f.Product, printer.Nozzles[0].Variant).FilamentID
	}
	out := trayFilamentSettings{kind: value("filament_type"), id: id}
	if out.kind == "" {
		return out, fmt.Errorf("%s doesn't have a filament type to send to the printer.", f.Name)
	}
	if out.low, err = number("nozzle_temperature_range_low", "lowest nozzle temperature"); err != nil {
		return out, err
	}
	if out.high, err = number("nozzle_temperature_range_high", "highest nozzle temperature"); err != nil {
		return out, err
	}
	if out.low > out.high {
		return out, fmt.Errorf("%s's nozzle temperature range runs from %d to %d °C, backwards. Fix it in the filament's settings first.", f.Name, out.low, out.high)
	}
	return out, nil
}
