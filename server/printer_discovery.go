package main

import (
	"bufio"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type DiscoveredPrinter struct {
	Kind         string `json:"kind"`
	Host         string `json:"host,omitempty"`
	URL          string `json:"url,omitempty"`
	Serial       string `json:"serial,omitempty"`
	Name         string `json:"name,omitempty"`
	Model        string `json:"model,omitempty"`
	Preset       string `json:"preset,omitempty"`
	CatalogModel string `json:"catalogModel,omitempty"`
	Connection   string `json:"connection,omitempty"`
	Source       string `json:"source"`
	Assigned     string `json:"assigned,omitempty"`
}

const discoveryWindow = 4 * time.Second

func (s *Server) discoverPrinters(w http.ResponseWriter, r *http.Request) {
	m := s.sends
	if m.store == nil {
		fail(w, 403, "printer_network_connections_disabled")
		return
	}
	if ok, retry := m.allowBurst("discover "+s.clientIP(r), 3, sendRate); !ok {
		w.Header().Set("Retry-After", fmt.Sprint(min(retry, 86400)))
		fail(w, 429, "network_scan_run_wait")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), discoveryWindow+2*time.Second)
	defer cancel()
	found, notes := discover(ctx, m.store.hosts, discoveryEnv{s.cfg.Resources, s.catalog})
	for i := range found {
		d := &found[i]
		for _, p := range m.connections() {
			if p.Printer != "" && (d.Serial != "" && p.Serial == d.Serial || d.URL != "" && p.URL == d.URL) {
				d.Assigned = p.Printer
			}
		}
	}
	audit(map[string]any{"event": "printer_discovery", "client": s.clientIP(r), "found": len(found)})
	writeJSON(w, 200, map[string]any{"printers": found, "notes": notes})
}

type discoveryEnv struct {
	resources string
	catalog   *resolve.Catalog
}

func discover(ctx context.Context, policy *hostPolicy, env discoveryEnv) ([]DiscoveredPrinter, []string) {
	var mu sync.Mutex
	found := map[string]DiscoveredPrinter{}
	notes := []string{}
	add := func(d DiscoveredPrinter, ip netip.Addr) {
		if policy.check(ip.Unmap(), false) != "" {
			return
		}
		key := d.Kind + "|" + d.Serial + "|" + d.Host + "|" + d.URL
		mu.Lock()
		defer mu.Unlock()
		if prev, ok := found[key]; ok && prev.Name != "" {
			return
		}
		found[key] = d
	}
	note := func(text string) { mu.Lock(); notes = append(notes, text); mu.Unlock() }
	var wg sync.WaitGroup
	for _, run := range []func(context.Context, func(DiscoveredPrinter, netip.Addr), func(string), discoveryEnv){discoverBambu, discoverMDNS, probeLocal} {
		wg.Add(1)
		go func() { defer wg.Done(); run(ctx, add, note, env) }()
	}
	wg.Wait()
	list := []DiscoveredPrinter{}
	for _, d := range found {
		list = append(list, d)
	}
	sort.Slice(list, func(i, j int) bool {
		return list[i].Kind+list[i].Host+list[i].URL < list[j].Kind+list[j].Host+list[j].URL
	})
	return list, notes
}

func bambuModels(c *resolve.Catalog) map[string][2]string {
	out := map[string][2]string{}
	for _, p := range c.Printers {
		if id := stringValue(p.Model["model_id"]); id != "" && p.Vendor == "BBL" {
			out[id] = [2]string{p.ID, c.Variant(p.ID, 0.4)}
		}
	}
	return out
}

const bambuSearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1990\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\nST: urn:bambulab-com:device:3dprinter:1\r\n\r\n"

