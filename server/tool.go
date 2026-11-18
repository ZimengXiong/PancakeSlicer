package main

import (
	"errors"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type toolOption struct {
	Key  string `json:"key"`
	Type string `json:"type"`
	Mode int    `json:"mode"`
}

func toolDump(c Config, args ...string) ([]byte, error) {
	dir, err := os.MkdirTemp("", "orca-catalog-*")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "catalog.json")
	args = append(args, path, "--profiles", filepath.Join(c.Resources, "profiles"), "--datadir", filepath.Join(dir, "data"))
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	cmd := exec.CommandContext(ctx, c.Tool, args...)
	cmd.Env = engineEnv(dir)
	configureKill(cmd)
	cmd.WaitDelay = 5 * time.Second
	if output, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("tool catalog: %w: %.1000s", err, output)
	}
	return os.ReadFile(path)
}

func loadOptions(c Config) (*cachedResponse, error) {
	b, err := toolDump(c, "--dump-options")
	if err != nil {
		return nil, fmt.Errorf("the engine cannot dump its options: %w", err)
	}
	var dump struct {
		Schema  string            `json:"schema"`
		Options []json.RawMessage `json:"options"`
	}
	if err := json.Unmarshal(b, &dump); err != nil || dump.Schema != "orca.options.v1" || len(dump.Options) == 0 {
		return nil, errors.New("the engine's options dump is not orca.options.v1")
	}
	options := map[string]map[string]any{}
	for _, raw := range dump.Options {
		var option toolOption
		var value map[string]any
		if json.Unmarshal(raw, &option) != nil || json.Unmarshal(raw, &value) != nil || option.Key == "" || option.Type == "" {
			return nil, errors.New("the engine's options dump has an invalid entry")
		}
		if resolve.HiddenSetting(option.Key) {
			continue
		}
		types := map[string]string{"float_or_percent": "FloatOrPercent", "floats_or_percents": "FloatsOrPercents", "points_groups": "PointsGroups", "ints_groups": "IntsGroups"}
		typ := types[option.Type]
		if typ == "" {
			typ = strings.ToUpper(option.Type[:1]) + option.Type[1:]
		}
		value["type"] = typ
		if option.Type == "strings" {
			if serialized, ok := value["default"].(string); ok {
				value["default"] = resolve.NativeStrings(serialized)
			}
		}
		mode := "Develop"
		switch option.Mode {
		case 0:
			mode = "Simple"
		case 1:
			mode = "Advanced"
		case 2:
			mode = "Expert"
		}
		value["mode"] = mode
		options[option.Key] = value
	}
	response, err := cacheJSON(options)
	if err != nil {
		return nil, err
	}
	log.Printf("engine option dictionary: %d options", len(options))
	return response, nil
}
