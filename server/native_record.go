package main

import (
	"bufio"
	"context"
	"encoding/hex"
	"encoding/json"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

func nativeOutputHashes(raw []byte) (hashes map[string]bool, failed int) {
	var manifest map[string]json.RawMessage
	json.Unmarshal(raw, &manifest)
	var schema int
	json.Unmarshal(manifest["schema"], &schema)
	hashes = map[string]bool{}
	for _, name := range []string{"benchy", "dome", "wave"} {
		var proof struct {
			Hits     *int `json:"clearance_hit_moves"`
			Verifier *struct {
				Hits        *int `json:"hit_moves"`
				MachineHits *int `json:"machine_hit_moves"`
			} `json:"verifier"`
			Native  bool   `json:"native_qualified"`
			Audited bool   `json:"full_audit_qualified"`
			Passed  int    `json:"audit_checks_passed"`
			Total   int    `json:"audit_checks_total"`
			Gcode   string `json:"gcode_sha256"`
			Package string `json:"native_package_sha256"`
		}
		digest := func(h string) bool { b, err := hex.DecodeString(h); return err == nil && len(b) == 32 }
		zero := func(n *int) bool { return n != nil && *n == 0 }
		passed := json.Unmarshal(manifest[name], &proof) == nil && proof.Native && proof.Audited && proof.Total > 0 && proof.Passed == proof.Total
		if schema >= 3 && !(passed && digest(proof.Gcode) && digest(proof.Package) && zero(proof.Hits) && proof.Verifier != nil &&
			zero(proof.Verifier.Hits) && zero(proof.Verifier.MachineHits)) {
			failed++
			continue
		}
		if !passed {
			continue
		}
		for _, hash := range []string{proof.Gcode, proof.Package} {
			if digest(hash) {
				hashes[hash] = true
			}
		}
	}
	return hashes, failed
}

func nativeWaveResults(path string) []map[string]any {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	var results []map[string]any
	scan := bufio.NewScanner(f)
	scan.Buffer(make([]byte, 4096), 1<<20)
	for scan.Scan() {
		value, ok := strings.CutPrefix(scan.Text(), "; nonplanar_wave_result = ")
		if !ok {
			continue
		}
		var wave map[string]any
		if json.Unmarshal([]byte(strings.TrimSpace(value)), &wave) == nil && wave != nil {
			results = append(results, wave)
		}
	}
	return results
}

func bindNativeRecord(raw []byte, c Config, j *Job) []byte {
	var record map[string]json.RawMessage
	if json.Unmarshal(raw, &record) != nil || record == nil {
		return raw
	}
	if j.Request.Submission != nil {
		record["submission"], _ = json.Marshal(j.Request.Submission)
	}
	if c.NativeHealth != nil {
		snapshot := c.NativeHealth.snapshot(c.Tool)
		if artifact, ok := snapshot["artifact_manifest"].(map[string]any); ok && artifact != nil {
			var inputs map[string]any
			if json.Unmarshal(record["inputs"], &inputs) == nil && inputs != nil {
				if _, exists := inputs["checkout_source_sha"]; !exists {
					checkout, exists := inputs["checkout_sha"]
					if !exists {
						checkout = inputs["source_sha"]
					}
					if checkout != nil {
						inputs["checkout_source_sha"] = checkout
					}
				}
				inputs["source_sha"] = artifact["source_sha"]
				inputs["artifact"] = map[string]any{"backend": artifact["backend"], "source_sha": artifact["source_sha"], "binary_sha256": artifact["binary_sha256"], "resources_sha256": artifact["resources_sha256"]}
				record["inputs"], _ = json.Marshal(inputs)
			}
			if j.Request.Output == "gcode" || j.Request.Output == "gcode.3mf" {
				delete(record, "print_qualification")
				if hash, err := bundleFileHash(j.OutputPath(j.Request.Output)); err == nil && c.NativeHealth.qualifiedOutput(hash) {
					record["print_qualification"], _ = json.Marshal(map[string]any{"passed": true, "output_sha256": hash})
				}
			}
		}
	}
	if j.Request.Output == "gcode" || j.Request.Output == "gcode.3mf" {
		waves := nativeWaveResults(j.OutputPath("gcode"))
		if len(waves) > 0 {
			record["wave_results"], _ = json.Marshal(waves)
			var report map[string]any
			if json.Unmarshal(record["nonplanar_report"], &report) != nil || report == nil {
				report = map[string]any{}
			}
			report["wave_results"] = waves
			if objects, ok := report["objects"].([]any); ok {
				for _, wave := range waves {
					index, ok := wave["object"].(float64)
					if !ok || index < 0 || index >= float64(len(objects)) || index != float64(int(index)) {
						continue
					}
					if object, ok := objects[int(index)].(map[string]any); ok {
						object["wave_result"] = wave
						if object["mode"] == "off" {
							object["curved_share"] = nil
						}
					}
				}
			}
			var summary map[string]string
			json.Unmarshal(record["text_summary"], &summary)
			if summary["clearance_checked"] == "1" {
				clearance := map[string]any{"checked": true}
				if previous, ok := report["clearance"].(map[string]any); ok && previous["clearance_override"] == true {
					clearance["clearance_override"] = true
				}
				for target, key := range map[string]string{"hits": "clearance_hit_moves", "body_hits": "clearance_body_hit_moves", "machine_hits": "clearance_machine_hit_moves", "cone_graze_moves": "clearance_cone_graze_moves"} {
					if n, err := strconv.ParseUint(summary[key], 10, 64); err == nil {
						clearance[target] = n
					}
				}
				report["clearance"] = clearance
			}
			if value, err := strconv.ParseFloat(summary["estimated_time_s"], 64); err == nil {
				report["time_s"] = value
			}
			extrusion, e1 := strconv.ParseFloat(summary["nonplanar_extrude_time_s"], 64)
			other, e2 := strconv.ParseFloat(summary["nonplanar_other_time_s"], 64)
			if e1 == nil && e2 == nil {
				report["curved_time_s"] = extrusion + other
			}
			record["nonplanar_report"], _ = json.Marshal(report)
		}
	}
	if flat := uncurvedObjects(record["nonplanar_report"], record["inputs"]); len(flat) > 0 {
		record["printed_flat"], _ = json.Marshal(flat)
	} else {
		delete(record, "printed_flat")
	}
	failures, touches := collidingObjects(record["nonplanar_report"], c, j)
	ductFailures, ductTouches := ductGrazedObjects(record["nonplanar_report"], j)
	failures, touches = append(failures, ductFailures...), append(touches, ductTouches...)
	if len(touches) > 0 {
		var warnings []string
		json.Unmarshal(record["object_warnings"], &warnings)
		record["object_warnings"], _ = json.Marshal(append(warnings, touches...))
	}
	if len(failures) > 0 {
		record["could_not_curve"], _ = json.Marshal(failures)
	} else {
		delete(record, "could_not_curve")
	}
	encoded, err := json.Marshal(record)
	if err != nil {
		return raw
	}
	return encoded
}

var methodLabels = map[string]string{"nonplanar_top": "Curved tops", "smooth_morph": "Smooth morph",
	"slant": "Slant", "conformal_layers": "Conformal layers", "multiplanar": "Multiplanar", "field": "Rotary curved layers"}

func uncurvedObjects(raw, rawInputs json.RawMessage) []map[string]any {
	var report struct {
		Objects   []map[string]any `json:"objects"`
		Clearance struct {
			Override bool `json:"clearance_override"`
		} `json:"clearance"`
	}
	if json.Unmarshal(raw, &report) != nil {
		return nil
	}
	var inputs struct {
		Head struct {
			File string `json:"file"`
		} `json:"head"`
	}
	json.Unmarshal(rawInputs, &inputs)
	marked := false
	if inputs.Head.File != "" {
		var head struct {
			Boxes []struct {
				Name   string `json:"name"`
				Duct   *bool  `json:"duct"`
				Nozzle bool   `json:"nozzle"`
				Block  bool   `json:"block"`
			} `json:"boxes"`
		}
		if b, err := os.ReadFile(inputs.Head.File); err == nil && json.Unmarshal(b, &head) == nil {
			for _, box := range head.Boxes {
				name := strings.ToLower(box.Name)
				if box.Duct != nil {
					marked = marked || *box.Duct
				} else {
					marked = marked || (!box.Nozzle && !box.Block && (strings.Contains(name, "duct") || strings.Contains(name, "shroud")))
				}
			}
		}
	}
	var out []map[string]any
	for i, o := range report.Objects {
		mode, _ := o["mode"].(string)
		share, measured := o["curved_share"].(float64)
		limit, _ := o["planar_limit"].(string)
		reason, _ := o["planar_fallback_reason"].(string)
		reasonCode, _ := o["planar_fallback_code"].(string)
		if mode == "" || mode == "off" || !measured || share > 0 || (limit == "" && reason == "") {
			continue
		}
		method := methodLabels[mode]
		if method == "" {
			method = mode
		}
		entry := map[string]any{"object": i, "name": o["name"], "mode": mode, "method": method, "planar_limit": limit, "reason": reason,
			"try_ignore_duct": marked && !report.Clearance.Override && o["access_by"] != "nozzle" && (limit == "access" || reasonCode == "head_strike")}
		for _, k := range []string{"curved_max_angle_deg", "reachable_slope_deg"} {
			if v, ok := o[k].(float64); ok {
				entry[k] = v
			}
		}
		out = append(out, entry)
	}
	return out
}

var verifiedModes = map[string]bool{"smooth_morph": true, "slant": true}

const verifierTimeout = 5 * time.Minute

type clearanceVerdict struct {
	Hits, DuctHits int
	MinMM          float64
	Checked        bool
	Tolerance      float64
	DuctLimit      int
	IgnoreDuct     bool
	Settings       bool
	Check          string
}

func collidingObjects(raw json.RawMessage, c Config, j *Job) (failures []map[string]any, warnings []string) {
	var report struct {
		Objects []map[string]any `json:"objects"`
	}
	if j == nil || !previewableRequest(j.Request) || json.Unmarshal(raw, &report) != nil {
		return nil, nil
	}
	var curved []int
	for i, o := range report.Objects {
		mode, _ := o["mode"].(string)
		if share, _ := o["curved_share"].(float64); verifiedModes[mode] && share > 0 {
			curved = append(curved, i)
		}
	}
	if len(curved) == 0 {
		return nil, nil
	}
	v := j.morphClearance(c)
	if v.Checked && v.Hits <= 0 {
		return nil, nil
	}
	past := math.Max(0, -v.MinMM)
	depth := past + v.Tolerance
	duct := v.DuctHits
	if v.IgnoreDuct {
		duct = 0
	}
	body := v.Hits
	if v.DuctHits > v.DuctLimit {
		body = max(0, body-v.DuctHits)
	}
	checked := v.Checked && v.Settings
	refuse := !checked || body > 0 || duct > v.DuctLimit
	for _, i := range curved {
		o := report.Objects[i]
		mode, _ := o["mode"].(string)
		method := methodLabels[mode]
		name, _ := o["name"].(string)
		if mode == "slant" && checked && refuse && v.Check != "block" {
			var parts []string
			if body > 0 {
				parts = append(parts, "the print head may touch the part in "+strconv.Itoa(body)+" moves, up to "+strconv.FormatFloat(depth, 'f', 2, 64)+
					" mm deep ("+strconv.FormatFloat(past, 'f', 2, 64)+" mm past your "+strconv.FormatFloat(v.Tolerance, 'f', -1, 64)+" mm tolerance)")
			}
			if duct > 0 {
				parts = append(parts, "the fan duct may graze it in "+strconv.Itoa(duct)+" moves")
			}
			warnings = append(warnings, name+": "+strings.Join(parts, "; ")+" (Head clearance check is "+checkLabel(v.Check)+")")
			continue
		}
		if !refuse {
			if duct > 0 {
				warnings = append(warnings, name+": May graze the part with the fan duct in "+strconv.Itoa(duct)+" spots")
			}
			continue
		}
		reason := "Couldn't check " + method + " for bumps — give it another try or slice flat!"
		if checked && body > 0 {
			reason = "Watch out! " + method + " would bump the part in " + strconv.Itoa(body) + " moves (up to " + strconv.FormatFloat(depth, 'f', 2, 64) + " mm deep, past your " + strconv.FormatFloat(v.Tolerance, 'f', -1, 64) + " mm tolerance)."
		} else if checked {
			reason = "Careful! " + method + " would graze the part with the fan duct in " + strconv.Itoa(duct) + " moves (past your " + strconv.Itoa(v.DuctLimit) + " limit)."
		}
		failures = append(failures, map[string]any{"object": i, "name": o["name"], "mode": mode, "method": method, "planar_limit": "clearance",
			"reason": reason, "try_ignore_duct": checked && duct > v.DuctLimit && body == 0})
	}
	return failures, warnings
}

func ductGrazedObjects(raw json.RawMessage, j *Job) (failures []map[string]any, warnings []string) {
	var report struct {
		Objects   []map[string]any `json:"objects"`
		Clearance struct {
			DuctGrazes int  `json:"duct_grazes"`
			Override   bool `json:"clearance_override"`
		} `json:"clearance"`
	}
	if j == nil || !previewableRequest(j.Request) || json.Unmarshal(raw, &report) != nil || report.Clearance.DuctGrazes <= 0 || report.Clearance.Override {
		return nil, nil
	}
	settings := gcodeSettings(j.OutputPath("gcode"), "nonplanar_duct_graze_limit", "nonplanar_ignore_duct")
	if settings["nonplanar_ignore_duct"] == "1" {
		return nil, nil
	}
	limit, err := strconv.Atoi(settings["nonplanar_duct_graze_limit"])
	if err != nil {
		limit = 200
	}
	duct := report.Clearance.DuctGrazes
	for i, o := range report.Objects {
		mode, _ := o["mode"].(string)
		if share, _ := o["curved_share"].(float64); mode == "" || mode == "off" || verifiedModes[mode] || share <= 0 {
			continue
		}
		method := methodLabels[mode]
		if method == "" {
			method = mode
		}
		name, _ := o["name"].(string)
		if duct <= limit {
			warnings = append(warnings, name+": May graze the part with the fan duct in "+strconv.Itoa(duct)+" spots")
			continue
		}
		failures = append(failures, map[string]any{"object": i, "name": o["name"], "mode": mode, "method": method, "planar_limit": "clearance",
			"reason":          method + " would brush the part with the cooling fan duct in " + strconv.Itoa(duct) + " places (above your limit of " + strconv.Itoa(limit) + ")",
			"try_ignore_duct": true})
	}
	return failures, warnings
}

func gcodeSettings(path string, keys ...string) map[string]string {
	out := map[string]string{}
	f, err := os.Open(path)
	if err != nil {
		return out
	}
	defer f.Close()
	const tail = 4 << 20
	if st, err := f.Stat(); err == nil && st.Size() > tail {
		f.Seek(st.Size()-tail, 0)
	}
	b, _ := io.ReadAll(f)
	want := map[string]bool{}
	for _, k := range keys {
		want[k] = true
	}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(strings.TrimPrefix(line, ";"))
		if k, v, ok := strings.Cut(line, " = "); ok && want[k] {
			out[k] = strings.TrimSpace(v)
		}
	}
	return out
}

