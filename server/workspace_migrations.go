package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

const migrationSlantMorph = "slant-morph-default-0"

const migrationFilamentsToUse = "drop-filaments-to-use"

const migrationSlotNoticeChoices = "slot-notices-choices-only"

const migrationRenamedValues = "renamed-values-h2d-head"

const migrationRecordRevision = "record-revision-not-stored"

const migrationVersionsInWorkspace = "versions-in-workspace"

const migrationLegacyTemps = "remove-legacy-write-temps"

const migrationDraftLifecycle = "draft-lifecycle"

const migrationSnapshotMixes = "snapshot-mixes-by-id"

func migrateWorkspace(d *workspaceData, now time.Time, root string) ([]string, error) {
	var ran []string
	mark := func(id string) {
		if d.Migrations == nil {
			d.Migrations = map[string]int64{}
		}
		d.Migrations[id] = now.UnixMilli()
		ran = append(ran, id)
	}
	if _, done := d.Migrations[migrationVersionsInWorkspace]; !done {
		if err := importVersionIndexes(d, filepath.Join(root, "versions")); err != nil {
			return nil, fmt.Errorf("%s: %w", migrationVersionsInWorkspace, err)
		}
		mark(migrationVersionsInWorkspace)
	}
	if _, done := d.Migrations[migrationSlantMorph]; !done {
		for id, p := range d.Projects {
			if p.Record == nil {
				continue
			}
			if raw, ok := slantMorphRecord(p.Record); ok {
				p.Record = raw
				d.Projects[id] = p
			}
		}
		for id, dr := range d.Drafts {
			if raw, ok := slantMorphRecord(dr.Document); ok {
				dr.Document = raw
				d.Drafts[id] = dr
			}
		}
		mark(migrationSlantMorph)
	}
	if _, done := d.Migrations[migrationFilamentsToUse]; !done {
		dropPreference(d, "orca.web.filamentsToUse")
		mark(migrationFilamentsToUse)
	}
	if _, done := d.Migrations[migrationSlotNoticeChoices]; !done {
		slotNoticeChoices(d)
		mark(migrationSlotNoticeChoices)
	}
	if _, done := d.Migrations[migrationRenamedValues]; !done {
		if err := renameWorkspaceValues(d); err != nil {
			return nil, fmt.Errorf("%s: %w", migrationRenamedValues, err)
		}
		mark(migrationRenamedValues)
	}
	if _, done := d.Migrations[migrationRecordRevision]; !done {
		if err := dropRecordRevisions(d); err != nil {
			return nil, fmt.Errorf("%s: %w", migrationRecordRevision, err)
		}
		mark(migrationRecordRevision)
	}
	if _, done := d.Migrations[migrationSnapshotMixes]; !done {
		for _, id := range sortedKeys(d.Projects) {
			p := d.Projects[id]
			if out, lines := snapshotMixesByID(p.Record); len(lines) > 0 {
				if err := d.moveProjectRevision(id, out, p.Revision, p.Revision+1); err != nil {
					return nil, fmt.Errorf("%s: %w", migrationSnapshotMixes, err)
				}
				for _, l := range lines {
					log.Printf("workspace migration: project %s: %s", id, l)
				}
			}
		}
		for _, id := range sortedKeys(d.Drafts) {
			dr := d.Drafts[id]
			if out, lines := snapshotMixesByID(dr.Document); len(lines) > 0 {
				dr.Document = out
				d.Drafts[id] = dr
				for _, l := range lines {
					log.Printf("workspace migration: draft %s: %s", id, l)
				}
			}
		}
		mark(migrationSnapshotMixes)
	}
	if _, done := d.Migrations[migrationLegacyTemps]; !done {
		if err := removeLegacyTemps(filepath.Dir(root)); err != nil {
			return nil, fmt.Errorf("%s: %w", migrationLegacyTemps, err)
		}
		mark(migrationLegacyTemps)
	}
	if _, done := d.Migrations[migrationDraftLifecycle]; !done {
		retireUnreachableDrafts(d)
		mark(migrationDraftLifecycle)
	}
	return ran, nil
}

