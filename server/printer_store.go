package main

import (
	"cmp"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode"
)

type printerStore struct {
	dir       string
	resources string
	hosts     *hostPolicy
	printers  []*PrinterConfig
}

type storedPrinter struct {
	ID          string `json:"id"`
	Printer     string `json:"printer"`
	Name        string `json:"name"`
	Kind        string `json:"kind"`
	Host        string `json:"host,omitempty"`
	Serial      string `json:"serial,omitempty"`
	Model       string `json:"model,omitempty"`
	TLS         string `json:"tls,omitempty"`
	Fingerprint string `json:"fingerprint,omitempty"`
	URL         string `json:"url,omitempty"`
	Auth        string `json:"auth,omitempty"`
	Username    string `json:"username,omitempty"`
}

func loadPrinterStore(dir, resources string, hosts *hostPolicy) (*printerStore, error) {
	st := &printerStore{dir: dir, resources: resources, hosts: hosts}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	var file struct {
		Connections []storedPrinter `json:"connections"`
	}
	b, err := os.ReadFile(filepath.Join(dir, "connections.json"))
	switch {
	case err == nil:
		if err := json.Unmarshal(b, &file); err != nil {
			return nil, fmt.Errorf("%s: %w", filepath.Join(dir, "connections.json"), err)
		}
	case !errors.Is(err, os.ErrNotExist):
		return nil, err
	}
	legacy, err := os.ReadFile(filepath.Join(dir, "printers.json"))
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	migrated := 0
	if err == nil {
		var old struct {
			Printers []storedPrinter `json:"printers"`
		}
		if err := json.Unmarshal(legacy, &old); err != nil {
			return nil, fmt.Errorf("%s: %w", filepath.Join(dir, "printers.json"), err)
		}
		for _, sp := range old.Printers {
			sp.Printer, sp.Kind = "", cmp.Or(sp.Kind, "bambu-lan")
			file.Connections = append(file.Connections, sp)
		}
		migrated = len(old.Printers)
	}
	for _, sp := range file.Connections {
		p, err := st.config(sp)
		if err != nil {
			return nil, fmt.Errorf("printer connection %q (%s): %w", sp.Name, sp.ID, err)
		}
		st.printers = append(st.printers, p)
	}
	if legacy != nil {
		if err := st.save(); err != nil {
			return nil, err
		}
		if err := os.Rename(filepath.Join(dir, "printers.json"), filepath.Join(dir, "printers.json.migrated")); err != nil {
			return nil, err
		}
		log.Printf("printer connections: migrated %d named printer(s) from printers.json as unassigned connections", migrated)
	}
	return st, nil
}

func (st *printerStore) config(sp storedPrinter) (*PrinterConfig, error) {
	if len(sp.ID) != 16 || strings.Trim(sp.ID, "0123456789abcdef") != "" {
		return nil, errors.New("bad id")
	}
	p := &PrinterConfig{Printer: sp.Printer, Name: sp.Name, Kind: sp.Kind, Host: sp.Host, Serial: sp.Serial, TLS: sp.TLS, Fingerprint: sp.Fingerprint,
		URL: sp.URL, Auth: sp.Auth, Username: sp.Username, CredentialFile: filepath.Join(st.dir, sp.ID+".code"), id: sp.ID, hosts: st.hosts, model: sp.Model}
	if p.Kind != "bambu-lan" && p.Kind != "bambu-connect" {
		u, err := hostURL(sp.URL)
		if err != nil {
			return nil, err
		}
		p.URL = u.String()
	}
	return p, p.validate(st.resources)
}

func (st *printerStore) stored(p *PrinterConfig) storedPrinter {
	return storedPrinter{ID: p.id, Printer: p.Printer, Name: p.Name, Kind: p.Kind, Host: p.Host, Serial: p.Serial, Model: p.model, TLS: p.TLS,
		Fingerprint: p.Fingerprint, URL: p.URL, Auth: p.Auth, Username: p.Username}
}

func (st *printerStore) save() error {
	var file struct {
		Connections []storedPrinter `json:"connections"`
	}
	file.Connections = []storedPrinter{}
	for _, p := range st.printers {
		file.Connections = append(file.Connections, st.stored(p))
	}
	b, _ := json.MarshalIndent(file, "", "  ")
	return writeFileAtomic(filepath.Join(st.dir, "connections.json"), b)
}