func discoverBambu(ctx context.Context, add func(DiscoveredPrinter, netip.Addr), note func(string), env discoveryEnv) {
	models := bambuModels(env.catalog)
	parse := func(packet []byte, from netip.Addr) {
		if !strings.Contains(strings.ToLower(string(packet)), "bambulab") {
			return
		}
		headers := map[string]string{}
		sc := bufio.NewScanner(strings.NewReader(string(packet)))
		for sc.Scan() {
			if k, v, ok := strings.Cut(sc.Text(), ":"); ok {
				headers[strings.ToLower(strings.TrimSpace(k))] = strings.TrimSpace(v)
			}
		}
		serial := headers["usn"]
		if !bambuSerial.MatchString(serial) {
			return
		}
		ip := from
		if loc, err := netip.ParseAddr(headers["location"]); err == nil {
			ip = loc
		}
		code := headers["devmodel.bambu.com"]
		model := models[code]
		d := DiscoveredPrinter{Kind: "bambu-lan", Host: ip.String(), Serial: strings.ToUpper(serial), Name: headers["devname.bambu.com"],
			Model: strings.TrimPrefix(cmpOr(model[0], bambuModel("", code)), "Bambu Lab "), Preset: model[1], CatalogModel: model[0], Connection: headers["devconnect.bambu.com"], Source: "ssdp"}
		add(d, ip)
	}
	listen := func(address string) net.PacketConn {
		c, err := (&net.ListenConfig{Control: reuseAddr}).ListenPacket(ctx, "udp4", address)
		if err != nil {
			note("cannot listen on " + address + " for Bambu printers: " + lastClause(err))
			return nil
		}
		return c
	}
	var conns []net.PacketConn
	if c := listen(":2021"); c != nil {
		conns = append(conns, c)
	}
	if c := listen(":0"); c != nil {
		for _, target := range []string{"239.255.255.250:1990", "255.255.255.255:2021"} {
			if addr, err := net.ResolveUDPAddr("udp4", target); err == nil {
				c.WriteTo([]byte(bambuSearch), addr)
			}
		}
		conns = append(conns, c)
	}
	readAll(ctx, conns, parse)
}

func readAll(ctx context.Context, conns []net.PacketConn, handle func([]byte, netip.Addr)) {
	deadline := time.Now().Add(discoveryWindow)
	var wg sync.WaitGroup
	for _, c := range conns {
		wg.Add(1)
		go func() {
			defer wg.Done()
			defer c.Close()
			c.SetReadDeadline(deadline)
			stop := context.AfterFunc(ctx, func() { c.SetReadDeadline(time.Unix(1, 0)) })
			defer stop()
			buf := make([]byte, 9000)
			for {
				n, from, err := c.ReadFrom(buf)
				if err != nil {
					return
				}
				if udp, ok := from.(*net.UDPAddr); ok {
					if ip, ok := netip.AddrFromSlice(udp.IP); ok {
						handle(append([]byte(nil), buf[:n]...), ip.Unmap())
					}
				}
			}
		}()
	}
	wg.Wait()
}

var mdnsServices = map[string]string{"_octoprint._tcp.local": "octoprint", "_moonraker._tcp.local": "moonraker"}

func discoverMDNS(ctx context.Context, add func(DiscoveredPrinter, netip.Addr), note func(string), _ discoveryEnv) {
	query := []byte{0, 0, 0, 0, 0, byte(len(mdnsServices)), 0, 0, 0, 0, 0, 0}
	for name := range mdnsServices {
		query = append(append(query, dnsName(name)...), 0, 12, 0, 1)
	}
	c, err := net.ListenPacket("udp4", ":0")
	if err != nil {
		note("cannot query mDNS: " + lastClause(err))
		return
	}
	c.WriteTo(query, &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353})
	type service struct {
		kind, target string
		port         uint16
	}
	var mu sync.Mutex
	instances := map[string]string{}
	services := map[string]service{}
	addrs := map[string]netip.Addr{}
	readAll(ctx, []net.PacketConn{c}, func(packet []byte, _ netip.Addr) {
		records, err := parseDNS(packet)
		if err != nil {
			return
		}
		mu.Lock()
		defer mu.Unlock()
		for _, rr := range records {
			switch rr.kind {
			case 12:
				if kind := mdnsServices[strings.ToLower(rr.name)]; kind != "" {
					instances[rr.text] = kind
				}
			case 33:
				services[rr.name] = service{target: rr.text, port: rr.port}
			case 1:
				addrs[strings.ToLower(rr.name)] = rr.addr
			}
		}
	})
	for instance, kind := range instances {
		srv, ok := services[instance]
		if !ok {
			continue
		}
		ip, ok := addrs[strings.ToLower(srv.target)]
		if !ok {
			continue
		}
		name, _, _ := strings.Cut(instance, "._")
		add(DiscoveredPrinter{Kind: kind, URL: "http://" + netip.AddrPortFrom(ip, srv.port).String(), Name: name, Source: "mdns"}, ip)
	}
}

