package main

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

const (
	leaseTTL       = 45 * time.Second
	leaseHeartbeat = 15 * time.Second
	sessionHeader  = "X-Orca-Session"
)

type projectLease struct {
	Session   string `json:"session"`
	Label     string `json:"label"`
	Since     int64  `json:"since"`
	LastSeen  int64  `json:"lastSeen"`
	Displaced string `json:"displaced,omitempty"`
}
type leaseHolder struct {
	Label    string `json:"label"`
	Since    int64  `json:"since"`
	LastSeen int64  `json:"lastSeen"`
}

type leaseState int

const (
	leaseFree leaseState = iota
	leaseHeld
	leaseExpired
)

func (l *projectLease) state(now time.Time) leaseState {
	switch {
	case l == nil:
		return leaseFree
	case now.UnixMilli()-l.LastSeen < leaseTTL.Milliseconds():
		return leaseHeld
	default:
		return leaseExpired
	}
}
func (l *projectLease) heldBy(session string, now time.Time) bool {
	return l.state(now) == leaseHeld && l.Session == session
}
func (l *projectLease) holder() leaseHolder { return leaseHolder{l.Label, l.Since, l.LastSeen} }

type leaseEvent int

const (
	leaseAcquire leaseEvent = iota
	leaseTakeover
	leaseRelease
)

type leaseRefusal string

func (st *workspaceStore) transition(id string, event leaseEvent, session, label string, now time.Time) (*projectLease, leaseRefusal) {
	leases := st.leaseTable()
	l := leases[id]
	state := l.state(now)
	switch {
	case event == leaseRelease:
		if l.heldBy(session, now) {
			delete(leases, id)
			st.saveLeases()
		}
		return nil, ""
	case state == leaseHeld && l.Session == session:
		l.LastSeen = now.UnixMilli()
		if label != "" {
			l.Label = label
		}
	case state == leaseHeld && event == leaseAcquire:
		if session == l.Displaced {
			return l, "lease_lost"
		}
		return l, "lease_held"
	default:
		next := &projectLease{Session: session, Label: label, Since: now.UnixMilli(), LastSeen: now.UnixMilli()}
		if state == leaseHeld {
			next.Displaced = l.Session
		}
		leases[id], l = next, next
	}
	st.saveLeases()
	return l, ""
}

func validSession(s string) bool { return len(s) >= 8 && len(s) <= 100 && workspaceID(s) }

func (st *workspaceStore) leaseTable() map[string]*projectLease {
	if st.leases != nil {
		return st.leases
	}
	st.leases = map[string]*projectLease{}
	raw, err := os.ReadFile(filepath.Join(st.root, "leases.json"))
	if err == nil && json.Unmarshal(raw, &st.leases) != nil {
		st.leases = map[string]*projectLease{}
	}
	return st.leases
}

func (st *workspaceStore) saveLeases() {
	now := time.Now()
	for id, l := range st.leases {
		if l.state(now) == leaseExpired {
			delete(st.leases, id)
		}
	}
	raw, _ := json.Marshal(st.leases)
	if err := writeFileAtomic(filepath.Join(st.root, "leases.json"), raw); err != nil {
		log.Printf("project leases: %v", err)
	}
}

func (st *workspaceStore) leaseConflict(w http.ResponseWriter, id, session string, revision uint64) bool {
	if !validSession(session) {
		workspaceFailure(w, 400, "session_required", "Oops! This write didn't say which tab it came from. Reload the page.", "reload", revision)
		return true
	}
	l := st.leaseTable()[id]
	if l.state(time.Now()) != leaseHeld || l.Session == session {
		return false
	}
	leaseFailure(w, l, session, revision)
	return true
}
func leaseFailure(w http.ResponseWriter, l *projectLease, session string, revision uint64) {
	body := workspaceError("lease_held", "Heads up — this project is open in another tab or device.", "take_control", revision)
	if session == l.Displaced {
		body = workspaceError("lease_lost", "Another tab or device took the wheel on this project!", "take_control", revision)
	}
	body["holder"] = l.holder()
	writeJSON(w, 409, body)
}

func (st *workspaceStore) claimLease(id, session string) {
	if now := time.Now(); validSession(session) && st.leaseTable()[id].state(now) != leaseHeld {
		st.transition(id, leaseAcquire, session, "", now)
	}
}

func (st *workspaceStore) leaseRoutes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/project-leases/{id}", func(w http.ResponseWriter, r *http.Request) {
		st.locked(w, func(d *workspaceData) {
			id, err := canonicalWorkspaceProject(d, r.PathValue("id"))
			if err != nil {
				workspaceFailure(w, 404, "project_missing", "Project not found.", "open_projects", 0)
				return
			}
			now, l, session := time.Now(), st.leaseTable()[id], r.Header.Get(sessionHeader)
			held := l.state(now) == leaseHeld
			out := map[string]any{"held": held, "mine": held && l.Session == session,
				"ttlMs": leaseTTL.Milliseconds(), "heartbeatMs": leaseHeartbeat.Milliseconds()}
			if held {
				out["holder"] = l.holder()
			}
			writeJSON(w, 200, out)
		})
	})
	mux.HandleFunc("POST /api/project-leases/{id}", func(w http.ResponseWriter, r *http.Request) {
		var in struct {
			Session string `json:"session"`
			Label   string `json:"label"`
			Take    bool   `json:"take"`
		}
		if !workspaceDecode(w, r, &in) {
			return
		}
		if !validSession(in.Session) || len(in.Label) > 120 {
			workspaceFailure(w, 400, "session_invalid", "A session ID of 8 to 100 letters, digits, '.', '_' or '-' is required.", "reload", 0)
			return
		}
		st.locked(w, func(d *workspaceData) {
			id, err := canonicalWorkspaceProject(d, r.PathValue("id"))
			if p, exists := d.Projects[id]; err != nil || !exists || p.Record == nil {
				workspaceFailure(w, 404, "project_missing", "Project not found.", "open_projects", 0)
				return
			}
			event := leaseAcquire
			if in.Take {
				event = leaseTakeover
			}
			l, refused := st.transition(id, event, in.Session, in.Label, time.Now())
			if refused != "" {
				leaseFailure(w, l, in.Session, 0)
				return
			}
			writeJSON(w, 200, map[string]any{"held": true, "mine": true, "holder": l.holder(),
				"ttlMs": leaseTTL.Milliseconds(), "heartbeatMs": leaseHeartbeat.Milliseconds()})
		})
	})
	mux.HandleFunc("POST /api/project-leases/{id}/release", func(w http.ResponseWriter, r *http.Request) {
		var in struct {
			Session string `json:"session"`
		}
		raw, _ := io.ReadAll(http.MaxBytesReader(w, r.Body, 4096))
		json.Unmarshal(raw, &in)
		st.locked(w, func(d *workspaceData) {
			id, err := canonicalWorkspaceProject(d, r.PathValue("id"))
			if err == nil {
				st.transition(id, leaseRelease, in.Session, "", time.Now())
			}
			w.WriteHeader(204)
		})
	})
}
