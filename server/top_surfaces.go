package main

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	surfaceSlopeBin = 0.5
	surfaceAzBins   = 24
)

type surfaceBin struct {
	Slope float64 `json:"s"`
	Az    int     `json:"a"`
	Area  float64 `json:"m"`
}

type surfaceRegion struct {
	Kind     string       `json:"kind"`
	Facets   []uint32     `json:"facets"`
	Area     float64      `json:"area"`
	Span     float64      `json:"span"`
	SlopeMax float64      `json:"slope_max"`
	Bins     []surfaceBin `json:"bins"`
}

type surfaceAnalysis struct {
	Head    string          `json:"head"`
	Regions []surfaceRegion `json:"regions"`
}

type headObstacle struct {
	z       float64
	corners [4][2]float64
}

func (o headObstacle) support(ux, uy float64) float64 {
	best := math.Inf(-1)
	for _, c := range o.corners {
		best = math.Max(best, c[0]*ux+c[1]*uy)
	}
	return best
}

type headReach struct {
	duct   []headObstacle
	block  []headObstacle
	margin float64
	marked bool
}

type topSurfaces struct {
	mu      sync.Mutex
	running map[string]*sync.Mutex
	slots   chan struct{}
}

var surfaceCache = topSurfaces{running: map[string]*sync.Mutex{}, slots: make(chan struct{}, 2)}

func validHeadID(id string) bool {
	if id == "" || len(id) > 96 {
		return false
	}
	for _, c := range id {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_' || c == '.') {
			return false
		}
	}
	return !strings.Contains(id, "..")
}

func (s *Server) topSurfaces(w http.ResponseWriter, r *http.Request) {
	var req struct {
		surfaceSettings
		Head   string `json:"head"`
		Meshes []struct {
			SHA256 string  `json:"sha256"`
			RotZ   float64 `json:"rot_z"`
		} `json:"meshes"`
	}
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10)).Decode(&req) != nil || len(req.Meshes) == 0 || len(req.Meshes) > 256 {
		fail(w, 400, "expected_head_mode_max")
		return
	}
	if !validHeadID(req.Head) {
		fail(w, 400, "printer_doesnt_have_3d")
		return
	}
	var missing []string
	for _, m := range req.Meshes {
		sum := m.SHA256
		if !blobDigest.MatchString(sum) {
			fail(w, 400, "meshes_must_lowercase_hex")
			return
		}
		if !s.blobs.has(sum) {
			missing = append(missing, sum)
		}
	}
	if len(missing) > 0 {
		writeJSON(w, 409, map[string]any{"error": "Re-syncing your model files with the slicing service…", "code": "blob_missing", "missing": missing})
		return
	}
	reach, err := loadHeadReach(s.cfg, req.Head)
	if err != nil {
		fail(w, 422, "top_surfaces_unprocessable", err.Error())
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Minute)
	defer cancel()
	objects := make([]map[string]any, len(req.Meshes))
	for i, m := range req.Meshes {
		sum := m.SHA256
		a, err := s.surfaceAnalysis(ctx, sum, req.Head)
		if err != nil {
			objects[i] = map[string]any{"sha256": sum, "error": err.Error()}
			continue
		}
		statuses := classifyObject(turned(a.Regions, m.RotZ), reach, req.surfaceSettings)
		regions := make([]map[string]any, 0, len(a.Regions))
		for k, g := range a.Regions {
			regions = append(regions, map[string]any{"kind": g.Kind, "facets": packRuns(g.Facets), "area": g.Area, "span": g.Span,
				"slope_max": g.SlopeMax, "status": statuses[k].status, "reason": statuses[k].reason, "limit_deg": statuses[k].deg})
		}
		objects[i] = map[string]any{"sha256": sum, "regions": regions}
	}
	writeJSON(w, 200, map[string]any{"schema": "pancake.top-surfaces.v1", "head": req.Head, "duct_ignorable": reach.marked, "objects": objects})
}

type surfaceSettings struct {
	Mode       string  `json:"mode"`
	MaxAngle   float64 `json:"max_angle"`
	MinArea    float64 `json:"min_area"`
	IgnoreDuct bool    `json:"ignore_duct"`
}

type surfaceStatus struct {
	status, reason string
	deg            float64
}

func (h *headReach) clearArea(g surfaceRegion, parts []headObstacle, cap float64) (clear, blocked float64) {
	for _, b := range g.Bins {
		if b.Slope > cap {
			continue
		}
		if h.clears(parts, b, g.Span) {
			clear += b.Area
		} else {
			blocked += b.Area
		}
	}
	return
}

