package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"path"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

const (
	orcaPresetVersion = "02.08.01.55"
	recordField       = "pancake_record"
	digestField       = "pancake_values"
	importMaxFile     = 64 << 20
	importMaxEntry    = 2 << 20
	importMaxEntries  = 2000
)

var orcaMeta = regexp.MustCompile(`^(inherits|name|from|version|type|setting_id|base_id|filament_id|is_custom_defined|instantiation|\w+_settings_id|compatible_printers(_condition)?|compatible_prints(_condition)?|renamed_from|print_host|printhost_\w+|host_type)$`)

type parsedPreset struct {
	File, Name, Type, Inherits, From string
	Values                           map[string]json.RawMessage
	Meta                             map[string]json.RawMessage
	Unreadable                       []string
	Record                           json.RawMessage
	Edited                           bool
	EditedID                         string
	Physical                         *struct {
		Printers []string          `json:"printers"`
		Config   map[string]string `json:"config"`
	}
	Error string
}

type importRow struct {
	importRecord
	File      string        `json:"file"`
	Name      string        `json:"name"`
	Type      string        `json:"type,omitempty"`
	Status    string        `json:"status"`
	Basis     string        `json:"basis,omitempty"`
	NotStored []string      `json:"notStored"`
	Note      string        `json:"note,omitempty"`
	Code      string        `json:"code,omitempty"`
	Physical  any           `json:"physical,omitempty"`
	Matches   []importMatch `json:"matches,omitempty"`
}

type importRecord struct {
	Kind            string          `json:"kind,omitempty"`
	Record          json.RawMessage `json:"record,omitempty"`
	SourceIDs       []string        `json:"sourceIds,omitempty"`
	ExportedPrinter bool            `json:"exportedPrinter,omitempty"`
}

type importMatch struct {
	ID       string          `json:"id"`
	Name     string          `json:"name"`
	Revision uint64          `json:"revision"`
	Record   json.RawMessage `json:"record"`
	Edited   string          `json:"edited,omitempty"`
	Differ   int             `json:"differ"`
}

func presetType(raw map[string]json.RawMessage) string {
	for key, t := range map[string]string{"printer_settings_id": "machine", "print_settings_id": "process", "filament_settings_id": "filament"} {
		if _, ok := raw[key]; ok {
			return t
		}
	}
	if t := stringValue(raw["type"]); t == "machine" || t == "process" || t == "filament" {
		return t
	}
	return ""
}

func firstText(raw json.RawMessage) string {
	var list []any
	if json.Unmarshal(raw, &list) == nil {
		if len(list) == 0 {
			return ""
		}
		return fmt.Sprint(list[0])
	}
	var v any
	if json.Unmarshal(raw, &v) != nil || v == nil {
		return ""
	}
	return fmt.Sprint(v)
}

func listText(raw json.RawMessage) []string {
	if raw == nil {
		return nil
	}
	var list []any
	if json.Unmarshal(raw, &list) == nil {
		out := make([]string, len(list))
		for i, v := range list {
			out[i] = strings.TrimSpace(fmt.Sprint(v))
		}
		return out
	}
	text := firstText(raw)
	if text == "" {
		return nil
	}
	parts := strings.Split(text, ",")
	for i := range parts {
		parts[i] = strings.TrimSpace(parts[i])
	}
	return parts
}

func settingValue(raw json.RawMessage) bool {
	var v any
	if json.Unmarshal(raw, &v) != nil {
		return false
	}
	switch t := v.(type) {
	case string, float64, bool:
		return true
	case []any:
		strs, nums := true, true
		for _, x := range t {
			_, s := x.(string)
			_, n := x.(float64)
			strs, nums = strs && s, nums && n
		}
		return strs || nums
	}
	return false
}

func parsePreset(data []byte, file string) parsedPreset {
	p := parsedPreset{File: file, Name: strings.TrimSuffix(path.Base(file), ".json"), Values: map[string]json.RawMessage{}, Meta: map[string]json.RawMessage{}}
	var raw map[string]json.RawMessage
	if json.Unmarshal(data, &raw) != nil || raw == nil {
		p.Error = "not a valid profile JSON object"
		return p
	}
	if n := firstText(raw["name"]); n != "" {
		p.Name = n
	}
	p.Inherits, p.From, p.Type = firstText(raw["inherits"]), firstText(raw["from"]), presetType(raw)
	if p.Type == "" {
		if raw["print_host"] != nil && (raw["preset_names"] != nil || raw["preset_name"] != nil) {
			p.Physical = &struct {
				Printers []string          `json:"printers"`
				Config   map[string]string `json:"config"`
			}{Config: map[string]string{}}
			names := raw["preset_names"]
			if names == nil {
				names = raw["preset_name"]
			}
			for _, n := range listText(names) {
				for _, part := range strings.Split(n, ";") {
					if part = strings.TrimSpace(part); part != "" {
						p.Physical.Printers = append(p.Physical.Printers, part)
					}
				}
			}
			for k, v := range raw {
				if regexp.MustCompile(`^(host_type|print_host|printhost_)`).MatchString(k) {
					p.Physical.Config[k] = firstText(v)
				}
			}
			return p
		}
		p.Error = "unrecognized profile type (missing printer, print, or filament ID)"
		return p
	}
	digest := ""
	for k, v := range raw {
		switch {
		case k == digestField:
			json.Unmarshal(v, &digest)
		case k == recordField:
			var text string
			if json.Unmarshal(v, &text) != nil || !json.Valid([]byte(text)) {
				p.Error = "the record this app exported with it is corrupted; re-export it"
				return p
			}
			p.Record = json.RawMessage(text)
		case !settingValue(v):
			p.Unreadable = append(p.Unreadable, k)
		case orcaMeta.MatchString(k):
			p.Meta[k] = v
		default:
			p.Values[k] = v
		}
	}
	if p.Record != nil && (digest != "" && digest != valuesDigest(p.Values) || digest == "" && storedDiffer(p.Record, p.Values)) {
		var probe struct {
			ID string `json:"id"`
		}
		json.Unmarshal(p.Record, &probe)
		p.Record, p.Edited, p.EditedID = nil, true, probe.ID
	}
	return p
}

func storedDiffer(record json.RawMessage, values map[string]json.RawMessage) bool {
	var r struct {
		Settings, Values, Machine map[string]json.RawMessage
	}
	json.Unmarshal(record, &r)
	for _, own := range []map[string]json.RawMessage{r.Settings, r.Values, r.Machine} {
		for k, v := range own {
			if file, ok := values[k]; ok && valueText(file) != valueText(v) {
				return true
			}
		}
	}
	return false
}

func withName(record json.RawMessage, name string) json.RawMessage {
	var r map[string]json.RawMessage
	if json.Unmarshal(record, &r) != nil {
		return record
	}
	r["name"] = must(json.Marshal(name))
	return must(json.Marshal(r))
}

