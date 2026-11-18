package main

import (
	"archive/zip"
	"bufio"
	"encoding/binary"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"math"
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type meshPoint [3]float64
type meshBounds struct {
	min, max meshPoint
	count    int
}

func (b *meshBounds) add(p meshPoint) error {
	for _, v := range p {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return fmt.Errorf("This model contains invalid (non-finite) vertex coordinates.")
		}
	}
	if b.count == 0 {
		b.min, b.max = p, p
	}
	for i, v := range p {
		b.min[i] = math.Min(b.min[i], v)
		b.max[i] = math.Max(b.max[i], v)
	}
	b.count++
	return nil
}

func meshVertices(file string, add func(meshPoint) error) error {
	f, err := os.Open(file)
	if err != nil {
		return err
	}
	defer f.Close()
	if strings.EqualFold(filepath.Ext(file), ".stl") {
		var header [84]byte
		n, _ := io.ReadFull(f, header[:])
		info, err := f.Stat()
		if err != nil {
			return err
		}
		if n == 84 && int64(84)+int64(binary.LittleEndian.Uint32(header[80:]))*50 == info.Size() {
			var triangle [50]byte
			for i := uint32(0); i < binary.LittleEndian.Uint32(header[80:]); i++ {
				if _, err := io.ReadFull(f, triangle[:]); err != nil {
					return err
				}
				for j := 12; j < 48; j += 12 {
					var p meshPoint
					for k := range p {
						p[k] = float64(math.Float32frombits(binary.LittleEndian.Uint32(triangle[j+4*k:])))
					}
					if err := add(p); err != nil {
						return err
					}
				}
			}
			return nil
		}
		if _, err := f.Seek(0, 0); err != nil {
			return err
		}
	}
	reader := bufio.NewReaderSize(f, 64<<10)
	for {
		line, err := reader.ReadSlice('\n')
		if err == bufio.ErrBufferFull {
			for err == bufio.ErrBufferFull {
				_, err = reader.ReadSlice('\n')
			}
			line = nil
		}
		if err != nil && err != io.EOF {
			return err
		}
		if fields := strings.Fields(string(line)); len(fields) >= 4 && (fields[0] == "v" || fields[0] == "vertex") {
			var p meshPoint
			for i := range p {
				v, perr := strconv.ParseFloat(fields[i+1], 64)
				if perr != nil {
					return perr
				}
				p[i] = v
			}
			if err := add(p); err != nil {
				return err
			}
		}
		if err == io.EOF {
			return nil
		}
	}
}

type modelTransform [12]float64

func transformValue(raw string) (modelTransform, error) {
	t := modelTransform{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}
	if raw == "" {
		return t, nil
	}
	fields := strings.Fields(raw)
	if len(fields) != 12 {
		return t, fmt.Errorf("Invalid object placement matrix in 3MF file.")
	}
	for i := range t {
		v, err := strconv.ParseFloat(fields[i], 64)
		if err != nil {
			return t, err
		}
		t[i] = v
	}
	return t, nil
}
func (t modelTransform) apply(p meshPoint) meshPoint {
	return meshPoint{p[0]*t[0] + p[1]*t[3] + p[2]*t[6] + t[9], p[0]*t[1] + p[1]*t[4] + p[2]*t[7] + t[10], p[0]*t[2] + p[1]*t[5] + p[2]*t[8] + t[11]}
}

type modelComponent struct {
	ID        string `xml:"objectid,attr"`
	Path      string `xml:"path,attr"`
	Transform string `xml:"transform,attr"`
}
type modelVertex struct {
	X float64 `xml:"x,attr"`
	Y float64 `xml:"y,attr"`
	Z float64 `xml:"z,attr"`
}
type boundsModel struct {
	Unit    string `xml:"unit,attr"`
	Objects []struct {
		ID         string           `xml:"id,attr"`
		Name       string           `xml:"name,attr"`
		Vertices   []modelVertex    `xml:"mesh>vertices>vertex"`
		Components []modelComponent `xml:"components>component"`
	} `xml:"resources>object"`
	Items []struct {
		ID        string `xml:"objectid,attr"`
		Transform string `xml:"transform,attr"`
		Printable string `xml:"printable,attr"`
	} `xml:"build>item"`
}
type namedBounds struct {
	name       string
	buildIndex int
	box        meshBounds
}

