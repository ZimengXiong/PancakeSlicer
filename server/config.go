package main

import (
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strings"
	"unicode"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)


var settingKey = regexp.MustCompile(`^[a-zA-Z][a-zA-Z0-9_]*$`)

func overrides(raw string) ([][2]string, error) {
	if len(raw) > 256<<10 {
		return nil, errors.New("Settings overrides exceed the 256 KiB limit.")
	}
	var values map[string]json.RawMessage
	if err := json.Unmarshal([]byte(raw), &values); err != nil || values == nil {
		return nil, errors.New("Settings overrides must be a JSON object.")
	}
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	out := make([][2]string, 0, len(keys))
	for _, saved := range keys {
		key := resolve.CanonicalKey(saved)
		if _, both := values[key]; both && key != saved {
			continue
		}
		if !settingKey.MatchString(key) || resolve.HiddenSetting(key) || resolve.OptionKind(key) == "" {
			return nil, fmt.Errorf("Unrecognized setting override key: %q", key)
		}
		value, err := configValue(key, values[saved])
		if err == nil && len(value) > 64<<10 {
			err = errors.New("Setting value exceeds the 64 KiB limit.")
		}
		if err != nil {
			return nil, fmt.Errorf("override %s: %w", key, err)
		}
		out = append(out, [2]string{key, value})
	}
	return migrateFastCurvedTops(out), nil
}

func escapeConfigString(value string) string {
	return strings.NewReplacer("\\", "\\\\", "\"", "\\\"", "\r", "\\r", "\n", "\\n").Replace(value)
}

func configValue(key string, raw json.RawMessage) (string, error) {
	var value any
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil {
		return "", err
	}
	kind := resolve.OptionKind(key)
	if array, ok := value.([]any); ok {
		if kind == "string" {
			return "", errors.New("This text setting expects a single value.")
		}
		parts := make([]string, len(array))
		for i, element := range array {
			part, err := resolve.ScalarConfigValue(element)
			if err != nil {
				return "", err
			}
			if kind != "strings" && strings.IndexFunc(part, unicode.IsControl) >= 0 {
				return "", errors.New("Value contains invalid control characters.")
			}
			if kind == "strings" && ((len(array) == 1 && part == "") || strings.ContainsAny(part, " \t;\\\"\r\n")) {
				part = "\"" + escapeConfigString(part) + "\""
			}
			parts[i] = part
		}
		separator := ","
		if kind == "" && len(array) > 0 {
			if _, stringsOnly := array[0].(string); stringsOnly {
				separator = ";"
			}
		}
		if kind == "strings" {
			separator = ";"
		}
		if kind == "groups" {
			separator = "#"
		}
		return strings.Join(parts, separator), nil
	}
	scalar, err := resolve.ScalarConfigValue(value)
	if err != nil {
		return "", err
	}
	if kind != "string" && kind != "strings" && strings.IndexFunc(scalar, unicode.IsControl) >= 0 {
		return "", errors.New("Value contains invalid control characters.")
	}
	return scalar, nil
}

func migrateFastCurvedTops(pairs [][2]string) [][2]string {
	fast := false
	for i, kv := range pairs {
		if kv[0] == "nonplanar_mode" && kv[1] == resolve.FastCurvedTops {
			pairs[i][1], fast = resolve.CurvedTops, true
		}
	}
	if !fast {
		return pairs
	}
	out := pairs[:0]
	for _, kv := range pairs {
		if kv[0] != "nonplanar_curved_top_layers" {
			out = append(out, kv)
		}
	}
	return append(out, [2]string{"nonplanar_curved_top_layers", "1"})
}

func migratedMethod(mode string) string {
	if mode == resolve.FastCurvedTops {
		return resolve.CurvedTops
	}
	return mode
}