func dnsName(name string) []byte {
	var out []byte
	for _, label := range strings.Split(strings.TrimSuffix(name, "."), ".") {
		out = append(append(out, byte(len(label))), label...)
	}
	return append(out, 0)
}

type dnsRecord struct {
	name, text string
	kind       uint16
	port       uint16
	addr       netip.Addr
}

func parseDNS(msg []byte) ([]dnsRecord, error) {
	if len(msg) < 12 {
		return nil, errors.New("short")
	}
	readName := func(off int) (string, int, error) {
		var labels []string
		end, jumps := -1, 0
		for {
			if off >= len(msg) {
				return "", 0, errors.New("bad name")
			}
			l := int(msg[off])
			switch {
			case l == 0:
				if end < 0 {
					end = off + 1
				}
				return strings.Join(labels, "."), end, nil
			case l&0xC0 == 0xC0:
				if off+1 >= len(msg) || jumps > 16 {
					return "", 0, errors.New("bad pointer")
				}
				if end < 0 {
					end = off + 2
				}
				off, jumps = int(binary.BigEndian.Uint16(msg[off:])&0x3FFF), jumps+1
			default:
				if off+1+l > len(msg) {
					return "", 0, errors.New("bad label")
				}
				labels = append(labels, string(msg[off+1:off+1+l]))
				off += 1 + l
			}
		}
	}
	qd := int(binary.BigEndian.Uint16(msg[4:]))
	rrs := int(binary.BigEndian.Uint16(msg[6:])) + int(binary.BigEndian.Uint16(msg[8:])) + int(binary.BigEndian.Uint16(msg[10:]))
	off := 12
	for i := 0; i < qd; i++ {
		_, next, err := readName(off)
		if err != nil {
			return nil, err
		}
		off = next + 4
	}
	var out []dnsRecord
	for i := 0; i < rrs; i++ {
		name, next, err := readName(off)
		if err != nil || next+10 > len(msg) {
			return out, err
		}
		kind := binary.BigEndian.Uint16(msg[next:])
		length := int(binary.BigEndian.Uint16(msg[next+8:]))
		data := next + 10
		if data+length > len(msg) {
			return out, errors.New("bad record")
		}
		rr := dnsRecord{name: name, kind: kind}
		switch kind {
		case 12:
			rr.text, _, err = readName(data)
		case 33:
			if length >= 7 {
				rr.port = binary.BigEndian.Uint16(msg[data+4:])
				rr.text, _, err = readName(data + 6)
			}
		case 1:
			if length == 4 {
				rr.addr = netip.AddrFrom4([4]byte(msg[data : data+4]))
			}
		}
		if err == nil {
			out = append(out, rr)
		}
		off = data + length
	}
	return out, nil
}

