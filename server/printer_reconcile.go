package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

func livePrinterProfiles(catalog *resolve.Catalog, doc map[string]json.RawMessage) (map[string]*resolve.Printer, error) {
	lib, err := catalog.ReadLibrary(doc)
	if err != nil {
		return nil, err
	}
	live := map[string]*resolve.Printer{}
	for id, p := range lib.Printers {
		if p.Live() {
			live[id] = p
		}
	}
	return live, nil
}

func (s *Server) storedLibrary() (map[string]json.RawMessage, error) {
	l := s.library
	l.mu.Lock()
	defer l.mu.Unlock()
	if _, err := os.Stat(filepath.Join(l.dir, libraryFile)); err != nil {
		return nil, err
	}
	e, err := l.read()
	if err != nil {
		return nil, err
	}
	return e.Document, nil
}

func (s *Server) reconcileStoredConnections(reason string) {
	if s.sends.store == nil {
		return
	}
	doc, err := s.storedLibrary()
	if errors.Is(err, os.ErrNotExist) {
		return
	}
	if err != nil {
		log.Printf("printer connections: not reconciled with the library (%s): %v", reason, err)
		return
	}
	s.reconcileConnections(doc, reason)
}

func (s *Server) reconcileConnections(doc map[string]json.RawMessage, reason string) {
	m := s.sends
	if m.store == nil {
		return
	}
	live, err := livePrinterProfiles(s.catalog, doc)
	if err != nil {
		log.Printf("printer connections: not reconciled with the library (%s): %v", reason, err)
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	why := map[*PrinterConfig]string{}
	for _, p := range m.conns {
		if p.Printer != "" && live[p.Printer] == nil {
			why[p] = fmt.Sprintf("its printer profile %s is archived or gone", p.Printer)
		}
	}
	keeper := map[string]*PrinterConfig{}
	for _, p := range m.conns {
		if p.Serial == "" || why[p] != "" {
			continue
		}
		if k := keeper[p.Serial]; k == nil || k.Printer == "" && p.Printer != "" {
			keeper[p.Serial] = p
		}
	}
	for _, p := range m.conns {
		if k := keeper[p.Serial]; p.Serial != "" && why[p] == "" && k != p {
			why[p] = fmt.Sprintf("printer %s is already connected as %q (%s)", p.Serial, k.Name, k.id)
		}
	}
	if len(why) == 0 {
		return
	}
	kept := []*PrinterConfig{}
	for _, p := range m.conns {
		if why[p] != "" && m.busy[p.Printer] && p.Printer != "" {
			log.Printf("printer connections: %q (%s) kept until its upload finishes; %s", p.Name, p.id, why[p])
			delete(why, p)
		}
		if why[p] == "" {
			kept = append(kept, p)
		}
	}
	m.store.printers = kept
	if err := m.store.save(); err != nil {
		m.store.printers = m.conns
		log.Printf("printer connections: cannot save the reconciled connections (%s): %v", reason, err)
		return
	}
	for _, p := range m.conns {
		if why[p] == "" {
			continue
		}
		os.Remove(p.CredentialFile)
		m.cameras.disconnect(p.Printer)
		log.Printf("printer connections: removed %q (%s, %s %s, setup %q) on %s: %s", p.Name, p.id, p.Kind, p.Serial, p.Printer, reason, why[p])
		audit(map[string]any{"event": "connection_removed", "printer": p.Printer, "id": p.id, "serial": p.Serial, "reason": why[p], "trigger": reason})
	}
	m.conns = kept
	m.hub.changed()
}

func (s *Server) connectionConflict(key, serial string, self *PrinterConfig) string {
	doc, err := s.storedLibrary()
	if err != nil {
		return "the printer profiles could not be read; try again"
	}
	live, err := livePrinterProfiles(s.catalog, doc)
	if err != nil {
		return "the printer profiles could not be read; try again"
	}
	if live[key] == nil {
		return "this printer profile is archived or gone; connect a printer in your library"
	}
	if serial == "" {
		return ""
	}
	for _, p := range s.sends.connections() {
		if p.Serial == serial && p != self {
			return fmt.Sprintf("this printer is already connected as %s; remove that connection first", p.Name)
		}
	}
	return ""
}
