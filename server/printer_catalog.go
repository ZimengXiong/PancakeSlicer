package main

import (
	"encoding/json"
	"math"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
)

type catalogModel struct {
	ID       string            `json:"id"`
	Name     string            `json:"name"`
	Family   string            `json:"family,omitempty"`
	Cover    string            `json:"cover,omitempty"`
	Nozzles  []string          `json:"nozzles"`
	Bed      string            `json:"bed,omitempty"`
	Machines map[string]string `json:"machines"`
}
type catalogVendor struct {
	ID     string         `json:"id"`
	Name   string         `json:"name"`
	Models []catalogModel `json:"models"`
}

var printerCatalog struct {
	once     sync.Once
	response *cachedResponse
	err      error
}

func printerCatalogResponse(root string) (*cachedResponse, error) {
	printerCatalog.once.Do(func() {
		vendors, err := buildPrinterCatalog(root)
		if err == nil {
			printerCatalog.response, err = cacheJSON(map[string]any{"vendors": vendors})
		}
		printerCatalog.err = err
	})
	return printerCatalog.response, printerCatalog.err
}

func buildPrinterCatalog(root string) ([]catalogVendor, error) {
	files, err := filepath.Glob(filepath.Join(root, "*.json"))
	if err != nil {
		return nil, err
	}
	vendors := []catalogVendor{}
	for _, file := range files {
		var index struct {
			Name   string `json:"name"`
			Models []struct {
				Name    string `json:"name"`
				SubPath string `json:"sub_path"`
			} `json:"machine_model_list"`
		}
		raw, err := os.ReadFile(file)
		if err != nil || json.Unmarshal(raw, &index) != nil || len(index.Models) == 0 {
			continue
		}
		id := strings.TrimSuffix(filepath.Base(file), ".json")
		dir := filepath.Join(root, id)
		machines, _ := readKind(filepath.Join(dir, "machine"))
		vendor := catalogVendor{ID: id, Name: index.Name, Models: []catalogModel{}}
		if vendor.Name == "" {
			vendor.Name = id
		}
		for _, entry := range index.Models {
			var model struct {
				Name    string `json:"name"`
				Family  string `json:"family"`
				Nozzles string `json:"nozzle_diameter"`
				Tech    string `json:"machine_tech"`
			}
			raw, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(entry.SubPath)))
			if err != nil || json.Unmarshal(raw, &model) != nil || model.Tech != "" && model.Tech != "FFF" {
				continue
			}
			if model.Name == "" {
				model.Name = entry.Name
			}
			m := catalogModel{ID: model.Name, Name: model.Name, Family: model.Family, Nozzles: []string{}}
			for _, n := range strings.Split(model.Nozzles, ";") {
				if n = strings.TrimSpace(n); n != "" {
					m.Nozzles = append(m.Nozzles, nozzleKey(n))
				}
			}
			slices.SortFunc(m.Nozzles, func(a, b string) int {
				if x, y := leadingNumber(a), leadingNumber(b); x != y {
					return int(math.Copysign(1, x-y))
				}
				return strings.Compare(a, b)
			})
			if _, err := os.Stat(filepath.Join(dir, model.Name+"_cover.png")); err == nil {
				m.Cover = "/api/printer-catalog/covers/" + url.PathEscape(id) + "/" + url.PathEscape(model.Name)
			}
			m.Bed = bedOf(machines, model.Name)
			m.Machines = machinesByNozzle(machines, model.Name)
			vendor.Models = append(vendor.Models, m)
		}
		if len(vendor.Models) > 0 {
			slices.SortFunc(vendor.Models, func(a, b catalogModel) int { return strings.Compare(a.Name, b.Name) })
			vendors = append(vendors, vendor)
		}
	}
	slices.SortFunc(vendors, func(a, b catalogVendor) int { return strings.Compare(strings.ToLower(a.Name), strings.ToLower(b.Name)) })
	return vendors, nil
}

func pathEscape(s string) string {
	return strings.ReplaceAll(strings.ReplaceAll(s, "%", "%25"), " ", "%20")
}

func bedOf(machines map[string]rawPreset, model string) string {
	var name string
	for n, p := range machines {
		if stringValue(p["printer_model"]) != model || stringValue(p["instantiation"]) == "false" {
			continue
		}
		if name == "" || strings.Contains(n, "0.4") != strings.Contains(name, "0.4") && strings.Contains(n, "0.4") ||
			strings.Contains(n, "0.4") == strings.Contains(name, "0.4") && n < name {
			name = n
		}
	}
	if name == "" {
		return ""
	}
	config := resolvedConfig(machines, name)
	minX, minY, maxX, maxY := math.Inf(1), math.Inf(1), math.Inf(-1), math.Inf(-1)
	for _, point := range stringList(config["printable_area"]) {
		x, y, ok := strings.Cut(point, "x")
		fx, ex := strconv.ParseFloat(x, 64)
		fy, ey := strconv.ParseFloat(y, 64)
		if !ok || ex != nil || ey != nil {
			return ""
		}
		minX, maxX, minY, maxY = math.Min(minX, fx), math.Max(maxX, fx), math.Min(minY, fy), math.Max(maxY, fy)
	}
	height := firstConfigText(config["printable_height"])
	if math.IsInf(minX, 0) || height == "" {
		return ""
	}
	return strconv.FormatFloat(maxX-minX, 'f', -1, 64) + "x" + strconv.FormatFloat(maxY-minY, 'f', -1, 64) + "x" + height
}

func (s *Server) printerCatalogRoutes(mux *http.ServeMux) {
	root := filepath.Join(s.cfg.Resources, "profiles")
	mux.HandleFunc("GET /api/printer-catalog", func(w http.ResponseWriter, r *http.Request) {
		response, err := printerCatalogResponse(root)
		if err != nil {
			fail(w, 500, err.Error())
			return
		}
		response.serve(w, r, "printer-catalog.json", "application/json", catalogCache, time.Time{})
	})
	mux.HandleFunc("GET /api/printer-catalog/covers/{vendor}/{model}", func(w http.ResponseWriter, r *http.Request) {
		vendor, model := r.PathValue("vendor"), r.PathValue("model")
		if vendor == "" || model == "" || strings.ContainsAny(vendor+model, `/\`) || strings.Contains(vendor+model, "..") {
			fail(w, 404, "unknown printer cover")
			return
		}
		w.Header().Set("Cache-Control", "public, max-age=86400")
		http.ServeFile(w, r, filepath.Join(root, vendor, model+"_cover.png"))
	})
}

func machinesByNozzle(machines map[string]rawPreset, model string) map[string]string {
	out := map[string]string{}
	for name, p := range machines {
		if stringValue(p["instantiation"]) == "false" {
			continue
		}
		config := resolvedConfig(machines, name)
		if firstConfigText(config["printer_model"]) != model {
			continue
		}
		key := firstConfigText(config["printer_variant"])
		if key == "" {
			key = firstConfigText(config["nozzle_diameter"])
		}
		if key = nozzleKey(key); key == "" {
			continue
		}
		if cur, ok := out[key]; !ok || name < cur {
			out[key] = name
		}
	}
	return out
}

var leadingNumberPattern = regexp.MustCompile(`^\d*\.?\d+`)

func leadingNumber(s string) float64 {
	n, _ := strconv.ParseFloat(leadingNumberPattern.FindString(s), 64)
	return n
}

func nozzleKey(variant string) string {
	variant = strings.TrimSpace(variant)
	if n, err := strconv.ParseFloat(variant, 64); err == nil {
		return strconv.FormatFloat(n, 'f', -1, 64)
	}
	return variant
}
