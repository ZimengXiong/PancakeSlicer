package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

type projectInstance struct {
	Printable *bool     `json:"printable"`
	Transform string    `json:"-"`
	Object    string    `json:"-"`
	Delta     []float64 `json:"transform"`
}

func projectTransformsWorkaround(path string, edits []projectInstance) error {
	source, err := savedProjectInstances(path)
	if err != nil {
		return err
	}
	if len(source) != len(edits) {
		return fmt.Errorf("project build item count changed")
	}
	for i := range edits {
		if edits[i].Delta == nil {
			continue
		}
		if len(edits[i].Delta) != 12 {
			return fmt.Errorf("project transform needs 12 numbers")
		}
		var delta modelTransform
		for k, v := range edits[i].Delta {
			if math.IsNaN(v) || math.IsInf(v, 0) {
				return fmt.Errorf("non-finite project transform")
			}
			delta[k] = v
		}
		t, err := transformValue(source[i].Transform)
		if err != nil {
			return err
		}
		values := make([]string, 12)
		for r := 0; r < 4; r++ {
			for c := 0; c < 3; c++ {
				v := 0.0
				if r == 3 {
					v = delta[9+c]
				}
				for k := 0; k < 3; k++ {
					v += t[r*3+k] * delta[k*3+c]
				}
				values[r*3+c] = strconv.FormatFloat(v, 'g', -1, 64)
			}
		}
		edits[i].Transform = strings.Join(values, " ")
	}
	return patchProject(path, edits)
}

var buildPattern = regexp.MustCompile(`(?s)<build\b[^>]*>.*?</build>`)
var itemPattern = regexp.MustCompile(`<item\b[^>]*>`)
var printablePattern = regexp.MustCompile(`\sprintable="[^"]*"`)
var transformPattern = regexp.MustCompile(`\stransform="([^"]*)"`)

func patchProject(path string, instances []projectInstance) error {
	if len(instances) == 0 {
		return nil
	}
	root, err := projectRoot(path)
	if err != nil {
		return err
	}
	n := 0
	found, err := rewriteProject(path, func(name string) bool { return strings.EqualFold(name, root) }, func(data []byte) ([]byte, error) {
		data = buildPattern.ReplaceAllFunc(data, func(build []byte) []byte {
			return itemPattern.ReplaceAllFunc(build, func(item []byte) []byte {
				if n >= len(instances) {
					n++
					return item
				}
				state := instances[n]
				n++
				if state.Printable != nil {
					value := "0"
					if *state.Printable {
						value = "1"
					}
					item = printablePattern.ReplaceAll(item, nil)
					item = bytes.Replace(item, []byte("<item"), []byte(`<item printable="`+value+`"`), 1)
				}
				if state.Transform != "" {
					item = transformPattern.ReplaceAll(item, nil)
					item = bytes.Replace(item, []byte("<item"), []byte(`<item transform="`+state.Transform+`"`), 1)
				}
				return item
			})
		})
		if n != len(instances) {
			return nil, fmt.Errorf("project has %d build items, expected %d", n, len(instances))
		}
		return data, nil
	})
	if err == nil && !found {
		err = fmt.Errorf("project has no root model part")
	}
	return err
}

func projectRoot(path string) (string, error) {
	src, err := zip.OpenReader(path)
	if err != nil {
		return "", err
	}
	defer src.Close()
	root := "3D/3dmodel.model"
	for _, f := range src.File {
		if !strings.EqualFold(f.Name, "_rels/.rels") {
			continue
		}
		r, err := f.Open()
		if err != nil {
			return "", err
		}
		var rels struct {
			Items []struct {
				Type   string `xml:"Type,attr"`
				Target string `xml:"Target,attr"`
			} `xml:"Relationship"`
		}
		err = xml.NewDecoder(io.LimitReader(r, 1<<20)).Decode(&rels)
		r.Close()
		if err != nil {
			return "", err
		}
		for _, rel := range rels.Items {
			if strings.HasSuffix(rel.Type, "/3dmodel") {
				root = strings.TrimPrefix(rel.Target, "/")
			}
		}
	}
	return root, nil
}

