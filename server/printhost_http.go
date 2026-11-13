package main

import (
	"context"
	"crypto/md5"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
	"unicode"
)

func sendHTTP(ctx context.Context, u *upload) (string, error) {
	p := u.printer
	base, _ := url.Parse(p.URL)
	client, err := hostClient(ctx, p, base)
	if err != nil {
		return "", err
	}
	h := &hostSession{client: client, printer: p, secret: u.secret}
	probe := map[string]string{"prusalink": "/api/version", "octoprint": "/api/version", "moonraker": "/server/info"}[p.Kind]
	if err := h.preflight(ctx, base.JoinPath(probe).String()); err != nil {
		return "", err
	}
	build := func() (*http.Request, error) {
		body := progressReader{io.LimitReader(u.file, u.size), u.progress}
		switch p.Kind {
		case "prusalink":
			req, err := http.NewRequestWithContext(ctx, "PUT", base.JoinPath("api/v1/files", p.Storage, u.filename).String(), body)
			if err != nil {
				return nil, err
			}
			req.ContentLength = u.size
			req.Header.Set("Content-Type", "text/x.gcode")
			req.Header.Set("Overwrite", "?1")
			if u.start {
				req.Header.Set("Print-After-Upload", "?1")
			}
			return req, nil
		case "octoprint":
			return multipartUpload(ctx, base.JoinPath("api/files/local").String(), u, body, [][2]string{{"print", fmt.Sprint(u.start)}})
		default:
			return multipartUpload(ctx, base.JoinPath("server/files/upload").String(), u, body, [][2]string{{"root", "gcodes"}, {"print", fmt.Sprint(u.start)}})
		}
	}
	var resp *http.Response
	for attempt := 0; ; attempt++ {
		req, err := build()
		if err != nil {
			return "", err
		}
		if resp, err = h.do(req); err != nil {
			return "", err
		}
		if resp.StatusCode != 401 || p.Auth != "digest" || attempt > 0 {
			break
		}
		resp.Body.Close()
		if h.digest = parseDigest(resp.Header.Get("WWW-Authenticate")); h.digest == nil {
			return "", errors.New("The printer requested an unsupported login method—try switching authorization to API key.")
		}
		if _, err := u.file.Seek(0, io.SeekStart); err != nil {
			return "", err
		}
		u.progress.Store(0)
	}
	defer resp.Body.Close()
	answer := printerMessage(resp)
	switch {
	case resp.StatusCode == 401 || resp.StatusCode == 403:
		return "", credentialsRejected(p)
	case resp.StatusCode == 409:
		return "", fmt.Errorf("The printer couldn't accept the file because it's currently busy printing or the file is locked%s.", answer)
	case resp.StatusCode == 413:
		return "", errors.New("Oops! The print file is too large for the printer's onboard storage.")
	case resp.StatusCode < 200 || resp.StatusCode > 299:
		return "", fmt.Errorf("The printer returned HTTP %d%s", resp.StatusCode, answer)
	}
	if u.start {
		return "Uploaded " + u.filename + " to " + p.Name + " and started printing.", nil
	}
	return "Uploaded " + u.filename + " to " + p.Name + ".", nil
}

func credentialsRejected(*PrinterConfig) error {
	return errors.New("The printer rejected your login—double-check your API key or password in Connection settings.")
}

func hostClient(ctx context.Context, p *PrinterConfig, base *url.URL) (*http.Client, error) {
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	target, err := p.hosts.resolve(ctx, base)
	if err != nil {
		var refused *hostRefusal
		if errors.As(err, &refused) {
			audit(map[string]any{"event": "printer_host_refused", "printer": p.Printer, "reason": refused.reason})
		}
		return nil, err
	}
	dial := func(ctx context.Context, network, _ string) (net.Conn, error) {
		conn, err := dialer.DialContext(ctx, "tcp", target.String())
		if err == nil {
			limitSendBuffer(conn)
		}
		return conn, err
	}
	client := &http.Client{
		Transport: &http.Transport{
			Proxy:                 nil,
			DialContext:           dial,
			TLSClientConfig:       p.tlsConfig(base.Hostname()),
			TLSHandshakeTimeout:   10 * time.Second,
			ResponseHeaderTimeout: 2 * time.Minute,
			DisableKeepAlives:     true,
		},
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
	return client, nil
}

func limitSendBuffer(conn net.Conn) {
	if tcp, ok := conn.(*net.TCPConn); ok {
		tcp.SetWriteBuffer(128 << 10)
	}
}

func multipartUpload(ctx context.Context, target string, u *upload, file io.Reader, fields [][2]string) (*http.Request, error) {
	return multipartFile(ctx, target, "file", u.filename, u.size, file, fields)
}

func multipartFile(ctx context.Context, target, field, filename string, size int64, file io.Reader, fields [][2]string) (*http.Request, error) {
	var head strings.Builder
	w := multipart.NewWriter(&head)
	for _, f := range fields {
		w.WriteField(f[0], f[1])
	}
	if _, err := w.CreateFormFile(field, filename); err != nil {
		return nil, err
	}
	tail := "\r\n--" + w.Boundary() + "--\r\n"
	req, err := http.NewRequestWithContext(ctx, "POST", target, io.MultiReader(strings.NewReader(head.String()), file, strings.NewReader(tail)))
	if err != nil {
		return nil, err
	}
	req.ContentLength = int64(head.Len()) + size + int64(len(tail))
	req.Header.Set("Content-Type", w.FormDataContentType())
	return req, nil
}

func printerMessage(resp *http.Response) string {
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<10))
	text := strings.TrimSpace(string(b))
	var j map[string]any
	if json.Unmarshal(b, &j) == nil {
		for _, key := range []string{"message", "error", "title"} {
			if v, ok := j[key]; ok {
				if nested, ok := v.(map[string]any); ok {
					v = nested["message"]
				}
				if s, ok := v.(string); ok {
					text = s
					break
				}
			}
		}
	}
	if mediaType, _, _ := mime.ParseMediaType(resp.Header.Get("Content-Type")); mediaType == "text/html" {
		text = ""
	}
	text = strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return ' '
		}
		return r
	}, text)
	if len(text) > 200 {
		text = text[:200] + "..."
	}
	if text == "" {
		return ""
	}
	return ": " + text
}

