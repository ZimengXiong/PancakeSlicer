package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"slices"
)

type EngineBuild struct {
	Manifest    []byte
	Binary      string
	Resources   string
	Qualified   bool
	Unqualified string
	Outputs     map[string]bool
}

func checkEngineBuild(c *Config) error {
	if c.SurfaceManifest == "" {
		return fmt.Errorf("no engine build manifest: start the service with -surface-manifest (ORCA_WEB_SURFACE_MANIFEST) naming the manifest shipped with engine %s", c.Tool)
	}
	raw, err := os.ReadFile(c.SurfaceManifest)
	if err != nil {
		return fmt.Errorf("engine build manifest unreadable: %w", err)
	}
	var manifest struct {
		Source    string `json:"source_sha"`
		Binary    string `json:"binary_sha256"`
		Resources string `json:"resources_sha256"`
	}
	if err := json.Unmarshal(raw, &manifest); err != nil || manifest.Binary == "" || manifest.Resources == "" {
		return fmt.Errorf("engine build manifest %s has no binary_sha256 and resources_sha256", c.SurfaceManifest)
	}
	binary, err := bundleFileHash(c.Tool)
	if err != nil {
		return fmt.Errorf("engine %s unreadable: %w", c.Tool, err)
	}
	resources, err := bundleResourceHash(c.Resources)
	if err != nil {
		return fmt.Errorf("engine resources %s unreadable: %w", c.Resources, err)
	}
	if binary != manifest.Binary {
		return fmt.Errorf("engine %s is not the engine this service was built with (sha256 %s, manifest %s names %s for engine %s); install the engine from the same release as the service", c.Tool, binary, c.SurfaceManifest, manifest.Binary, manifest.Source)
	}
	if resources != manifest.Resources {
		return fmt.Errorf("engine resources %s are not the ones this service was built with (sha256 %s, manifest %s names %s for engine %s); install the resources from the same release as the service", c.Resources, resources, c.SurfaceManifest, manifest.Resources, manifest.Source)
	}
	c.Engine = EngineBuild{Manifest: raw, Binary: binary, Resources: resources}
	c.Engine.Outputs, c.Engine.Unqualified = qualifyEngine(c.ArtifactQualification, raw, binary, resources)
	c.Engine.Qualified = c.Engine.Unqualified == ""
	return nil
}

func qualifyEngine(path string, raw []byte, binary, resources string) (map[string]bool, string) {
	var manifest struct {
		Schema          int               `json:"schema"`
		PublicationMain string            `json:"publication_main_sha"`
		Platform        *artifactPlatform `json:"platform"`
		Source          string            `json:"source_sha"`
		PrintQualified  bool              `json:"print_qualified"`
	}
	var proof struct {
		Ready           bool              `json:"ready"`
		PublicationMain string            `json:"publication_main_sha"`
		Platform        *artifactPlatform `json:"platform"`
		Source          string            `json:"source_sha"`
		Binary          string            `json:"binary_sha256"`
		Resources       string            `json:"resources_sha256"`
		Manifest        string            `json:"manifest_sha256"`
	}
	json.Unmarshal(raw, &manifest)
	if path == "" {
		return nil, "no qualification record: start the service with -artifact-qualification (ORCA_WEB_ARTIFACT_QUALIFICATION) naming the engine's artifact-qualification.ready"
	}
	proofRaw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Sprintf("qualification record %s unreadable: %v", path, err)
	}
	if err := json.Unmarshal(proofRaw, &proof); err != nil {
		return nil, fmt.Sprintf("qualification record %s is not JSON: %v", path, err)
	}
	manifestHash := sha256.Sum256(raw)
	running := runningPlatform()
	switch {
	case !proof.Ready:
		return nil, fmt.Sprintf("qualification record %s is not ready", path)
	case proof.Binary != binary:
		return nil, fmt.Sprintf("qualification record %s is for engine sha256 %s, not this engine's %s", path, proof.Binary, binary)
	case proof.Resources != resources:
		return nil, fmt.Sprintf("qualification record %s is for resources sha256 %s, not this engine's %s", path, proof.Resources, resources)
	case proof.Manifest != hex.EncodeToString(manifestHash[:]):
		return nil, fmt.Sprintf("qualification record %s is for manifest sha256 %s, not this engine's %s", path, proof.Manifest, hex.EncodeToString(manifestHash[:]))
	case manifest.Source == "" || proof.Source != manifest.Source:
		return nil, fmt.Sprintf("qualification record %s names engine source %q, the manifest %q", path, proof.Source, manifest.Source)
	case !manifest.PrintQualified:
		return nil, "the engine's manifest says it is not print-qualified"
	case manifest.Schema >= 3 && (manifest.Platform == nil || *manifest.Platform != running || proof.Platform == nil || *proof.Platform != *manifest.Platform):
		return nil, fmt.Sprintf("the engine was qualified on %v, not on this %s/%s", manifest.Platform, running.OS, running.Arch)
	case manifest.Schema >= 3 && (manifest.PublicationMain == "" || proof.PublicationMain != manifest.PublicationMain):
		return nil, fmt.Sprintf("qualification record %s names publication %q, the manifest %q", path, proof.PublicationMain, manifest.PublicationMain)
	}
	outputs, failed := nativeOutputHashes(raw)
	if failed > 0 {
		return nil, "a reference output is missing, failed its audit or has clearance hits"
	}
	return outputs, ""
}

func checkMethodModes(options *cachedResponse) error {
	var dict map[string]struct {
		Values []string `json:"enum_values"`
	}
	if err := json.Unmarshal(options.plain, &dict); err != nil {
		return err
	}
	for _, rule := range methodTable.Methods {
		key := map[string]string{"mode": "nonplanar_mode", "wave": "nonplanar_wave"}[rule.Target]
		if key != "" && rule.Disabled == "" && !slices.Contains(dict[key].Values, rule.ID) {
			return fmt.Errorf("the engine's %s has no %q, which this build's method table offers", key, rule.ID)
		}
	}
	return nil
}

var engineEnvKeys = []string{"PATH", "HOME", "USER", "LD_LIBRARY_PATH", "LANG",
	"SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "PATHEXT", "USERNAME", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "PROGRAMDATA"}

func engineEnv(tmp string) []string {
	env := []string{"TMPDIR=" + tmp, "TMP=" + tmp, "TEMP=" + tmp}
	for _, key := range engineEnvKeys {
		if v, ok := os.LookupEnv(key); ok {
			env = append(env, key+"="+v)
		}
	}
	return env
}
