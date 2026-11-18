package resolve

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"
)

func FirstConfigText(raw json.RawMessage) string {
	var list []any
	if json.Unmarshal(raw, &list) == nil {
		if len(list) == 0 {
			return ""
		}
		return strings.TrimSpace(fmt.Sprint(list[0]))
	}
	var scalar any
	json.Unmarshal(raw, &scalar)
	if scalar == nil {
		return ""
	}
	text, _, _ := strings.Cut(fmt.Sprint(scalar), ",")
	return strings.Trim(strings.TrimSpace(text), `"`)
}

func SameMm(a, b float64) bool { return math.Abs(a-b) < 0.0005 }

func must(b []byte, _ error) []byte { return b }
