package resolve

import (
	"encoding/json"
	"errors"
	"strings"
	"unicode"
)

var hiddenSettings = substrings{"print_host", "printhost", "printer_agent", "host_type", "api_key", "bbl_use_printhost", "upload", "cloud", "device", "octoprint", "_plugin_config_overrides"}

type substrings []string

func HiddenSetting(key string) bool { return hiddenSettings.match(key) }

func (s substrings) match(key string) bool {
	for _, part := range s {
		if strings.Contains(key, part) {
			return true
		}
	}
	return false
}

func ScalarConfigValue(value any) (string, error) {
	switch v := value.(type) {
	case string:
		if len(v) > 32<<10 {
			return "", errors.New("Text value exceeds the 32 KiB limit.")
		}
		if strings.IndexFunc(v, func(r rune) bool { return unicode.IsControl(r) && r != '\n' && r != '\r' && r != '\t' }) >= 0 {
			return "", errors.New("Value contains invalid control characters.")
		}
		return v, nil
	case json.Number:
		if len(v) > 32<<10 {
			return "", errors.New("Text value exceeds the 32 KiB limit.")
		}
		return string(v), nil
	case bool:
		if v {
			return "1", nil
		}
		return "0", nil
	default:
		return "", errors.New("Expected a string, number, boolean, or flat list.")
	}
}

var renamedKeys = map[string]string{"nonplanar_curved_shells": "nonplanar_curved_top_layers"}

func CanonicalKey(key string) string {
	if to, ok := renamedKeys[key]; ok {
		return to
	}
	return key
}

var renamedValues = map[string]map[string]string{
	"nonplanar_head": {"bambu-h2d-left": "bambu-h2d", "bambu-h2d-right": "bambu-h2d"},
}

func CanonicalText(key, value string) string {
	if to, ok := renamedValues[key][value]; ok {
		return to
	}
	return value
}

func CanonicalValue(key string, value json.RawMessage) json.RawMessage {
	if renamedValues[key] == nil {
		return value
	}
	var text string
	if json.Unmarshal(value, &text) == nil {
		if to := CanonicalText(key, text); to != text {
			return must(json.Marshal(to))
		}
		return value
	}
	var list []string
	if json.Unmarshal(value, &list) != nil {
		return value
	}
	changed := false
	for i, t := range list {
		if to := CanonicalText(key, t); to != t {
			list[i], changed = to, true
		}
	}
	if !changed {
		return value
	}
	return must(json.Marshal(list))
}

func RenamedValue(key string) bool { return renamedValues[key] != nil }

func RenamedKeys() []string {
	keys := []string{}
	for k := range renamedValues {
		keys = append(keys, k)
	}
	return keys
}

const FastCurvedTops, CurvedTops = "curved_tops", "nonplanar_top"

func NativeStrings(value string) []string {
	if value == "" {
		return []string{}
	}
	var result []string
	var part strings.Builder
	quoted, escaped := false, false
	for _, ch := range value {
		if escaped {
			switch ch {
			case 'n':
				ch = '\n'
			case 'r':
				ch = '\r'
			}
			part.WriteRune(ch)
			escaped = false
		} else {
			switch ch {
			case '\\':
				escaped = true
			case '"':
				quoted = !quoted
			case ';':
				if quoted {
					part.WriteRune(ch)
				} else {
					result = append(result, part.String())
					part.Reset()
				}
			default:
				part.WriteRune(ch)
			}
		}
	}
	if escaped {
		part.WriteRune('\\')
	}
	return append(result, part.String())
}
