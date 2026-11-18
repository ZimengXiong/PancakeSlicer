package resolve

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
)

type Data struct {
	Rules, VariantKeys, Options []byte
	Feeds         []byte
	LibrarySchema int
}

type Rules struct {
	Revision int `json:"revision"`
	CalibrationKeys     []string `json:"calibrationKeys"`
	CalibrationPrefixes []string `json:"calibrationPrefixes"`
	Round             float64              `json:"round"`
	MaxLayerOfNozzle  float64              `json:"maxLayerOfNozzle"`
	LineWidthOfNozzle [2]float64           `json:"lineWidthOfNozzle"`
	DefaultScales     []Scale              `json:"defaultScales"`
	SourceOrder       []string             `json:"sourceOrder"`
	NotScalable       []string             `json:"notScalable"`
	SpeedFeatures     []SpeedFeature       `json:"speedFeatures"`
	SpeedSeeds        map[string]SpeedSeed `json:"speedIntents"`
	Heights           struct {
		Step, MinOfNozzle, MinFloor, MaxOfNozzle float64
	} `json:"heights"`
	HeightRules map[string]struct {
		CoarsestOfNozzle float64 `json:"coarsestOfNozzle"`
		NearestOfNozzle  float64 `json:"nearestOfNozzle"`
	} `json:"heightRules"`
}

var (
	profileRules        Rules
	filamentVariantKeys map[string]bool
	profileDefaults map[string]map[string]json.RawMessage
	optionDictionary    []byte
	vectors             map[string]bool
	configKinds         map[string]string
)

type Tables struct {
	Feeds int `json:"feeds"`
	Rules int `json:"rules"`
}

var tables Tables

func TableRevisions() Tables { return tables }

func (t Tables) Newer(other Tables) bool { return t.Feeds > other.Feeds || t.Rules > other.Rules }

func Configure(d Data) error {
	var r Rules
	if err := json.Unmarshal(d.Rules, &r); err != nil || r.Round == 0 || len(r.DefaultScales) == 0 {
		return errors.New("profile_rules.json is invalid")
	}
	var keys []string
	if err := json.Unmarshal(d.VariantKeys, &keys); err != nil {
		return fmt.Errorf("filament_variant_keys.json: %w", err)
	}
	filamentVariantKeys = map[string]bool{}
	for _, key := range keys {
		filamentVariantKeys[key] = true
	}
	kinds, err := optionKinds(d.Options)
	if err != nil {
		return err
	}
	defaults, err := optionProfiles(d.Options)
	if err != nil {
		return err
	}
	if len(r.CalibrationKeys) == 0 || len(r.CalibrationPrefixes) == 0 {
		return errors.New("profile_rules.json: calibrationKeys and calibrationPrefixes are required")
	}
	var feeds struct {
		Revision int             `json:"revision"`
		Models   map[string]Feed `json:"models"`
	}
	if err := json.Unmarshal(d.Feeds, &feeds); err != nil || feeds.Models == nil || feeds.Revision <= 0 || r.Revision <= 0 {
		return errors.New("printer_feeds.json and profile_rules.json need their models and a revision")
	}
	tables = Tables{Feeds: feeds.Revision, Rules: r.Revision}
	if d.LibrarySchema <= 0 {
		return errors.New("the library schema is required")
	}
	printerFeeds, librarySchema, profileDefaults = feeds.Models, d.LibrarySchema, defaults
	profileRules, optionDictionary, vectors, configKinds = r, d.Options, vectorOptions(d.Options), kinds
	return nil
}

func optionKinds(dictionary []byte) (map[string]string, error) {
	var defs map[string]struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal(dictionary, &defs); err != nil || len(defs) == 0 {
		return nil, errors.New("the option dictionary is invalid")
	}
	kinds := map[string]string{}
	for key, def := range defs {
		switch def.Type {
		case "String":
			kinds[key] = "string"
		case "Strings":
			kinds[key] = "strings"
		case "PointsGroups", "IntsGroups":
			kinds[key] = "groups"
		default:
			kinds[key] = "comma"
		}
	}
	return kinds, nil
}

