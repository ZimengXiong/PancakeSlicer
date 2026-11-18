package resolve

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
)

type Scale struct {
	Key    string  `json:"key"`
	Basis  string  `json:"basis"`
	Factor float64 `json:"factor"`
}

type AutoProfile struct {
	Schema          int         `json:"schema"`
	Scales          []Scale     `json:"scales"`
	PreferredHeight *float64    `json:"preferredHeight,omitempty"`
	HeightRule      string      `json:"heightRule,omitempty"`
	SpeedModel      *SpeedModel `json:"speedModel,omitempty"`
}

var settingKeyShape = regexp.MustCompile(`^[a-z][a-z0-9_]{0,63}$`)

func ValidateAuto(p map[string]json.RawMessage, typ string) error {
	raw, has := p["auto"]
	if _, linked := p["parentId"]; linked && typ == "process" {
		return errors.New("Print profiles don't use a base template—duplicating creates a standalone copy.")
	}
	if !has {
		return nil
	}
	if typ != "process" {
		return errors.New("auto belongs to print profiles only")
	}
	var a AutoProfile
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if dec.Decode(&a) != nil || a.Schema != 3 || a.Scales == nil {
		return errors.New("auto must be {schema:3, scales, preferredHeight?}")
	}
	if len(a.Scales) > 64 {
		return errors.New("auto.scales holds at most 64 rows")
	}
	if err := validateSpeedModel(a.SpeedModel); err != nil {
		return err
	}
	seen := map[string]bool{}
	for _, row := range a.Scales {
		if !settingKeyShape.MatchString(row.Key) || seen[row.Key] || (row.Basis != "nozzle" && row.Basis != "layer" && row.Basis != "perLayer") || !(row.Factor > 0) || row.Factor > 1000 {
			return fmt.Errorf("auto.scales row %q needs a setting key used once, a basis of nozzle, layer or perLayer, and a factor above 0 and at most 1000", row.Key)
		}
		seen[row.Key] = true
	}
	if a.PreferredHeight != nil && (*a.PreferredHeight < 0.04 || *a.PreferredHeight > 1.2) {
		return errors.New("preferredHeight is a layer height from 0.04 to 1.2 mm")
	}
	if _, ok := profileRules.HeightRules[a.HeightRule]; a.HeightRule != "" && (!ok || a.PreferredHeight != nil) {
		return errors.New("heightRule is draft or fine, and never beside preferredHeight")
	}
	return nil
}