func writeFileAtomic(path string, b []byte) error { return durableWrite(path, b) }

func newSendID() string {
	id := make([]byte, 8)
	rand.Read(id)
	return hex.EncodeToString(id)
}

func (m *SendManager) replace(old, next *PrinterConfig) error {
	conns := []*PrinterConfig{}
	for _, p := range m.conns {
		if p != old {
			conns = append(conns, p)
		}
	}
	if next != nil {
		conns = append(conns, next)
	}
	m.store.printers = conns
	if err := m.store.save(); err != nil {
		m.store.printers = m.conns
		return err
	}
	m.conns = conns
	if next != nil {
		m.watchTrust(next)
	}
	return nil
}

func (m *SendManager) watchTrust(p *PrinterConfig) {
	if p.TLS != "tofu" {
		return
	}
	p.trusted = func(mode string, fp []byte) {
		m.mu.Lock()
		defer m.mu.Unlock()
		for _, q := range m.conns {
			if q != p {
				continue
			}
			next := *p
			next.TLS, next.trusted, next.fingerprint, next.Fingerprint = mode, nil, fp, ""
			if fp != nil {
				next.Fingerprint = "sha256:" + hex.EncodeToString(fp)
			}
			if err := m.replace(p, &next); err != nil {
				log.Printf("warning: cannot record printer %q certificate: %v", p.Name, err)
				return
			}
			m.hub.changed()
			log.Printf("printer %q: trusted its %s certificate on first use", p.Name, map[string]string{"pin": "self-signed", "bambu-ca": "Bambu-signed"}[mode])
		}
	}
}

type connectionEdit struct {
	Printer    string `json:"printer"`
	Kind       string `json:"kind"`
	Name       string `json:"name"`
	Host       string `json:"host"`
	Serial     string `json:"serial"`
	AccessCode string `json:"access_code"`
	Model      string `json:"model"`
	URL        string `json:"url"`
	Auth       string `json:"auth"`
	Username   string `json:"username"`
	Key        string `json:"key"`
	ClearKey   bool   `json:"clear_key"`
}

func (b *connectionEdit) secret() string {
	if b.Kind == "bambu-lan" {
		return b.AccessCode
	}
	return b.Key
}

func (s *Server) connectionBody(w http.ResponseWriter, r *http.Request) (*connectionEdit, bool) {
	if s.sends.store == nil {
		fail(w, 403, "Printer network connections are disabled on this server.")
		return nil, false
	}
	var body connectionEdit
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 4<<10))
	if err != nil || json.Unmarshal(raw, &body) != nil {
		fail(w, 400, `expected {"kind", "host", "serial", "access_code"} or {"kind", "url", "key"}`)
		return nil, false
	}
	if connectionKinds[body.Kind] == "" {
		fail(w, 400, "kind must be bambu-lan, bambu-connect, moonraker, octoprint, prusalink, duet or elegoolink")
		return nil, false
	}
	body.Name, body.Host, body.Serial, body.AccessCode = strings.TrimSpace(body.Name), strings.TrimSpace(body.Host), strings.ToUpper(strings.TrimSpace(body.Serial)), strings.TrimSpace(body.AccessCode)
	body.URL, body.Username = strings.TrimSpace(body.URL), strings.TrimSpace(body.Username)
	if body.AccessCode != "" && (len(body.AccessCode) > 64 || strings.IndexFunc(body.AccessCode, func(r rune) bool { return unicode.IsSpace(r) || unicode.IsControl(r) }) >= 0) {
		fail(w, 400, "Enter the 8-character access code shown under Settings › Network on your printer.")
		return nil, false
	}
	if len(body.Key) > 256 || strings.IndexFunc(body.Key, unicode.IsControl) >= 0 || len(body.Username) > 64 || strings.IndexFunc(body.Username, unicode.IsControl) >= 0 {
		fail(w, 400, "The API key, username, or password is too long or contains invalid characters.")
		return nil, false
	}
	if len(body.Model) > 60 || strings.IndexFunc(body.Model, unicode.IsControl) >= 0 {
		body.Model = ""
	}
	if body.Kind != "prusalink" || body.Auth == "apikey" {
		body.Auth, body.Username = "", ""
	}
	if body.Auth != "" && body.Auth != "digest" {
		fail(w, 400, "auth must be apikey or digest")
		return nil, false
	}
	return &body, true
}