func classifyObject(regions []surfaceRegion, h *headReach, c surfaceSettings) []surfaceStatus {
	const limit = 45.
	maxAngle := c.MaxAngle
	if maxAngle <= 0 {
		maxAngle = 30
	}
	duct := c.IgnoreDuct && h.marked
	parts := h.duct
	if duct {
		parts = h.block
	}
	cap := math.Min(maxAngle, limit)
	out := make([]surfaceStatus, len(regions))
	for i, g := range regions {
		var total float64
		for _, b := range g.Bins {
			total += b.Area
		}
		clear, _ := h.clearArea(g, parts, cap)
		switch {
		case g.Span < 0.05 || g.SlopeMax < 1:
			out[i] = surfaceStatus{"flat", "Flat, prints exactly", 0}
		case c.MinArea > 0 && g.Area < c.MinArea:
			out[i] = surfaceStatus{"small", fmt.Sprintf("Smaller than %s mm²", trimFloat(c.MinArea)), 0}
		case total > 0 && clear >= total/2:
			out[i] = surfaceStatus{"curves", "Curves", 0}
		default:
			ductCap := cap
			if blockClear, _ := h.clearArea(g, h.block, ductCap); !duct && h.marked && total > 0 && blockClear >= total/2 {
				out[i] = surfaceStatus{"duct", "Needs Ignore cooling duct", math.Round(h.reachDeg(h.duct, g))}
			} else if under, _ := h.clearArea(g, nil, ductCap); total > 0 && under < total/2 {
				if ductCap >= limit {
					out[i] = surfaceStatus{"steep", fmt.Sprintf("Too steep for this method (%s° max)", trimFloat(limit)), limit}
				} else {
					out[i] = surfaceStatus{"steep", fmt.Sprintf("Above your %s° curved-top limit", trimFloat(maxAngle)), maxAngle}
				}
			} else {
				deg := math.Round(math.Min(h.reachDeg(parts, g), cap))
				out[i] = surfaceStatus{"steep", fmt.Sprintf("Too steep for this head (%s° max)", trimFloat(deg)), deg}
			}
		}
	}
	return out
}

func turned(regions []surfaceRegion, deg float64) []surfaceRegion {
	shift := int(math.Round(deg / (360 / surfaceAzBins)))
	if shift%surfaceAzBins == 0 {
		return regions
	}
	out := make([]surfaceRegion, len(regions))
	for i, g := range regions {
		out[i] = g
		out[i].Bins = make([]surfaceBin, len(g.Bins))
		for k, b := range g.Bins {
			b.Az = ((b.Az+shift)%surfaceAzBins + surfaceAzBins) % surfaceAzBins
			out[i].Bins[k] = b
		}
	}
	return out
}

func packRuns(facets []uint32) string {
	var packed []byte
	for k := 0; k < len(facets); {
		n := 1
		for k+n < len(facets) && facets[k+n] == facets[k]+uint32(n) {
			n++
		}
		packed = binary.LittleEndian.AppendUint32(binary.LittleEndian.AppendUint32(packed, facets[k]), uint32(n))
		k += n
	}
	return base64.StdEncoding.EncodeToString(packed)
}

func trimFloat(v float64) string {
	return strings.TrimSuffix(strings.TrimRight(fmt.Sprintf("%.1f", v), "0"), ".")
}

func azimuth(bin int) (float64, float64) {
	a := (float64(bin) + 0.5) * 2 * math.Pi / surfaceAzBins
	return math.Cos(a), math.Sin(a)
}

func (h *headReach) clears(parts []headObstacle, b surfaceBin, span float64) bool {
	t := math.Tan(b.Slope * math.Pi / 180)
	ux, uy := azimuth(b.Az)
	for _, p := range parts {
		z := p.z - h.margin
		if z > span {
			continue
		}
		if z <= 0 || p.support(ux, uy)*t >= z {
			return false
		}
	}
	return true
}

func (h *headReach) reachDeg(parts []headObstacle, g surfaceRegion) float64 {
	seen := map[int]bool{}
	for _, b := range g.Bins {
		seen[b.Az] = true
	}
	best := 90.0
	for az := range seen {
		ux, uy := azimuth(az)
		for _, p := range parts {
			z := p.z - h.margin
			if z > g.Span || z <= 0 {
				continue
			}
			if sup := p.support(ux, uy); sup > 0 {
				best = math.Min(best, math.Atan2(z, sup)*180/math.Pi)
			}
		}
	}
	return best
}

var headReaches sync.Map