func patchProjectSettings(path string, settings [][2]string) error {
	if len(settings) == 0 {
		return nil
	}
	_, err := rewriteProject(path, func(name string) bool {
		return strings.EqualFold(name, "Metadata/model_settings.config") || strings.EqualFold(name, "Metadata/layer_config_ranges.xml")
	}, func(data []byte) ([]byte, error) {
		for _, kv := range settings {
			key, value := regexp.QuoteMeta(kv[0]), xmlEscape(kv[1])
			for _, re := range []*regexp.Regexp{
				regexp.MustCompile(`(<metadata\s+key="` + key + `"\s+value=")[^"]*(")`),
				regexp.MustCompile(`(<option\s+opt_key="` + key + `"\s*>)[^<]*(</option>)`),
			} {
				data = re.ReplaceAllFunc(data, func(m []byte) []byte {
					g := re.FindSubmatch(m)
					return []byte(string(g[1]) + value + string(g[2]))
				})
			}
		}
		return data, nil
	})
	return err
}

func xmlEscape(s string) string {
	var b strings.Builder
	_ = xml.EscapeText(&b, []byte(s))
	return strings.ReplaceAll(b.String(), `"`, "&#34;")
}

func rewriteProject(path string, match func(name string) bool, edit func(data []byte) ([]byte, error)) (bool, error) {
	src, err := zip.OpenReader(path)
	if err != nil {
		return false, err
	}
	defer src.Close()
	tmp, err := os.CreateTemp(filepath.Dir(path), ".project-*.3mf")
	if err != nil {
		return false, err
	}
	name := tmp.Name()
	defer os.Remove(name)
	dst := zip.NewWriter(tmp)
	found := false
	for _, f := range src.File {
		if !match(f.Name) {
			if err = dst.Copy(f); err != nil {
				break
			}
			continue
		}
		found = true
		var r io.ReadCloser
		r, err = f.Open()
		if err != nil {
			break
		}
		var data []byte
		data, err = io.ReadAll(io.LimitReader(r, 256<<20))
		r.Close()
		if err != nil {
			break
		}
		if data, err = edit(data); err != nil {
			break
		}
		var w io.Writer
		w, err = dst.CreateHeader(&f.FileHeader)
		if err != nil {
			break
		}
		if _, err = w.Write(data); err != nil {
			break
		}
	}
	if closeErr := dst.Close(); err == nil {
		err = closeErr
	}
	if err == nil {
		err = tmp.Sync()
	}
	if closeErr := tmp.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		return false, err
	}
	if err = src.Close(); err != nil {
		return false, err
	}
	if err = os.Rename(name, path); err != nil {
		return false, err
	}
	return found, syncWorkspaceDirectory(filepath.Dir(path))
}

func savedProjectInstances(path string) ([]projectInstance, error) {
	src, err := zip.OpenReader(path)
	if err != nil {
		return nil, err
	}
	defer src.Close()
	for _, f := range src.File {
		if !strings.HasSuffix(strings.ToLower(f.Name), ".model") {
			continue
		}
		r, err := f.Open()
		if err != nil {
			return nil, err
		}
		decoder := xml.NewDecoder(r)
		var out []projectInstance
		inBuild := false
		for {
			token, parseErr := decoder.Token()
			if parseErr == io.EOF {
				break
			}
			if parseErr != nil {
				r.Close()
				return nil, parseErr
			}
			switch t := token.(type) {
			case xml.StartElement:
				if t.Name.Local == "build" {
					inBuild = true
				}
				if inBuild && t.Name.Local == "item" {
					state := projectInstance{}
					for _, a := range t.Attr {
						if a.Name.Local == "transform" {
							state.Transform = a.Value
						}
						if a.Name.Local == "objectid" {
							state.Object = a.Value
						}
						if a.Name.Local == "printable" {
							v := a.Value != "0"
							state.Printable = &v
						}
					}
					if state.Transform == "" {
						state.Transform = "1 0 0 0 1 0 0 0 1 0 0 0"
					}
					out = append(out, state)
				}
			case xml.EndElement:
				if t.Name.Local == "build" {
					inBuild = false
				}
			}
		}
		r.Close()
		if len(out) > 0 {
			return out, nil
		}
	}
	return nil, fmt.Errorf("project has no build items")
}

