package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sync"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

var (
	streamsClosing = make(chan struct{})
	closeStreams   sync.Once
)

func closeLiveStreams() { closeStreams.Do(func() { close(streamsClosing) }) }

func (s *Server) liveEvents(w http.ResponseWriter, r *http.Request) {
	controller := http.NewResponseController(w)
	if _, ok := w.(http.Flusher); !ok {
		fail(w, 500, "live_updates_arent_available")
		return
	}
	hub, l, ws := s.sends.hub, s.library, s.workspace
	defer hub.subscribe()()
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	defer controller.SetWriteDeadline(time.Time{})
	heartbeat := time.NewTicker(time.Second)
	defer heartbeat.Stop()
	if _, err := io.WriteString(w, "retry: 600000\n\n"); err != nil {
		return
	}
	sent, sessionSent, last := uint64(0), uint64(0), []byte(nil)
	for {
		changed, sessionChanged := l.wait(), ws.sessionWait()
		hub.mu.Lock()
		wake := hub.wake
		hub.mu.Unlock()
		if controller.SetWriteDeadline(time.Now().Add(15*time.Second)) != nil {
			return
		}
		l.mu.Lock()
		e, err := l.read()
		l.mu.Unlock()
		if err == nil {
			if revision := resolve.LibraryRevision(e.Document); revision != sent {
				sent = revision
				if _, err := fmt.Fprintf(w, "event: revision\ndata: {\"revision\":%d}\n\n", revision); err != nil {
					return
				}
			}
		}
		if revision := ws.sessionRevision(); revision != sessionSent {
			sessionSent = revision
			if _, err := fmt.Fprintf(w, "event: session\ndata: {\"revision\":%d}\n\n", revision); err != nil {
				return
			}
		}
		snap := s.snapshot()
		if b, _ := json.Marshal(snap); !bytes.Equal(b, last) {
			if _, err := fmt.Fprintf(w, "event: connections\ndata: %s\n\n", b); err != nil {
				return
			}
			last = b
		}
		if controller.Flush() != nil {
			return
		}
		var tick <-chan time.Time
		for _, t := range snap.Transfers {
			if t.active() {
				tick = time.After(250 * time.Millisecond)
			}
		}
		select {
		case <-r.Context().Done():
			return
		case <-streamsClosing:
			return
		case <-changed:
		case <-sessionChanged:
		case <-wake:
		case <-tick:
		case <-heartbeat.C:
			if controller.SetWriteDeadline(time.Now().Add(15*time.Second)) != nil {
				return
			}
			if _, err := io.WriteString(w, "event: ping\ndata: {}\n\n"); err != nil || controller.Flush() != nil {
				return
			}
		}
	}
}