func loadHeadReach(c Config, id string) (*headReach, error) {
	path := filepath.Join(userHeadDir(c), id+".json")
	st, err := os.Stat(path)
	if err != nil {
		path = filepath.Join(c.Resources, "nonplanar", "heads", id+".json")
		if st, err = os.Stat(path); err != nil {
			return nil, fmt.Errorf("Print head profile %q isn't installed on the server.", id)
		}
	}
	key := path + "\x00" + st.ModTime().String()
	if v, ok := headReaches.Load(key); ok {
		return v.(*headReach), nil
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var head struct {
		Margin float64 `json:"margin"`
		Boxes  []struct {
			Name      string     `json:"name"`
			Min       [3]float64 `json:"min"`
			Max       [3]float64 `json:"max"`
			Nozzle    bool       `json:"nozzle"`
			Block     bool       `json:"block"`
			Duct      *bool      `json:"duct"`
			InfiniteX bool       `json:"infinite_x"`
		} `json:"boxes"`
	}
	if err := json.Unmarshal(raw, &head); err != nil {
		return nil, fmt.Errorf("Couldn't read print head profile %q: %v", id, err)
	}
	box := func(z, x0, x1, y0, y1 float64) headObstacle {
		return headObstacle{z: z, corners: [4][2]float64{{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}}}
	}
	h := &headReach{margin: head.Margin}
	for _, b := range head.Boxes {
		if b.Nozzle {
			continue
		}
		x0, x1 := b.Min[0], b.Max[0]
		if b.InfiniteX {
			x0, x1 = -1e6, 1e6
		}
		o := box(b.Min[2], x0, x1, b.Min[1], b.Max[1])
		h.duct = append(h.duct, o)
		name := strings.ToLower(b.Name)
		isDuct := !b.Block && (strings.Contains(name, "duct") || strings.Contains(name, "shroud"))
		if b.Duct != nil {
			isDuct = *b.Duct
		}
		if isDuct {
			h.marked = true
		} else {
			h.block = append(h.block, o)
		}
	}
	headReaches.Store(key, h)
	return h, nil
}

func (s *Server) surfaceDir() string { return filepath.Join(s.cfg.JobRoot, "top-surfaces-cache") }

func (s *Server) surfaceAnalysis(ctx context.Context, sum, head string) (*surfaceAnalysis, error) {
	h := sha256.New()
	if st, err := os.Stat(s.cfg.Tool); err == nil {
		h.Write([]byte(s.cfg.Tool + "\x00" + st.ModTime().String() + "\x00"))
	}
	h.Write([]byte("v1\x00" + head + "\x00" + sum))
	key := hex.EncodeToString(h.Sum(nil))
	path := filepath.Join(s.surfaceDir(), key+".json")
	read := func() *surfaceAnalysis {
		var a surfaceAnalysis
		if raw, err := os.ReadFile(path); err == nil && json.Unmarshal(raw, &a) == nil {
			return &a
		}
		return nil
	}
	if a := read(); a != nil {
		return a, nil
	}
	surfaceCache.mu.Lock()
	lock := surfaceCache.running[key]
	if lock == nil {
		lock = &sync.Mutex{}
		surfaceCache.running[key] = lock
	}
	surfaceCache.mu.Unlock()
	lock.Lock()
	defer lock.Unlock()
	if a := read(); a != nil {
		return a, nil
	}
	select {
	case surfaceCache.slots <- struct{}{}:
		defer func() { <-surfaceCache.slots }()
	case <-ctx.Done():
		return nil, ctx.Err()
	}
	a, err := s.measureSurfaces(ctx, sum, head)
	if err != nil {
		return nil, err
	}
	raw, _ := json.Marshal(a)
	if os.MkdirAll(s.surfaceDir(), 0o700) == nil {
		tmp := path + ".tmp"
		if os.WriteFile(tmp, raw, 0o600) != nil || os.Rename(tmp, path) != nil {
			os.Remove(tmp)
		}
	}
	return a, nil
}

func (s *Server) measureSurfaces(ctx context.Context, sum, head string) (*surfaceAnalysis, error) {
	dir, err := os.MkdirTemp(s.cfg.JobRoot, "top-surfaces-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	mesh := filepath.Join(dir, "input.stl")
	if err := s.blobs.link(sum, mesh); err != nil {
		return nil, err
	}
	input, err := readBinarySTL(mesh)
	if err != nil {
		return nil, err
	}
	run := func(args ...string) error { return runToolIn(ctx, s.cfg.Tool, dir, args...) }
	repaired, regions := filepath.Join(dir, "mesh.stl"), filepath.Join(dir, "regions.json")
	if err := run("--repair-mesh", mesh, repaired); err != nil {
		return nil, err
	}
	if err := run("--detect-regions", regions, repaired, "--profiles", filepath.Join(s.cfg.Resources, "profiles"),
		"--datadir", filepath.Join(dir, "datadir"), "--set", "nonplanar_head="+head); err != nil {
		return nil, err
	}
	tool, err := readBinarySTL(repaired)
	if err != nil {
		return nil, err
	}
	var report struct {
		Head    string `json:"head"`
		Regions []struct {
			Kind   string   `json:"kind"`
			Facets []uint32 `json:"facets"`
			Object int      `json:"object"`
		} `json:"regions"`
	}
	raw, err := os.ReadFile(regions)
	if err != nil || json.Unmarshal(raw, &report) != nil {
		return nil, errors.New("The slicer didn't find any top surfaces to preview.")
	}
	toInput := matchFacets(input, tool)
	out := &surfaceAnalysis{Head: report.Head, Regions: []surfaceRegion{}}
	for _, g := range report.Regions {
		bins := map[[2]int]float64{}
		type slopeArea struct{ s, a float64 }
		var slopes []slopeArea
		region := surfaceRegion{Kind: g.Kind}
		zmin, zmax := math.Inf(1), math.Inf(-1)
		for _, f := range g.Facets {
			if int(f) >= len(tool) || toInput[f] < 0 {
				continue
			}
			t := tool[f]
			ux, uy, uz := t[3]-t[0], t[4]-t[1], t[5]-t[2]
			vx, vy, vz := t[6]-t[0], t[7]-t[1], t[8]-t[2]
			nx, ny, nz := uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
			length := math.Sqrt(nx*nx + ny*ny + nz*nz)
			if length <= 0 || nz <= 0 {
				continue
			}
			area := nz / 2
			slope := math.Acos(math.Min(1, nz/length)) * 180 / math.Pi
			az := int(math.Floor(math.Mod(math.Atan2(-ny, -nx)+2*math.Pi, 2*math.Pi)/(2*math.Pi)*surfaceAzBins)) % surfaceAzBins
			bins[[2]int{int(slope / surfaceSlopeBin), az}] += area
			slopes = append(slopes, slopeArea{slope, area})
			region.Area += area
			region.Facets = append(region.Facets, uint32(toInput[f]))
			for _, z := range []float64{t[2], t[5], t[8]} {
				zmin, zmax = math.Min(zmin, z), math.Max(zmax, z)
			}
		}
		if len(region.Facets) == 0 {
			continue
		}
		region.Span = zmax - zmin
		sort.Slice(slopes, func(a, b int) bool { return slopes[a].s < slopes[b].s })
		acc := 0.
		for _, sa := range slopes {
			acc += sa.a
			region.SlopeMax = sa.s
			if acc >= 0.95*region.Area {
				break
			}
		}
		for k, a := range bins {
			region.Bins = append(region.Bins, surfaceBin{Slope: (float64(k[0]) + 0.5) * surfaceSlopeBin, Az: k[1], Area: a})
		}
		sort.Slice(region.Facets, func(a, b int) bool { return region.Facets[a] < region.Facets[b] })
		out.Regions = append(out.Regions, region)
	}
	return out, nil
}

func lastLine(s string) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	return lines[len(lines)-1]
}

func readBinarySTL(path string) ([][9]float64, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	head := make([]byte, 84)
	if _, err := io.ReadFull(f, head); err != nil {
		return nil, errors.New("The model file isn't a binary STL.")
	}
	n := binary.LittleEndian.Uint32(head[80:])
	st, err := f.Stat()
	if err != nil || st.Size() != 84+50*int64(n) {
		return nil, errors.New("The model file isn't a binary STL.")
	}
	body := make([]byte, 50*int(n))
	if _, err := io.ReadFull(f, body); err != nil {
		return nil, err
	}
	out := make([][9]float64, n)
	for i := range out {
		rec := body[50*i+12:]
		for k := 0; k < 9; k++ {
			out[i][k] = float64(math.Float32frombits(binary.LittleEndian.Uint32(rec[4*k:])))
		}
	}
	return out, nil
}

func matchFacets(input, tool [][9]float64) []int {
	same := func(a, b [9]float64) bool {
		for _, p := range [3]int{0, 3, 6} {
			found := false
			for _, q := range [3]int{0, 3, 6} {
				if math.Abs(a[p]-b[q]) < 1e-3 && math.Abs(a[p+1]-b[q+1]) < 1e-3 && math.Abs(a[p+2]-b[q+2]) < 1e-3 {
					found = true
					break
				}
			}
			if !found {
				return false
			}
		}
		return true
	}
	out := make([]int, len(tool))
	i := 0
	for j := range tool {
		out[j] = -1
		for k := i; k < len(input) && k < i+256; k++ {
			if same(input[k], tool[j]) {
				out[j], i = k, k+1
				break
			}
		}
	}
	return out
}