func optionProfiles(dictionary []byte) (map[string]map[string]json.RawMessage, error) {
	var defs map[string]struct {
		Profile string          `json:"profile"`
		Default json.RawMessage `json:"default"`
	}
	if err := json.Unmarshal(dictionary, &defs); err != nil {
		return nil, errors.New("the option dictionary is invalid")
	}
	out := map[string]map[string]json.RawMessage{"machine": {}, "process": {}, "filament": {}}
	for key, def := range defs {
		if out[def.Profile] == nil {
			return nil, fmt.Errorf("the option dictionary gives %s no profile kind; the engine predates option profile kinds", key)
		}
		out[def.Profile][key] = def.Default
	}
	return out, nil
}

func OptionKind(key string) string { return configKinds[key] }

func ProfileRules() *Rules { return &profileRules }

func roundRule(x float64) float64 { return math.Round(x*profileRules.Round) / profileRules.Round }

func ruleHeight(rule string, heights []float64, nozzle float64) float64 {
	r, ok := profileRules.HeightRules[rule]
	if !ok || len(heights) == 0 {
		return 0
	}
	if r.CoarsestOfNozzle > 0 {
		best := heights[0]
		for _, h := range heights {
			if h <= roundRule(r.CoarsestOfNozzle*nozzle)+1e-6 {
				best = h
			}
		}
		return best
	}
	best, target := heights[0], r.NearestOfNozzle*nozzle
	for _, h := range heights {
		if math.Abs(h-target) < math.Abs(best-target)-1e-9 {
			best = h
		}
	}
	return best
}

func clampHeight(layerHeight, nozzle float64) float64 {
	if nozzle > 0 {
		return math.Min(layerHeight, roundRule(nozzle*profileRules.MaxLayerOfNozzle))
	}
	return layerHeight
}
func isLineWidth(key string) bool {
	return key == "line_width" || strings.HasSuffix(key, "_line_width")
}

func scaledValue(row Scale, nozzle, layerHeight float64) float64 {
	h := clampHeight(layerHeight, nozzle)
	switch row.Basis {
	case "nozzle":
		v := row.Factor * nozzle
		if isLineWidth(row.Key) {
			v = math.Min(profileRules.LineWidthOfNozzle[1]*nozzle, math.Max(profileRules.LineWidthOfNozzle[0]*nozzle, v))
		}
		return roundRule(v)
	case "layer":
		v := row.Factor * h
		if isLineWidth(row.Key) {
			v = math.Min(profileRules.LineWidthOfNozzle[1]*nozzle, math.Max(profileRules.LineWidthOfNozzle[0]*nozzle, v))
		}
		return roundRule(v)
	}
	return math.Ceil(row.Factor/h - 1e-6)
}

func scaleValues(rows []Scale, nozzle, layerHeight float64) map[string]string {
	out := map[string]string{}
	if !(nozzle > 0) || !(layerHeight > 0) {
		return out
	}
	for _, row := range rows {
		out[row.Key] = strconv.FormatFloat(scaledValue(row, nozzle, layerHeight), 'f', -1, 64)
	}
	return out
}

func heightsFor(nozzle, min, max float64) []float64 {
	hr := profileRules.Heights
	lo := min
	if !(lo > 0) {
		lo = math.Max(hr.MinFloor, math.Round(nozzle*hr.MinOfNozzle*100)/100)
	}
	hi := nozzle * hr.MaxOfNozzle
	if max > 0 {
		hi = max
	}
	hi = math.Min(hi, nozzle*profileRules.MaxLayerOfNozzle)
	out := []float64{}
	for h := lo; h <= hi+1e-6; h += hr.Step {
		out = append(out, math.Round(h*100)/100)
	}
	if len(out) == 0 {
		out = append(out, lo)
	}
	return out
}