func valuesDigest(values map[string]json.RawMessage) string {
	keys := make([]string, 0, len(values))
	for k := range values {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	h := sha256.New()
	for _, k := range keys {
		var v any
		json.Unmarshal(values[k], &v)
		fmt.Fprintf(h, "%s=%s\n", k, must(json.Marshal(v)))
	}
	return hex.EncodeToString(h.Sum(nil))
}

func readPresetFiles(name string, data []byte) []parsedPreset {
	failed := func(why string) []parsedPreset {
		return []parsedPreset{{File: name, Name: path.Base(name), Error: why}}
	}
	lower := strings.ToLower(name)
	if strings.HasSuffix(lower, ".json") {
		return []parsedPreset{parsePreset(data, name)}
	}
	if !regexp.MustCompile(`\.(zip|bbscfg|orca_printer|orca_filament|orca_bundle)$`).MatchString(lower) {
		return failed("not a valid profile file")
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return failed("couldn't open archive (" + err.Error() + ")")
	}
	var out []parsedPreset
	count, total := 0, uint64(0)
	for _, f := range zr.File {
		base := path.Base(f.Name)
		if !strings.HasSuffix(strings.ToLower(base), ".json") || strings.EqualFold(base, "bundle_structure.json") || strings.Contains(f.Name, "__MACOSX/") {
			continue
		}
		count++
		total += f.UncompressedSize64
		if count > importMaxEntries || f.UncompressedSize64 > importMaxEntry || total > importMaxFile {
			out = append(out, parsedPreset{File: name + "/" + f.Name, Name: base, Error: "this archive has too many files or is too large, so the remaining files were skipped"})
			break
		}
		rc, err := f.Open()
		if err != nil {
			out = append(out, parsedPreset{File: name + "/" + f.Name, Name: base, Error: err.Error()})
			continue
		}
		body, err := io.ReadAll(io.LimitReader(rc, importMaxEntry+1))
		rc.Close()
		if err != nil {
			out = append(out, parsedPreset{File: name + "/" + f.Name, Name: base, Error: err.Error()})
			continue
		}
		out = append(out, parsePreset(body, name+"/"+f.Name))
	}
	return out
}

func productID(name string) string {
	if i := strings.Index(name, " @"); i >= 0 {
		name = name[:i]
	}
	return strings.TrimSpace(name)
}

func variantOf(c *resolve.Catalog, id string) (*resolve.CatalogPrinter, *resolve.CatalogVariant) {
	for _, p := range c.Printers {
		for i := range p.Variants {
			if p.Variants[i].ID == id {
				return p, &p.Variants[i]
			}
		}
	}
	return nil, nil
}

func productOf(c *resolve.Catalog, kind, id string) *resolve.Product {
	if kind == "filaments" {
		return c.Filaments[id]
	}
	return c.Prints[id]
}

func tuningOf(p *resolve.Product, variant string) *resolve.Tuning {
	for i := range p.Tunings {
		if slices.Contains(p.Tunings[i].Fits, variant) {
			return &p.Tunings[i]
		}
	}
	return nil
}

func orcaParent(p *resolve.Product, variant string) string {
	if t := tuningOf(p, variant); t != nil {
		return t.Source.Name
	}
	if p.BaseFrom.Name != "" && productID(p.BaseFrom.Name) == p.ID {
		return p.BaseFrom.Name
	}
	for _, t := range p.Tunings {
		if productID(t.Source.Name) == p.ID {
			return t.Source.Name
		}
	}
	return ""
}

func defaultPrintOn(c *resolve.Catalog, variant string) *resolve.Preset {
	if _, v := variantOf(c, variant); v != nil && v.DefaultPrint != "" {
		return c.Resolve("prints", v.DefaultPrint, variant)
	}
	return nil
}

func valueText(raw json.RawMessage) string {
	var v any
	json.Unmarshal(raw, &v)
	if list, ok := v.([]any); ok {
		parts := make([]string, len(list))
		for i, x := range list {
			parts[i] = orcaScalar(x)
		}
		return strings.Join(parts, ",")
	}
	return orcaScalar(v)
}

func uniqueID() string { return newID("profile") }

func (s *Server) planImport(presets []parsedPreset, lib *resolve.Library) []importRow {
	c := s.catalog
	byName := map[string]*parsedPreset{}
	hasExportedPrinter := false
	for i := range presets {
		p := &presets[i]
		if p.Type != "" && p.Error == "" {
			hasExportedPrinter = hasExportedPrinter || p.Type == "machine" && p.Record != nil
			if _, seen := byName[p.Type+"\x00"+p.Name]; !seen {
				byName[p.Type+"\x00"+p.Name] = p
			}
		}
	}
	var flatten func(p *parsedPreset, seen map[*parsedPreset]bool) (map[string]json.RawMessage, string, error)
	flatten = func(p *parsedPreset, seen map[*parsedPreset]bool) (map[string]json.RawMessage, string, error) {
		if seen[p] {
			return nil, "", fmt.Errorf("its parents form a cycle")
		}
		seen[p] = true
		parent := byName[p.Type+"\x00"+p.Inherits]
		if p.Inherits == "" || parent == nil || parent == p {
			return p.Values, p.Inherits, nil
		}
		up, root, err := flatten(parent, seen)
		if err != nil {
			return nil, "", err
		}
		out := map[string]json.RawMessage{}
		for k, v := range up {
			out[k] = v
		}
		for k, v := range p.Values {
			out[k] = v
		}
		return out, root, nil
	}
	existing := func(kind, name string) []string {
		var ids []string
		switch kind {
		case "printer":
			for id, r := range lib.Printers {
				if r.Name == name && r.Live() {
					ids = append(ids, id)
				}
			}
		case "filament":
			for id, r := range lib.Filaments {
				if r.Name == name && r.Live() {
					ids = append(ids, id)
				}
			}
		case "process":
			for id, r := range lib.Processes {
				if r.Name == name && r.Live() {
					ids = append(ids, id)
				}
			}
		}
		sort.Strings(ids)
		return ids
	}
	live := func(kind, id string) bool {
		switch kind {
		case "printer":
			return lib.Printers[id] != nil && lib.Printers[id].Live()
		case "filament":
			return lib.Filaments[id] != nil && lib.Filaments[id].Live()
		case "process":
			return lib.Processes[id] != nil && lib.Processes[id].Live()
		}
		return false
	}
	recordOf := func(kind, id string) (string, uint64, *resolve.Printer) {
		switch kind {
		case "printer":
			if r := lib.Printers[id]; r != nil {
				return id, *r.Revision, r
			}
		case "filament":
			if r := lib.Filaments[id]; r != nil {
				return id, *r.Revision, nil
			}
		case "process":
			if r := lib.Processes[id]; r != nil {
				return id, *r.Revision, nil
			}
		}
		return "", 0, nil
	}
	stored := func(kind, id string) json.RawMessage {
		var record any
		switch kind {
		case "printer":
			if p := lib.Printers[id]; p != nil {
				record = p
			}
		case "filament":
			if f := lib.Filaments[id]; f != nil {
				record = f
			}
		case "process":
			if p := lib.Processes[id]; p != nil {
				record = p
			}
		}
		if record == nil {
			return nil
		}
		return must(json.Marshal(record))
	}
	kinds := map[string]string{"machine": "printer", "filament": "filament", "process": "process"}
	rows := []importRow{}
	taken := map[string]*parsedPreset{}
	claimed := map[string]*parsedPreset{}
	claim := func(id string, p *parsedPreset) bool {
		if !strings.HasPrefix(id, "profile:") || id == "profile:" || lib.Printers[id] != nil || lib.Filaments[id] != nil || lib.Processes[id] != nil || lib.Alias[id] != "" {
			return false
		}
		if owner := claimed[id]; owner != nil && owner != p {
			return false
		}
		claimed[id] = p
		return true
	}
	kindLabel := map[string]string{"printer": "printer", "filament": "filament", "process": "print profile"}
	freeName := func(kind, wanted string) string {
		name := wanted
		for n := 2; len(existing(kind, name)) > 0 || taken[kind+"\x00"+name] != nil; n++ {
			name = fmt.Sprintf("%s %d", wanted, n)
		}
		return name
	}
	for i := range presets {
		p := &presets[i]
		row := importRow{File: p.File, Name: p.Name, Type: p.Type, NotStored: []string{}}
		skip := func(code, note string) { row.Status, row.Code, row.Note = "skip", code, note; rows = append(rows, row) }
		if p.Physical != nil {
			row.Status, row.Physical = "new", p.Physical
			row.Note = "Connection for " + cmpOr(strings.Join(p.Physical.Printers, ", "), "no printer")
			rows = append(rows, row)
			continue
		}
		if p.Error != "" {
			skip("import_invalid", p.Error)
			continue
		}
		kind := kinds[p.Type]
		row.Kind = kind
		name := cmpOr(productID(p.Name), p.Name)
		if first, seen := taken[kind+"\x00"+name]; seen {
			if first.File != p.File {
				skip("import_repeat", fmt.Sprintf("%s also has a %s named %s; only the first is imported", path.Base(first.File), kind, name))
			} else {
				skip("import_repeat", "the same "+kind+" as "+first.Name)
			}
			continue
		}
		taken[kind+"\x00"+name] = p
		var source struct {
			ID      string   `json:"id"`
			Aliases []string `json:"aliases"`
		}
		json.Unmarshal(p.Record, &source)
		exported := cmpOr(source.ID, p.EditedID)
		if kind == "filament" || kind == "process" {
			if strings.HasPrefix(exported, "profile:") {
				row.SourceIDs = append(row.SourceIDs, exported)
			}
			if kind == "filament" {
				row.SourceIDs = append(row.SourceIDs, source.Aliases...)
			}
		}
		row.ExportedPrinter = kind == "printer" && p.Record != nil
		decode := func(target string, row importRow) importRow {
			row.NotStored = append([]string{}, row.NotStored...)
			skipped := func(note string) importRow {
				row.Status, row.Code, row.Note = "skip", "import_invalid", note
				return row
			}
			id, revision, current := "", uint64(0), (*resolve.Printer)(nil)
			if target != "" {
				id, revision, current = recordOf(kind, target)
			}
			row.Status = "new"
			if id != "" {
				row.Status = "replace"
			} else {
				id = uniqueID()
			}
			for _, k := range p.Unreadable {
				row.NotStored = append(row.NotStored, k+" (not a setting value)")
			}
			for k := range p.Meta {
				if strings.HasPrefix(k, "compatible_") {
					row.NotStored = append(row.NotStored, k+" (every profile works with every printer)")
				}
			}
			head := map[string]any{"id": id, "name": name, "revision": revision}
			if p.Record != nil {
				var record map[string]json.RawMessage
				if json.Unmarshal(p.Record, &record) != nil || stringValue(record["name"]) == "" ||
					(kind == "printer" && len(listText(record["nozzles"])) == 0) || (kind == "filament" && stringValue(record["product"]) == "") {
					return skipped("the record exported with it is incomplete")
				}
				delete(record, "archived")
				if row.Status == "new" && claim(exported, p) {
					head["id"] = exported
				}
				for k, v := range head {
					record[k] = must(json.Marshal(v))
				}
				if kind == "filament" {
					var aliases []string
					if current := lib.Filaments[id]; current != nil {
						aliases = slices.Clone(current.Aliases)
					}
					for _, alias := range row.SourceIDs {
						if alias != head["id"] && !slices.Contains(aliases, alias) && claim(alias, p) {
							aliases = append(aliases, alias)
						}
					}
					if len(aliases) == 0 {
						delete(record, "aliases")
					} else {
						record["aliases"] = must(json.Marshal(aliases))
					}
				}
				row.Record = must(json.Marshal(record))
				row.Basis = cmpOr(stringValue(record["product"]), "Auto")
				if kind == "printer" {
					var nozzles []resolve.Nozzle
					json.Unmarshal(record["nozzles"], &nozzles)
					if len(nozzles) > 0 {
						row.Basis = nozzles[0].Variant
					}
				}
				sort.Strings(row.NotStored)
				return row
			}
			values, root, err := flatten(p, map[*parsedPreset]bool{})
			if err != nil {
				return skipped(err.Error())
			}
			same := func(_ string, a, b json.RawMessage) bool { return b != nil && valueText(a) == valueText(b) }
			var mine map[string]json.RawMessage
			switch kind {
			case "printer":
				if current != nil {
					mine = current.Machine
				}
			case "filament":
				if f := lib.Filaments[id]; f != nil {
					mine = f.Settings
				}
			case "process":
				if p := lib.Processes[id]; p != nil {
					mine = p.Values
				}
			}
			keep := func(allowed map[string]bool, drop map[string]bool, below map[string]json.RawMessage) map[string]json.RawMessage {
				out := map[string]json.RawMessage{}
				for k, v := range values {
					switch {
					case drop[k]:
					case below != nil && same(k, v, below[k]) && !(mine[k] != nil && same(k, v, mine[k])):
					case kind == "filament" && slices.Contains(resolve.ColourKeys, k):
						row.NotStored = append(row.NotStored, k+" (colour belongs to the printer's slot)")
					case allowed[k]:
						out[k] = v
					default:
						row.NotStored = append(row.NotStored, fmt.Sprintf("%s (not a %s setting)", k, map[string]string{"printer": "printer", "filament": "filament", "process": "print"}[kind]))
					}
				}
				return out
			}
			madeFor := ""
			for _, v := range listText(p.Meta["compatible_printers"]) {
				if _, found := variantOf(c, v); found != nil && madeFor == "" {
					madeFor = v
				}
			}
			onVariant := func(kindName, product string) map[string]json.RawMessage {
				pr := productOf(c, kindName, product)
				if pr == nil {
					return nil
				}
				v := madeFor
				for _, t := range pr.Tunings {
					if v == "" && t.Source.Name == root && len(t.Fits) > 0 {
						v = t.Fits[0]
					}
				}
				if r := c.Resolve(kindName, product, v); r != nil {
					return r.Config
				}
				return nil
			}
			switch kind {
			case "filament":
				product := ""
				for _, cand := range []string{root, p.Name} {
					if cand != "" && product == "" && c.Filaments[productID(cand)] != nil {
						product = productID(cand)
					}
				}
				if product == "" {
					generic := "Generic " + firstText(values["filament_type"])
					if c.Filaments[generic] == nil {
						return skipped("no catalog filament to start from: neither " + cmpOr(root, "its parent") + " nor " + p.Name + " is a catalog filament, and there is no " + generic + " product")
					}
					product, row.Note = generic, "no catalog parent: starts from "+generic+", keeping the values that differ from it"
				}
				record := head
				record["product"] = product
				if settings := keep(c.FilamentSettings(), nil, onVariant("filaments", product)); len(settings) > 0 {
					record["settings"] = settings
				}
				if f := lib.Filaments[id]; f != nil && len(f.Aliases) > 0 {
					record["aliases"] = f.Aliases
				}
				row.Basis, row.Record = product, must(json.Marshal(record))
			case "process":
				product := ""
				for _, cand := range []string{root, p.Name} {
					if cand != "" && product == "" && c.Prints[productID(cand)] != nil {
						product = productID(cand)
					}
				}
				keys := c.PrintSettings()
				record := head
				if product != "" {
					record["product"] = product
					row.Basis = product
				} else {
					row.Basis, row.Note = "Auto", "no parent: ✦ Auto, keeping the values that differ from the printer's default print"
				}
				var below map[string]json.RawMessage
				if product != "" {
					below = onVariant("prints", product)
				} else if d := defaultPrintOn(c, madeFor); d != nil {
					below = d.Config
				}
				if own := keep(keys, nil, below); len(own) > 0 {
					record["values"] = own
				}
				if p := lib.Processes[id]; p != nil {
					if len(p.Scales) > 0 {
						record["scales"] = p.Scales
					}
					if p.PreferredHeight != nil {
						record["preferredHeight"] = *p.PreferredHeight
					}
					if p.HeightRule != "" {
						record["heightRule"] = p.HeightRule
					}
					if p.SpeedModel != nil {
						record["speedModel"] = p.SpeedModel
					}
				}
				row.Record = must(json.Marshal(record))
			case "printer":
				if _, v := variantOf(c, root); v == nil {
					if _, own := variantOf(c, p.Name); own != nil {
						root = p.Name
					}
				}
				model := firstText(values["printer_model"])
				if model == "" {
					model = regexp.MustCompile(`\s+[\d.]+\s+nozzle$`).ReplaceAllString(root, "")
				}
				m, remapped := c.Printers[model], false
				for _, p := range c.Printers {
					for _, vendor := range []string{p.Vendor, p.VendorName} {
						if m == nil && vendor != "" && model == vendor+" "+p.ID {
							m, model, remapped = p, p.ID, true
						}
					}
				}
				if m == nil {
					return skipped("no catalog printer " + cmpOr(model, "(no printer_model and no parent)"))
				}
				var diameters []float64
				for _, d := range listText(values["nozzle_diameter"]) {
					if n, err := strconv.ParseFloat(d, 64); err == nil && n > 0 {
						diameters = append(diameters, n)
					}
				}
				variant := (*resolve.CatalogVariant)(nil)
				for i := range m.Variants {
					if m.Variants[i].ID == root {
						variant = &m.Variants[i]
					}
				}
				if variant == nil {
					want := 0.4
					if len(diameters) > 0 {
						want = diameters[0]
					}
					if vid := c.Variant(model, want); vid != "" {
						for i := range m.Variants {
							if m.Variants[i].ID == vid {
								variant = &m.Variants[i]
							}
						}
					}
				}
				if variant == nil {
					return skipped(model + " has no catalog variant " + cmpOr(root, "for that nozzle"))
				}
				if variant.ID != root {
					row.Note = "on " + variant.ID
				}
				extruders := max(1, len(listText(json.RawMessage(must(json.Marshal(cmpOr(variant.Values["nozzle_diameter"], m.Base["nozzle_diameter"])))))))
				own := func(key string) []string {
					if m := c.Machine(variant.ID); m != nil && valueText(values[key]) == valueText(m.Config[key]) {
						return nil
					}
					return listText(values[key])
				}
				volumes, types, offsets := own("nozzle_volume_type"), own("extruder_type"), own("extruder_offset")
				at := func(list []string, i int) string { return list[min(i, len(list)-1)] }
				nozzles := make([]resolve.Nozzle, extruders)
				for i := range nozzles {
					d, _ := strconv.ParseFloat(variant.Nozzle, 64)
					if len(diameters) > 0 {
						d = diameters[min(i, len(diameters)-1)]
					}
					n := resolve.Nozzle{Variant: variant.ID, Diameter: d}
					if len(volumes) > 0 {
						n.Volume = at(volumes, i)
					}
					if len(types) > 0 {
						n.Type = at(types, i)
					}
					if len(offsets) > 0 {
						var xy [2]float64
						fmt.Sscanf(strings.Replace(at(offsets, i), "x", " ", 1), "%g %g", &xy[0], &xy[1])
						n.Offset = &xy
					}
					nozzles[i] = n
				}
				drop := map[string]bool{}
				for _, k := range resolve.HardwareKeys {
					drop[k] = true
				}
				if remapped {
					drop["printer_model"] = true
				}
				record := head
				record["vendor"], record["model"], record["nozzles"] = m.Vendor, m.ID, nozzles
				var below map[string]json.RawMessage
				if machine := c.Machine(variant.ID); machine != nil {
					below = machine.Config
				}
				if machine := keep(c.PrinterSettings(m.ID), drop, below); len(machine) > 0 {
					record["machine"] = machine
				}
				record["swap"], record["slots"] = "manual", []resolve.Slot{{ID: newID("slot")}}
				if current != nil {
					record["swap"], record["slots"] = current.Swap, current.Slots
					if current.Process != "" {
						record["process"] = current.Process
					}
					if len(current.PrintOverrides) > 0 {
						record["printOverrides"] = current.PrintOverrides
					}
					if len(current.Units) > 0 {
						record["units"] = current.Units
					}
					if len(current.Mixes) > 0 {
						record["mixes"] = current.Mixes
					}
				}
				row.Basis, row.Record = variant.ID, must(json.Marshal(record))
			}
			sort.Strings(row.NotStored)
			return row
		}
		matches := existing(kind, name)
		own := exported
		if own == "" || !live(kind, own) {
			own = ""
		}
		edited := func(r importRow) importRow {
			if p.Edited && r.Status != "skip" {
				r.Note = strings.TrimPrefix(r.Note+"; Edited outside PancakeSlicer", "; ")
			}
			return r
		}
		switch {
		case len(matches) > 1:
			skip("import_invalid", fmt.Sprintf("you have %d %s profiles named %s; rename all but one, then import it", len(matches), kind, name))
		case len(matches) == 1 || own != "":
			target, renamed := own, true
			if len(matches) == 1 {
				target, renamed = matches[0], false
			}
			alt := decode(target, row)
			current := stored(kind, target)
			// Selecting an identical local dependency can still change the printer's reference.
			distinctSource := hasExportedPrinter && len(row.SourceIDs) > 0 && exported != target
			if alt.Status == "skip" || !distinctSource && sameRecordJSON(alt.Record, current) {
				rows = append(rows, edited(alt))
				break
			}
			added := decode("", row)
			if added.Status != "skip" {
				var was struct {
					Name   string `json:"name"`
					Edited string `json:"edited"`
				}
				json.Unmarshal(current, &was)
				_, rev, _ := recordOf(kind, target)
				if renamed {
					added.Note = fmt.Sprintf("Same profile as '%s' (renamed in the file)", was.Name)
				} else {
					free := freeName(kind, name+" (imported)")
					added.Record = withName(added.Record, free)
					taken[kind+"\x00"+free] = p
					added.Note = fmt.Sprintf("Heads up! A %s named %s already exists, so we added it as '%s' unless you pick Replace.", kindLabel[kind], name, free)
				}
				added.Matches = []importMatch{{ID: target, Name: was.Name, Revision: rev, Record: alt.Record, Edited: was.Edited, Differ: settingsDiffer(current, alt.Record)}}
			}
			rows = append(rows, edited(added))
		default:
			rows = append(rows, edited(decode("", row)))
		}
	}
	// Preview the default choices while retaining the original bundle references for submission.
	defaults := make([]importRecord, len(rows))
	for i, r := range rows {
		if r.Status != "skip" {
			defaults[i] = r.importRecord
		}
	}
	cleared := remapImportedPrinters(defaults, lib)
	for i := range rows {
		r := &rows[i]
		if cleared[i] {
			r.Note = strings.TrimPrefix(r.Note+"; "+missingImportProfiles, "; ")
		}
		if r.Status != "replace" || r.Record == nil {
			continue
		}
		var probe struct {
			ID string `json:"id"`
		}
		json.Unmarshal(r.Record, &probe)
		if current := stored(r.Kind, probe.ID); current != nil && sameRecordJSON(defaults[i].Record, current) {
			r.Status, r.Note = "unchanged", "the same as the library's "+r.Kind+"; nothing to import"
		}
	}
	return rows
}

const missingImportProfiles = "Some filaments or the print profile weren't in the library or the import, so pick them again"

func remapImportedPrinters(selected []importRecord, lib *resolve.Library) []bool {
	targets := map[string]map[string]string{"filament": {}, "process": {}}
	for _, row := range selected {
		ids := targets[row.Kind]
		if ids == nil {
			continue
		}
		var record struct {
			ID string `json:"id"`
		}
		if json.Unmarshal(row.Record, &record) != nil || record.ID == "" {
			continue
		}
		for _, source := range row.SourceIDs {
			if ids[source] == "" {
				ids[source] = record.ID
			}
		}
	}
	find := func(kind, id string) string {
		if target := targets[kind][id]; target != "" {
			return target
		}
		if kind == "filament" {
			if f := lib.Filament(id); f != nil && f.Live() {
				return id
			}
		} else if p := lib.Processes[id]; p != nil && p.Live() {
			return id
		}
		return ""
	}
	cleared := make([]bool, len(selected))
	for i := range selected {
		row := &selected[i]
		if row.Kind != "printer" || !row.ExportedPrinter {
			continue
		}
		var record map[string]json.RawMessage
		if json.Unmarshal(row.Record, &record) != nil {
			continue
		}
		changed := false
		var slots []map[string]json.RawMessage
		if json.Unmarshal(record["slots"], &slots) == nil {
			slotsChanged := false
			for _, slot := range slots {
				id := stringValue(slot["filament"])
				if id == "" {
					continue
				}
				target := find("filament", id)
				if target != id {
					slot["filament"] = must(json.Marshal(target))
					slotsChanged = true
					cleared[i] = cleared[i] || target == ""
				}
			}
			if slotsChanged {
				record["slots"] = must(json.Marshal(slots))
				changed = true
			}
		}
		if id := stringValue(record["process"]); id != "" {
			target := find("process", id)
			if target != id {
				if target == "" {
					delete(record, "process")
					cleared[i] = true
				} else {
					record["process"] = must(json.Marshal(target))
				}
				changed = true
			}
		}
		if changed {
			row.Record = must(json.Marshal(record))
		}
	}
	return cleared
}

func orcaText(raw json.RawMessage) any {
	var v any
	json.Unmarshal(raw, &v)
	switch t := v.(type) {
	case []any:
		out := make([]string, len(t))
		for i, x := range t {
			out[i] = orcaScalar(x)
		}
		return out
	case string:
		return t
	default:
		return orcaScalar(t)
	}
}
func orcaScalar(v any) string {
	switch t := v.(type) {
	case bool:
		if t {
			return "1"
		}
		return "0"
	case float64:
		return strconv.FormatFloat(t, 'f', -1, 64)
	case nil:
		return ""
	}
	return fmt.Sprint(v)
}

type exportTarget struct {
	Kind    string `json:"kind"`
	ID      string `json:"id"`
	Printer string `json:"printer,omitempty"`
	Name    string `json:"name,omitempty"`
}
type orcaPreset struct {
	Type, Name string
	JSON       map[string]any
}

func (s *Server) orcaPresetOf(lib *resolve.Library, t exportTarget, values map[string]json.RawMessage) (orcaPreset, error) {
	types := map[string]string{"printer": "machine", "filament": "filament", "process": "process"}
	settingsID := map[string]string{"machine": "printer_settings_id", "filament": "filament_settings_id", "process": "print_settings_id"}
	var name, inherits, product string
	var record any
	switch t.Kind {
	case "printer":
		p := lib.Printers[t.ID]
		if p == nil {
			return orcaPreset{}, fmt.Errorf("that printer isn't in your library anymore")
		}
		name, record, inherits = p.Name, p, p.Nozzles[0].Variant
	case "filament":
		f := lib.Filament(t.ID)
		if f == nil {
			return orcaPreset{}, fmt.Errorf("that filament is no longer in your library")
		}
		name, record, product = f.Name, f, f.Product
	case "process":
		p := lib.Processes[t.ID]
		if p == nil {
			return orcaPreset{}, fmt.Errorf("that print profile is no longer in your library")
		}
		name, record, product = p.Name, p, p.Product
	default:
		return orcaPreset{}, fmt.Errorf("unknown kind %q", t.Kind)
	}
	variant := ""
	if p := lib.Printers[t.Printer]; p != nil {
		variant = p.Nozzles[0].Variant
	}
	if product != "" {
		catalog := s.catalog.Filaments
		if t.Kind == "process" {
			catalog = s.catalog.Prints
		}
		if pr := catalog[product]; pr != nil {
			inherits = orcaParent(pr, variant)
		}
	}
	if t.Name != "" {
		name = t.Name
	}
	var allowed map[string]bool
	switch t.Kind {
	case "process":
		allowed = s.catalog.PrintSettings()
	case "filament":
		allowed = s.catalog.FilamentSettings()
	case "printer":
		allowed = map[string]bool{}
		for k := range s.catalog.PrinterSettings(lib.Printers[t.ID].Model) {
			allowed[k] = true
		}
		for _, k := range resolve.HardwareKeys {
			allowed[k] = true
		}
	}
	out := map[string]any{}
	for k, v := range values {
		if allowed[k] && !orcaMeta.MatchString(k) {
			out[k] = orcaText(v)
		}
	}
	typ := types[t.Kind]
	out["from"], out["inherits"], out["name"], out["version"] = "User", inherits, name, orcaPresetVersion
	if t.Kind != "printer" && variant != "" {
		out["compatible_printers"] = []string{variant}
	}
	if typ == "filament" {
		out[settingsID[typ]] = []string{name}
	} else {
		out[settingsID[typ]] = name
	}
	plain := map[string]json.RawMessage{}
	for k, v := range out {
		if !orcaMeta.MatchString(k) {
			plain[k] = must(json.Marshal(v))
		}
	}
	out[recordField], out[digestField] = string(must(json.Marshal(record))), valuesDigest(plain)
	return orcaPreset{typ, name, out}, nil
}

func (s *Server) exportPresets(lib *resolve.Library, targets []exportTarget, selected string) ([]orcaPreset, error) {
	out := []orcaPreset{}
	types := map[string]string{"printer": "machine", "filament": "filament", "process": "process"}
	for _, t := range targets {
		t.Printer = cmpOr(t.Printer, selected)
		if t.Kind == "printer" {
			t.Printer = t.ID
		}
		if t.Printer == "" {
			return nil, fmt.Errorf("pick a printer first: print and filament profiles export with the values they print with on a printer")
		}
		answer := s.catalog.Answer(lib, resolve.Query{Request: resolve.Request{Printer: t.Printer}, Profiles: []resolve.Ask{{Type: types[t.Kind], ID: t.ID, Printer: t.Printer}}})
		if len(answer.Profiles) != 1 || !answer.Profiles[0].Status.OK {
			why := "the service could not resolve it"
			if len(answer.Profiles) == 1 {
				why = answer.Profiles[0].Status.Message
			}
			return nil, fmt.Errorf("%s: %s", t.ID, why)
		}
		values := answer.Profiles[0].ProfileValues
		p, err := s.orcaPresetOf(lib, t, values.Values)
		if err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, nil
}

var unsafeFileName = regexp.MustCompile(`[<>:"/\\|?*\x00-\x1f]`)

func presetFileName(name string) string { return unsafeFileName.ReplaceAllString(name, "_") }
func presetText(p orcaPreset) []byte {
	b, _ := json.MarshalIndent(p.JSON, "", "\t")
	return append(b, '\n')
}

func (s *Server) libraryIORoutes(mux *http.ServeMux) {
	mux.HandleFunc("POST /api/library/import", func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseMultipartForm(importMaxFile); err != nil {
			fail(w, 400, "send_profile_files_multipart")
			return
		}
		var presets []parsedPreset
		var projects []string
		var projectData [][]byte
		for _, fh := range r.MultipartForm.File["file"] {
			if fh.Size > importMaxFile {
				presets = append(presets, parsedPreset{File: fh.Filename, Name: fh.Filename, Error: fmt.Sprintf("file is larger than %d MB", importMaxFile>>20)})
				continue
			}
			f, err := fh.Open()
			if err != nil {
				presets = append(presets, parsedPreset{File: fh.Filename, Name: fh.Filename, Error: err.Error()})
				continue
			}
			data, err := io.ReadAll(io.LimitReader(f, importMaxFile+1))
			f.Close()
			if err != nil {
				presets = append(presets, parsedPreset{File: fh.Filename, Name: fh.Filename, Error: err.Error()})
				continue
			}
			if strings.HasSuffix(strings.ToLower(fh.Filename), ".3mf") {
				projects = append(projects, fh.Filename)
				projectData = append(projectData, data)
				continue
			}
			presets = append(presets, readPresetFiles(fh.Filename, data)...)
		}
		lib, err := s.libraryDocument()
		if err != nil {
			libraryFailure(w, 503, "library_unavailable", "Oops! Couldn't read profiles.", "retry_storage", 0)
			return
		}
		decoded := []projectImport{}
		for i, name := range projects {
			decoded = append(decoded, s.decodeProject(name, projectData[i], lib))
		}
		writeJSON(w, 200, map[string]any{"rows": s.planImport(presets, lib), "projects": decoded, "revision": lib.Revision})
	})
	mux.HandleFunc("POST /api/library/import/resolve", func(w http.ResponseWriter, r *http.Request) {
		var records []importRecord
		if !workspaceDecode(w, r, &records) {
			return
		}
		lib, err := s.libraryDocument()
		if err != nil {
			libraryFailure(w, 503, "library_unavailable", "Oops! Couldn't read profiles.", "retry_storage", 0)
			return
		}
		var problems []string
		if slices.Contains(remapImportedPrinters(records, lib), true) {
			problems = append(problems, missingImportProfiles)
		}
		writeJSON(w, 200, map[string]any{"records": records, "problems": problems})
	})
	mux.HandleFunc("POST /api/library/export", func(w http.ResponseWriter, r *http.Request) {
		var in struct {
			Kind      string           `json:"kind"`
			IDs       []string         `json:"ids"`
			Targets   []exportTarget   `json:"targets"`
			Printer   string           `json:"printer"`
			Selection *resolve.Request `json:"selection"`
			Type      string           `json:"type"`
			Slot      int              `json:"slot"`
		}
		if !workspaceDecode(w, r, &in) {
			return
		}
		lib, err := s.libraryDocument()
		if err != nil {
			libraryFailure(w, 503, "library_unavailable", "Oops! Couldn't read profiles.", "retry_storage", 0)
			return
		}
		live := func(kind string) []string {
			var ids []string
			switch kind {
			case "filament":
				for id, f := range lib.Filaments {
					if f.Live() {
						ids = append(ids, id)
					}
				}
			case "process":
				for id, p := range lib.Processes {
					if p.Live() {
						ids = append(ids, id)
					}
				}
			case "printer":
				for id, p := range lib.Printers {
					if p.Live() {
						ids = append(ids, id)
					}
				}
			}
			sort.Strings(ids)
			return ids
		}
		stamp := time.Now().UTC().Format("20060102150405")
		files := map[string][]byte{}
		var archive string
		bad := func(err error) { fail(w, 422, "library_io_unprocessable", err.Error()) }
		switch in.Kind {
		case "preset":
			if len(in.Targets) != 1 {
				fail(w, 400, "export_one_preset_time")
				return
			}
			presets, err := s.exportPresets(lib, in.Targets, in.Printer)
			if err != nil {
				bad(err)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", presetFileName(presets[0].Name)+".json"))
			w.Write(presetText(presets[0]))
			return
		case "current":
			if in.Selection == nil {
				fail(w, 400, "pick_what_youd_like")
				return
			}
			job := s.catalog.Answer(lib, resolve.Query{Request: *in.Selection})
			t, values, ok, why := exportTarget{Printer: in.Selection.Printer}, map[string]json.RawMessage(nil), false, ""
			switch {
			case in.Type == "machine":
				t.Kind, t.ID, values, ok, why = "printer", in.Selection.Printer, job.Printer.Values, job.Printer.Status.OK, job.Printer.Status.Message
			case in.Type == "process":
				t.Kind, t.ID, values, ok, why = "process", in.Selection.Print, job.Print.Values, job.Print.Status.OK, job.Print.Status.Message
			case in.Type == "filament" && in.Slot >= 0 && in.Slot < len(job.Filaments) && in.Slot < len(in.Selection.Filaments):
				f := job.Filaments[in.Slot]
				t.Kind, t.ID, values, ok, why = "filament", in.Selection.Filaments[in.Slot], f.Values, f.Status.OK, f.Status.Message
			default:
				fail(w, 400, "type_machine_process_filament")
				return
			}
			if !ok {
				fail(w, 422, "library_io_unprocessable", cmpOr(why, "Oops, the service couldn't resolve that."))
				return
			}
			p, err := s.orcaPresetOf(lib, t, values)
			if err != nil {
				bad(err)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", presetFileName(p.Name)+".json"))
			w.Write(presetText(p))
			return
		case "printer", "filament", "process":
			targets := []exportTarget{}
			for _, id := range in.IDs {
				targets = append(targets, exportTarget{Kind: in.Kind, ID: id})
			}
			presets, err := s.exportPresets(lib, targets, in.Printer)
			if err != nil {
				bad(err)
				return
			}
			for _, p := range presets {
				files[presetFileName(p.Name)+".json"] = presetText(p)
			}
			archive = map[string]string{"printer": "Printer", "filament": "Filament", "process": "Process"}[in.Kind] + " presets.zip"
		case "printer_bundle":
			if len(in.IDs) != 1 || lib.Printers[in.IDs[0]] == nil {
				fail(w, 400, "printer_bundle_one_library")
				return
			}
			printer := in.IDs[0]
			targets := []exportTarget{{Kind: "printer", ID: printer}}
			for _, id := range live("filament") {
				targets = append(targets, exportTarget{Kind: "filament", ID: id, Printer: printer})
			}
			for _, id := range live("process") {
				targets = append(targets, exportTarget{Kind: "process", ID: id, Printer: printer})
			}
			presets, err := s.exportPresets(lib, targets, printer)
			if err != nil {
				bad(err)
				return
			}
			paths := map[string][]string{"machine": {}, "filament": {}, "process": {}}
			dirs := map[string]string{"machine": "printer", "filament": "filament", "process": "process"}
			for _, p := range presets {
				file := dirs[p.Type] + "/" + presetFileName(p.Name) + ".json"
				files[file] = presetText(p)
				paths[p.Type] = append(paths[p.Type], file)
			}
			name := lib.Printers[printer].Name
			files["bundle_structure.json"] = must(json.Marshal(map[string]any{"bundle_id": "offline_" + name + "_" + stamp, "bundle_type": "printer config bundle",
				"printer_preset_name": name, "version": "", "printer_config": paths["machine"], "filament_config": paths["filament"], "process_config": paths["process"]}))
			archive = presetFileName(name) + ".orca_printer"
		case "filament_bundle":
			if len(in.IDs) != 1 || lib.Filament(in.IDs[0]) == nil {
				fail(w, 400, "filament_bundle_one_library")
				return
			}
			f := lib.Filament(in.IDs[0])
			var targets []exportTarget
			var vendors []string
			for _, id := range live("printer") {
				p := lib.Printers[id]
				targets = append(targets, exportTarget{Kind: "filament", ID: f.ID, Printer: id, Name: f.Name + " @" + p.Name})
				vendors = append(vendors, p.Vendor)
			}
			presets, err := s.exportPresets(lib, targets, "")
			if err != nil {
				bad(err)
				return
			}
			byVendor := map[string][]string{}
			var order []string
			for i, p := range presets {
				file := vendors[i] + "/" + presetFileName(p.Name) + ".json"
				files[file] = presetText(p)
				if byVendor[vendors[i]] == nil {
					order = append(order, vendors[i])
				}
				byVendor[vendors[i]] = append(byVendor[vendors[i]], file)
			}
			list := []map[string]any{}
			for _, v := range order {
				list = append(list, map[string]any{"vendor": v, "filament_path": byVendor[v]})
			}
			files["bundle_structure.json"] = must(json.Marshal(map[string]any{"bundle_id": "offline_" + f.Name + "_" + stamp, "bundle_type": "filament config bundle",
				"filament_name": f.Name, "version": "", "printer_vendor": list}))
			archive = presetFileName(f.Name) + ".orca_filament"
		default:
			fail(w, 400, "kind_preset_printer_bundle")
			return
		}
		var buf bytes.Buffer
		zw := zip.NewWriter(&buf)
		names := make([]string, 0, len(files))
		for n := range files {
			names = append(names, n)
		}
		sort.Strings(names)
		for _, n := range names {
			fw, err := zw.Create(n)
			if err == nil {
				_, err = fw.Write(files[n])
			}
			if err != nil {
				fail(w, 500, "library_io_internal", err.Error())
				return
			}
		}
		zw.Close()
		w.Header().Set("Content-Type", "application/zip")
		w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", archive))
		w.Write(buf.Bytes())
	})
}

type projectProfile struct {
	Name    string `json:"name"`
	Variant string `json:"variant,omitempty"`
	Product string `json:"product,omitempty"`
	Library string `json:"library"`
}
type projectObject struct {
	ID     string                     `json:"id"`
	Name   string                     `json:"name"`
	Values map[string]json.RawMessage `json:"values"`
}
type projectImport struct {
	File      string                       `json:"file"`
	Printer   projectProfile               `json:"printer"`
	Print     projectProfile               `json:"print"`
	Filaments []projectProfile             `json:"filaments"`
	Machine   map[string]json.RawMessage   `json:"machine"`
	Process   map[string]json.RawMessage   `json:"process"`
	Slots     []map[string]json.RawMessage `json:"slots"`
	Objects   []projectObject              `json:"objects"`
	Settings  bool                         `json:"settings"`
	BedType   string                       `json:"bedType,omitempty"`
	NotStored []string                     `json:"notStored"`
	Error     string                       `json:"error,omitempty"`
}

var xmlObject = regexp.MustCompile(`(?s)<object\s+id="([^"]*)"[^>]*>(.*?)</object>`)
var xmlMetadata = regexp.MustCompile(`<metadata\s+key="([^"]*)"\s+value="([^"]*)"\s*/>`)

var objectMeta = map[string]bool{"name": true, "extruder": true, "matrix": true, "source_file": true, "source_object_id": true, "source_volume_id": true,
	"source_offset_x": true, "source_offset_y": true, "source_offset_z": true}

func xmlText(s string) string {
	return strings.NewReplacer("&quot;", `"`, "&apos;", "'", "&lt;", "<", "&gt;", ">", "&amp;", "&").Replace(s)
}

type archiveObject struct {
	ID, Name string
	Values   map[string]string
	Parts    []map[string]string
	Ranges   []map[string]string
}

var xmlPart = regexp.MustCompile(`(?s)<part\b.*?</part>`)
var xmlRangeObject = regexp.MustCompile(`(?s)<object\s+id="([^"]*)"[^>]*>(.*?)</object>`)
var xmlRange = regexp.MustCompile(`(?s)<range\b[^>]*>(.*?)</range>`)
var xmlOption = regexp.MustCompile(`(?s)<option\s+opt_key="([^"]*)"\s*>(.*?)</option>`)

func archiveSettings(read func(string) []byte) []archiveObject {
	var out []archiveObject
	ranges := map[string][]map[string]string{}
	if raw := read("Metadata/layer_config_ranges.xml"); raw != nil {
		for _, o := range xmlRangeObject.FindAllStringSubmatch(string(raw), -1) {
			for _, r := range xmlRange.FindAllStringSubmatch(o[2], -1) {
				values := map[string]string{}
				for _, m := range xmlOption.FindAllStringSubmatch(r[1], -1) {
					values[m[1]] = xmlText(strings.TrimSpace(m[2]))
				}
				ranges[o[1]] = append(ranges[o[1]], values)
			}
		}
	}
	if raw := read("Metadata/model_settings.config"); raw != nil {
		for _, o := range xmlObject.FindAllStringSubmatch(string(raw), -1) {
			own := strings.SplitN(o[2], "<part", 2)
			obj := archiveObject{ID: o[1], Values: map[string]string{}, Ranges: ranges[o[1]]}
			for _, m := range xmlMetadata.FindAllStringSubmatch(own[0], -1) {
				switch key, value := m[1], xmlText(m[2]); {
				case key == "name":
					obj.Name = value
				case !objectMeta[key]:
					obj.Values[key] = value
				}
			}
			if len(own) > 1 {
				for _, part := range xmlPart.FindAllString("<part"+own[1], -1) {
					values := map[string]string{}
					for _, m := range xmlMetadata.FindAllStringSubmatch(part, -1) {
						if !objectMeta[m[1]] && m[1] != "name" && !strings.HasPrefix(m[1], "source_") {
							values[m[1]] = xmlText(m[2])
						}
					}
					obj.Parts = append(obj.Parts, values)
				}
			}
			out = append(out, obj)
		}
	}
	return out
}

func archiveReader(zr *zip.Reader) func(string) []byte {
	return func(path string) []byte {
		for _, f := range zr.File {
			if strings.EqualFold(f.Name, path) && f.UncompressedSize64 <= importMaxFile {
				if rc, err := f.Open(); err == nil {
					b, _ := io.ReadAll(rc)
					rc.Close()
					return b
				}
			}
		}
		return nil
	}
}

func archivePlate(data []byte) []byte {
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return nil
	}
	read := archiveReader(zr)
	type settings map[string]string
	type entry struct {
		Settings settings `json:"settings"`
		Volumes  []struct {
			Settings settings `json:"settings"`
		} `json:"volumes"`
		Ranges []struct {
			Settings settings `json:"settings"`
		} `json:"ranges"`
	}
	doc := struct {
		Objects []entry `json:"objects"`
	}{}
	for _, a := range archiveSettings(read) {
		e := entry{Settings: a.Values}
		for _, p := range a.Parts {
			e.Volumes = append(e.Volumes, struct {
				Settings settings `json:"settings"`
			}{p})
		}
		for _, r := range a.Ranges {
			e.Ranges = append(e.Ranges, struct {
				Settings settings `json:"settings"`
			}{r})
		}
		doc.Objects = append(doc.Objects, e)
	}
	return must(json.Marshal(doc))
}

func (s *Server) decodeProject(name string, data []byte, lib *resolve.Library) projectImport {
	out := projectImport{File: name, Machine: map[string]json.RawMessage{}, Process: map[string]json.RawMessage{}, Slots: []map[string]json.RawMessage{},
		Objects: []projectObject{}, NotStored: []string{}, Filaments: []projectProfile{}}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		out.Error = "couldn't open the 3MF (" + err.Error() + ")"
		return out
	}
	read := archiveReader(zr)
	decodeObjects := func() {
		for _, a := range archiveSettings(read) {
			obj := projectObject{ID: a.ID, Name: a.Name, Values: map[string]json.RawMessage{}}
			for key, value := range a.Values {
				if s.catalog.PrintSettings()[key] {
					obj.Values[key] = resolve.CanonicalValue(key, must(json.Marshal(value)))
				} else {
					out.NotStored = append(out.NotStored, fmt.Sprintf("%s on %s (not an object setting)", key, cmpOr(obj.Name, "object "+a.ID)))
				}
			}
			for _, part := range a.Parts {
				for key := range part {
					out.NotStored = append(out.NotStored, fmt.Sprintf("%s on a part of %s (part settings are not imported)", key, cmpOr(obj.Name, "object "+a.ID)))
				}
			}
			if len(obj.Values) > 0 {
				out.Objects = append(out.Objects, obj)
			}
		}
	}
	var config map[string]json.RawMessage
	if raw := read("Metadata/project_settings.config"); raw == nil || json.Unmarshal(raw, &config) != nil {
		decodeObjects()
		sort.Strings(out.NotStored)
		return out
	}
	for key, value := range config {
		config[key] = resolve.CanonicalValue(key, value)
	}
	out.Settings = true
	c := s.catalog
	group := listText(config["inherits_group"])
	inheritsAt := func(i int) string {
		if i >= 0 && i < len(group) {
			return group[i]
		}
		return ""
	}
	printerName := firstText(config["printer_settings_id"])
	variant := ""
	for _, cand := range []string{printerName, inheritsAt(len(group) - 1)} {
		if _, found := variantOf(c, cand); found != nil && variant == "" {
			variant = cand
		}
	}
	if variant == "" {
		d, _ := strconv.ParseFloat(firstText(config["nozzle_diameter"]), 64)
		variant = c.Variant(firstText(config["printer_model"]), d)
	}
	out.Printer = projectProfile{Name: printerName, Variant: variant}
	if variant == "" {
		out.NotStored = append(out.NotStored, "the project's printer, print and filament settings (the catalog has no printer "+cmpOr(firstText(config["printer_model"]), printerName)+")")
		out.Settings = false
		decodeObjects()
		sort.Strings(out.NotStored)
		return out
	}
	for id, p := range lib.Printers {
		if p.Live() && p.Nozzles[0].Variant == variant && out.Printer.Library == "" {
			out.Printer.Library = id
		}
	}
	product := func(catalog map[string]*resolve.Product, names ...string) string {
		for _, n := range names {
			id := productID(n)
			if catalog[id] != nil {
				return id
			}
			if into := c.FoldedInto(id); into != "" && catalog[into] != nil {
				return into
			}
		}
		return ""
	}
	printName := firstText(config["print_settings_id"])
	out.Print = projectProfile{Name: printName, Product: product(c.Prints, printName, inheritsAt(0))}
	for id, p := range lib.Processes {
		if p.Live() && out.Print.Product != "" && p.Product == out.Print.Product && out.Print.Library == "" {
			out.Print.Library = id
		}
	}
	var slotsOf *resolve.Printer
	if out.Printer.Library != "" {
		slotsOf = lib.Printers[out.Printer.Library]
	}
	filamentIDs := make([]string, 0, len(lib.Filaments))
	for id := range lib.Filaments {
		filamentIDs = append(filamentIDs, id)
	}
	sort.Strings(filamentIDs)
	for i, n := range listText(config["filament_settings_id"]) {
		f := projectProfile{Name: n, Product: product(c.Filaments, n, inheritsAt(1+i))}
		if f.Product != "" && slotsOf != nil && i < len(slotsOf.Slots) {
			if r := lib.Filament(slotsOf.Slots[i].Filament); r != nil && r.Live() && r.Product == f.Product {
				f.Library = r.ID
			}
		}
		for _, id := range filamentIDs {
			if r := lib.Filaments[id]; f.Library == "" && f.Product != "" && r.Live() && r.Product == f.Product {
				f.Library = id
			}
		}
		out.Filaments = append(out.Filaments, f)
	}
	below := func(ask resolve.Ask, catalogValues func() map[string]json.RawMessage) map[string]json.RawMessage {
		if ask.ID != "" && out.Printer.Library != "" {
			if v, err := s.catalog.Profile(lib, out.Printer.Library, ask); err == nil {
				return v.Values
			}
		}
		return catalogValues()
	}
	machineBelow := below(resolve.Ask{Type: "machine", ID: out.Printer.Library}, func() map[string]json.RawMessage {
		if m := c.Machine(variant); m != nil {
			return m.Config
		}
		return nil
	})
	printBelow := below(resolve.Ask{Type: "process", ID: out.Print.Library}, func() map[string]json.RawMessage {
		if out.Print.Product != "" {
			if r := c.Resolve("prints", out.Print.Product, variant); r != nil {
				return r.Config
			}
		}
		if d := defaultPrintOn(c, variant); d != nil {
			return d.Config
		}
		return nil
	})
	text := func(_ string, v json.RawMessage) string { return valueText(v) }
	model, _ := variantOf(c, variant)
	machineKeys, printKeys, filamentKeys := c.PrinterSettings(model.ID), c.PrintSettings(), c.FilamentSettings()
	slotBelow := make([]map[string]json.RawMessage, len(out.Filaments))
	for i, f := range out.Filaments {
		out.Slots = append(out.Slots, map[string]json.RawMessage{})
		slotBelow[i] = below(resolve.Ask{Type: "filament", ID: f.Library}, func() map[string]json.RawMessage {
			if f.Product != "" {
				if r := c.Resolve("filaments", f.Product, variant); r != nil {
					return r.Config
				}
			}
			return nil
		})
	}
	hardware := map[string]bool{}
	for _, k := range resolve.HardwareKeys {
		hardware[k] = true
	}
	for k, v := range config {
		switch {
		case orcaMeta.MatchString(k) || k == "inherits_group" || k == "different_settings_to_system":
		case k == "curr_bed_type":
			out.BedType = firstText(v)
		case hardware[k]:
			if text(k, v) != text(k, machineBelow[k]) {
				out.NotStored = append(out.NotStored, fmt.Sprintf("%s %s (the printer's nozzles are set on the printer)", k, firstText(v)))
			}
		case slices.Contains(resolve.ColourKeys, k):
		case filamentKeys[k] && !machineKeys[k]:
			for i, value := range listText(v) {
				if i >= len(out.Slots) {
					break
				}
				if belowValues := listText(slotBelow[i][k]); len(belowValues) == 0 || belowValues[0] != value {
					out.Slots[i][k] = must(json.Marshal([]string{value}))
				}
			}
		case machineKeys[k]:
			if text(k, v) != text(k, machineBelow[k]) {
				out.Machine[k] = v
			}
		case printKeys[k]:
			if text(k, v) != text(k, printBelow[k]) {
				out.Process[k] = v
			}
		default:
			out.NotStored = append(out.NotStored, k+" (not a printer, print or filament setting)")
		}
	}
	decodeObjects()
	sort.Strings(out.NotStored)
	return out
}

func sameRecordJSON(a, b json.RawMessage) bool {
	canonical := func(raw json.RawMessage) string {
		var v map[string]any
		if json.Unmarshal(raw, &v) != nil {
			return ""
		}
		delete(v, "revision")
		delete(v, "edited")
		return string(must(json.Marshal(v)))
	}
	ca := canonical(a)
	return ca != "" && ca == canonical(b)
}

func settingsDiffer(a, b json.RawMessage) int {
	flat := func(raw json.RawMessage) map[string]string {
		out := map[string]string{}
		var walk func(path string, v any)
		walk = func(path string, v any) {
			if m, ok := v.(map[string]any); ok {
				for k, x := range m {
					walk(path+"/"+k, x)
				}
				return
			}
			out[path] = string(must(json.Marshal(v)))
		}
		var v map[string]any
		json.Unmarshal(raw, &v)
		delete(v, "revision")
		delete(v, "edited")
		delete(v, "name")
		walk("", v)
		return out
	}
	fa, fb := flat(a), flat(b)
	n := 0
	for k, v := range fa {
		if fb[k] != v {
			n++
		}
	}
	for k := range fb {
		if _, ok := fa[k]; !ok {
			n++
		}
	}
	return n
}
