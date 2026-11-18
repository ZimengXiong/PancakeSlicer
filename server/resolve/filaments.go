package resolve

import (
	"encoding/json"
	"fmt"
	"strings"
)

func filamentVectorValues(key string, raw json.RawMessage) ([]string, error) {
	var value any
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return nil, err
	}
	if array, ok := value.([]any); ok {
		out := make([]string, len(array))
		for i, item := range array {
			text, err := ScalarConfigValue(item)
			if err != nil {
				return nil, err
			}
			out[i] = text
		}
		return out, nil
	}
	text, err := ScalarConfigValue(value)
	if err != nil {
		return nil, err
	}
	if configKinds[key] == "strings" {
		return NativeStrings(text), nil
	}
	if configKinds[key] == "groups" {
		return strings.Split(text, "#"), nil
	}
	return strings.Split(text, ","), nil
}

func assembleFilamentConfig(configs []map[string]json.RawMessage, dictionary []byte) (map[string]json.RawMessage, error) {
	var defs map[string]struct {
		Type    string          `json:"type"`
		Default json.RawMessage `json:"default"`
	}
	if err := json.Unmarshal(dictionary, &defs); err != nil {
		return nil, fmt.Errorf("filament option metadata: %w", err)
	}
	variants := make([][]string, len(configs))
	keys := map[string]bool{}
	for i, config := range configs {
		for key := range config {
			if _, ok := defs[key]; ok {
				keys[key] = true
			}
		}
		if raw := config["filament_extruder_variant"]; len(raw) > 0 {
			values, err := filamentVectorValues("filament_extruder_variant", raw)
			if err != nil {
				return nil, fmt.Errorf("filament %d variants: %w", i+1, err)
			}
			variants[i] = values
		}
		if len(variants[i]) == 0 {
			variants[i] = []string{"Direct Drive Standard"}
		}
	}
	out := map[string]json.RawMessage{}
	for key := range keys {
		def := defs[key]
		vector := strings.HasSuffix(def.Type, "s") || filamentVariantKeys[key]
		if !vector {
			if len(configs) > 0 {
				out[key] = configs[0][key]
			}
			continue
		}
		values := []string{}
		for i, config := range configs {
			raw := config[key]
			if len(raw) == 0 {
				raw = def.Default
			}
			if len(raw) == 0 {
				return nil, fmt.Errorf("filament %d has no value or default for %s", i+1, key)
			}
			parts, err := filamentVectorValues(key, raw)
			if err != nil {
				return nil, fmt.Errorf("filament %d %s: %w", i+1, key, err)
			}
			count := 1
			if filamentVariantKeys[key] {
				count = len(variants[i])
			}
			for j := 0; j < count; j++ {
				value := ""
				if len(parts) > 0 {
					value = parts[0]
					if j < len(parts) {
						value = parts[j]
					}
				}
				values = append(values, value)
			}
		}
		out[key], _ = json.Marshal(values)
	}
	self := []int{}
	names := []string{}
	for i, list := range variants {
		for _, name := range list {
			self = append(self, i+1)
			names = append(names, name)
		}
	}
	out["filament_self_index"], _ = json.Marshal(self)
	out["filament_extruder_variant"], _ = json.Marshal(names)
	return out, nil
}
