package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
)

//go:embed method_capabilities.json
var methodTableJSON []byte

type methodRules struct {
	KinematicsAxes map[string]int `json:"kinematicsAxes"`
	Methods        []struct {
		ID        string `json:"id"`
		Target    string `json:"target"`
		Hardware  string `json:"hardware"`
		Readiness string `json:"readiness"`
		Disabled  string `json:"disabled"`
	} `json:"methods"`
}

var methodTable = func() methodRules {
	var table methodRules
	if err := json.Unmarshal(methodTableJSON, &table); err != nil {
		panic(err)
	}
	return table
}()

type methodDecision struct {
	ID        string `json:"id"`
	Target    string `json:"target"`
	Supported bool   `json:"supported"`
	Reason    string `json:"reason,omitempty"`
	Recovery  string `json:"recovery,omitempty"`
}
type methodProblem struct {
	Message          string   `json:"error"`
	Code             string   `json:"code"`
	Method           string   `json:"method"`
	Recovery         string   `json:"recovery"`
	AllowedModes     []string `json:"allowed_modes"`
	AllowedWaveModes []string `json:"allowed_wave_modes"`
}

var methodDigest = regexp.MustCompile(`(?i)^[a-f0-9]{64}$`)

func positiveNozzles(value any) bool {
	var values []any
	switch v := value.(type) {
	case []any:
		values = v
	case string:
		for _, s := range strings.FieldsFunc(v, func(r rune) bool { return r == ',' || r == ';' }) {
			values = append(values, s)
		}
	default:
		values = []any{value}
	}
	if len(values) == 0 {
		return false
	}
	for _, value := range values {
		n, err := strconv.ParseFloat(fmt.Sprint(value), 64)
		if err != nil || n <= 0 || math.IsNaN(n) || math.IsInf(n, 0) {
			return false
		}
	}
	return true
}
func (s *Server) methodDecisions(printer map[string]any) []methodDecision {
	axes := methodTable.KinematicsAxes[fmt.Sprint(printer["nonplanar_kinematics"])]
	known := printer["printer_technology"] == "FFF" && axes > 0 && positiveNozzles(printer["nozzle_diameter"])
	rotary := known && printer["nonplanar_kinematics"] != "none" && axes >= 4
	artifact := map[string]any{}
	if h := s.jobs.cfg.NativeHealth; h != nil {
		artifact, _ = h.snapshot(s.cfg.Tool)["artifact_manifest"].(map[string]any)
	}
	ready, ok := artifact["preview_ready"].(bool)
	if !ok {
		ready = artifact["validated"] == true
	}
	ready = ready && methodDigest.MatchString(fmt.Sprint(artifact["binary_sha256"])) && methodDigest.MatchString(fmt.Sprint(artifact["resources_sha256"]))
	af := map[string]bool{}
	if raw, err := json.Marshal(artifact["features"]); err == nil {
		json.Unmarshal(raw, &af)
	}
	waveModes := []string{}
	if raw, err := json.Marshal(artifact["wave_modes"]); err == nil {
		json.Unmarshal(raw, &waveModes)
	}
	decisions := []methodDecision{}
	for _, rule := range methodTable.Methods {
		d := methodDecision{ID: rule.ID, Target: rule.Target, Supported: true}
		if rule.ID != "off" {
			switch {
			case rule.Disabled != "":
				d.Reason = rule.Disabled
			case !known:
				d.Reason = "Resolve the printer axes, kinematics and nozzle before using this method."
				d.Recovery = "repair_printer"
			case rule.Hardware == "rotary" && !rotary:
				d.Reason = "This method requires configured rotary axes."
			case rule.Readiness != "" && (!ready || !af[rule.Readiness] || rule.Target == "wave" && !slices.Contains(waveModes, rule.ID)):
				d.Reason = "This method is not ready in the current engine bundle."
				d.Recovery = "update_engine"
			}
		}
		if d.Reason != "" {
			d.Supported = false
			if d.Recovery == "" {
				d.Recovery = "change_method"
			}
		}
		decisions = append(decisions, d)
	}
	return decisions
}
func decodeMethodConfig(raw map[string]json.RawMessage) map[string]any {
	result := map[string]any{}
	for k, v := range raw {
		var value any
		if json.Unmarshal(v, &value) == nil {
			result[k] = value
		}
	}
	return result
}
func methodSelection(config map[string]any, decisions []methodDecision) *methodProblem {
	allowed, waves := []string{}, []string{}
	for _, d := range decisions {
		if d.Supported {
			if d.Target == "mode" {
				allowed = append(allowed, d.ID)
			} else {
				waves = append(waves, d.ID)
			}
		}
	}
	for _, selection := range [][2]string{{"mode", "nonplanar_mode"}, {"wave", "nonplanar_wave"}} {
		id := config[selection[1]]
		if id == nil || id == "off" {
			continue
		}
		reason, recovery := "Hmm, the saved slicing method isn't supported by this printer and engine.", "change_method"
		supported := false
		for _, d := range decisions {
			if d.Target == selection[0] && d.ID == id {
				supported = d.Supported
				if !supported {
					reason, recovery = d.Reason, d.Recovery
				}
				break
			}
		}
		if !supported {
			return &methodProblem{reason, "mode_incompatible", fmt.Sprint(id), recovery, allowed, waves}
		}
	}
	return nil
}
func (s *Server) validateJobMethods(j *Job, req JobRequest, presets *jobPresets) *methodProblem {
	if req.SupportFins || req.Output == "3mf" || overlayOutput(req.Output) || req.Orient || req.Arrange || req.WipeTower || req.Explode || req.Cut != "" || req.MeshOp != "" {
		return nil
	}
	printer := decodeMethodConfig(presets.machine.Config)
	config := decodeMethodConfig(presets.process.Config)
	var projectScopes []map[string]any
	if !req.Plate && len(req.Files) > 0 && strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
		saved, scopes, err := savedProjectMethods(filepath.Join(j.Dir, "input", req.Files[0]))
		if err != nil {
			return &methodProblem{Message: "Oops! Couldn't read the saved project's slicing methods — repair or reimport the project before slicing.", Code: "mode_incompatible", Recovery: "change_method"}
		}
		for key, value := range saved {
			config[key] = value
		}
		projectScopes = scopes
	}

	for k, v := range printer {
		config[k] = v
	}
	for _, pairs := range [][][2]string{req.Overrides, req.ProjectSettings} {
		for _, kv := range pairs {
			config[kv[0]] = kv[1]
			printer[kv[0]] = kv[1]
		}
	}
	if req.ForcePlanar {
		config["nonplanar_mode"] = "off"
	}
	decisions := s.methodDecisions(printer)
	if p := methodSelection(config, decisions); p != nil {
		return p
	}
	for _, scope := range projectScopes {
		effective := map[string]any{}
		for key, value := range config {
			effective[key] = value
		}
		for key, value := range scope {
			effective[key] = value
		}
		for _, kv := range req.ProjectSettings {
			effective[kv[0]] = kv[1]
		}
		if req.ForcePlanar {
			effective["nonplanar_mode"] = "off"
		}
		if p := methodSelection(effective, decisions); p != nil {
			return p
		}
	}
	var walk func(any, map[string]any) *methodProblem
	walk = func(node any, parent map[string]any) *methodProblem {
		switch v := node.(type) {
		case []any:
			for _, child := range v {
				if p := walk(child, parent); p != nil {
					return p
				}
			}
		case map[string]any:
			effective := map[string]any{}
			for k, value := range parent {
				effective[k] = value
			}
			if settings, ok := v["settings"].(map[string]any); ok {
				for k, value := range settings {
					effective[k] = value
				}
			}
			for _, kv := range req.ProjectSettings {
				effective[kv[0]] = kv[1]
			}
			if req.ForcePlanar {
				effective["nonplanar_mode"] = "off"
			}
			if p := methodSelection(effective, decisions); p != nil {
				return p
			}
			for _, key := range []string{"plates", "objects", "volumes", "ranges", "edits"} {
				if p := walk(v[key], effective); p != nil {
					return p
				}
			}
		}
		return nil
	}
	if req.Plate || req.Edits {
		name := "plate.json"
		if !req.Plate {
			name = "edits.json"
		}
		raw, err := os.ReadFile(filepath.Join(j.Dir, "input", name))
		if err == nil {
			var plate any
			if json.Unmarshal(raw, &plate) == nil {
				return walk(plate, config)
			}
		}
	}
	return nil
}
