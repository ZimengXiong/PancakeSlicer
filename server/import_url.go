package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"html"
	"io"
	"mime"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"os"
	"path"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

const urlImportLimit int64 = 200 << 20
const urlMetadataLimit int64 = 8 << 20

var urlImportSlots = make(chan struct{}, 2)

type urlImportFile struct {
	Name   string `json:"name"`
	URL    string `json:"url,omitempty"`
	Key    string `json:"key,omitempty"`
	SHA256 string `json:"sha256,omitempty"`
	Size   int64  `json:"size,omitempty"`
}
type urlImportResult struct {
	SourceURL string          `json:"sourceUrl"`
	Files     []urlImportFile `json:"files"`
}
type urlImportError struct {
	status  int
	message string
}

func (e *urlImportError) Error() string            { return e.message }
func importError(status int, message string) error { return &urlImportError{status, message} }

func importURL(raw string) (*url.URL, error) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || u == nil || (u.Scheme != "https" && u.Scheme != "http") || u.Hostname() == "" || u.User != nil || len(raw) > 8192 {
		return nil, importError(400, "Pop in an http or https link without embedded credentials.")
	}
	u.Fragment = ""
	return u, nil
}

func urlImportClient() *http.Client {
	transport := &http.Transport{TLSHandshakeTimeout: 10 * time.Second, ResponseHeaderTimeout: 20 * time.Second,
		DisableKeepAlives: true, MaxResponseHeaderBytes: 1 << 20}
	transport.DialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
		host, port, err := net.SplitHostPort(address)
		if err != nil {
			return nil, err
		}
		ips, err := net.DefaultResolver.LookupNetIP(ctx, "ip", host)
		if err != nil {
			return nil, importError(422, "Oops! Couldn't find that link's host.")
		}
		if len(ips) == 0 {
			return nil, importError(422, "Hmm, that link's host doesn't have a public address.")
		}
		for _, ip := range ips {
			if !urlPublicIP(ip) {
				return nil, importError(400, "Private, loopback, and reserved network addresses are blocked.")
			}
		}
		var last error
		for _, ip := range ips {
			conn, e := (&net.Dialer{Timeout: 10 * time.Second}).DialContext(ctx, network, net.JoinHostPort(ip.String(), port))
			if e == nil {
				return conn, nil
			}
			last = e
		}
		return nil, last
	}
	return &http.Client{Transport: transport, Timeout: 90 * time.Second, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) > 5 {
			return importError(422, "Whoa, that link redirects more than five times!")
		}
		_, err := importURL(req.URL.String())
		return err
	}}
}

var urlBlockedNetworks = []netip.Prefix{
	netip.MustParsePrefix("0.0.0.0/8"), netip.MustParsePrefix("100.64.0.0/10"),
	netip.MustParsePrefix("192.0.0.0/24"), netip.MustParsePrefix("192.0.2.0/24"),
	netip.MustParsePrefix("192.88.99.0/24"), netip.MustParsePrefix("198.18.0.0/15"),
	netip.MustParsePrefix("198.51.100.0/24"), netip.MustParsePrefix("203.0.113.0/24"),
	netip.MustParsePrefix("240.0.0.0/4"), netip.MustParsePrefix("2001::/23"),
	netip.MustParsePrefix("2001:db8::/32"), netip.MustParsePrefix("2002::/16"),
}

func urlPublicIP(ip netip.Addr) bool {
	ip = ip.Unmap()
	if !ip.IsGlobalUnicast() || ip.IsPrivate() || ip.IsLoopback() || ip.IsLinkLocalUnicast() {
		return false
	}
	if ip.Is6() && !netip.MustParsePrefix("2000::/3").Contains(ip) {
		return false
	}
	for _, block := range urlBlockedNetworks {
		if block.Contains(ip) {
			return false
		}
	}
	return true
}