func retireUnreachableDrafts(d *workspaceData) {
	if v, ok := d.Values["session"]; ok {
		var session map[string]json.RawMessage
		if json.Unmarshal(v.Document, &session) == nil && session["projectDraftOwners"] != nil {
			log.Printf("workspace migration: session draft owner lineage removed (%s)", session["projectDraftOwners"])
			delete(session, "projectDraftOwners")
			v.Document, v.Revision = must(json.Marshal(session)), v.Revision+1
			d.Values["session"] = v
		}
	}
	type draft struct {
		key      string
		project  string
		base     uint64
		edited   float64
		document map[string]json.RawMessage
	}
	var dirty []draft
	retire := func(key, why string) {
		delete(d.Drafts, key)
		log.Printf("workspace migration: draft %s retired: %s", key, why)
	}
	for _, key := range sortedKeys(d.Drafts) {
		var x struct {
			ProjectID string                     `json:"projectId"`
			Base      uint64                     `json:"baseRevision"`
			Kind      string                     `json:"kind"`
			EditedAt  float64                    `json:"editedAt"`
			UpdatedAt float64                    `json:"updatedAt"`
			Doc       map[string]json.RawMessage `json:"doc"`
		}
		json.Unmarshal(d.Drafts[key].Document, &x)
		if saved := projectDocument(d, x.ProjectID); saved != nil && x.Doc != nil && sameDocument(x.Doc, saved) {
			retire(key, fmt.Sprintf("its document is project %s's saved one", x.ProjectID))
			continue
		}
		if x.Kind == "snapshot" {
			d.Drafts[key] = offeredSnapshot(d.Drafts[key])
			log.Printf("workspace migration: draft %s, a snapshot of project %s (base revision %d), is offered when the project opens", key, x.ProjectID, x.Base)
		}
		dirty = append(dirty, draft{key, x.ProjectID, x.Base, max(x.EditedAt, x.UpdatedAt), x.Doc})
	}
	sort.SliceStable(dirty, func(i, j int) bool { return dirty[i].edited > dirty[j].edited })
	var kept []draft
	for _, x := range dirty {
		if i := slices.IndexFunc(kept, func(k draft) bool {
			return k.project == x.project && k.base == x.base && x.document != nil && k.document != nil && sameDocument(k.document, x.document)
		}); i >= 0 {
			retire(x.key, fmt.Sprintf("a copy of draft %s of project %s", kept[i].key, x.project))
			continue
		}
		kept = append(kept, x)
	}
	log.Printf("workspace migration: %d drafts kept, each offered when its project opens", len(d.Drafts))
}

func offeredSnapshot(dr workspaceDraft) workspaceDraft {
	kind := func(raw json.RawMessage) json.RawMessage {
		var m map[string]json.RawMessage
		if json.Unmarshal(raw, &m) != nil || m == nil {
			return raw
		}
		m["kind"] = json.RawMessage(`"dirty"`)
		return must(json.Marshal(m))
	}
	dr.Document = kind(dr.Document)
	if len(dr.Metadata) > 0 {
		dr.Metadata = kind(dr.Metadata)
	}
	return dr
}

func dropPreference(d *workspaceData, key string) {
	v, ok := d.Values["preferences"]
	var prefs map[string]json.RawMessage
	if !ok || json.Unmarshal(v.Document, &prefs) != nil || prefs[key] == nil {
		log.Printf("workspace migration: preference %s was not set", key)
		return
	}
	log.Printf("workspace migration: removed preference %s (%s)", key, prefs[key])
	delete(prefs, key)
	v.Document, v.Revision = must(json.Marshal(prefs)), v.Revision+1
	d.Values["preferences"] = v
}

func slotNoticeChoices(d *workspaceData) {
	v, ok := d.Values["session"]
	var session map[string]json.RawMessage
	if !ok || json.Unmarshal(v.Document, &session) != nil || session["projectSlotNotices"] == nil {
		return
	}
	var notices map[string]struct {
		At        json.Number `json:"at"`
		Dismissed bool        `json:"dismissed"`
	}
	if json.Unmarshal(session["projectSlotNotices"], &notices) != nil {
		return
	}
	kept := map[string]any{}
	for id, n := range notices {
		if n.Dismissed {
			kept[id] = map[string]any{"dismissed": true, "at": n.At}
		}
	}
	log.Printf("workspace migration: project slot notices kept %d choices of %d entries", len(kept), len(notices))
	session["projectSlotNotices"] = must(json.Marshal(kept))
	v.Document, v.Revision = must(json.Marshal(session)), v.Revision+1
	d.Values["session"] = v
}

