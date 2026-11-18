package main

import (
	"archive/zip"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"math"
	"strconv"
	"strings"
)

type projectPlate struct {
	Index int    `json:"index"`
	Name  string `json:"name"`
}
type projectLayout struct {
	Plates       []projectPlate `json:"source_plates"`
	Membership   map[int]int    `json:"-"`
	Width, Depth float64
	HasConfig    bool `json:"-"`
}

func (p projectLayout) hasPlate(index int) bool {
	for _, plate := range p.Plates {
		if plate.Index == index {
			return true
		}
	}
	return false
}
func (p projectLayout) origin(index int) [2]float64 {
	count := 1
	for _, plate := range p.Plates {
		if plate.Index > count {
			count = plate.Index
		}
	}
	columns := int(math.Ceil(math.Sqrt(float64(count))))
	return [2]float64{float64((index-1)%columns) * p.Width * 1.2, -float64((index-1)/columns) * p.Depth * 1.2}
}

func readProjectLayout(file string) (projectLayout, error) {
	out := projectLayout{Membership: map[int]int{}}
	z, err := zip.OpenReader(file)
	if err != nil {
		return out, err
	}
	defer z.Close()
	type metadata struct {
		Key   string `xml:"key,attr"`
		Value string `xml:"value,attr"`
	}
	var settings struct {
		Plates []struct {
			Metadata  []metadata `xml:"metadata"`
			Instances []struct {
				Metadata []metadata `xml:"metadata"`
			} `xml:"model_instance"`
		} `xml:"plate"`
	}
	type buildModel struct {
		Items []struct {
			ID string `xml:"objectid,attr"`
		} `xml:"build>item"`
	}
	var model buildModel
	var config map[string]json.RawMessage
	for _, f := range z.File {
		name := strings.ToLower(f.Name)
		if name == "metadata/project_settings.config" || name == "metadata/slic3r_pe.config" {
			out.HasConfig = true
		}
		if name != "metadata/model_settings.config" && name != "metadata/project_settings.config" && !strings.HasSuffix(name, ".model") {
			continue
		}
		r, err := f.Open()
		if err != nil {
			return out, err
		}
		if name == "metadata/model_settings.config" {
			err = xml.NewDecoder(io.LimitReader(r, 16<<20)).Decode(&settings)
		} else if name == "metadata/project_settings.config" {
			err = json.NewDecoder(io.LimitReader(r, 16<<20)).Decode(&config)
		} else {
			var m buildModel
			err = xml.NewDecoder(io.LimitReader(r, 512<<20)).Decode(&m)
			if len(m.Items) > 0 {
				model = m
			}
		}
		r.Close()
		if err != nil {
			return out, fmt.Errorf("invalid project plate metadata: %w", err)
		}
	}
	value := func(meta []metadata, key string) string {
		for _, m := range meta {
			if m.Key == key {
				return m.Value
			}
		}
		return ""
	}
	membership := map[string]int{}
	for _, p := range settings.Plates {
		var index int
		fmt.Sscan(value(p.Metadata, "plater_id"), &index)
		if index < 1 || out.hasPlate(index) {
			return out, fmt.Errorf("invalid saved plate index")
		}
		out.Plates = append(out.Plates, projectPlate{Index: index, Name: value(p.Metadata, "plater_name")})
		for _, inst := range p.Instances {
			membership[value(inst.Metadata, "object_id")+":"+value(inst.Metadata, "instance_id")] = index
		}
	}
	counts := map[string]int{}
	for i, item := range model.Items {
		key := fmt.Sprintf("%s:%d", item.ID, counts[item.ID])
		counts[item.ID]++
		out.Membership[i] = membership[key]
	}
	points := stringList(config["printable_area"])
	if s := stringValue(config["printable_area"]); s != "" {
		points = strings.Split(s, ",")
	}
	b := meshBounds{}
	for _, point := range points {
		xy := strings.Split(point, "x")
		if len(xy) != 2 {
			continue
		}
		x, e1 := strconv.ParseFloat(xy[0], 64)
		y, e2 := strconv.ParseFloat(xy[1], 64)
		if e1 != nil || e2 != nil {
			continue
		}
		if err := b.add(meshPoint{x, y, 0}); err != nil {
			return out, err
		}
	}
	out.Width, out.Depth = b.max[0]-b.min[0], b.max[1]-b.min[1]
	return out, nil
}