func urlFetch(ctx context.Context, client *http.Client, raw, query string, limit int64) ([]byte, string, error) {
	u, err := importURL(raw)
	if err != nil {
		return nil, "", err
	}
	var body io.Reader
	method := http.MethodGet
	if query != "" {
		method = http.MethodPost
		body = strings.NewReader(query)
	}
	req, err := http.NewRequestWithContext(ctx, method, u.String(), body)
	if err != nil {
		return nil, "", err
	}
	req.Header.Set("User-Agent", "Mozilla/5.0 (compatible; PancakeSlicer URL import)")
	if query != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode == 404 {
		return nil, "", importError(404, "Oops! Couldn't find that file or model page (404).")
	}
	if resp.StatusCode == 401 || resp.StatusCode == 403 {
		if strings.HasSuffix(u.Hostname(), "makerworld.com") {
			return nil, "", importError(422, "This file needs a MakerWorld sign-in — download it yourself and drop it right here!")
		}
		return nil, "", importError(422, "This site asks for a sign-in or blocks automatic downloads. Paste a public direct file link instead!")
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, "", importError(422, fmt.Sprintf("Oops, the site returned HTTP %d.", resp.StatusCode))
	}
	if resp.ContentLength > limit {
		return nil, "", importError(413, "Whoa, that download is over the 200 MiB import limit!")
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return nil, "", err
	}
	if int64(len(b)) > limit {
		return nil, "", importError(413, "Whoa, that download is over the 200 MiB import limit!")
	}
	name := path.Base(resp.Request.URL.Path)
	if _, params, e := mime.ParseMediaType(resp.Header.Get("Content-Disposition")); e == nil && params["filename"] != "" {
		name = path.Base(strings.ReplaceAll(params["filename"], "\\", "/"))
	}
	return b, name, nil
}

func (s *Server) importFromURL(w http.ResponseWriter, r *http.Request) {
	select {
	case urlImportSlots <- struct{}{}:
		defer func() { <-urlImportSlots }()
	default:
		fail(w, 429, "two_url_imports_already")
		return
	}
	var in struct {
		URL     string `json:"url"`
		FileURL string `json:"fileUrl"`
		Name    string `json:"name"`
	}
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 32<<10)).Decode(&in) != nil {
		fail(w, 400, "enter_valid_url")
		return
	}
	u, err := importURL(in.URL)
	if err != nil {
		fail(w, 400, "import_url_invalid", err.Error())
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Minute)
	defer cancel()
	client := urlImportClient()
	defer client.CloseIdleConnections()
	result := urlImportResult{SourceURL: u.String()}
	run := func() error {
		target := in.FileURL
		if target == "" {
			choices, recognized, e := urlModelPage(ctx, client, u)
			if e != nil {
				return e
			}
			if recognized {
				if len(choices) == 0 {
					return importError(422, "Hmm, this page doesn't have any publicly downloadable model files. Try pasting a direct file link!")
				}
				if len(choices) > 1 {
					result.Files = choices
					return nil
				}
				target = choices[0].URL
				in.Name = choices[0].Name
			} else {
				target = urlGitHubRaw(u).String()
			}
		}
		target, e := urlPrintablesDownload(ctx, client, target)
		if e != nil {
			return e
		}
		b, name, e := urlFetch(ctx, client, target, "", urlImportLimit)
		if e != nil {
			return e
		}
		if strings.HasPrefix(strings.TrimSpace(string(b[:min(len(b), 128)])), "{") {
			var metadata any
			if json.Unmarshal(b, &metadata) == nil {
				choices := urlMetadataFiles(metadata, u)
				if len(choices) == 1 {
					b, name, e = urlFetch(ctx, client, choices[0].URL, "", urlImportLimit)
					if e != nil {
						return e
					}
				}
			}
		}
		if in.Name != "" {
			name = in.Name
		}
		files, e := s.urlImportAssets(ctx, b, name)
		if e != nil {
			return e
		}
		result.Files = files
		return nil
	}
	if err = run(); err != nil {
		var failure *urlImportError
		if errors.As(err, &failure) {
			fail(w, failure.status, "import_url_refused", failure.message)
		} else if ctx.Err() != nil {
			fail(w, 408, "url_download_timed_out")
		} else {
			fail(w, 422, "couldnt_download_model_link")
		}
		return
	}
	writeJSON(w, 200, result)
}
func urlGitHubRaw(u *url.URL) *url.URL {
	copy := *u
	pieces := strings.Split(strings.TrimPrefix(u.Path, "/"), "/")
	if strings.EqualFold(u.Hostname(), "github.com") && len(pieces) >= 5 && pieces[2] == "blob" {
		copy.Host = "raw.githubusercontent.com"
		copy.Path = "/" + strings.Join(append(pieces[:2], pieces[3:]...), "/")
		copy.RawPath = ""
		copy.RawQuery = ""
	}
	return &copy
}

var urlModelExt = regexp.MustCompile(`(?i)\.(stl|3mf|obj|step|stp|amf|zip)$`)
var urlModelID = regexp.MustCompile(`/(?:model|models)/(\d+)`)
var urlThingID = regexp.MustCompile(`(?:thing:|/things/)(\d+)`)