func projectBounds(file string, includeNonPrintable ...bool) ([]namedBounds, error) {
	z, err := zip.OpenReader(file)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	entries := map[string]*zip.File{}
	for _, f := range z.File {
		entries[strings.ToLower(path.Clean("/"+f.Name))] = f
	}
	read := func(name string, out any) error {
		f := entries[strings.ToLower(path.Clean("/"+name))]
		if f == nil {
			return fmt.Errorf("Missing 3MF model part: %s", name)
		}
		if f.UncompressedSize64 > 512<<20 {
			return fmt.Errorf("A model part inside the 3MF file is too large to process.")
		}
		r, err := f.Open()
		if err != nil {
			return err
		}
		defer r.Close()
		return xml.NewDecoder(io.LimitReader(r, 512<<20)).Decode(out)
	}
	root := "/3D/3dmodel.model"
	var rels struct {
		Items []struct {
			Target string `xml:"Target,attr"`
			Type   string `xml:"Type,attr"`
		} `xml:"Relationship"`
	}
	if err := read("/_rels/.rels", &rels); err == nil {
		for _, r := range rels.Items {
			if strings.HasSuffix(r.Type, "/3dmodel") {
				root = r.Target
				break
			}
		}
	}
	models := map[string]*boundsModel{}
	load := func(name string) (*boundsModel, error) {
		name = path.Clean("/" + name)
		if m := models[name]; m != nil {
			return m, nil
		}
		var m boundsModel
		if err := read(name, &m); err != nil {
			return nil, err
		}
		models[name] = &m
		return &m, nil
	}
	m, err := load(root)
	if err != nil {
		return nil, err
	}
	unitScale, ok := map[string]float64{"": 1, "micron": 0.001, "millimeter": 1, "centimeter": 10, "inch": 25.4, "foot": 304.8, "meter": 1000}[m.Unit]
	if !ok {
		return nil, fmt.Errorf("Unsupported measurement unit in 3MF file: %s", m.Unit)
	}
	names := map[string]string{}
	var settings struct {
		Objects []struct {
			ID       string `xml:"id,attr"`
			Metadata []struct {
				Key   string `xml:"key,attr"`
				Value string `xml:"value,attr"`
			} `xml:"metadata"`
		} `xml:"object"`
	}
	if err := read("/Metadata/model_settings.config", &settings); err == nil {
		for _, o := range settings.Objects {
			for _, meta := range o.Metadata {
				if meta.Key == "name" {
					names[o.ID] = meta.Value
				}
			}
		}
	}
	var collect func(string, string, []modelTransform, *meshBounds, int) error
	collect = func(part, id string, chain []modelTransform, b *meshBounds, depth int) error {
		if depth > 32 {
			return fmt.Errorf("Part assembly nesting in this 3MF file is too deep.")
		}
		m, err := load(part)
		if err != nil {
			return err
		}
		for _, o := range m.Objects {
			if o.ID != id {
				continue
			}
			for _, v := range o.Vertices {
				p := meshPoint{v.X, v.Y, v.Z}
				for _, t := range chain {
					p = t.apply(p)
				}
				for i := range p {
					p[i] *= unitScale
				}
				if err := b.add(p); err != nil {
					return err
				}
			}
			for _, c := range o.Components {
				t, err := transformValue(c.Transform)
				if err != nil {
					return err
				}
				next := part
				if c.Path != "" {
					next = c.Path
					if !strings.HasPrefix(next, "/") {
						next = path.Join(path.Dir(part), next)
					}
				}
				if err := collect(next, c.ID, append([]modelTransform{t}, chain...), b, depth+1); err != nil {
					return err
				}
			}
			return nil
		}
		return fmt.Errorf("Missing 3MF object: %s", id)
	}
	out := []namedBounds{}
	for buildIndex, item := range m.Items {
		if item.Printable == "0" && (len(includeNonPrintable) == 0 || !includeNonPrintable[0]) {
			continue
		}
		t, err := transformValue(item.Transform)
		if err != nil {
			return nil, err
		}
		name := names[item.ID]
		if name == "" {
			name = "Object " + item.ID
		}
		b := namedBounds{name: name, buildIndex: buildIndex}
		if err := collect(root, item.ID, []modelTransform{t}, &b.box, 0); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, nil
}

type outsideBedError struct{ name, extent string }

func (e outsideBedError) Error() string {
	return newFailure("outside_build_plate", e.name, e.extent).Error
}

func validatePlateBounds(dir string, req JobRequest, machine *resolve.Preset, rawPlate string) error {
	area := stringList(machine.Config["printable_area"])
	if raw := stringValue(machine.Config["printable_area"]); raw != "" {
		area = strings.Split(raw, ",")
	}
	for _, o := range req.Overrides {
		if o[0] == "printable_area" && o[1] != "" {
			area = strings.Split(o[1], ",")
		}
	}
	bed := meshBounds{}
	for _, s := range area {
		xy := strings.Split(s, "x")
		if len(xy) != 2 {
			return fmt.Errorf("Invalid print bed dimensions in printer profile.")
		}
		x, e1 := strconv.ParseFloat(xy[0], 64)
		y, e2 := strconv.ParseFloat(xy[1], 64)
		if e1 != nil || e2 != nil {
			return fmt.Errorf("Invalid print bed dimensions in printer profile.")
		}
		if err := bed.add(meshPoint{x, y, 0}); err != nil {
			return err
		}
	}
	if bed.count < 3 {
		return fmt.Errorf("Printer profile is missing print bed dimensions.")
	}
	check := func(name string, b meshBounds) error {
		if b.count == 0 {
			return fmt.Errorf("%s: model has no vertices", name)
		}
		const tolerance = 1e-3
		if b.min[0] < bed.min[0]-tolerance || b.min[1] < bed.min[1]-tolerance || b.max[0] > bed.max[0]+tolerance || b.max[1] > bed.max[1]+tolerance {
			return outsideBedError{name, fmt.Sprintf("Model XY [%.3f, %.3f]–[%.3f, %.3f], printable area [%.3f, %.3f]–[%.3f, %.3f]", b.min[0], b.min[1], b.max[0], b.max[1], bed.min[0], bed.min[1], bed.max[0], bed.max[1])}
		}
		return nil
	}
	if rawPlate == "" {
		if strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
			objects, err := projectBounds(filepath.Join(dir, req.Files[0]))
			if err != nil {
				return err
			}
			layout, err := readProjectLayout(filepath.Join(dir, req.Files[0]))
			if err != nil {
				return err
			}
			deleted := map[int]bool{}
			if req.Edits {
				if deleted, rawPlate, err = projectEditBounds(dir, req.Files[0], req.PlateIndex); err != nil {
					return err
				}
			}
			var shift [2]float64
			if !layout.HasConfig && req.PlateIndex == 0 {
				var assembly meshBounds
				for _, o := range objects {
					if o.box.count > 0 {
						if err := assembly.add(o.box.min); err != nil {
							return err
						}
						if err := assembly.add(o.box.max); err != nil {
							return err
						}
					}
				}
				for axis := range shift {
					shift[axis] = (bed.min[axis] + bed.max[axis] - assembly.min[axis] - assembly.max[axis]) / 2
				}
			}
			for _, o := range objects {
				if deleted[o.buildIndex] {
					continue
				}
				for axis := range shift {
					o.box.min[axis] += shift[axis]
					o.box.max[axis] += shift[axis]
				}
				if req.PlateIndex > 0 {
					if layout.Membership[o.buildIndex] != req.PlateIndex {
						continue
					}
					origin := layout.origin(req.PlateIndex)
					for axis := 0; axis < 2; axis++ {
						o.box.min[axis] -= origin[axis]
						o.box.max[axis] -= origin[axis]
					}
				}
				if err := check(o.name, o.box); err != nil {
					return err
				}
			}
			if rawPlate == "" {
				return nil
			}
		}
		if rawPlate == "" && !strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") && !isStep(req.Files[0]) {
			b := meshBounds{}
			if err := meshVertices(filepath.Join(dir, req.Files[0]), b.add); err != nil {
				return err
			}
			for i := 0; i < 2; i++ {
				shift := (bed.min[i] + bed.max[i] - b.min[i] - b.max[i]) / 2
				b.min[i] += shift
				b.max[i] += shift
			}
			return check(req.Files[0], b)
		}
		if rawPlate == "" {
			return nil
		}
	}
	type placement struct {
		Printable *bool   `json:"printable"`
		X         float64 `json:"x"`
		Y         float64 `json:"y"`
		Rot       float64 `json:"rot"`
	}
	var plate struct {
		Objects []struct {
			placement
			File      string      `json:"file"`
			Name      string      `json:"name"`
			Instances []placement `json:"instances"`
			Volumes   []struct {
				File      string          `json:"file"`
				Type      string          `json:"type"`
				Transform json.RawMessage `json:"transform"`
			} `json:"volumes"`
		} `json:"objects"`
	}
	if err := json.Unmarshal([]byte(rawPlate), &plate); err != nil {
		return err
	}
	for _, o := range plate.Objects {
		if isStep(o.File) || o.File == "" {
			continue
		}
		name := o.Name
		if name == "" {
			name = o.File
		}
		file := filepath.Join(dir, o.File)
		instances := o.Instances
		if len(instances) == 0 {
			instances = []placement{o.placement}
		}
		for _, p := range instances {
			if p.Printable != nil && !*p.Printable {
				continue
			}
			b := meshBounds{}
			angle := p.Rot * math.Pi / 180
			c, s := math.Cos(angle), math.Sin(angle)
			rotate := func(v meshPoint) error { return b.add(meshPoint{c*v[0] - s*v[1], s*v[0] + c*v[1], v[2]}) }
			if err := meshVertices(file, rotate); err != nil {
				return err
			}
			for _, v := range o.Volumes {
				if v.Type != "" && v.Type != "part" {
					continue
				}
				m, err := volumeTransform(v.Transform)
				if err != nil {
					return err
				}
				if err := meshVertices(filepath.Join(dir, v.File), func(p meshPoint) error {
					return rotate(meshPoint{m[0]*p[0] + m[1]*p[1] + m[2]*p[2] + m[3], m[4]*p[0] + m[5]*p[1] + m[6]*p[2] + m[7], m[8]*p[0] + m[9]*p[1] + m[10]*p[2] + m[11]})
				}); err != nil {
					return err
				}
			}
			dx, dy := p.X-(b.min[0]+b.max[0])/2, p.Y-(b.min[1]+b.max[1])/2
			b.min[0] += dx
			b.max[0] += dx
			b.min[1] += dy
			b.max[1] += dy
			if err := check(name, b); err != nil {
				return err
			}
		}
	}
	return nil
}

func volumeTransform(raw json.RawMessage) ([16]float64, error) {
	m := [16]float64{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}
	if len(raw) == 0 || string(raw) == "null" {
		return m, nil
	}
	var flat []float64
	var rows [][]float64
	if json.Unmarshal(raw, &flat) == nil && len(flat) == 16 {
		copy(m[:], flat)
		return m, nil
	}
	if json.Unmarshal(raw, &rows) == nil && len(rows) == 4 {
		for r, row := range rows {
			if len(row) != 4 {
				return m, fmt.Errorf("a volume transform is a row-major 4x4 matrix: 16 numbers or 4 rows of 4")
			}
			copy(m[4*r:], row)
		}
		return m, nil
	}
	return m, fmt.Errorf("a volume transform is a row-major 4x4 matrix: 16 numbers or 4 rows of 4")
}

func isStep(file string) bool {
	ext := strings.ToLower(filepath.Ext(file))
	return ext == ".step" || ext == ".stp"
}