func (s *Server) editConfig(ctx context.Context, body *connectionEdit, sp storedPrinter) (*PrinterConfig, error) {
	st := s.sends.store
	sp.Kind, sp.Name = body.Kind, body.Name
	var u *url.URL
	switch body.Kind {
	case "bambu-lan":
		sp.Host, sp.Serial = body.Host, body.Serial
		u = &url.URL{Scheme: "tcp", Host: body.Host + ":8883"}
	case "bambu-connect":
	default:
		var err error
		if u, err = hostURL(body.URL); err != nil {
			return nil, err
		}
		sp.URL, sp.Auth, sp.Username = u.String(), body.Auth, body.Username
	}
	p, err := st.config(sp)
	if err != nil {
		return nil, err
	}
	if u != nil {
		if _, err := st.hosts.resolve(ctx, u); err != nil {
			var refused *hostRefusal
			if errors.As(err, &refused) {
				audit(map[string]any{"event": "printer_host_refused", "printer": body.Name, "reason": refused.reason})
			}
			return nil, err
		}
	}
	return p, nil
}

func (s *Server) listConnections(w http.ResponseWriter, r *http.Request) {
	m := s.sends
	if m.store == nil {
		writeJSON(w, 200, map[string]any{"enabled": false, "kinds": connectionKinds, "connections": []ConnectionView{}})
		return
	}
	list := []ConnectionView{}
	for _, p := range m.connections() {
		list = append(list, p.view())
	}
	writeJSON(w, 200, map[string]any{"enabled": true, "kinds": connectionKinds, "network": m.store.hosts.String(), "connections": list})
}

func (m *SendManager) connectionFor(key string) *PrinterConfig {
	if id, ok := strings.CutPrefix(key, "unassigned:"); ok {
		m.mu.Lock()
		defer m.mu.Unlock()
		for _, p := range m.conns {
			if p.Printer == "" && p.id == id {
				return p
			}
		}
		return nil
	}
	return m.printer(key)
}