func urlGraphQL(ctx context.Context, c *http.Client, query string, out any) error {
	body, _ := json.Marshal(map[string]string{"query": query})
	b, _, err := urlFetch(ctx, c, "https://api.printables.com/graphql/", string(body), urlMetadataLimit)
	if err != nil {
		return err
	}
	var envelope struct {
		Data   json.RawMessage   `json:"data"`
		Errors []json.RawMessage `json:"errors"`
	}
	if json.Unmarshal(b, &envelope) != nil || len(envelope.Errors) > 0 || len(envelope.Data) == 0 {
		return importError(422, "Printables didn't share public download info for that one.")
	}
	return json.Unmarshal(envelope.Data, out)
}
func urlModelPage(ctx context.Context, c *http.Client, u *url.URL) ([]urlImportFile, bool, error) {
	host := strings.TrimPrefix(strings.ToLower(u.Hostname()), "www.")
	id := urlModelID.FindStringSubmatch(u.Path)
	if host == "printables.com" && len(id) > 1 {
		var data struct {
			Print struct {
				Stls       []struct{ ID, Name string }
				OtherFiles []struct{ ID, Name string }
			}
		}
		err := urlGraphQL(ctx, c, fmt.Sprintf(`{ print(id: %q) { stls { id name } otherFiles { id name } } }`, id[1]), &data)
		if err != nil {
			return nil, true, err
		}
		files := []urlImportFile{}
		for _, f := range data.Print.Stls {
			if urlModelExt.MatchString(f.Name) {
				files = append(files, urlImportFile{Name: f.Name, URL: "https://api.printables.com/graphql/?pancakePrint=" + id[1] + "&pancakeFile=" + f.ID + "&pancakeType=stl"})
			}
		}
		for _, f := range data.Print.OtherFiles {
			if urlModelExt.MatchString(f.Name) {
				files = append(files, urlImportFile{Name: f.Name, URL: "https://api.printables.com/graphql/?pancakePrint=" + id[1] + "&pancakeFile=" + f.ID + "&pancakeType=other"})
			}
		}
		return files, true, nil
	}
	if host == "makerworld.com" && len(id) > 1 {
		b, _, err := urlFetch(ctx, c, "https://makerworld.com/api/v1/design-service/design/"+id[1], "", urlMetadataLimit)
		if err != nil {
			return nil, true, err
		}
		var data any
		if json.Unmarshal(b, &data) != nil {
			return nil, true, importError(422, "Hmm, MakerWorld sent back invalid model metadata.")
		}
		files := urlMetadataFiles(data, u)
		if len(files) == 0 {
			var model struct {
				Instances []struct {
					ID    int
					Title string
				}
			}
			if json.Unmarshal(b, &model) == nil {
				for _, instance := range model.Instances {
					files = append(files, urlImportFile{Name: instance.Title + ".3mf", URL: fmt.Sprintf("https://makerworld.com/api/v1/design-service/instance/%d/f3mf", instance.ID)})
				}
			}
		}
		return files, true, nil
	}
	if host == "thingiverse.com" && len(urlThingID.FindStringSubmatch(u.String())) > 1 {
		b, _, err := urlFetch(ctx, c, u.String(), "", urlMetadataLimit)
		if err != nil {
			return nil, true, importError(422, "Thingiverse blocks automatic downloads — grab the file yourself and drop it here!")
		}
		files := urlPageFiles(b, u)
		if len(files) == 0 {
			return nil, true, importError(422, "Thingiverse blocks automatic downloads — grab the file yourself and drop it here!")
		}
		return files, true, nil
	}
	return nil, false, nil
}

func urlMetadataFiles(data any, base *url.URL) []urlImportFile {
	files := []urlImportFile{}
	seen := map[string]bool{}
	var walk func(any)
	walk = func(value any) {
		switch v := value.(type) {
		case []any:
			for _, x := range v {
				walk(x)
			}
		case map[string]any:
			name, _ := v["name"].(string)
			if name == "" {
				name, _ = v["modelName"].(string)
			}
			for _, key := range []string{"download_url", "downloadUrl", "contentUrl", "modelUrl", "fileUrl", "url"} {
				raw, _ := v[key].(string)
				if raw == "" {
					continue
				}
				u, e := base.Parse(html.UnescapeString(raw))
				if e != nil {
					continue
				}
				if !urlModelExt.MatchString(u.Path) && !(urlModelExt.MatchString(name) && strings.Contains(strings.ToLower(key), "download")) {
					continue
				}
				if _, e = importURL(u.String()); e != nil || seen[u.String()] {
					continue
				}
				n := name
				if !urlModelExt.MatchString(n) {
					n = path.Base(u.Path)
				}
				files = append(files, urlImportFile{Name: n, URL: u.String()})
				seen[u.String()] = true
			}
			for _, x := range v {
				walk(x)
			}
		}
	}
	walk(data)
	sort.Slice(files, func(i, j int) bool {
		if files[i].Name == files[j].Name {
			return files[i].URL < files[j].URL
		}
		return files[i].Name < files[j].Name
	})
	return files
}

