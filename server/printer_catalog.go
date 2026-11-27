package main

import (
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type catalogPrinterEntry struct {
	ID         string            `json:"id"`
	Vendor     string            `json:"vendor"`
	VendorName string            `json:"vendorName"`
	Family     string            `json:"family,omitempty"`
	Cover      string            `json:"cover,omitempty"`
	Bed        string            `json:"bed,omitempty"`
	Extruders  int               `json:"extruders"`
	Nozzles    []string          `json:"nozzles"`
	Firmwares  []catalogFirmware `json:"firmwares,omitempty"`
}

type catalogFirmware struct {
	ID      string   `json:"id"`
	Nozzles []string `json:"nozzles"`
}

func variantFirmware(p *resolve.CatalogPrinter, v resolve.CatalogVariant) string {
	flavor := cmpOr(v.Values["gcode_flavor"], p.Base["gcode_flavor"])
	if strings.HasPrefix(flavor, "marlin") {
		return "marlin"
	}
	return flavor
}

func catalogFirmwares(p *resolve.CatalogPrinter, preferred string) []catalogFirmware {
	out := []catalogFirmware{}
	for _, v := range p.Variants {
		id := variantFirmware(p, v)
		i := slices.IndexFunc(out, func(f catalogFirmware) bool { return f.ID == id })
		if i < 0 {
			out, i = append(out, catalogFirmware{ID: id}), len(out)
		}
		if !slices.Contains(out[i].Nozzles, v.Nozzle) {
			out[i].Nozzles = append(out[i].Nozzles, v.Nozzle)
		}
	}
	if len(out) < 2 {
		return nil
	}
	for _, f := range out {
		slices.SortFunc(f.Nozzles, compareNozzles)
	}
	if i := slices.IndexFunc(out, func(f catalogFirmware) bool { return f.ID == preferred }); i > 0 {
		out = append([]catalogFirmware{out[i]}, slices.Delete(out, i, i+1)...)
	}
	return out
}

func compareNozzles(a, b string) int {
	x, _ := strconv.ParseFloat(a, 64)
	y, _ := strconv.ParseFloat(b, 64)
	return int(math.Copysign(1, x-y))
}

func (s *Server) catalogPrinters() []catalogPrinterEntry {
	root := filepath.Join(s.cfg.Resources, "profiles")
	out := []catalogPrinterEntry{}
	for _, id := range s.catalog.Order("printers") {
		p := s.catalog.Printers[id]
		if tech := stringValue(p.Model["machine_tech"]); tech != "" && tech != "FFF" {
			continue
		}
		e := catalogPrinterEntry{ID: p.ID, Vendor: p.Vendor, VendorName: cmpOr(p.VendorName, p.Vendor), Family: stringValue(p.Model["family"]), Nozzles: []string{}}
		defaults := map[string]string{}
		for _, v := range p.Variants {
			if v.Default {
				defaults[v.Nozzle] = v.ID
				e.Nozzles = append(e.Nozzles, v.Nozzle)
			}
		}
		slices.SortFunc(e.Nozzles, compareNozzles)
		if _, err := os.Stat(filepath.Join(root, p.Vendor, p.ID+"_cover.png")); err == nil {
			e.Cover = "/api/catalog/covers/" + url.PathEscape(p.Vendor) + "/" + url.PathEscape(p.ID)
		}
		variant := defaults["0.4"]
		if variant == "" && len(e.Nozzles) > 0 {
			variant = defaults[e.Nozzles[0]]
		}
		if machine := s.catalog.Machine(variant); machine != nil {
			v := s.catalog.VariantByID(variant)
			e.Bed, e.Extruders = bedSize(machine), v.Extruders
			e.Firmwares = catalogFirmwares(p, variantFirmware(p, *v))
		}
		out = append(out, e)
	}
	slices.SortStableFunc(out, func(a, b catalogPrinterEntry) int {
		if c := strings.Compare(strings.ToLower(a.VendorName), strings.ToLower(b.VendorName)); c != 0 {
			return c
		}
		return strings.Compare(a.ID, b.ID)
	})
	return out
}

func bedSize(machine *resolve.Preset) string {
	minX, minY, maxX, maxY := math.Inf(1), math.Inf(1), math.Inf(-1), math.Inf(-1)
	for _, point := range resolve.ConfigList(machine.Config["printable_area"]) {
		x, y, ok := strings.Cut(point, "x")
		fx, ex := strconv.ParseFloat(x, 64)
		fy, ey := strconv.ParseFloat(y, 64)
		if !ok || ex != nil || ey != nil {
			return ""
		}
		minX, maxX, minY, maxY = math.Min(minX, fx), math.Max(maxX, fx), math.Min(minY, fy), math.Max(maxY, fy)
	}
	height := resolve.FirstConfigText(machine.Config["printable_height"])
	if math.IsInf(minX, 0) || height == "" {
		return ""
	}
	return strconv.FormatFloat(maxX-minX, 'f', -1, 64) + "x" + strconv.FormatFloat(maxY-minY, 'f', -1, 64) + "x" + height
}

func (s *Server) catalogCover(w http.ResponseWriter, r *http.Request) {
	vendor, model := r.PathValue("vendor"), r.PathValue("model")
	if vendor == "" || model == "" || strings.ContainsAny(vendor+model, `/\`) || strings.Contains(vendor+model, "..") {
		fail(w, 404, "unknown_printer_cover")
		return
	}
	w.Header().Set("Cache-Control", "public, max-age=86400")
	http.ServeFile(w, r, filepath.Join(s.cfg.Resources, "profiles", vendor, model+"_cover.png"))
}