func (s *Server) putConnection(w http.ResponseWriter, r *http.Request) {
	key := r.PathValue("printer")
	if key == "" || len(key) > 300 || strings.HasPrefix(key, "unassigned:") || strings.IndexFunc(key, unicode.IsControl) >= 0 {
		fail(w, 400, "The request URL needs to include the printer name.")
		return
	}
	body, ok := s.connectionBody(w, r)
	if !ok {
		return
	}
	m := s.sends
	prev := m.printer(key)
	sp := storedPrinter{Printer: key, TLS: map[bool]string{true: "tofu"}[body.Kind == "bambu-lan"], Model: body.Model}
	if prev != nil {
		sp.ID = prev.id
		if prev.Kind == body.Kind {
			sp.Model = cmp.Or(body.Model, prev.model)
		}
		if prev.Kind == "bambu-lan" && body.Kind == "bambu-lan" && prev.Host == body.Host && prev.Serial == body.Serial {
			sp.TLS, sp.Fingerprint = prev.TLS, prev.Fingerprint
		}
	} else {
		sp.ID = newSendID()
	}
	_, statErr := os.Stat(filepath.Join(m.store.dir, sp.ID+".code"))
	hasSecret := statErr == nil && (prev == nil || prev.Kind == body.Kind)
	if secretRequired(body.Kind) && body.secret() == "" && !hasSecret {
		fail(w, 400, map[bool]string{true: "enter the printer's access code (Settings > Network on the printer)", false: "enter the printer's API key or password"}[body.Kind == "bambu-lan"])
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	p, err := s.editConfig(ctx, body, sp)
	if err != nil {
		fail(w, 400, err.Error())
		return
	}
	if why := s.connectionConflict(key, p.Serial, prev); why != "" {
		fail(w, 409, why)
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.printerLocked(key) != prev {
		fail(w, 409, "This printer's connection settings were just updated elsewhere—please try again.")
		return
	}
	if prev != nil && m.busy[key] {
		fail(w, 409, "This printer is currently receiving a file upload—wait for it to finish before changing connection settings.")
		return
	}
	switch secret := body.secret(); {
	case secret != "":
		if err := writeFileAtomic(p.CredentialFile, []byte(secret+"\n")); err != nil {
			fail(w, 500, "Oops! Couldn't save the printer's access code or password on the server.")
			return
		}
	case body.ClearKey && !secretRequired(body.Kind) || prev != nil && prev.Kind != body.Kind || body.Kind == "bambu-connect":
		os.Remove(p.CredentialFile)
	}
	if err := m.replace(prev, p); err != nil {
		fail(w, 500, "Oops! Couldn't save the printer connection.")
		return
	}
	audit(map[string]any{"event": map[bool]string{true: "connection_edited", false: "connection_added"}[prev != nil], "printer": key, "kind": p.Kind, "client": s.clientIP(r)})
	m.cameras.disconnect(key)
	m.hub.changed()
	writeJSON(w, map[bool]int{true: 200, false: 201}[prev == nil], p.view())
}

func (s *Server) deleteConnection(w http.ResponseWriter, r *http.Request) {
	m := s.sends
	if m.store == nil {
		fail(w, 403, "Printer network connections are disabled on this server.")
		return
	}
	key := r.PathValue("printer")
	p := m.connectionFor(key)
	if p == nil {
		fail(w, 404, "This printer doesn't have a network connection set up yet.")
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.holds(p) {
		fail(w, 404, "This printer doesn't have a network connection set up yet.")
		return
	}
	if m.busy[p.Printer] && p.Printer != "" {
		fail(w, 409, "This printer is currently receiving a file upload—wait for it to finish before removing the connection.")
		return
	}
	if err := m.replace(p, nil); err != nil {
		fail(w, 500, "Oops! Couldn't save your printer connections.")
		return
	}
	os.Remove(p.CredentialFile)
	m.cameras.disconnect(key)
	audit(map[string]any{"event": "connection_removed", "printer": key, "client": s.clientIP(r)})
	m.hub.changed()
	w.WriteHeader(204)
}

func (s *Server) adoptConnection(w http.ResponseWriter, r *http.Request) {
	m := s.sends
	if m.store == nil {
		fail(w, 403, "Printer network connections are disabled on this server.")
		return
	}
	key := r.PathValue("printer")
	var body struct {
		ID   string `json:"id"`
		Name string `json:"name"`
	}
	if raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<10)); err != nil || json.Unmarshal(raw, &body) != nil || body.ID == "" || strings.HasPrefix(key, "unassigned:") {
		fail(w, 400, `expected {"id": unassigned connection id, "name": printer setup name}`)
		return
	}
	src := m.connectionFor("unassigned:" + body.ID)
	if src == nil {
		fail(w, 404, "Couldn't find a saved connection with that ID.")
		return
	}
	if why := s.connectionConflict(key, src.Serial, src); why != "" {
		fail(w, 409, why)
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if !m.holds(src) {
		fail(w, 404, "Couldn't find a saved connection with that ID.")
		return
	}
	if m.printerLocked(key) != nil {
		fail(w, 409, "This printer already has a connection set up—remove the existing one first.")
		return
	}
	next := *src
	next.Printer, next.Name = key, cmp.Or(strings.TrimSpace(body.Name), src.Name)
	if err := m.replace(src, &next); err != nil {
		fail(w, 500, "Oops! Couldn't save your printer connections.")
		return
	}
	m.hub.changed()
	writeJSON(w, 200, next.view())
}

var legacyHostTypes = map[string]string{"prusalink": "prusalink", "octoprint": "octoprint", "moonraker": "moonraker", "duet": "duet", "elegoolink": "elegoolink"}

func (s *Server) importLegacyConnection(w http.ResponseWriter, r *http.Request) {
	m := s.sends
	if m.store == nil {
		fail(w, 403, "Printer network connections are disabled on this server.")
		return
	}
	key := r.PathValue("printer")
	var body struct {
		Name   string            `json:"name"`
		Config map[string]string `json:"config"`
	}
	if raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 8<<10)); err != nil || json.Unmarshal(raw, &body) != nil || body.Config == nil || strings.HasPrefix(key, "unassigned:") {
		fail(w, 400, `expected {"name", "config": {"host_type", "print_host", ...}}`)
		return
	}
	c := body.Config
	hostType := cmp.Or(c["host_type"], "octoprint")
	kind := legacyHostTypes[hostType]
	if kind == "" {
		writeJSON(w, 422, map[string]any{"error": "The server doesn't support " + hostType + " directly; those settings stay saved inside the printer profile.", "code": "unsupported_host_type", "host_type": hostType})
		return
	}
	if strings.TrimSpace(c["print_host"]) == "" {
		writeJSON(w, 422, map[string]any{"error": "This printer profile doesn't have a host address configured.", "code": "no_print_host"})
		return
	}
	edit := &connectionEdit{Kind: kind, Name: strings.TrimSpace(body.Name), URL: strings.TrimSpace(c["print_host"]), Key: c["printhost_apikey"]}
	if port := strings.TrimSpace(c["printhost_port"]); port != "" && !strings.Contains(strings.TrimPrefix(strings.TrimPrefix(edit.URL, "http://"), "https://"), ":") {
		edit.URL += ":" + port
	}
	if c["printhost_authorization_type"] == "user" {
		if kind != "prusalink" {
			writeJSON(w, 422, map[string]any{"error": hostType + " login with a username and password isn't supported—please use an API key instead.", "code": "unsupported_login"})
			return
		}
		edit.Auth, edit.Username, edit.Key = "digest", c["printhost_user"], c["printhost_password"]
	}
	if p := m.printer(key); p != nil {
		writeJSON(w, 409, map[string]any{"error": "This printer already has a network connection configured.", "code": "connection_exists", "connection": p.view()})
		return
	}
	sp := storedPrinter{ID: newSendID(), Printer: key}
	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()
	p, err := s.editConfig(ctx, edit, sp)
	if err != nil {
		writeJSON(w, 422, map[string]any{"error": err.Error(), "code": "invalid_print_host"})
		return
	}
	if secretRequired(kind) && edit.Key == "" {
		writeJSON(w, 422, map[string]any{"error": "This profile doesn't have a saved API key or password for " + hostType + ".", "code": "no_credential"})
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if q := m.printerLocked(key); q != nil {
		writeJSON(w, 409, map[string]any{"error": "This printer already has a network connection configured.", "code": "connection_exists", "connection": q.view()})
		return
	}
	if edit.Key != "" {
		if err := writeFileAtomic(p.CredentialFile, []byte(edit.Key+"\n")); err != nil {
			fail(w, 500, "Oops! Couldn't save the printer's access code or password on the server.")
			return
		}
	}
	if err := m.replace(nil, p); err != nil {
		fail(w, 500, "Oops! Couldn't save the printer connection.")
		return
	}
	audit(map[string]any{"event": "connection_migrated", "printer": key, "kind": kind, "client": s.clientIP(r)})
	m.hub.changed()
	writeJSON(w, 201, p.view())
}

func (s *Server) testConnection(w http.ResponseWriter, r *http.Request) {
	body, ok := s.connectionBody(w, r)
	if !ok {
		return
	}
	client := s.clientIP(r)
	if ok, retry := s.sends.allowBurst("test "+client, 10); !ok {
		w.Header().Set("Retry-After", fmt.Sprint(min(retry, 86400)))
		fail(w, 429, "Too many connection tests in a row—wait a few seconds and try again.")
		return
	}
	if body.Kind == "bambu-connect" {
		writeJSON(w, 200, PrinterTest{OK: true, Stage: "ok", Message: "Bambu Connect opens on this computer when you send"})
		return
	}
	saved := s.sends.printer(body.Printer)
	secret := body.secret()
	if secret == "" && saved != nil && saved.Kind == body.Kind && !body.ClearKey {
		secret, _ = saved.secret()
	}
	if secret == "" && secretRequired(body.Kind) {
		fail(w, 400, map[bool]string{true: "enter the printer's access code (Settings > Network on the printer)", false: "enter the printer's API key or password"}[body.Kind == "bambu-lan"])
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 45*time.Second)
	defer cancel()
	savedModel := ""
	if saved != nil && saved.Kind == body.Kind {
		savedModel = saved.model
	}
	sp := storedPrinter{ID: "0000000000000000", Printer: body.Printer, Name: cmp.Or(body.Name, "the printer"), Model: cmp.Or(body.Model, savedModel), TLS: map[bool]string{true: "tofu"}[body.Kind == "bambu-lan"]}
	p, err := s.editConfig(ctx, body, sp)
	if err != nil {
		stage := map[bool]string{true: "address", false: "invalid"}[errors.Is(err, errHostRefused)]
		writeJSON(w, 200, PrinterTest{Stage: stage, Message: err.Error()})
		return
	}
	if saved != nil && saved.Kind == "bambu-lan" && saved.Host == p.Host && saved.Serial == p.Serial {
		p = saved
	}
	var result PrinterTest
	if p.Kind == "bambu-lan" {
		result = testBambu(ctx, p, secret)
		if result.OK && p == saved && result.Model != "" {
			s.sends.setModel(p, result.Model)
		}
	} else {
		result = testHost(ctx, p, secret)
	}
	result.Message = redact(result.Message, secret)
	audit(map[string]any{"event": "printer_test", "printer": body.Printer, "kind": body.Kind, "client": client, "stage": result.Stage})
	writeJSON(w, 200, result)
}

func (m *SendManager) setModel(p *PrinterConfig, model string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, q := range m.conns {
		if q == p && q.model != model {
			next := *q
			next.model = model
			m.replace(q, &next)
		}
	}
}

func (s *Server) amsConnection(w http.ResponseWriter, r *http.Request) {
	p := s.sends.printer(r.PathValue("printer"))
	if p == nil || p.Kind != "bambu-lan" {
		fail(w, 404, "This printer doesn't have a Bambu LAN connection set up.")
		return
	}
	secret, err := p.secret()
	if err != nil {
		fail(w, 409, err.Error())
		return
	}
	client := s.clientIP(r)
	if ok, retry := s.sends.allowBurst("ams "+client, 10); !ok {
		w.Header().Set("Retry-After", fmt.Sprint(min(retry, 86400)))
		fail(w, 429, "Checking AMS too frequently—wait a few seconds and try again.")
		return
	}
	if st, ok := s.sends.hub.current(p.Printer); ok && st.Online && st.Trays != nil {
		writeJSON(w, 200, map[string]any{"trays": st.Trays})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	trays, err := amsBambu(ctx, p, secret)
	if err != nil {
		fail(w, 502, redact(err.Error(), secret))
		return
	}
	writeJSON(w, 200, map[string]any{"trays": trays})
}

func (s *Server) controlConnection(w http.ResponseWriter, r *http.Request) {
	p := s.sends.printer(r.PathValue("printer"))
	if p == nil || p.Kind == "bambu-connect" {
		fail(w, 404, "This printer doesn't have a network connection that supports remote commands.")
		return
	}
	var body struct {
		Action string `json:"action"`
	}
	if raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<10)); err != nil || json.Unmarshal(raw, &body) != nil || body.Action != "stop" && body.Action != "pause" && body.Action != "resume" {
		fail(w, 400, `expected {"action": "stop" | "pause" | "resume"}`)
		return
	}
	secret, err := p.secret()
	if err != nil {
		fail(w, 409, err.Error())
		return
	}
	if ok, retry := s.sends.allowBurst("control "+s.clientIP(r), 10); !ok {
		w.Header().Set("Retry-After", fmt.Sprint(min(retry, 86400)))
		fail(w, 429, "Whoa, slow down! Too many printer commands sent at once—wait a moment and try again.")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()
	switch p.Kind {
	case "bambu-lan":
		err = controlBambu(ctx, p, secret, body.Action)
	case "duet":
		err = controlDuet(ctx, p, secret, body.Action)
	case "elegoolink":
		err = controlElegoo(ctx, p, secret, body.Action)
	default:
		err = controlHTTP(ctx, p, secret, body.Action)
	}
	audit(map[string]any{"event": "printer_" + body.Action, "printer": p.Printer, "kind": p.Kind, "client": s.clientIP(r), "error": fmt.Sprint(err)})
	if err != nil {
		fail(w, 502, redact(err.Error(), secret))
		return
	}
	s.sends.hub.poke(p.Printer)
	w.WriteHeader(204)
}