var urlScripts = regexp.MustCompile(`(?is)<script\b[^>]*>(.*?)</script>`)
var urlHref = regexp.MustCompile(`(?i)(?:href|content)\s*=\s*["']([^"']+)["']`)

func urlPageFiles(b []byte, base *url.URL) []urlImportFile {
	files := []urlImportFile{}
	seen := map[string]bool{}
	add := func(entries []urlImportFile) {
		for _, f := range entries {
			if !seen[f.URL] {
				seen[f.URL] = true
				files = append(files, f)
			}
		}
	}
	for _, match := range urlScripts.FindAllSubmatch(b, -1) {
		var value any
		if json.Unmarshal(match[1], &value) == nil {
			add(urlMetadataFiles(value, base))
		}
	}
	for _, match := range urlHref.FindAllSubmatch(b, -1) {
		add(urlMetadataFiles(map[string]any{"url": string(match[1])}, base))
	}
	return files
}

func urlModelKind(b []byte) string {
	if len(b) >= 84 && binary.LittleEndian.Uint32(b[80:84]) > 0 && uint64(binary.LittleEndian.Uint32(b[80:84]))*50+84 == uint64(len(b)) {
		return ".stl"
	}
	text := strings.TrimSpace(string(b[:min(len(b), 256<<10)]))
	lower := strings.ToLower(text)
	if strings.HasPrefix(lower, "solid") && strings.Contains(lower, "facet") && strings.Contains(lower, "vertex") {
		return ".stl"
	}
	if strings.HasPrefix(text, "ISO-10303-21;") {
		return ".step"
	}
	if regexp.MustCompile(`(?m)^v\s+[-+.0-9]`).MatchString(text) && regexp.MustCompile(`(?m)^f\s+-?\d`).MatchString(text) {
		return ".obj"
	}
	decoder := xml.NewDecoder(strings.NewReader(text))
	for {
		token, e := decoder.Token()
		if e != nil {
			break
		}
		if element, ok := token.(xml.StartElement); ok {
			if strings.EqualFold(element.Name.Local, "amf") {
				return ".amf"
			}
			break
		}
	}
	return ""
}
func (s *Server) urlImportAssets(ctx context.Context, b []byte, name string) ([]urlImportFile, error) {
	remaining := uint64(urlImportLimit)
	return s.urlReadModels(ctx, b, name, true, &remaining)
}
func (s *Server) urlReadModels(ctx context.Context, b []byte, name string, archive bool, remaining *uint64) ([]urlImportFile, error) {
	if len(b) >= 4 && bytes.Equal(b[:4], []byte{'P', 'K', 3, 4}) {
		z, e := zip.NewReader(bytes.NewReader(b), int64(len(b)))
		if e != nil {
			return nil, importError(422, "Oops, that download isn't a readable ZIP or 3MF file.")
		}
		if len(z.File) > 4096 {
			return nil, importError(413, "Whoa, that archive has way too many entries!")
		}
		model, types := false, false
		var total uint64
		for _, f := range z.File {
			if f.UncompressedSize64 > *remaining-total {
				return nil, importError(413, "Whoa, that unpacked archive goes over 200 MiB!")
			}
			total += f.UncompressedSize64
			if strings.EqualFold(f.Name, "[Content_Types].xml") {
				types = true
			}
			if strings.HasSuffix(strings.ToLower(f.Name), ".model") {
				reader, err := f.Open()
				if err != nil {
					return nil, err
				}
				decoder := xml.NewDecoder(io.LimitReader(reader, 64<<10))
				for {
					token, err := decoder.Token()
					if err != nil {
						break
					}
					if element, ok := token.(xml.StartElement); ok {
						model = element.Name.Local == "model"
						break
					}
				}
				reader.Close()
			}
		}
		*remaining -= total
		if model && types {
			f, e := s.urlStoreAsset(ctx, b, urlImportName(name, ".3mf"))
			return []urlImportFile{f}, e
		}
		if !archive {
			return nil, importError(422, "ZIPs inside ZIPs aren't supported — link the model file directly instead!")
		}
		files := []urlImportFile{}
		var expanded uint64
		for _, f := range z.File {
			if !urlModelExt.MatchString(f.Name) || strings.HasSuffix(strings.ToLower(f.Name), ".zip") || strings.HasPrefix(f.Name, "__MACOSX/") || f.FileInfo().IsDir() {
				continue
			}
			if len(files) >= 128 || f.UncompressedSize64 > uint64(urlImportLimit)-expanded {
				return nil, importError(413, "Whoa, that archive has over 128 models or 200 MiB unpacked!")
			}
			reader, e := f.Open()
			if e != nil {
				return nil, e
			}
			data, e := io.ReadAll(io.LimitReader(reader, int64(f.UncompressedSize64)+1))
			reader.Close()
			if e != nil {
				return nil, e
			}
			expanded += uint64(len(data))
			if expanded > uint64(urlImportLimit) {
				return nil, importError(413, "Whoa, that unpacked archive goes over 200 MiB!")
			}
			entries, e := s.urlReadModels(ctx, data, path.Base(f.Name), false, remaining)
			if e != nil {
				return nil, fmt.Errorf("%s: %w", path.Base(f.Name), e)
			}
			files = append(files, entries...)
		}
		if len(files) == 0 {
			return nil, importError(422, "Hmm, there aren't any supported model files inside that ZIP.")
		}
		return files, nil
	}
	kind := urlModelKind(b)
	if kind == "" {
		return nil, importError(422, "Oops, that doesn't look like an STL, 3MF, OBJ, STEP, AMF, or model ZIP — it might be a web page instead of a file.")
	}
	f, e := s.urlStoreAsset(ctx, b, urlImportName(name, kind))
	return []urlImportFile{f}, e
}
func urlImportName(name, ext string) string {
	name = path.Base(strings.ReplaceAll(name, "\\", "/"))
	if name == "." || name == "/" || name == "" {
		name = "model"
	}
	if !strings.EqualFold(path.Ext(name), ext) && !(ext == ".step" && strings.EqualFold(path.Ext(name), ".stp")) {
		name = strings.TrimSuffix(name, path.Ext(name)) + ext
	}
	return name
}
func (s *Server) urlStoreAsset(ctx context.Context, b []byte, name string) (urlImportFile, error) {
	sum := sha256.Sum256(b)
	digest := hex.EncodeToString(sum[:])
	key := "sha256-" + digest
	file := urlImportFile{Name: name, Key: key, SHA256: digest, Size: int64(len(b))}
	if _, err := os.Stat(s.workspace.assetPath(key)); err == nil {
		return file, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return file, importError(503, "Oops, asset storage is unavailable right now.")
	}
	req, _ := http.NewRequestWithContext(ctx, http.MethodPut, "/api/assets/"+key, bytes.NewReader(b))
	req.SetPathValue("key", key)
	response := &urlAssetResponse{header: make(http.Header)}
	s.workspace.putAsset(response, req)
	if response.status != 200 {
		return file, importError(response.status, "Uh-oh, we couldn't save the downloaded model to asset storage.")
	}
	return file, nil
}