func writeProjectEdits(dir string, files []string, uploaded map[string]bool, raw string) error {
	if !strings.EqualFold(filepath.Ext(files[0]), ".3mf") {
		return fmt.Errorf("project_edits requires the 3MF project as the first upload")
	}
	var edits []map[string]json.RawMessage
	if json.Unmarshal([]byte(raw), &edits) != nil || len(edits) == 0 {
		return fmt.Errorf("project_edits needs a non-empty array of edits")
	}
	used := map[string]bool{}
	mesh := func(file string) bool {
		return uploaded[file] && file != files[0] && !strings.EqualFold(filepath.Ext(file), ".3mf")
	}
	for _, e := range edits {
		var op string
		if json.Unmarshal(e["op"], &op) != nil {
			return fmt.Errorf("project_edits: every edit needs an op")
		}
		_, hasFile := e["file"]
		if op != "add_object" && op != "add_volume" {
			if hasFile {
				return fmt.Errorf("project_edits: only add_object and add_volume take a file")
			}
			continue
		}
		var file string
		if json.Unmarshal(e["file"], &file) != nil || !mesh(file) {
			return fmt.Errorf("project_edits: %s must name an uploaded mesh", op)
		}
		used[file] = true
		if rows, ok := e["volumes"]; ok && op == "add_object" {
			var list []struct {
				File string `json:"file"`
			}
			if json.Unmarshal(rows, &list) != nil {
				return fmt.Errorf("project_edits: add_object volumes must be an array")
			}
			for _, v := range list {
				if !mesh(v.File) {
					return fmt.Errorf("project_edits: add_object volumes must name uploaded meshes")
				}
				used[v.File] = true
			}
		}
	}
	for _, f := range files[1:] {
		if !used[f] {
			return fmt.Errorf("project_edits: upload %s is not used by an edit", f)
		}
	}
	data, err := json.Marshal(map[string]any{"edits": edits})
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "edits.json"), data, 0600)
}

func projectEditBounds(dir, project string, plate int) (map[int]bool, string, error) {
	data, err := os.ReadFile(filepath.Join(dir, "edits.json"))
	if err != nil {
		return nil, "", err
	}
	var list struct {
		Edits []map[string]json.RawMessage `json:"edits"`
	}
	if err := json.Unmarshal(data, &list); err != nil {
		return nil, "", err
	}
	items, err := savedProjectInstances(filepath.Join(dir, project))
	if err != nil {
		return nil, "", err
	}
	deleted := map[int]bool{}
	var added []map[string]json.RawMessage
	for _, e := range list.Edits {
		var op string
		json.Unmarshal(e["op"], &op)
		switch op {
		case "delete":
			var object, instance int
			json.Unmarshal(e["object"], &object)
			_, one := e["instance"]
			json.Unmarshal(e["instance"], &instance)
			seen := map[string]int{}
			for i, item := range items {
				n := seen[item.Object]
				seen[item.Object]++
				if item.Object == strconv.Itoa(object) && (!one || n == instance) {
					deleted[i] = true
				}
			}
		case "add_object":
			on := 1
			json.Unmarshal(e["plate"], &on)
			if plate == 0 || on == plate {
				added = append(added, e)
			}
		}
	}
	if len(added) == 0 {
		return deleted, "", nil
	}
	raw, err := json.Marshal(map[string]any{"objects": added})
	return deleted, string(raw), err
}
