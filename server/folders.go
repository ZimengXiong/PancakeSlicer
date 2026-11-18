package main

import (
	"encoding/json"
	"net/http"
	"sort"
	"strings"
	"time"
	"unicode"
)

type workspaceFolder struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	ParentID  string `json:"parentId,omitempty"`
	Revision  uint64 `json:"revision"`
	CreatedAt int64  `json:"createdAt"`
	UpdatedAt int64  `json:"updatedAt"`
}

const maxFolders, maxFolderDepth = 1000, 16

func folderName(name string) (string, bool) {
	name = strings.TrimSpace(name)
	if name == "" || len(name) > 120 {
		return "", false
	}
	for _, r := range name {
		if unicode.IsControl(r) {
			return "", false
		}
	}
	return name, true
}

func folderNameTaken(d *workspaceData, parent, name, except string) bool {
	for id, f := range d.Folders {
		if id != except && f.ParentID == parent && strings.EqualFold(f.Name, name) {
			return true
		}
	}
	return false
}

func folderDepth(d *workspaceData, parent, except string) int {
	depth := 0
	for parent != "" {
		f, ok := d.Folders[parent]
		if !ok || parent == except || depth > maxFolderDepth {
			return -1
		}
		parent, depth = f.ParentID, depth+1
	}
	return depth
}

func foldersView(d *workspaceData) map[string]any {
	list := make([]workspaceFolder, 0, len(d.Folders))
	for _, f := range d.Folders {
		list = append(list, f)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].ID < list[j].ID })
	return map[string]any{"revision": d.FolderRevision, "folders": list}
}

func (st *workspaceStore) folderRoutes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/folders", func(w http.ResponseWriter, r *http.Request) {
		st.view(w, func(d *workspaceData) { writeJSON(w, 200, foldersView(d)) })
	})
	mux.HandleFunc("POST /api/folders", func(w http.ResponseWriter, r *http.Request) { st.folderWrite(w, r, "create") })
	mux.HandleFunc("PATCH /api/folders/{id}", func(w http.ResponseWriter, r *http.Request) { st.folderWrite(w, r, "update") })
	mux.HandleFunc("POST /api/folders/{id}/delete", func(w http.ResponseWriter, r *http.Request) { st.folderWrite(w, r, "delete") })
	mux.HandleFunc("POST /api/folders/move", func(w http.ResponseWriter, r *http.Request) { st.folderWrite(w, r, "move") })
}