type urlAssetResponse struct {
	header http.Header
	status int
}

func (r *urlAssetResponse) Header() http.Header    { return r.header }
func (r *urlAssetResponse) WriteHeader(status int) { r.status = status }
func (r *urlAssetResponse) Write(b []byte) (int, error) {
	if r.status == 0 {
		r.status = 200
	}
	return len(b), nil
}

func urlPrintablesDownload(ctx context.Context, c *http.Client, raw string) (string, error) {
	u, e := importURL(raw)
	if e != nil {
		return "", e
	}
	if u.Hostname() != "api.printables.com" || u.Path != "/graphql/" || u.Query().Get("pancakeFile") == "" {
		return raw, nil
	}
	q := u.Query()
	printID, fileID := q.Get("pancakePrint"), q.Get("pancakeFile")
	kind := q.Get("pancakeType")
	if _, e = strconv.ParseUint(printID, 10, 64); e != nil {
		return "", importError(400, "Hmm, that doesn't look like a valid Printables model.")
	}
	if _, e = strconv.ParseUint(fileID, 10, 64); e != nil || kind != "stl" && kind != "other" {
		return "", importError(400, "Hmm, that doesn't look like a valid Printables file.")
	}
	var data struct {
		GetDownloadLink struct {
			OK     bool `json:"ok"`
			Output struct{ Link string }
		}
	}
	e = urlGraphQL(ctx, c, fmt.Sprintf(`mutation { getDownloadLink(id: %q, printId: %q, source: model_detail, fileType: %s) { ok output { link } } }`, fileID, printID, kind), &data)
	if e != nil {
		return "", e
	}
	if !data.GetDownloadLink.OK || data.GetDownloadLink.Output.Link == "" {
		return "", importError(422, "Printables didn't give a public download link for this file.")
	}
	return data.GetDownloadLink.Output.Link, nil
}
