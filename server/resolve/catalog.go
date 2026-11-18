package resolve

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
)

const catalogSchema = "pancake.catalog.v1"

type Source struct {
	Name     string   `json:"name"`
	Vendor   string   `json:"vendor"`
	Version  string   `json:"version"`
	File     string   `json:"file"`
	Inherits []string `json:"inherits"`
	FilamentID string `json:"filament_id,omitempty"`
	NativeSupportCapable bool `json:"native_support_capable,omitempty"`
}

type BaseFrom struct {
	Rule   string `json:"rule"`
	Vendor string `json:"vendor,omitempty"`
	Name   string `json:"name,omitempty"`
}

type Tuning struct {
	Fits   []string          `json:"fits"`
	Values map[string]string `json:"values"`
	Source Source            `json:"source"`
}

type Product struct {
	ID      string            `json:"id"`
	Brand   string            `json:"brand,omitempty"`
	Type    string            `json:"type,omitempty"`
	Aliases []string          `json:"aliases,omitempty"`
	Base    map[string]string `json:"base"`
	Tunings []Tuning          `json:"tunings"`
	BaseFrom BaseFrom `json:"base_from"`
	line     []byte
}

type CatalogVariant struct {
	ID      string `json:"id"`
	Nozzle  string `json:"nozzle"`
	Default bool   `json:"default"`
	DefaultPrint string `json:"defaultPrint,omitempty"`
	Extruders int               `json:"extruders"`
	Nozzles   []VariantNozzle   `json:"nozzles"`
	Values    map[string]string `json:"values"`
	Source    Source            `json:"source"`
}

type VariantNozzle struct {
	Diameter string `json:"diameter"`
}

type CatalogPrinter struct {
	ID         string                     `json:"id"`
	Vendor     string                     `json:"vendor"`
	VendorName string                     `json:"vendor_name"`
	Model      map[string]json.RawMessage `json:"model"`
	Base       map[string]string          `json:"base"`
	Variants   []CatalogVariant           `json:"variants"`
	line       []byte
}

type Catalog struct {
	Version   string
	Printers  map[string]*CatalogPrinter
	Filaments map[string]*Product
	Prints    map[string]*Product
	printSettings map[string]bool
	order    map[string][]string
	variants map[string]variantRef
	resolved sync.Map
}
type variantRef struct {
	printer *CatalogPrinter
	index   int
}

