package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type profileProblem struct {
	Message  string           `json:"error"`
	Code     string           `json:"code"`
	Recovery string           `json:"recovery"`
	Profile  profileReference `json:"profile"`
	status   int
}
type profileReference struct {
	Kind   string `json:"kind"`
	Name   string `json:"name"`
	Vendor string `json:"vendor"`
	Slot   int    `json:"slot,omitempty"`
}

func (p *profileProblem) write(w http.ResponseWriter) { writeJSON(w, p.status, p) }
func profileSelectionProblem(code, kind, name, vendor string, slot int) *profileProblem {
	if name != "" {
		name = presetDisplayName(name)
	}
	p := &profileProblem{Code: code, Recovery: "select_profile", Profile: profileReference{kind, name, vendor, slot}, status: 400}
	if code == "profile_missing" {
		p.Message = "Choose a " + kind + " profile before continuing."
	} else {
		p.Message = fmt.Sprintf("Oops! The selected %s profile %q isn't available. Pick another one or import it again.", kind, presetDisplayName(name))
	}
	return p
}
type jobPresets struct {
	machine, process *resolve.Preset
	filaments        []*resolve.Preset
}

func (s *Server) resolveJobProfiles(req *JobRequest) (*jobPresets, *profileProblem) {
	for _, selection := range []struct{ kind, name string }{{"printer", req.Printer}, {"process", req.Process}} {
		if selection.name == "" {
			return nil, profileSelectionProblem("profile_missing", selection.kind, "", "", 0)
		}
	}
	if len(req.Filaments) == 0 {
		return nil, profileSelectionProblem("profile_missing", "filament", "", "", 1)
	}
	out := &jobPresets{machine: s.catalog.Machine(req.Printer), process: s.catalog.Resolve("prints", req.Process, req.Printer)}
	if out.machine == nil {
		return nil, profileSelectionProblem("profile_unknown", "printer", req.Printer, "", 0)
	}
	if out.process == nil {
		return nil, profileSelectionProblem("profile_unknown", "process", req.Process, "", 0)
	}
	for i, name := range req.Filaments {
		f := s.catalog.Resolve("filaments", name, req.Printer)
		if f == nil {
			return nil, profileSelectionProblem("profile_unknown", "filament", name, "", i+1)
		}
		out.filaments = append(out.filaments, f)
	}
	return out, nil
}

type submittedConfig struct {
	Library   *resolve.Request `json:"library,omitempty"`
	Schema    int             `json:"schema"`
	Catalog   string          `json:"catalog"`
	Printer   string          `json:"printer"`
	Process   string          `json:"process"`
	Filaments []string        `json:"filaments"`
	Profiles  struct {
		Printer   *resolve.Preset   `json:"printer"`
		Process   *resolve.Preset   `json:"process"`
		Filaments []*resolve.Preset `json:"filaments"`
	} `json:"profiles"`
	Overrides         [][2]string       `json:"overrides"`
	ProjectSettings   [][2]string       `json:"project_settings,omitempty"`
	InputConfigSHA256 map[string]string `json:"input_config_sha256,omitempty"`
	Artifact          map[string]any    `json:"artifact"`
	ConfigSHA256      string            `json:"config_sha256"`
}

func (s *Server) captureSubmission(j *Job, req *JobRequest, presets *jobPresets) error {
	snapshot := &submittedConfig{Schema: 2, Catalog: s.catalog.Version, Printer: req.Printer, Process: req.Process, Filaments: append([]string{}, req.Filaments...), Overrides: append([][2]string{}, req.Overrides...), ProjectSettings: append([][2]string{}, req.ProjectSettings...), InputConfigSHA256: map[string]string{}, Artifact: map[string]any{}}
	snapshot.Library = req.Library
	snapshot.Profiles.Printer, snapshot.Profiles.Process, snapshot.Profiles.Filaments = presets.machine, presets.process, presets.filaments
	if health := s.jobs.cfg.NativeHealth; health != nil {
		if artifact, ok := health.snapshot(s.cfg.Tool)["artifact_manifest"].(map[string]any); ok {
			for _, key := range []string{"backend", "source_sha", "binary_sha256", "resources_sha256"} {
				snapshot.Artifact[key] = artifact[key]
			}
		}
	}
	for _, name := range []string{"plate.json", "edits.json"} {
		path := filepath.Join(j.Dir, "input", name)
		if _, err := os.Stat(path); os.IsNotExist(err) {
			continue
		}
		digest, err := bundleFileHash(path)
		if err != nil {
			return err
		}
		snapshot.InputConfigSHA256[name] = digest
	}
	encoded, err := json.Marshal(snapshot)
	if err != nil {
		return err
	}
	digest := sha256.Sum256(encoded)
	snapshot.ConfigSHA256 = hex.EncodeToString(digest[:])
	req.Submission = snapshot
	return nil
}