func migrateVersionRecord(d *workspaceData, raw json.RawMessage, at int64) json.RawMessage {
	if ran, ok := d.Migrations[migrationSlantMorph]; ok && at < ran {
		if out, changed := slantMorphRecord(raw); changed {
			raw = out
		}
	}
	if ran, ok := d.Migrations[migrationRenamedValues]; ok && at < ran {
		if out, changes := renameStoredValues(raw); len(changes) > 0 {
			raw = out
		}
	}
	if ran, ok := d.Migrations[migrationSnapshotMixes]; ok && at < ran {
		if out, lines := snapshotMixesByID(raw); len(lines) > 0 {
			raw = out
		}
	}
	return raw
}

func snapshotMixesByID(raw json.RawMessage) (json.RawMessage, []string) {
	var record map[string]json.RawMessage
	var doc, snapshots, printer map[string]json.RawMessage
	if json.Unmarshal(raw, &record) != nil || json.Unmarshal(record["doc"], &doc) != nil || json.Unmarshal(doc["profileSnapshots"], &snapshots) != nil ||
		json.Unmarshal(snapshots["printer"], &printer) != nil || len(printer["mixes"]) == 0 || printer["mixes"][0] != '{' {
		return raw, nil
	}
	var printerSlots, ownSlots []map[string]json.RawMessage
	json.Unmarshal(printer["slots"], &printerSlots)
	_, hasOwn := snapshots["slots"]
	json.Unmarshal(snapshots["slots"], &ownSlots)
	name := "printer snapshot " + stringValue(printer["name"])
	slots := printerSlots
	if hasOwn {
		slots = ownSlots
	}
	kept, mixes, lines := slotMixesByID(name, slots, printer["mixes"])
	if hasOwn {
		snapshots["slots"] = must(json.Marshal(kept))
		var keys map[string]json.RawMessage
		json.Unmarshal(printer["mixes"], &keys)
		physical := []map[string]json.RawMessage{}
		for i, s := range printerSlots {
			if _, mixed := keys[strconv.Itoa(i)]; !mixed {
				physical = append(physical, s)
			}
		}
		printerSlots = physical
	} else {
		printerSlots = kept
	}
	printer["slots"] = must(json.Marshal(printerSlots))
	if len(mixes) > 0 {
		printer["mixes"] = must(json.Marshal(mixes))
	} else {
		delete(printer, "mixes")
	}
	snapshots["printer"] = must(json.Marshal(printer))
	doc["profileSnapshots"] = must(json.Marshal(snapshots))
	record["doc"] = must(json.Marshal(doc))
	return must(json.Marshal(record)), append(lines, "printer snapshot mixes keyed by id")
}

func renameWorkspaceValues(d *workspaceData) error {
	logChanges := func(what string, changes []string) {
		for _, c := range changes {
			log.Printf("workspace migration: %s: %s", what, c)
		}
	}
	for _, id := range sortedKeys((d.Projects)) {
		p := d.Projects[id]
		if out, changes := renameStoredValues(p.Record); len(changes) > 0 {
			if err := d.moveProjectRevision(id, out, p.Revision, p.Revision+1); err != nil {
				return err
			}
			logChanges("project "+id, changes)
		}
	}
	for _, id := range sortedKeys((d.Drafts)) {
		dr := d.Drafts[id]
		if out, changes := renameStoredValues(dr.Document); len(changes) > 0 {
			dr.Document = out
			d.Drafts[id] = dr
			logChanges("draft "+id, changes)
		}
	}
	for _, key := range sortedKeys((d.Values)) {
		v := d.Values[key]
		if out, changes := renameStoredValues(v.Document); len(changes) > 0 {
			v.Document, v.Revision = out, v.Revision+1
			d.Values[key] = v
			logChanges("value "+key, changes)
		}
	}
	return nil
}