type hostSession struct {
	client  *http.Client
	printer *PrinterConfig
	secret  string
	digest  map[string]string
	nc      int
	session string
}

func (h *hostSession) authorize(req *http.Request) {
	if h.session != "" {
		req.Header.Set("X-Session-Key", h.session)
		return
	}
	if h.secret == "" || h.printer.Kind == "duet" {
		return
	}
	if h.printer.Auth != "digest" {
		req.Header.Set("X-Api-Key", h.secret)
		return
	}
	if h.digest == nil {
		return
	}
	h.nc++
	d := h.digest
	uri := req.URL.RequestURI()
	hash := func(s string) string { sum := md5.Sum([]byte(s)); return hex.EncodeToString(sum[:]) }
	ha1 := hash(h.printer.Username + ":" + d["realm"] + ":" + h.secret)
	ha2 := hash(req.Method + ":" + uri)
	header := fmt.Sprintf(`Digest username=%q, realm=%q, nonce=%q, uri=%q, algorithm=MD5`, h.printer.Username, d["realm"], d["nonce"], uri)
	if qop := d["qop"]; qop != "" {
		nonceCount := fmt.Sprintf("%08x", h.nc)
		raw := make([]byte, 8)
		rand.Read(raw)
		cnonce := hex.EncodeToString(raw)
		header += fmt.Sprintf(`, qop=auth, nc=%s, cnonce=%q, response=%q`, nonceCount, cnonce, hash(ha1+":"+d["nonce"]+":"+nonceCount+":"+cnonce+":auth:"+ha2))
	} else {
		header += fmt.Sprintf(`, response=%q`, hash(ha1+":"+d["nonce"]+":"+ha2))
	}
	if opaque := d["opaque"]; opaque != "" {
		header += fmt.Sprintf(`, opaque=%q`, opaque)
	}
	req.Header.Set("Authorization", header)
}

func parseDigest(header string) map[string]string {
	scheme, rest, _ := strings.Cut(strings.TrimSpace(header), " ")
	if !strings.EqualFold(scheme, "Digest") {
		return nil
	}
	params := map[string]string{}
	for rest = strings.TrimSpace(rest); rest != ""; rest = strings.TrimLeft(strings.TrimSpace(rest), ",") {
		rest = strings.TrimSpace(rest)
		key, value, ok := strings.Cut(rest, "=")
		if !ok {
			break
		}
		key = strings.ToLower(strings.TrimSpace(key))
		value = strings.TrimSpace(value)
		if strings.HasPrefix(value, `"`) {
			end := strings.Index(value[1:], `"`)
			if end < 0 {
				return nil
			}
			params[key], rest = value[1:end+1], value[end+2:]
		} else {
			v, after, _ := strings.Cut(value, ",")
			params[key], rest = strings.TrimSpace(v), after
		}
	}
	if algorithm := params["algorithm"]; algorithm != "" && !strings.EqualFold(algorithm, "MD5") {
		return nil
	}
	if qop := params["qop"]; qop != "" {
		auth := false
		for _, q := range strings.Split(qop, ",") {
			auth = auth || strings.TrimSpace(q) == "auth"
		}
		if !auth {
			return nil
		}
		params["qop"] = "auth"
	}
	return params
}

func (h *hostSession) do(req *http.Request) (*http.Response, error) {
	h.authorize(req)
	resp, err := h.client.Do(req)
	if err != nil {
		if cause := context.Cause(req.Context()); cause != nil {
			return nil, cause
		}
		return nil, netError(err, "the printer")
	}
	return resp, nil
}

func (h *hostSession) preflight(ctx context.Context, target string) error {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	for attempt := 0; ; attempt++ {
		req, err := http.NewRequestWithContext(ctx, "GET", target, nil)
		if err != nil {
			return err
		}
		resp, err := h.do(req)
		if err != nil {
			return err
		}
		io.Copy(io.Discard, io.LimitReader(resp.Body, 64<<10))
		resp.Body.Close()
		if resp.StatusCode == 401 && h.printer.Auth == "digest" && attempt == 0 {
			if h.digest = parseDigest(resp.Header.Get("WWW-Authenticate")); h.digest == nil {
				return errors.New("The printer requested an unsupported login method—try switching authorization to API key.")
			}
			continue
		}
		switch {
		case resp.StatusCode == 401 || resp.StatusCode == 403:
			return credentialsRejected(h.printer)
		case resp.StatusCode == 404:
			return fmt.Errorf("Hmm, that address doesn't look like a %s host (got HTTP 404 at %s).", connectionKinds[h.printer.Kind], req.URL.Path)
		case resp.StatusCode < 200 || resp.StatusCode > 299:
			return fmt.Errorf("The printer returned HTTP %d", resp.StatusCode)
		}
		return nil
	}
}