func (st *workspaceStore) folderWrite(w http.ResponseWriter, r *http.Request, action string) {
	var in struct {
		OperationID      string          `json:"operationId"`
		ExpectedRevision uint64          `json:"expectedRevision"`
		ID               string          `json:"id"`
		Name             *string         `json:"name"`
		ParentID         json.RawMessage `json:"parentId"`
		FolderID         *string         `json:"folderId"`
		ProjectIDs       []string        `json:"projectIds"`
	}
	if !workspaceDecode(w, r, &in) {
		return
	}
	if action != "create" && action != "move" {
		in.ID = r.PathValue("id")
	}
	digest := workspaceDigest([]any{"folder", action, in})
	invalid := func(message string) { workspaceFailure(w, 400, "folder_invalid", message, "reload_and_reapply", 0) }
	st.locked(w, func(d *workspaceData) {
		if workspaceReplay(w, d, in.OperationID, digest) {
			return
		}
		if in.ExpectedRevision != d.FolderRevision {
			workspaceFailure(w, 409, "revision_conflict", "Heads up — folders changed in another window! Reload and try again.", "reload_and_reapply", d.FolderRevision)
			return
		}
		now := time.Now().UnixMilli()
		parent, parentSet := "", len(in.ParentID) > 0
		if parentSet && string(in.ParentID) != "null" && json.Unmarshal(in.ParentID, &parent) != nil {
			invalid("parentId must be a folder ID or null.")
			return
		}
		switch action {
		case "create":
			name, ok := folderName(stringOr(in.Name))
			switch {
			case !workspaceID(in.ID) || in.ID == "move":
				invalid("We need a fresh, stable folder ID here.")
				return
			case d.Folders[in.ID].ID != "":
				invalid("Oops, that folder ID already exists.")
				return
			case !ok:
				invalid("Folder names need to be 1 to 120 characters.")
				return
			case len(d.Folders) >= maxFolders:
				invalid("Whoa, a library can hold up to 1,000 folders max!")
				return
			case folderDepth(d, parent, "") < 0 || folderDepth(d, parent, "") >= maxFolderDepth:
				invalid("Oops, the parent folder is missing or nested a bit too deep.")
				return
			case folderNameTaken(d, parent, name, ""):
				workspaceFailure(w, 409, "folder_name_taken", "There's already a folder with that name here!", "rename", d.FolderRevision)
				return
			}
			d.Folders[in.ID] = workspaceFolder{in.ID, name, parent, 1, now, now}
		case "update":
			f, exists := d.Folders[in.ID]
			if !exists {
				workspaceFailure(w, 404, "folder_missing", "Hmm, that folder isn't around anymore.", "reload_and_reapply", d.FolderRevision)
				return
			}
			if in.Name != nil {
				name, ok := folderName(*in.Name)
				if !ok {
					invalid("Folder names need to be 1 to 120 characters.")
					return
				}
				f.Name = name
			}
			if parentSet {
				if depth := folderDepth(d, parent, in.ID); depth < 0 || depth >= maxFolderDepth {
					invalid("A folder can't move into itself, a missing folder, or too deep!")
					return
				}
				f.ParentID = parent
			}
			if folderNameTaken(d, f.ParentID, f.Name, in.ID) {
				workspaceFailure(w, 409, "folder_name_taken", "There's already a folder with that name here!", "rename", d.FolderRevision)
				return
			}
			f.Revision++
			f.UpdatedAt = now
			d.Folders[in.ID] = f
		case "delete":
			f, exists := d.Folders[in.ID]
			if !exists {
				workspaceFailure(w, 404, "folder_missing", "Hmm, that folder isn't around anymore.", "reload_and_reapply", d.FolderRevision)
				return
			}
			delete(d.Folders, in.ID)
			for id, child := range d.Folders {
				if child.ParentID == in.ID {
					child.ParentID = f.ParentID
					for folderNameTaken(d, child.ParentID, child.Name, id) {
						child.Name += " (moved)"
					}
					child.Revision++
					child.UpdatedAt = now
					d.Folders[id] = child
				}
			}
			for id, p := range d.Projects {
				if p.Folder == in.ID {
					p.Folder = ""
					d.Projects[id] = p
				}
			}
		case "move":
			target := stringOr(in.FolderID)
			if target != "" && d.Folders[target].ID == "" {
				workspaceFailure(w, 404, "folder_missing", "Oops, the destination folder isn't around anymore.", "reload_and_reapply", d.FolderRevision)
				return
			}
			if len(in.ProjectIDs) == 0 || len(in.ProjectIDs) > 10000 {
				invalid("projectIds lists the projects to move.")
				return
			}
			ids := make([]string, 0, len(in.ProjectIDs))
			for _, raw := range in.ProjectIDs {
				id, err := canonicalWorkspaceProject(d, raw)
				if _, exists := d.Projects[id]; err != nil || !exists {
					workspaceFailure(w, 404, "project_missing", "Hmm, a project you're trying to move isn't around anymore.", "reload_and_reapply", d.FolderRevision)
					return
				}
				ids = append(ids, id)
			}
			for _, id := range ids {
				p := d.Projects[id]
				p.Folder = target
				d.Projects[id] = p
			}
		}
		d.FolderRevision++
		st.commit(w, d, in.OperationID, digest, map[string]any{"revision": d.FolderRevision, "operationId": in.OperationID})
	})
}

func stringOr(s *string) string {
	if s == nil {
		return ""
	}
	return *s
}
