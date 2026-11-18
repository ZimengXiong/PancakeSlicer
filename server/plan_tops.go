package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
)

func runToolIn(ctx context.Context, tool, dir string, args ...string) error {
	cmd := exec.CommandContext(ctx, tool, args...)
	cmd.Dir = dir
	cmd.Env = engineEnv(dir)
	if err := limitJobMemory(cmd); err != nil {
		return err
	}
	configureJobProcess(cmd)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("Oops! The slicer couldn't read the model file: %s", sanitizeToolError(errors.New(lastLine(string(out)))))
	}
	return nil
}

func surfaceFacts(mesh [][9]float64, facets []uint32) (area, span, slopeMax float64) {
	type slopeArea struct{ s, a float64 }
	var slopes []slopeArea
	zmin, zmax := math.Inf(1), math.Inf(-1)
	for _, f := range facets {
		t := mesh[f]
		ux, uy, uz := t[3]-t[0], t[4]-t[1], t[5]-t[2]
		vx, vy, vz := t[6]-t[0], t[7]-t[1], t[8]-t[2]
		nx, ny, nz := uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
		length := math.Sqrt(nx*nx + ny*ny + nz*nz)
		for _, z := range []float64{t[2], t[5], t[8]} {
			zmin, zmax = math.Min(zmin, z), math.Max(zmax, z)
		}
		if length <= 0 || nz <= 0 {
			continue
		}
		slopes = append(slopes, slopeArea{math.Acos(math.Min(1, nz/length)) * 180 / math.Pi, nz / 2})
		area += nz / 2
	}
	sort.Slice(slopes, func(a, b int) bool { return slopes[a].s < slopes[b].s })
	acc := 0.
	for _, sa := range slopes {
		acc += sa.a
		slopeMax = sa.s
		if acc >= 0.95*area {
			break
		}
	}
	if zmax > zmin {
		span = zmax - zmin
	}
	return
}

func (m *JobManager) mapPlanFacets(ctx context.Context, j *Job, raw []byte) ([]byte, error) {
	var plan map[string]any
	if err := json.Unmarshal(raw, &plan); err != nil {
		return nil, err
	}
	in := filepath.Join(j.Dir, "input")
	var files []string
	if j.Request.Plate {
		var plate struct {
			Objects []struct {
				File string `json:"file"`
			} `json:"objects"`
		}
		b, err := os.ReadFile(filepath.Join(in, "plate.json"))
		if err != nil || json.Unmarshal(b, &plate) != nil {
			return nil, errors.New("Oops! Couldn't read the build plate for this preview.")
		}
		for _, o := range plate.Objects {
			files = append(files, o.File)
		}
	} else if len(j.Request.Files) == 1 {
		files = j.Request.Files
	}
	type meshMap struct {
		input [][9]float64
		toIn  []int
	}
	maps := map[int]*meshMap{}
	meshOf := func(oi int) (*meshMap, error) {
		if mm, ok := maps[oi]; ok {
			return mm, nil
		}
		if oi < 0 || oi >= len(files) || filepath.Ext(files[oi]) != ".stl" {
			maps[oi] = nil
			return nil, nil
		}
		path := filepath.Join(in, filepath.Base(files[oi]))
		input, err := readBinarySTL(path)
		if err != nil {
			return nil, err
		}
		repaired := filepath.Join(j.Dir, "tmp", fmt.Sprintf("plan-mesh-%d.stl", oi))
		if err := os.MkdirAll(filepath.Dir(repaired), 0o700); err != nil {
			return nil, err
		}
		if err := runToolIn(ctx, m.cfg.Tool, filepath.Dir(repaired), "--repair-mesh", path, repaired); err != nil {
			return nil, err
		}
		tool, err := readBinarySTL(repaired)
		if err != nil {
			return nil, err
		}
		mm := &meshMap{input: input, toIn: matchFacets(input, tool)}
		maps[oi] = mm
		return mm, nil
	}
	regions, _ := plan["regions"].([]any)
	kept := make([]any, 0, len(regions))
	for _, r := range regions {
		region, ok := r.(map[string]any)
		if !ok {
			continue
		}
		oi, _ := region["object"].(float64)
		if vi, _ := region["volume"].(float64); vi != 0 {
			continue
		}
		mm, err := meshOf(int(oi))
		if err != nil {
			return nil, err
		}
		if mm == nil {
			continue
		}
		raw, _ := region["facets"].([]any)
		facets := make([]uint32, 0, len(raw))
		for _, v := range raw {
			if f, ok := v.(float64); ok && int(f) >= 0 && int(f) < len(mm.toIn) && mm.toIn[int(f)] >= 0 {
				facets = append(facets, uint32(mm.toIn[int(f)]))
			}
		}
		if len(facets) == 0 {
			continue
		}
		sort.Slice(facets, func(a, b int) bool { return facets[a] < facets[b] })
		region["facets"] = packRuns(facets)
		region["area_xy"], region["span"], region["slope_max"] = surfaceFacts(mm.input, facets)
		kept = append(kept, region)
	}
	plan["regions"] = kept
	return json.Marshal(plan)
}
