package resolve

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"
)

func productID(name string) string {
	if i := strings.Index(name, " @"); i >= 0 {
		name = name[:i]
	}
	return strings.TrimSpace(name)
}

func (c *Catalog) foldedBase(alias string) (map[string]json.RawMessage, error) {
	into := c.FoldedInto(alias)
	if into == "" {
		return nil, fmt.Errorf("%s is not a folded filament", alias)
	}
	p := c.Filaments[into]
	var first *Tuning
	for i := range p.Tunings {
		t := &p.Tunings[i]
		if productID(t.Source.Name) == alias && (first == nil || t.Source.Name < first.Source.Name) {
			first = t
		}
	}
	if first == nil {
		return nil, fmt.Errorf("Hmm, %s doesn't have any tuning from %s's presets", into, alias)
	}
	config, _ := layered(p.Base, first.Values, "tuning")
	projectVariants(config, p.Base, first.Values)
	return config, nil
}

func (c *Catalog) FoldDiff(product, variant string) (map[string]json.RawMessage, error) {
	into, alias := product, ""
	if c.Filaments[product] == nil {
		into, alias = c.FoldedInto(product), product
	}
	p := c.Filaments[into]
	if p == nil || len(p.Aliases) == 0 {
		return nil, nil
	}
	now := c.Resolve("filaments", into, variant)
	t := p.tuning(variant)
	folded := t != nil && slices.Contains(p.Aliases, productID(t.Source.Name))
	var before map[string]json.RawMessage
	switch {
	case alias != "" && folded && productID(t.Source.Name) == alias, alias == "" && !folded:
		return map[string]json.RawMessage{}, nil
	case alias != "":
		base, err := c.foldedBase(alias)
		if err != nil {
			return nil, err
		}
		before = base
	default:
		before, _ = layered(p.Base, nil, "tuning")
	}
	out := map[string]json.RawMessage{}
	for k, v := range before {
		if !HiddenSetting(k) && string(now.Config[k]) != string(v) {
			out[k] = listedValue(vectors, k, v)
		}
	}
	return out, nil
}

func (c *Catalog) FoldTarget(id string) bool {
	p := c.Filaments[id]
	return p != nil && len(p.Aliases) > 0
}