func importVersionIndexes(d *workspaceData, dir string) error {
	entries, err := os.ReadDir(dir)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	imported := 0
	for _, e := range entries {
		id := e.Name()
		if !e.IsDir() || id[0] == '.' {
			continue
		}
		path := filepath.Join(dir, id, "index.json")
		skip := func(code, file string, err error) {
			if d.Problems == nil {
				d.Problems = map[string]workspaceProblem{}
			}
			rel, relErr := filepath.Rel(filepath.Dir(dir), file)
			if relErr != nil {
				rel = filepath.Base(file)
			}
			reason := err
			for pe := (*fs.PathError)(nil); errors.As(reason, &pe); {
				reason = pe.Err
			}
			d.Problems[id] = workspaceProblem{Code: code, File: filepath.ToSlash(rel), Message: reason.Error()}
			log.Printf("workspace migration: project %s history not imported (%s): %s: %v", id, code, file, err)
		}
		raw, err := os.ReadFile(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			skip("version_history_unreadable", path, err)
			continue
		}
		p, ok := d.Projects[id]
		if !ok {
			log.Printf("workspace migration: %s belongs to no project in the workspace; left as it is", path)
			continue
		}
		var x versionIndex
		if err := json.Unmarshal(raw, &x); err != nil {
			skip("version_history_unreadable", path, err)
			continue
		}
		if x.Versions == nil {
			x.Versions = []projectVersion{}
		}
		var record []byte
		if last := lastVersion(&x); last != nil && last.Revision != p.Revision && p.Record != nil {
			file := filepath.Join(dir, id, "records", last.Record+".json")
			if record, err = os.ReadFile(file); err != nil {
				skip("version_record_missing", file, fmt.Errorf("version %d's record: %w", last.ID, err))
				continue
			}
		}
		p.Versions = &x
		d.Projects[id] = p
		imported++
		newest := "none"
		if last := lastVersion(&x); last != nil {
			newest = fmt.Sprint(last.Revision)
		}
		log.Printf("workspace migration: project %s history: %d versions, newest on revision %s, project at %d", id, len(x.Versions), newest, p.Revision)
		if last := lastVersion(&x); record != nil {
			if recordContent(record) == recordContent(p.Record) {
				log.Printf("workspace migration: project %s version %d shows the project's document; revision %d -> %d", id, last.ID, last.Revision, p.Revision)
				last.Revision = p.Revision
			} else {
				log.Printf("workspace migration: project %s version %d holds an older document than revision %d; the next save makes its version", id, last.ID, p.Revision)
			}
		}
	}
	log.Printf("workspace migration: %d version histories moved into the workspace from %s", imported, dir)
	return nil
}

func dropRecordRevisions(d *workspaceData) error {
	dropped := 0
	for _, id := range sortedKeys(d.Projects) {
		p := d.Projects[id]
		if p.Record == nil {
			continue
		}
		var record struct {
			Meta struct {
				Revision *uint64 `json:"revision"`
			} `json:"meta"`
		}
		if err := json.Unmarshal(p.Record, &record); err != nil {
			return fmt.Errorf("Saved project %s has corrupted data: %w", id, err)
		}
		named := record.Meta.Revision
		if named == nil {
			continue
		}
		if *named != p.Revision {
			if err := d.moveProjectRevision(id, p.Record, *named, p.Revision); err != nil {
				return err
			}
		} else if next, err := withRecord(p, id, p.Record, p.Revision); err != nil {
			return fmt.Errorf("Saved project %s has corrupted data: %w", id, err)
		} else {
			d.Projects[id] = next
		}
		dropped++
	}
	log.Printf("workspace migration: meta.revision left %d stored records; the project's revision is the one copy", dropped)
	return nil
}

func (d *workspaceData) moveProjectRevision(id string, raw json.RawMessage, from, to uint64) error {
	p, err := withRecord(d.Projects[id], id, raw, to)
	if err != nil {
		return fmt.Errorf("Saved project %s has corrupted data: %w", id, err)
	}
	if x := p.history(); lastVersion(x) != nil && lastVersion(x).Revision == from {
		lastVersion(x).Revision = to
		p.Versions = x
		log.Printf("workspace migration: project %s version %d revision %d -> %d", id, lastVersion(x).ID, from, to)
	}
	d.Projects[id] = p
	log.Printf("workspace migration: project %s revision %d -> %d", id, from, to)
	rebaseDrafts(d, id, from, to)
	return nil
}