func LoadCatalog(printers, filaments, prints io.Reader) (*Catalog, error) {
	c := &Catalog{Printers: map[string]*CatalogPrinter{}, Filaments: map[string]*Product{}, Prints: map[string]*Product{},
		order: map[string][]string{}, variants: map[string]variantRef{}}
	versions := map[string]string{}
	var err error
	versions["printers"], err = readCatalog(printers, "printers", func(line []byte) error {
		p := &CatalogPrinter{line: slices.Clone(line)}
		if err := json.Unmarshal(line, p); err != nil {
			return err
		}
		c.Printers[p.ID] = p
		c.order["printers"] = append(c.order["printers"], p.ID)
		for i, v := range p.Variants {
			c.variants[v.ID] = variantRef{p, i}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	for _, kind := range []struct {
		name string
		r    io.Reader
		into map[string]*Product
	}{{"filaments", filaments, c.Filaments}, {"prints", prints, c.Prints}} {
		versions[kind.name], err = readCatalog(kind.r, kind.name, func(line []byte) error {
			p := &Product{line: slices.Clone(line)}
			if err := json.Unmarshal(line, p); err != nil {
				return err
			}
			kind.into[p.ID] = p
			c.order[kind.name] = append(c.order[kind.name], p.ID)
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	if versions["printers"] != versions["filaments"] || versions["printers"] != versions["prints"] {
		return nil, errors.New("the catalog files come from different builds")
	}
	c.Version = versions["printers"]
	c.printSettings = map[string]bool{}
	for _, p := range c.Prints {
		for key := range p.Base {
			c.printSettings[key] = true
		}
	}
	return c, nil
}

func readCatalog(in io.Reader, kind string, each func(line []byte) error) (string, error) {
	r := bufio.NewReaderSize(in, 1<<20)
	var header struct {
		Schema  string `json:"schema"`
		Version string `json:"version"`
	}
	first := true
	for {
		line, err := r.ReadBytes('\n')
		if line = bytes.TrimRight(line, "\n"); len(line) > 0 {
			if first {
				if json.Unmarshal(line, &header) != nil || header.Schema != catalogSchema {
					return "", fmt.Errorf("%s.jsonl is not a %s catalog", kind, catalogSchema)
				}
				first = false
			} else if e := each(line); e != nil {
				return "", fmt.Errorf("%s.jsonl: %w", kind, e)
			}
		}
		if err != nil {
			break
		}
	}
	if first {
		return "", fmt.Errorf("%s.jsonl is empty", kind)
	}
	return header.Version, nil
}

func catalogValue(key, text string) json.RawMessage {
	if configKinds[key] == "strings" {
		return must(json.Marshal(NativeStrings(text)))
	}
	return must(json.Marshal(text))
}

func layered(base, over map[string]string, overName string) (map[string]json.RawMessage, map[string]string) {
	out, layers := map[string]json.RawMessage{}, map[string]string{}
	for _, step := range []struct {
		name   string
		values map[string]string
	}{{"base", base}, {overName, over}} {
		for k, v := range step.values {
			if !HiddenSetting(k) {
				out[k], layers[k] = catalogValue(k, v), step.name
			}
		}
	}
	return out, layers
}

type Preset struct {
	Name    string
	Product string
	Vendor  string
	Config  map[string]json.RawMessage
	Layers     map[string]string
	FilamentID string
	LayerHeights []float64
	Nozzle float64
}

func (c *Catalog) Machine(variant string) *Preset {
	if p, ok := c.resolved.Load("machine\x00" + variant); ok {
		return p.(*Preset)
	}
	ref, ok := c.variants[variant]
	if !ok {
		return nil
	}
	v := ref.printer.Variants[ref.index]
	nozzle, _ := strconv.ParseFloat(v.Nozzle, 64)
	m := &Preset{Name: v.ID, Product: ref.printer.ID, Vendor: ref.printer.Vendor, Nozzle: nozzle}
	m.Config, m.Layers = layered(ref.printer.Base, v.Values, "tuning")
	seen := map[float64]bool{}
	for _, id := range c.order["prints"] {
		for _, t := range c.Prints[id].Tunings {
			if !slices.Contains(t.Fits, variant) {
				continue
			}
			text, ok := t.Values["layer_height"]
			if !ok {
				text = c.Prints[id].Base["layer_height"]
			}
			if h, err := strconv.ParseFloat(text, 64); err == nil && h > 0 && !seen[math.Round(h*1000)/1000] {
				seen[math.Round(h*1000)/1000] = true
				m.LayerHeights = append(m.LayerHeights, math.Round(h*1000)/1000)
			}
		}
	}
	slices.Sort(m.LayerHeights)
	p, _ := c.resolved.LoadOrStore("machine\x00"+variant, m)
	return p.(*Preset)
}

func (c *Catalog) VariantByID(id string) *CatalogVariant {
	ref, ok := c.variants[id]
	if !ok {
		return nil
	}
	return &ref.printer.Variants[ref.index]
}

func (c *Catalog) Variant(model string, nozzle float64) string {
	p := c.Printers[model]
	if p == nil {
		return ""
	}
	for _, v := range p.Variants {
		if n, err := strconv.ParseFloat(v.Nozzle, 64); err == nil && v.Default && SameMm(n, nozzle) {
			return v.ID
		}
	}
	return ""
}

func (c *Catalog) FoldedInto(id string) string {
	if c.Filaments[id] != nil {
		return ""
	}
	for _, p := range c.Filaments {
		if slices.Contains(p.Aliases, id) {
			return p.ID
		}
	}
	return ""
}

func (c *Catalog) product(kind, id string) *Product {
	if kind == "filaments" {
		return c.Filaments[id]
	}
	return c.Prints[id]
}

func (c *Catalog) Resolve(kind, id, variant string) *Preset {
	key := kind + "\x00" + id + "\x00" + variant
	if p, ok := c.resolved.Load(key); ok {
		return p.(*Preset)
	}
	product := c.product(kind, id)
	if product == nil {
		return nil
	}
	out := &Preset{Name: id, Product: id}
	var tuning map[string]string
	if t := product.tuning(variant); t != nil {
		tuning = t.Values
		out.Name, out.Vendor, out.FilamentID = t.Source.Name, t.Source.Vendor, t.Source.FilamentID
	}
	out.Config, out.Layers = layered(product.Base, tuning, "tuning")
	if kind == "filaments" && tuning != nil {
		projectVariants(out.Config, product.Base, tuning)
	}
	p, _ := c.resolved.LoadOrStore(key, out)
	return p.(*Preset)
}

func projectVariants(config map[string]json.RawMessage, base, tuning map[string]string) {
	named := NativeStrings(base["filament_extruder_variant"])
	variants := named
	if own, ok := tuning["filament_extruder_variant"]; ok {
		variants = NativeStrings(own)
	}
	if slices.Equal(named, variants) {
		return
	}
	for key := range filamentVariantKeys {
		text, inBase := base[key]
		if _, set := tuning[key]; key == "filament_extruder_variant" || set || !inBase || HiddenSetting(key) {
			continue
		}
		strs := configKinds[key] == "strings"
		entries := strings.Split(text, ",")
		if strs {
			entries = NativeStrings(text)
		}
		uniform := !slices.ContainsFunc(entries, func(e string) bool { return e != entries[0] })
		projected := make([]string, 0, len(variants))
		for _, v := range variants {
			if i := slices.Index(named, v); i >= 0 && i < len(entries) {
				projected = append(projected, entries[i])
			} else if uniform && len(entries) > 0 {
				projected = append(projected, entries[0])
			} else {
				projected = nil
				break
			}
		}
		if projected == nil {
			continue
		}
		if strs {
			config[key], _ = json.Marshal(projected)
		} else {
			config[key], _ = json.Marshal(strings.Join(projected, ","))
		}
	}
}

func (p *Product) tuning(variant string) *Tuning {
	for i := range p.Tunings {
		if slices.Contains(p.Tunings[i].Fits, variant) {
			return &p.Tunings[i]
		}
	}
	return nil
}

var standardName = regexp.MustCompile(`^\d+(\.\d+)?mm Standard\b`)

func (c *Catalog) standardPrint(machine *Preset) *Preset {
	nozzle, _ := strconv.ParseFloat(FirstConfigText(machine.Config["nozzle_diameter"]), 64)
	pick := func(named bool) *Preset {
		var best string
		gap := math.Inf(1)
		for _, id := range c.order["prints"] {
			p := c.Prints[id]
			if named && !standardName.MatchString(id) || !slices.ContainsFunc(p.Tunings, func(t Tuning) bool { return slices.Contains(t.Fits, machine.Name) }) {
				continue
			}
			resolved := c.Resolve("prints", id, machine.Name)
			height, _ := strconv.ParseFloat(FirstConfigText(resolved.Config["layer_height"]), 64)
			if g := math.Abs(height - nozzle/2); g < gap || g == gap && id < best {
				best, gap = id, g
			}
		}
		if best == "" {
			return nil
		}
		return c.Resolve("prints", best, machine.Name)
	}
	if p := pick(true); p != nil {
		return p
	}
	return pick(false)
}

type CatalogEntry struct {
	ID     string `json:"id"`
	Brand  string `json:"brand,omitempty"`
	Type   string `json:"type,omitempty"`
	Vendor string `json:"vendor,omitempty"`
}

func (c *Catalog) List(kind string) []CatalogEntry {
	out := make([]CatalogEntry, 0, len(c.order[kind]))
	for _, id := range c.order[kind] {
		if kind == "printers" {
			out = append(out, CatalogEntry{ID: id, Vendor: c.Printers[id].Vendor})
		} else {
			p := c.product(kind, id)
			out = append(out, CatalogEntry{ID: id, Brand: p.Brand, Type: p.Type})
		}
	}
	return out
}

func (c *Catalog) Order(kind string) []string { return c.order[kind] }

func (c *Catalog) Line(kind, id string) []byte {
	switch {
	case kind == "printers" && c.Printers[id] != nil:
		return c.Printers[id].line
	case (kind == "filaments" || kind == "prints") && c.product(kind, id) != nil:
		return c.product(kind, id).line
	}
	return nil
}