func (j *Job) morphClearance(c Config) clearanceVerdict {
	j.clearanceOnce.Do(func() {
		gcode := j.OutputPath("gcode")
		if _, err := os.Stat(gcode); err != nil {
			j.log("server", "clearance verification found no G-code")
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), verifierTimeout)
		defer cancel()
		cmd := exec.CommandContext(ctx, c.Tool, "--verify-gcode", gcode, "--profiles", filepath.Join(c.Resources, "profiles"))
		cmd.Env = engineEnv(os.TempDir())
		out, err := cmd.Output()
		v := clearanceVerdict{}
		duct := false
		for _, line := range strings.Split(string(out), "\n") {
			f := strings.Fields(line)
			switch {
			case len(f) == 2 && f[0] == "clearance_hit_moves":
				n, perr := strconv.Atoi(f[1])
				v.Hits, v.Checked = n, perr == nil
			case len(f) == 2 && f[0] == "clearance_min_mm":
				v.MinMM, _ = strconv.ParseFloat(f[1], 64)
			case len(f) == 2 && f[0] == "clearance_duct_moves":
				v.DuctHits, _ = strconv.Atoi(f[1])
				duct = true
			case len(f) > 2 && f[0] == "clearance_hit_classes" && !duct:
				for k := 1; k+1 < len(f); k += 2 {
					if f[k] == "body" {
						v.DuctHits, _ = strconv.Atoi(f[k+1])
					}
				}
			}
		}
		settings := gcodeSettings(gcode, "nonplanar_collision_tolerance", "nonplanar_duct_graze_limit", "nonplanar_ignore_duct", "nonplanar_collision_check")
		v.Check = settings["nonplanar_collision_check"]
		defaults := optionDefaults(c.Options.plain)
		for key, policy := range map[string]string{"nonplanar_collision_tolerance": "1", "nonplanar_duct_graze_limit": "200", "nonplanar_ignore_duct": "0"} {
			if _, ok := settings[key]; ok {
				continue
			}
			var value string
			if raw, ok := defaults[key]; !ok || json.Unmarshal(raw, &value) != nil || value == "" {
				value = policy
			}
			settings[key] = value
			j.log("server", "clearance verification: "+key+" not in the G-code, using "+value)
		}
		tol, e1 := strconv.ParseFloat(settings["nonplanar_collision_tolerance"], 64)
		limit, e2 := strconv.Atoi(settings["nonplanar_duct_graze_limit"])
		v.Tolerance, v.DuctLimit, v.IgnoreDuct, v.Settings = tol, limit, settings["nonplanar_ignore_duct"] == "1", e1 == nil && e2 == nil
		if v.Checked {
			j.log("server", "verified clearance hit moves "+strconv.Itoa(v.Hits)+" min "+strconv.FormatFloat(v.MinMM, 'f', 3, 64)+" mm duct "+strconv.Itoa(v.DuctHits))
			j.clearance = v
			return
		}
		why := "no clearance_hit_moves line"
		if err != nil {
			why = sanitizeToolError(err)
		}
		j.log("server", "clearance verification gave no count: "+why)
	})
	return j.clearance
}

func (j *Job) curveRefusal() string {
	var record struct {
		Failures []struct {
			Name   string `json:"name"`
			Method string `json:"method"`
			Reason string `json:"reason"`
			Limit  string `json:"planar_limit"`
		} `json:"could_not_curve"`
	}
	if json.Unmarshal(j.Record(), &record) != nil || len(record.Failures) == 0 {
		return ""
	}
	f := record.Failures[0]
	why := f.Reason
	if why == "" {
		why = "no top surface could be curved (" + f.Limit + ")"
	}
	return "Could not slice " + f.Name + " with " + f.Method + ": " + why
}

func checkLabel(check string) string {
	switch check {
	case "off":
		return "Off"
	case "block":
		return "Block export"
	}
	return "Warn"
}