func rebaseDrafts(d *workspaceData, id string, from, to uint64) {
	rebase := func(raw json.RawMessage) (json.RawMessage, bool) {
		var doc map[string]json.RawMessage
		var project string
		var base uint64
		if json.Unmarshal(raw, &doc) != nil || json.Unmarshal(doc["projectId"], &project) != nil || project != id ||
			json.Unmarshal(doc["baseRevision"], &base) != nil || base != from {
			return raw, false
		}
		doc["baseRevision"] = must(json.Marshal(to))
		return must(json.Marshal(doc)), true
	}
	for _, key := range sortedKeys(d.Drafts) {
		dr := d.Drafts[key]
		document, inDocument := rebase(dr.Document)
		metadata, inMetadata := rebase(dr.Metadata)
		if inDocument || inMetadata {
			dr.Document, dr.Metadata = document, metadata
			d.Drafts[key] = dr
			log.Printf("workspace migration: draft %s of project %s baseRevision %d -> %d", key, id, from, to)
		}
	}
}

func renameStoredValues(raw json.RawMessage) (json.RawMessage, []string) {
	if len(raw) == 0 || !renamedKeyIn(raw) {
		return raw, nil
	}
	var doc any
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if decoder.Decode(&doc) != nil {
		return raw, nil
	}
	var changes []string
	var walk func(v any, path string) any
	walk = func(v any, path string) any {
		switch x := v.(type) {
		case map[string]any:
			for _, k := range sortedKeys((x)) {
				if resolve.RenamedValue(k) {
					old := must(json.Marshal(x[k]))
					if next := resolve.CanonicalValue(k, old); !bytes.Equal(next, old) {
						var value any
						json.Unmarshal(next, &value)
						x[k] = value
						changes = append(changes, fmt.Sprintf("%s%s: %s -> %s", path, k, old, next))
						continue
					}
				}
				x[k] = walk(x[k], path+k+".")
			}
		case []any:
			for i := range x {
				x[i] = walk(x[i], fmt.Sprintf("%s%d.", path, i))
			}
		}
		return v
	}
	doc = walk(doc, "")
	if len(changes) == 0 {
		return raw, nil
	}
	return must(json.Marshal(doc)), changes
}

func renamedKeyIn(raw json.RawMessage) bool {
	for _, k := range resolve.RenamedKeys() {
		if bytes.Contains(raw, []byte(`"`+k+`"`)) {
			return true
		}
	}
	return false
}

func slantMorphRecord(raw json.RawMessage) (json.RawMessage, bool) {
	var record map[string]json.RawMessage
	if json.Unmarshal(raw, &record) != nil || record == nil {
		return raw, false
	}
	var doc map[string]any
	decoder := json.NewDecoder(bytes.NewReader(record["doc"]))
	decoder.UseNumber()
	if decoder.Decode(&doc) != nil || doc == nil || !choosesSlant(doc) {
		return raw, false
	}
	dirty, _ := doc["dirty"].(map[string]any)
	if dirty == nil {
		dirty = map[string]any{}
	}
	process, _ := dirty["process"].(map[string]any)
	if process == nil {
		process = map[string]any{}
	}
	if _, set := process["nonplanar_slant_morph_layers"]; set {
		return raw, false
	}
	process["nonplanar_slant_morph_layers"] = "10"
	dirty["process"] = process
	doc["dirty"] = dirty
	encoded, err := json.Marshal(doc)
	if err != nil {
		return raw, false
	}
	record["doc"] = encoded
	out, err := json.Marshal(record)
	if err != nil {
		return raw, false
	}
	return out, true
}

func choosesSlant(v any) bool {
	switch x := v.(type) {
	case map[string]any:
		for k, child := range x {
			if k == "nonplanar_mode" && child == "slant" {
				return true
			}
			if choosesSlant(child) {
				return true
			}
		}
	case []any:
		for _, child := range x {
			if choosesSlant(child) {
				return true
			}
		}
	}
	return false
}

func sortedKeys[V any](m map[string]V) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