func probeLocal(ctx context.Context, add func(DiscoveredPrinter, netip.Addr), note func(string), env discoveryEnv) {
	var targets []netip.Addr
	ifaces, _ := net.Interfaces()
	for _, iface := range ifaces {
		name := iface.Name
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 || strings.HasPrefix(name, "docker") || strings.HasPrefix(name, "br-") ||
			strings.HasPrefix(name, "veth") || strings.HasPrefix(name, "virbr") || strings.HasPrefix(name, "cni") {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, a := range addrs {
			ipnet, ok := a.(*net.IPNet)
			if !ok || ipnet.IP.To4() == nil {
				continue
			}
			ones, _ := ipnet.Mask.Size()
			own, _ := netip.AddrFromSlice(ipnet.IP.To4())
			prefix := netip.PrefixFrom(own, max(ones, 24)).Masked()
			for ip := prefix.Addr().Next(); prefix.Contains(ip) && ip != lastAddr(prefix); ip = ip.Next() {
				targets = append(targets, ip)
			}
		}
	}
	roots := x509.NewCertPool()
	if pem, err := os.ReadFile(filepath.Join(env.resources, "cert", "printer.cer")); err == nil {
		roots.AppendCertsFromPEM(pem)
	}
	models := bambuModels(env.catalog)
	work := make(chan netip.Addr)
	var wg sync.WaitGroup
	for i := 0; i < 64; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for ip := range work {
				probeOne(ctx, ip, add)
				probeBambu(ctx, ip, roots, models, add)
			}
		}()
	}
	for _, ip := range targets {
		select {
		case work <- ip:
		case <-ctx.Done():
		}
	}
	close(work)
	wg.Wait()
}

func probeBambu(ctx context.Context, ip netip.Addr, roots *x509.CertPool, models map[string][2]string, add func(DiscoveredPrinter, netip.Addr)) {
	dctx, cancel := context.WithTimeout(ctx, 1500*time.Millisecond)
	defer cancel()
	raw, err := (&net.Dialer{Timeout: 400 * time.Millisecond}).DialContext(dctx, "tcp", netip.AddrPortFrom(ip, 8883).String())
	if err != nil {
		return
	}
	defer raw.Close()
	var leaf *x509.Certificate
	conn := tls.Client(raw, &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS12, VerifyConnection: func(cs tls.ConnectionState) error {
		if len(cs.PeerCertificates) == 0 {
			return errors.New("no certificate")
		}
		intermediates := x509.NewCertPool()
		for _, c := range cs.PeerCertificates[1:] {
			intermediates.AddCert(c)
		}
		if _, err := cs.PeerCertificates[0].Verify(x509.VerifyOptions{Roots: roots, Intermediates: intermediates, KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageAny}}); err != nil {
			return err
		}
		leaf = cs.PeerCertificates[0]
		return nil
	}})
	if conn.HandshakeContext(dctx) != nil || leaf == nil || !bambuSerial.MatchString(leaf.Subject.CommonName) {
		return
	}
	conn.Close()
	serial := strings.ToUpper(leaf.Subject.CommonName)
	d := DiscoveredPrinter{Kind: "bambu-lan", Host: ip.String(), Serial: serial, Model: bambuModel("", serial[:3]), Source: "probe"}
	for _, m := range models {
		if m[0] == "Bambu Lab "+d.Model {
			d.Preset, d.CatalogModel = m[1], m[0]
		}
	}
	add(d, ip)
}

func probeOne(ctx context.Context, ip netip.Addr, add func(DiscoveredPrinter, netip.Addr)) {
	address := netip.AddrPortFrom(ip, 7125).String()
	conn, err := (&net.Dialer{Timeout: 400 * time.Millisecond}).DialContext(ctx, "tcp", address)
	if err != nil {
		return
	}
	conn.Close()
	client := &http.Client{Timeout: 1500 * time.Millisecond, Transport: &http.Transport{Proxy: nil}, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	req, _ := http.NewRequestWithContext(ctx, "GET", "http://"+address+"/server/info", nil)
	resp, err := client.Do(req)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	var info struct {
		Result *struct {
			Klippy string `json:"klippy_state"`
		} `json:"result"`
		Error *struct {
			Code int `json:"code"`
		} `json:"error"`
	}
	if json.Unmarshal(raw, &info) != nil || info.Result == nil && (info.Error == nil || info.Error.Code != 401) {
		return
	}
	d := DiscoveredPrinter{Kind: "moonraker", URL: "http://" + address, Source: "probe"}
	req, _ = http.NewRequestWithContext(ctx, "GET", "http://"+address+"/printer/info", nil)
	if resp, err := client.Do(req); err == nil {
		var printer struct {
			Result struct {
				Hostname string `json:"hostname"`
			} `json:"result"`
		}
		json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&printer)
		resp.Body.Close()
		d.Name = printer.Result.Hostname
	}
	add(d, ip)
}
