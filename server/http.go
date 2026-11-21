package main

import (
	"cmp"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"math"
	"mime"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

type Server struct {
	cfg           Config
	jobs          *JobManager
	catalog       *resolve.Catalog
	library       *libraryStore
	workspace     *workspaceStore
	sends         *SendManager
	blobs         *blobStore
	fontsOnce     sync.Once
	fonts         []byte
	fontsErr      error
	filamentSlots *filamentSlotStore
}

func NewServer(c Config, jobs *JobManager, catalog *resolve.Catalog) http.Handler {
	s := &Server{cfg: c, jobs: jobs, catalog: catalog, library: &libraryStore{dir: c.DataDir}, sends: NewSendManager(c.PrinterStore), filamentSlots: newFilamentSlotStore(filepath.Join(c.DataDir, "printers"))}
	s.library.swap, s.library.catalog = catalog.Swap, catalog
	s.library.extruders = func(variant string) int {
		if v := catalog.VariantByID(variant); v != nil {
			return v.Extruders
		}
		return 1
	}
	s.library.toolheads = func(vendor, model string, nozzle float64) int {
		return s.library.extruders(catalog.Variant(model, nozzle))
	}
	if _, err := os.Stat(filepath.Join(c.DataDir, libraryFile)); err == nil {
		if _, err := s.libraryDocument(); err != nil {
			log.Printf("profile library: %v", err)
		}
		s.library.reportLoad()
	}
	s.reconcileStoredConnections("startup")
	desktopReadyNonce := os.Getenv("ORCA_DESKTOP_READY_NONCE")
	healthProbe := newEngineHealth(c)
	jobs.cfg.NativeHealth = healthProbe
	mux := http.NewServeMux()
	workspace := &workspaceStore{root: filepath.Join(c.DataDir, "workspace")}
	workspace.register(mux)
	s.workspace = workspace
	s.versionRoutes(mux)
	s.blobs = &blobStore{root: filepath.Join(c.DataDir, "blobs"), limit: blobCacheBytes}
	s.blobs.register(mux, c.MaxUploadSize)
	mux.HandleFunc("POST /api/imports/url", s.importFromURL)
	mux.HandleFunc("GET /api/healthz", func(w http.ResponseWriter, r *http.Request) {
		q, n := jobs.Counts()
		health := healthProbe.snapshot(c.Tool)
		if desktopReadyNonce != "" {
			health["desktop_ready_nonce"] = desktopReadyNonce
			w.Header().Set("Cache-Control", "no-store")
		}
		health["status"], health["queued"], health["running"] = "ok", q, n
		health["library_schema"] = schemas.Library
		health["workspace_schema"] = schemas.Workspace
		health["server_revision"] = buildAbout(c).Revision
		health["max_concurrency"], health["max_queue"] = jobs.cfg.MaxJobs, jobs.cfg.MaxQueue
		if jobResultStats != nil {
			health["result_cache"] = jobResultStats(jobs)
		}
		writeJSON(w, 200, health)
	})
	mux.HandleFunc("GET /api/source", s.source)
	mux.HandleFunc("GET /api/about", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, buildAbout(c))
	})
	mux.HandleFunc("POST /api/access", s.saveAccess)
	mux.HandleFunc("DELETE /api/access", s.forgetAccess)
	mux.HandleFunc("POST /api/jobs", s.createJob)
	mux.HandleFunc("POST /api/patch-preview", s.createJob)
	mux.HandleFunc("POST /api/top-surfaces", s.topSurfaces)
	mux.HandleFunc("POST /api/top-surfaces/plan", s.createJob)
	mux.HandleFunc("GET /api/jobs/{id}", s.getJob)
	mux.HandleFunc("DELETE /api/jobs/{id}", s.deleteJob)
	mux.HandleFunc("GET /api/jobs/{id}/events", s.events)
	mux.HandleFunc("GET /api/jobs/{id}/transitions", s.jobTransitions)
	mux.HandleFunc("POST /api/jobs/{id}/cancel", s.cancelJob)
	mux.HandleFunc("POST /api/jobs/{id}/retry", s.retryJob)
	mux.HandleFunc("GET /api/jobs/{id}/download", s.download)
	mux.HandleFunc("GET /api/jobs/{id}/moves", s.moves)
	mux.HandleFunc("GET /api/jobs/{id}/gcode-lines", s.gcodeLines)
	mux.HandleFunc("GET /api/jobs/{id}/bambu-mapping", s.bambuJobMapping)
	mux.HandleFunc("GET /api/jobs/{id}/surface-grid", s.surfaceGrid)
	mux.HandleFunc("GET /api/jobs/{id}/exploded/{name}", s.exploded)
	mux.HandleFunc("POST /api/jobs/{id}/send", s.sendJob)
	mux.HandleFunc("GET /api/jobs/{id}/send", s.getSend)
	mux.HandleFunc("GET /api/jobs/{id}/send/plan", s.getSendPlan)
	mux.HandleFunc("POST /api/jobs/{id}/send/cancel", s.cancelSend)
	mux.HandleFunc("GET /api/printer-connections", s.listConnections)
	mux.HandleFunc("GET /api/events", s.liveEvents)
	mux.HandleFunc("GET /api/printer-connections/{printer}/camera", s.printerCameras)
	mux.HandleFunc("GET /api/printer-connections/{printer}/job-thumbnail", s.printerJobThumbnail)
	mux.HandleFunc("GET /api/printer-connections/{printer}/camera/{camera}", s.printerCamera)
	mux.HandleFunc("POST /api/printer-connections/test", s.testConnection)
	mux.HandleFunc("POST /api/printer-connections/discover", s.discoverPrinters)
	mux.HandleFunc("PUT /api/printer-connections/{printer}", s.putConnection)
	mux.HandleFunc("DELETE /api/printer-connections/{printer}", s.deleteConnection)
	mux.HandleFunc("POST /api/printer-connections/{printer}/adopt", s.adoptConnection)
	mux.HandleFunc("POST /api/printer-connections/{printer}/legacy", s.importLegacyConnection)
	mux.HandleFunc("POST /api/printer-connections/{printer}/ams", s.amsConnection)
	mux.HandleFunc("POST /api/printer-connections/{printer}/tray", s.setTray)
	mux.HandleFunc("POST /api/printer-connections/{printer}/control", s.controlConnection)
	mux.HandleFunc("POST /api/printer-connections/{printer}/device", s.deviceControl)
	mux.HandleFunc("GET /api/printer-sends", s.listSends)
	s.printerSlotRoutes(mux)
	s.catalogRoutes(mux)
	s.updateRoutes(mux)
	s.libraryRoutes(mux)
	s.libraryIORoutes(mux)
	mux.HandleFunc("POST /api/resolve", s.postResolve)
	registerOptions(mux, c.Options, s.catalog, devResetEnabled())
	if devResetEnabled() {
		mux.HandleFunc("POST /api/dev/reset", s.devReset)
	}
	mux.HandleFunc("GET /api/handy-models/{name}", s.handyModel)
	mux.HandleFunc("GET /api/emboss-fonts", s.embossFonts)
	mux.HandleFunc("GET /api/user-heads", s.listUserHeads)
	mux.HandleFunc("POST /api/user-heads", s.saveUserHead)
	mux.HandleFunc("/api/", func(w http.ResponseWriter, r *http.Request) { fail(w, 404, "unknown_api_endpoint") })
	mux.HandleFunc("GET /api/boot", s.getBoot)
	mux.HandleFunc("POST /api/slot-offer", s.postSlotOffer)
	mux.Handle("/", spaFiles(c.StaticDir, c.Resources, s.bootScript))
	if c.Metrics {
		mux.HandleFunc("GET /metrics", s.metrics)
	}
	return gzipJSON(s.middleware(revalidate(mux)))
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(value)
}

func jobUsedSlots(r *http.Request, slots int, sent []int) ([]int, error) {
	if plate := r.FormValue("plate"); plate != "" {
		used, err := resolve.PlateUsedSlots([]byte(plate), slots)
		if err != nil {
			return nil, errors.New("The plate needs at least one model on it.")
		}
		return used, nil
	}
	var files []struct {
		Name string `json:"name"`
	}
	json.Unmarshal([]byte(r.FormValue("files")), &files)
	if len(files) > 0 && strings.EqualFold(filepath.Ext(files[0].Name), ".3mf") {
		if len(sent) > 0 && !slices.ContainsFunc(sent, func(n int) bool { return n < 1 || n > slots }) {
			return sent, nil
		}
		all := make([]int, slots)
		for i := range all {
			all[i] = i + 1
		}
		return all, nil
	}
	return []int{1}, nil
}

func (s *Server) jobPlateSettings(r *http.Request) []byte {
	if plate := r.FormValue("plate"); plate != "" {
		return []byte(plate)
	}
	var files []struct {
		Name   string `json:"name"`
		SHA256 string `json:"sha256"`
	}
	json.Unmarshal([]byte(r.FormValue("files")), &files)
	if len(files) == 0 || !strings.EqualFold(filepath.Ext(files[0].Name), ".3mf") || !blobDigest.MatchString(files[0].SHA256) {
		return nil
	}
	data, err := os.ReadFile(s.blobs.path(files[0].SHA256))
	if err != nil {
		return nil
	}
	return archivePlate(data)
}

func presetDisplayName(name string) string {
	return path.Base(strings.ReplaceAll(name, "\\", "/"))
}

const uploadIdleTimeout = 10 * time.Second
const uploadProgressBytes = 64 << 10

type uploadBody struct {
	io.ReadCloser
	controller   *http.ResponseController
	rate         int64
	pending      int
	rateDeadline time.Time
}

func (b *uploadBody) Read(p []byte) (int, error) {
	n, err := b.ReadCloser.Read(p)
	b.pending += n
	if b.pending >= uploadProgressBytes {
		b.rateDeadline = b.rateDeadline.Add(time.Duration(float64(b.pending) / float64(b.rate) * float64(time.Second)))
		b.pending = 0
		deadline := time.Now().Add(uploadIdleTimeout)
		if b.rateDeadline.Before(deadline) {
			deadline = b.rateDeadline
		}
		if deadlineErr := b.controller.SetReadDeadline(deadline); deadlineErr != nil && err == nil {
			err = deadlineErr
		}
	}
	return n, err
}

func (s *Server) createJob(w http.ResponseWriter, r *http.Request) {
	key, err := submissionKey(r.URL.Path, r.Header.Get("Idempotency-Key"))
	if err != nil {
		fail(w, 400, "http_invalid", err.Error())
		return
	}
	existing, finish, err := s.jobs.reserve(r.Context(), key)
	if err != nil {
		fail(w, 408, "http_timeout", err.Error())
		return
	}
	if existing != nil {
		w.Header().Set("Location", "/api/jobs/"+existing.ID)
		writeJSON(w, 202, existing.View())
		return
	}
	var accepted *Job
	defer func() { finish(accepted) }()
	j, err := s.jobs.NewJob(s.clientIP(r))
	if err != nil {
		admissionFailure(w, err)
		return
	}
	j.IdempotencyKey = key
	started := false
	defer func() {
		if started {
			accepted = j
		}
		s.jobs.releaseUpload(j)
		if !started {
			os.RemoveAll(j.Dir)
		}
	}()

	if r.ContentLength > s.cfg.MaxUploadSize {
		fail(w, http.StatusRequestEntityTooLarge, "upload_larger_than_servers")
		return
	}
	controller := http.NewResponseController(w)
	deadline := time.Now().Add(uploadIdleTimeout)
	if err := controller.SetReadDeadline(deadline); err != nil {
		fail(w, 500, "couldnt_set_upload_timeout")
		return
	}
	defer controller.SetReadDeadline(time.Time{})
	r.Body = &uploadBody{ReadCloser: http.MaxBytesReader(w, r.Body, s.cfg.MaxUploadSize),
		controller: controller, rate: s.cfg.UploadMinRate, rateDeadline: deadline}
	parseErr := r.ParseMultipartForm(8 << 20)
	if r.MultipartForm != nil {
		defer r.MultipartForm.RemoveAll()
	}
	if err := parseErr; err != nil {
		var tooLarge *http.MaxBytesError
		var timeout net.Error
		status := 400
		if errors.As(err, &tooLarge) {
			status = 413
		}
		if errors.As(err, &timeout) && timeout.Timeout() {
			status = http.StatusRequestTimeout
			w.Header().Set("Connection", "close")
			fail(w, status, "upload_took_too_long")
			return
		}
		fail(w, status, "invalid_file_upload", err.Error())
		return
	}
	req := JobRequest{Orient: r.FormValue("operation") == "orient" || r.FormValue("operation") == "orient-scores", Output: r.FormValue("output"), Filename: r.FormValue("filename")}
	if v := r.Header.Get(sessionHeader); len(v) <= 256 {
		req.Session = v
	}
	if v := r.Header.Get("X-Slice-Group"); len(v) <= 256 {
		req.SliceGroup = v
	}
	var resolved *resolve.SliceConfig
	if raw := r.FormValue("library"); raw == "" {
		fail(w, 400, "job_names_library_printer")
		return
	} else {
		var in resolve.Request
		lib, err := s.libraryDocument()
		if err == nil {
			err = json.Unmarshal([]byte(raw), &in)
		}
		if err == nil {
			in.UsedSlots, err = jobUsedSlots(r, len(in.Filaments), in.UsedSlots)
			in.Saving = r.FormValue("output") == "3mf" && r.FormValue("operation") == ""
			in.PlateNonplanar = resolve.PlateNonplanar(s.jobPlateSettings(r))
		}
		meshing := len(in.Filaments) == 0 && r.FormValue("output") == "3mf" && r.FormValue("plate") == "" && r.FormValue("operation") == ""
		if err == nil && meshing {
			resolved, err = s.catalog.Meshing(lib, in)
		} else if err == nil {
			resolved, err = s.catalog.Job(lib, in)
		}
		if err != nil {
			code := "resolve_failed"
			if resolve.ErrorCode(err) == "slot_unpicked" {
				code = "slot_unpicked"
			}
			libraryFailure(w, 422, code, err.Error(), "choose_profiles", 0)
			return
		}
		req.Printer, req.Process, req.Filaments = resolved.Engine.Printer, resolved.Engine.Process, resolved.Engine.Filaments
		req.Library = &in
	}
	switch op := r.FormValue("operation"); op {
	case "", "orient":
	case "orient-scores":
		req.OrientScores = true
	case "arrange":
		req.Arrange, req.ArrangeNoRotation = true, r.FormValue("arrange_rotation") == "0"
		if raw := r.FormValue("arrange_spacing"); raw != "" {
			v, err := strconv.ParseFloat(raw, 64)
			if err != nil || v < 0 || v > 100 || math.IsNaN(v) {
				fail(w, 400, "object_spacing_must_between")
				return
			}
			req.ArrangeSpacing = v
		}
		if raw := r.FormValue("arrange_multi_materials"); raw != "" {
			if raw != "0" && raw != "1" {
				fail(w, 400, "arrange_multi_materials_must")
				return
			}
			req.ArrangeMaterials = raw
		}
	case "support-fins":
		if req.Output != "" {
			fail(w, 400, "http_invalid", "Support Fins chooses its own STL output")
			return
		}
		req.SupportFins, req.Output = true, "stl"
		nozzle, nozzleErr := strconv.ParseFloat(r.FormValue("support_fins_nozzle"), 64)
		layer, layerErr := strconv.ParseFloat(r.FormValue("support_fins_layer_height"), 64)
		allowedNozzle := map[float64]bool{0.2: true, 0.25: true, 0.4: true, 0.6: true, 0.8: true, 1: true, 1.2: true, 1.4: true, 1.6: true, 1.8: true, 2: true}
		if nozzleErr != nil || !allowedNozzle[nozzle] {
			fail(w, 400, "http_invalid", "Support Fins nozzle must be one of 0.2, 0.25, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8 or 2.0 mm")
			return
		}
		if layerErr != nil || math.IsNaN(layer) || math.IsInf(layer, 0) || layer < 0.08 || layer > 0.6 {
			fail(w, 400, "http_invalid", "Support Fins layer height must be between 0.08 and 0.6 mm")
			return
		}
		req.SupportFinsNozzle, req.SupportFinsLayer = nozzle, layer
	case "repair", "simplify":
		req.MeshOp = r.FormValue("operation")
		if req.MeshOp == "simplify" {
			ratio, triangles, maxErr := r.FormValue("simplify_ratio"), r.FormValue("simplify_triangles"), r.FormValue("simplify_max_error")
			v, errR := strconv.ParseFloat(ratio, 64)
			n, errT := strconv.Atoi(triangles)
			e, errE := strconv.ParseFloat(maxErr, 64)
			switch {
			case maxErr != "" && ratio == "" && triangles == "" && errE == nil && e > 0 && e <= 10:
				req.SimplifyMaxError = e
			case ratio != "" && triangles == "" && errR == nil && v > 0 && v <= 1:
				req.SimplifyRatio = v
			case triangles != "" && ratio == "" && errT == nil && n > 0:
				req.SimplifyTriangles = n
			default:
				fail(w, 400, "simplify_needs_simplify_ratio")
				return
			}
		}
	case "export":
		format := r.FormValue("export_format")
		if format != "drc" && format != "generic3mf" {
			fail(w, 400, "export_format_must_generic3mf")
			return
		}
		req.MeshOp, req.ExportFormat = "export", format
		if raw := r.FormValue("draco_bits"); raw != "" {
			n, err := strconv.Atoi(raw)
			if err != nil || n < 0 || n > 30 || format != "drc" {
				fail(w, 400, "draco_bits_must_drc")
				return
			}
			req.DracoBits = n
		}
	case "boolean":
		op := r.FormValue("boolean_op")
		if op != "union" && op != "difference" && op != "intersection" {
			fail(w, 400, "boolean_op_must_union")
			return
		}
		req.MeshOp, req.BooleanOp = "boolean", op
	case "svg":
		args := []string{}
		for _, f := range []struct {
			key, flag string
			lo, hi    float64
		}{{"depth", "--depth", 0.01, 1000}, {"size", "--size", 0.1, 1000}} {
			raw := r.FormValue(f.key)
			if raw == "" && f.key == "size" {
				continue
			}
			v, err := strconv.ParseFloat(raw, 64)
			if err != nil || math.IsNaN(v) || v < f.lo || v > f.hi {
				fail(w, 400, "must_v_v", f.key, f.lo, f.hi)
				return
			}
			args = append(args, f.flag, strconv.FormatFloat(v, 'f', -1, 64))
		}
		req.MeshOp, req.Text = "svg", args
		if r.FormValue("use_surface") == "1" {
			surface, err := surfaceArgs(r)
			if err != nil {
				fail(w, 400, "http_invalid", err.Error())
				return
			}
			req.TextSurface = surface
		}
	case "text":
		args, err := textArgs(r)
		if err != nil {
			fail(w, 400, "http_invalid", err.Error())
			return
		}
		req.MeshOp, req.Text = "text", args
		if r.FormValue("use_surface") == "1" {
			if req.TextSurface, err = surfaceArgs(r); err != nil {
				fail(w, 400, "http_invalid", err.Error())
				return
			}
		}
	case "explode":
		req.Explode = true
	case "wipe-tower":
		req.WipeTower = true
	case "cut":
		req.Cut, req.CutKeep, req.CutPlaceLower = r.FormValue("cut"), r.FormValue("cut_keep"), r.FormValue("cut_place_lower") == "1"
		if req.CutKeep == "" {
			req.CutKeep = "both"
		}
		if !validCutPlane(req.Cut) || (req.CutKeep != "upper" && req.CutKeep != "lower" && req.CutKeep != "both") {
			fail(w, 400, "cut_needs_plane_z")
			return
		}
		req.CutConnectors, req.CutDovetail = r.FormValue("cut_connectors"), r.FormValue("cut_dovetail")
		if msg := cutJoinError(req); msg != "" {
			fail(w, 400, "http_invalid", msg)
			return
		}
	default:
		fail(w, 400, "unrecognized_operation")
		return
	}
	if strings.ContainsAny(req.Filename, "/\\\r\n") || len(req.Filename) > 240 || strings.IndexFunc(req.Filename, unicode.IsControl) >= 0 || req.Filename == "." || req.Filename == ".." {
		fail(w, 400, "filename_must_basename_most")
		return
	}
	if req.Output == "" {
		req.Output = "gcode"
	}
	switch p := r.FormValue("priority"); p {
	case "", "normal", "interactive":
	case "low", "batch":
		req.Priority = p
	default:
		fail(w, 400, "priority_must_interactive_normal")
		return
	}
	if r.FormValue("force_planar") != "" {
		req.ForcePlanar = true
	}
	if r.FormValue("floating") != "" {
		req.Floating = true
	}
	if r.FormValue("allow_mixed_temperatures") != "" {
		req.AllowMixedTemperatures = true
	}
	if r.URL.Path == "/api/patch-preview" {
		budget, err := strconv.ParseFloat(cmp.Or(r.FormValue("budget"), "2"), 64)
		if err != nil || !(budget >= 0.1 && budget <= 60) {
			fail(w, 400, "budget_must_seconds")
			return
		}
		req.Output, req.Priority, req.PreviewBudget = "patch-preview", "low", budget
	} else if r.URL.Path == "/api/top-surfaces/plan" {
		req.Output, req.Priority = "plan-tops", "low"
	} else if req.Output != "gcode" && req.Output != "gcode.3mf" && req.Output != "3mf" && !(req.SupportFins && req.Output == "stl") {
		fail(w, 400, "output_must_gcode_gcode")
		return
	}
	presets, problem := s.resolveJobProfiles(&req)
	if problem != nil {
		problem.write(w)
		return
	}
	values := resolved.EngineValues()
	encoded, _ := json.Marshal(values)
	if req.Overrides, err = overrides(string(encoded)); err != nil {
		fail(w, 400, "http_invalid", err.Error())
		return
	}
	if raw := r.FormValue("events"); raw != "" {
		if json.Unmarshal([]byte(raw), &req.Events) != nil {
			fail(w, 400, "events_must_array_strings")
			return
		}
	}
	if len(req.Events) > 0 {
		rawHeight := presets.machine.Config["printable_height"]
		heightText := stringValue(rawHeight)
		if heightText == "" {
			heightText = string(rawHeight)
		}
		maxHeight, _ := strconv.ParseFloat(heightText, 64)
		for _, event := range req.Events {
			text, _, hasType := strings.Cut(event, ":")
			height, err := strconv.ParseFloat(text, 64)
			if err != nil || !hasType || math.IsNaN(height) || math.IsInf(height, 0) || height <= 0 || !(height < maxHeight) {
				fail(w, 400, "event_height_must_finite")
				return
			}
		}
	}
	var files []struct {
		Name   string `json:"name"`
		SHA256 string `json:"sha256"`
	}
	if raw := r.FormValue("files"); raw != "" && json.Unmarshal([]byte(raw), &files) != nil {
		fail(w, 400, "files_must_array_name")
		return
	}
	if kind := r.FormValue("calib"); kind != "" {
		params := r.FormValue("calib_params")
		switch {
		case !calibKinds[kind]:
			fail(w, 400, "unrecognized_calibration_test_type")
		case !calibParams.MatchString(params):
			fail(w, 400, "calib_params_must_key")
		case !calibHead.MatchString(r.FormValue("calib_head")):
			fail(w, 400, "calib_head_must_head")
		case len(files) > 0 || r.FormValue("plate") != "" || req.Output == "3mf" || req.Orient:
			fail(w, 400, "calibration_takes_no_files")
		default:
			req.Calib, req.CalibParams, req.CalibHead = kind, params, r.FormValue("calib_head")
			s.startJob(w, j, req, &started)
		}
		return
	}
	if req.MeshOp == "text" && len(req.TextSurface) == 0 {
		if len(files) > 0 || r.FormValue("plate") != "" || req.Output == "3mf" {
			fail(w, 400, "text_takes_no_files")
			return
		}
		s.startJob(w, j, req, &started)
		return
	}
	if len(files) == 0 {
		fail(w, 400, "files_required")
		return
	}
	names := map[string]bool{}
	var missing []string
	for _, f := range files {
		name := f.Name
		ext := strings.ToLower(filepath.Ext(name))
		other := ext == ".step" || ext == ".stp" || ext == ".drc" || req.MeshOp == "svg" && ext == ".svg"
		if name == "" || len(name) > 240 || strings.IndexFunc(name, unicode.IsControl) >= 0 || strings.ContainsAny(name, "/\\") || name != filepath.Base(name) || name == "." || name == ".." || (ext != ".stl" && ext != ".obj" && ext != ".3mf" && !other) || names[name] {
			fail(w, 400, "each_uploaded_stl_obj")
			return
		}
		if !blobDigest.MatchString(f.SHA256) {
			fail(w, 400, "each_file_needs_lowercase")
			return
		}
		names[name] = true
		req.Files = append(req.Files, name)
		if err := s.blobs.link(f.SHA256, filepath.Join(j.Dir, "input", name)); errors.Is(err, fs.ErrNotExist) {
			missing = append(missing, f.SHA256)
		} else if err != nil {
			fail(w, 500, "http_internal", err.Error())
			return
		}
	}
	if len(missing) > 0 {
		writeJSON(w, 409, map[string]any{"error": "Re-syncing your model files with the slicing service…", "code": "blob_missing", "missing": missing})
		return
	}
	rawPlate := r.FormValue("plate")
	if raw := rawPlate; raw != "" {
		var plate map[string]json.RawMessage
		var objects []map[string]json.RawMessage
		if json.Unmarshal([]byte(raw), &plate) != nil || json.Unmarshal(plate["objects"], &objects) != nil || len(objects) == 0 {
			fail(w, 400, "plate_needs_least_one")
			return
		}
		for _, obj := range objects {
			var file string
			var volumes []struct {
				File string `json:"file"`
				Type string `json:"type"`
			}
			if obj["volumes"] != nil && json.Unmarshal(obj["volumes"], &volumes) != nil ||
				(obj["file"] != nil || len(volumes) == 0) && (json.Unmarshal(obj["file"], &file) != nil || !names[file]) {
				fail(w, 400, "every_object_plate_must")
				return
			}
			for _, v := range volumes {
				if !names[v.File] {
					fail(w, 400, "every_part_volume_plate")
					return
				}
			}
		}
		if err := validatePlatePlacement([]byte(raw), req.Output == "3mf"); err != nil {
			fail(w, 400, "http_invalid", err.Error())
			return
		}
		if err := os.WriteFile(filepath.Join(j.Dir, "input", "plate.json"), []byte(raw), 0600); err != nil {
			fail(w, 500, "http_internal", err.Error())
			return
		}
		req.Plate = true
	} else if raw := r.FormValue("project_edits"); raw != "" {
		if err := writeProjectEdits(filepath.Join(j.Dir, "input"), req.Files, names, raw); err != nil {
			fail(w, 400, "http_invalid", err.Error())
			return
		}
		req.Edits = true
	} else if len(files) != 1 && req.MeshOp != "boolean" && req.MeshOp != "svg" {
		fail(w, 400, "multiple_uploads_require_plate")
		return
	}
	if raw := r.FormValue("plate_index"); raw != "" {
		req.PlateIndex, err = strconv.Atoi(raw)
		if err != nil || req.PlateIndex < 1 || req.Plate || req.Orient || len(req.Files) != 1 && !req.Edits || !strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
			fail(w, 400, "plate_index_requires_positive")
			return
		}
	}
	if req.Explode && (req.Plate || len(req.Files) != 1 || !strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") || req.PlateIndex > 0) {
		fail(w, 400, "explode_requires_one_3mf")
		return
	}
	if !req.Plate && !req.Explode && strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
		layout, layoutErr := readProjectLayout(filepath.Join(j.Dir, "input", req.Files[0]))
		if layoutErr != nil {
			fail(w, 400, "http_invalid", layoutErr.Error())
			return
		}
		if req.PlateIndex > 0 && req.Output != "3mf" {
			if layout.Width <= 0 || layout.Depth <= 0 {
				fail(w, 400, "selected_project_doesnt_define")
				return
			}
			for _, plate := range layout.Membership {
				if plate == 0 {
					fail(w, 400, "couldnt_match_every_object")
					return
				}
			}
		}
		req.SourcePlates = layout.Plates
		if req.PlateIndex > 0 && !layout.hasPlate(req.PlateIndex) {
			fail(w, 400, "plate_number_doesnt_exist")
			return
		}
		if req.Output != "3mf" && len(layout.Plates) > 1 && req.PlateIndex == 0 {
			fail(w, 400, "multi_plate_project_pick")
			return
		}
	}
	if raw := r.FormValue("project_instances"); raw != "" {
		if req.Plate || len(req.Files) != 1 && !req.Edits || !strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
			fail(w, 400, "project_instances_requires_one")
			return
		}
		var instances []projectInstance
		if json.Unmarshal([]byte(raw), &instances) != nil || len(instances) == 0 {
			fail(w, 400, "project_instances_needs_non")
			return
		}
		if err := projectTransformsWorkaround(filepath.Join(j.Dir, "input", req.Files[0]), instances); err != nil {
			fail(w, 400, "http_invalid", err.Error())
			return
		}
	}
	if raw := r.FormValue("project_settings"); raw != "" {
		if req.Plate || len(req.Files) != 1 && !req.Edits || !strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
			fail(w, 400, "project_settings_requires_one")
			return
		}
		settings, err := overrides(raw)
		if err != nil {
			fail(w, 400, "project_settings", err.Error())
			return
		}
		if err := patchProjectSettings(filepath.Join(j.Dir, "input", req.Files[0]), settings); err != nil {
			fail(w, 400, "http_invalid", err.Error())
			return
		}
		req.ProjectSettings = settings
	}
	if !req.Plate && strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf") {
		machine := presets.machine
		selected := make([][2]string, 0, len(machine.Config)+len(req.Overrides))
		for key, raw := range machine.Config {
			if resolve.OptionKind(key) == "" || resolve.HiddenSetting(key) {
				continue
			}
			value, err := configValue(key, raw)
			if err != nil {
				fail(w, 500, "invalid_system_printer_setting", key)
				return
			}
			selected = append(selected, [2]string{key, value})
		}
		req.Overrides = append(selected, req.Overrides...)
	}
	if req.MeshOp == "svg" {
		want := map[bool]int{true: 2, false: 1}[len(req.TextSurface) > 0]
		if req.Output != "gcode" || req.Plate || len(req.Files) != want || !strings.EqualFold(filepath.Ext(req.Files[0]), ".svg") || want == 2 && !allSTL(req.Files[1:]) {
			fail(w, 400, "svg_takes_svg_use")
			return
		}
	} else if want := map[bool]int{true: 2, false: 1}[req.MeshOp == "boolean"]; req.MeshOp != "" && (req.Output != "gcode" || req.Plate || len(req.Files) != want || !allSTL(req.Files) && !(req.MeshOp == "export" && strings.EqualFold(filepath.Ext(req.Files[0]), ".3mf"))) {
		fail(w, 400, "mesh_operation_takes_v", want)
		return
	}
	if req.Cut != "" && (req.Output != "gcode" || req.Plate || len(req.Files) != 1 || !strings.EqualFold(filepath.Ext(req.Files[0]), ".stl")) {
		fail(w, 400, "cut_requires_one_stl")
		return
	}
	if (req.Arrange || req.WipeTower) && (!req.Plate || req.Output != "gcode") {
		fail(w, 400, "arrange_prime_tower_estimate")
		return
	}
	if req.SupportFins && (req.Plate || len(req.Files) != 1 || !strings.EqualFold(filepath.Ext(req.Files[0]), ".stl")) {
		fail(w, 400, "http_invalid", "Support Fins requires one baked STL without plate JSON")
		return
	}
	if req.Orient && (req.Output == "3mf" || req.Plate || len(req.Files) != 1 || !strings.EqualFold(filepath.Ext(req.Files[0]), ".stl")) {
		fail(w, 400, "orient_requires_one_stl")
		return
	}
	if !req.SupportFins && !req.Orient && !req.Arrange && !req.WipeTower && !req.Explode && req.Cut == "" && req.MeshOp == "" && req.Output != "3mf" && !overlayOutput(req.Output) {
		if err := validatePlateBounds(filepath.Join(j.Dir, "input"), req, presets.machine, rawPlate); err != nil {
			var outside outsideBedError
			if errors.As(err, &outside) {
				fail(w, 400, "outside_build_plate", outside.name, outside.extent)
			} else {
				fail(w, 400, "http_invalid", err.Error())
			}
			return
		}
	}
	s.startJob(w, j, req, &started)
}

var calibKinds = map[string]bool{"cornering": true, "flow-rate": true, "input-shaping-freq": true, "input-shaping-damp": true,
	"max-volumetric-speed": true, "pa-line": true, "pa-pattern": true, "pa-tower": true, "retraction": true, "temp-tower": true, "vfa": true,
	"nonplanar-clearance": true, "nonplanar-tilt-flow": true, "nonplanar-curved-top": true, "nonplanar-verifier": true}
var calibHead = regexp.MustCompile(`^[a-z0-9_-]{0,64}$`)
var calibParams = regexp.MustCompile(`^([a-z_]+=[A-Za-z0-9_.;-]*(,[a-z_]+=[A-Za-z0-9_.;-]*)*)?$`)

func (s *Server) startJob(w http.ResponseWriter, j *Job, req JobRequest, started *bool) {
	presets, problem := s.resolveJobProfiles(&req)
	if problem != nil {
		problem.write(w)
		return
	}
	if problem := s.validateJobMethods(j, req, presets); problem != nil {
		writeJSON(w, 409, problem)
		return
	}
	if err := s.captureSubmission(j, &req, presets); err != nil {
		fail(w, 500, "couldnt_save_print_settings")
		return
	}
	j.Request = req
	if err := s.jobs.Start(j); err != nil {
		admissionFailure(w, err)
		return
	}
	*started = true
	w.Header().Set("Location", "/api/jobs/"+j.ID)
	writeJSON(w, 202, map[string]string{"id": j.ID})
}

func (s *Server) job(w http.ResponseWriter, r *http.Request) *Job {
	j := s.jobs.Get(r.PathValue("id"))
	if j == nil {
		fail(w, 404, "couldnt_find_slice_job")
	}
	return j
}
func (s *Server) getJob(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	v, echo := j.View(), r.URL.Query().Get("request") != "0"
	if echo {
		writeJSON(w, 200, v)
		return
	}
	var record map[string]json.RawMessage
	if json.Unmarshal(v.Slice, &record) == nil && record["submission"] != nil {
		delete(record, "submission")
		v.Slice, _ = json.Marshal(record)
	}
	writeJSON(w, 200, struct {
		JobView
		Request *struct{} `json:"request,omitempty"`
	}{JobView: v})
}

func (s *Server) surfaceGrid(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	var record map[string]json.RawMessage
	if !j.Previewable() || json.Unmarshal(j.Record(), &record) != nil || record["surface_grid"] == nil {
		fail(w, 404, "surface_preview_grid_isnt")
		return
	}
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	writeJSON(w, 200, record["surface_grid"])
}
func (s *Server) deleteJob(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	if s.jobs.Cancel(j) || !j.State().Finished() {
		writeJSON(w, 202, map[string]string{"id": j.ID, "state": "cancelling"})
		return
	}
	if err := s.jobs.Remove(j); err != nil {
		fail(w, 500, "couldnt_remove_job", err.Error())
		return
	}
	w.WriteHeader(204)
}
func (s *Server) download(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	if j.AwaitFinished(r.Context()) != StateDone {
		fail(w, 409, "job_doesnt_have_finished")
		return
	}
	if refusal := j.curveRefusal(); refusal != "" && previewableRequest(j.Request) {
		writeJSON(w, 409, map[string]string{"error": refusal, "code": "could_not_curve"})
		return
	}
	path, name := j.OutputPath(j.Request.Output), j.DownloadName()
	if j.Request.MeshOp != "" {
		path, name = j.OutputPath(meshExt(j.Request)), outputBase(j.Request)+"."+meshExt(j.Request)
	}
	if j.Request.Cut != "" {
		side := r.URL.Query().Get("piece")
		if !validPiece(side) {
			fail(w, 400, "cut_jobs_download_needs")
			return
		}
		path, name = j.PiecePath(side), outputBase(j.Request)+"."+side+".stl"
	}
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			j.setState(StateStale, "completed output is unavailable; slice again")
			writeJSON(w, 410, map[string]string{"error": "Oops! That sliced file has expired. Please slice the plate again.", "code": "job_output_expired", "recovery": "slice_again"})
		} else {
			fail(w, 503, "couldnt_open_exported_file")
		}
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		fail(w, 500, "http_internal", err.Error())
		return
	}
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": name}))
	w.Header().Set("Content-Type", "application/octet-stream")
	if strings.HasSuffix(path, ".3mf") || r.Header.Get("Range") != "" {
		http.ServeContent(w, r, st.Name(), st.ModTime(), f)
		return
	}
	serveGzipped(w, r, path, st, ".gz", `W/"%x-%x"`, func(src io.Reader, dst io.Writer, _ func() error) error {
		_, err := io.Copy(dst, src)
		return err
	})
}

var explodedName = regexp.MustCompile(`^object-[0-9]+-volume-[0-9]+\.3mf$`)

func (s *Server) exploded(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	name := r.PathValue("name")
	if !j.Request.Explode || j.State() != StateDone || !explodedName.MatchString(name) {
		fail(w, 404, "couldnt_find_extracted_model")
		return
	}
	f, err := os.Open(filepath.Join(j.ExplodedDir(), name))
	if err != nil {
		fail(w, 404, "couldnt_find_extracted_model")
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		fail(w, 500, "http_internal", err.Error())
		return
	}
	w.Header().Set("Content-Type", "model/3mf")
	serveStream(w, r, f, st, "private, max-age=300, must-revalidate")
}

func (s *Server) moves(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	path := j.MovesPath()
	if r.URL.Query().Get("extended") == "1" {
		if _, err := os.Stat(j.ExtendedMovesPath()); err == nil {
			path = j.ExtendedMovesPath()
		}
	}
	f, err := os.Open(path)
	if err != nil || !j.Previewable() {
		if f != nil {
			f.Close()
		}
		fail(w, 404, "toolpath_preview_data_isnt")
		return
	}
	st, err := f.Stat()
	f.Close()
	if err != nil {
		fail(w, 500, "http_internal", err.Error())
		return
	}
	serveMoves(w, r, path, st)
}

func (s *Server) events(w http.ResponseWriter, r *http.Request) {
	j := s.job(w, r)
	if j == nil {
		return
	}
	from := 0
	if last := r.Header.Get("Last-Event-ID"); last != "" {
		n, err := strconv.Atoi(last)
		if err != nil || n < 0 {
			fail(w, 400, "invalid_last_event_id")
			return
		}
		j.mu.Lock()
		latest := j.nextEvent
		j.mu.Unlock()
		if n > latest {
			fail(w, 400, "last_event_id_ahead")
			return
		}
		from = n
	}
	controller := http.NewResponseController(w)
	if _, ok := w.(http.Flusher); !ok {
		fail(w, 500, "live_progress_streaming_isnt")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")
	defer controller.SetWriteDeadline(time.Time{})
	heartbeat := time.NewTicker(15 * time.Second)
	defer heartbeat.Stop()
	for {
		if err := controller.SetWriteDeadline(time.Now().Add(15 * time.Second)); err != nil {
			return
		}
		events, changed, done := j.EventsSince(from)
		for _, e := range events {
			b, _ := json.Marshal(e)
			if _, err := fmt.Fprintf(w, "id: %d\nevent: %s\ndata: %s\n\n", e.ID, e.Type, b); err != nil {
				return
			}
			from = e.ID
		}
		if err := controller.Flush(); err != nil {
			return
		}
		if done {
			return
		}
		select {
		case <-r.Context().Done():
			return
		case <-changed:
		case <-heartbeat.C:
			if err := controller.SetWriteDeadline(time.Now().Add(15 * time.Second)); err != nil {
				return
			}
			if _, err := io.WriteString(w, ": keepalive\n\n"); err != nil {
				return
			}
			if err := controller.Flush(); err != nil {
				return
			}
		}
	}
}

func spaFiles(dir, resources string, boot func(*http.Request) []byte) http.Handler {
	cache := &staticCache{files: map[string]staticEntry{}}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			w.WriteHeader(405)
			return
		}
		if build := cache.build(dir); build != "" {
			w.Header().Set("X-Build", build)
		}
		clean := path.Clean("/" + r.URL.Path)
		if clean == "/metrics" || clean == "/api" || strings.HasPrefix(clean, "/api/") {
			http.NotFound(w, r)
			return
		}
		relative := filepath.FromSlash(strings.TrimPrefix(clean, "/"))
		if strings.Contains(r.URL.Path, "\\") || (relative != "" && !filepath.IsLocal(relative)) {
			http.NotFound(w, r)
			return
		}
		file := filepath.Join(dir, relative)
		st, err := os.Stat(file)
		if err == nil && st.IsDir() {
			file = filepath.Join(file, "index.html")
			st, err = os.Stat(file)
		}
		if err != nil && resources != "" && strings.HasPrefix(clean, "/profiles/") {
			if ext := strings.ToLower(path.Ext(clean)); ext == ".png" || ext == ".stl" || ext == ".svg" {
				file = filepath.Join(resources, relative)
				st, err = os.Stat(file)
			}
		}
		if err != nil {
			if strings.HasPrefix(clean, "/images/") || strings.HasPrefix(clean, "/profiles/") || filepath.Ext(clean) != "" {
				http.NotFound(w, r)
				return
			}
			file = filepath.Join(dir, "index.html")
			st, err = os.Stat(file)
		}
		if err != nil || !st.Mode().IsRegular() {
			http.NotFound(w, r)
			return
		}
		cache.serve(w, r, file, st, boot)
	})
}

func cutJoinError(req JobRequest) string {
	if req.CutConnectors == "" && req.CutDovetail == "" {
		return ""
	}
	var connectors []map[string]any
	var dovetail map[string]any
	switch {
	case req.CutConnectors != "" && req.CutDovetail != "":
		return "a dovetail cut takes no separate connectors"
	case req.CutKeep != "both":
		return "connectors and dovetails need cut_keep both"
	case req.CutConnectors != "" && (json.Unmarshal([]byte(req.CutConnectors), &connectors) != nil || len(connectors) == 0 || len(connectors) > 256):
		return "cut_connectors must be a JSON array of 1 to 256 connectors"
	case req.CutDovetail != "" && json.Unmarshal([]byte(req.CutDovetail), &dovetail) != nil:
		return "cut_dovetail must be a JSON object"
	}
	return ""
}

func validCutPlane(plane string) bool {
	fields := strings.Split(plane, ",")
	if len(fields) != 1 && len(fields) != 6 {
		return false
	}
	v := make([]float64, len(fields))
	for i, f := range fields {
		x, err := strconv.ParseFloat(strings.TrimSpace(f), 64)
		if err != nil || math.IsNaN(x) || math.IsInf(x, 0) {
			return false
		}
		v[i] = x
	}
	return len(v) == 1 || math.Hypot(math.Hypot(v[3], v[4]), v[5]) > 1e-9
}

var handyModels = map[string]bool{"OrcaCube_v2.drc": true, "OrcaPlug_v2.drc": true, "OrcaSliced.3mf": true, "OrcaBadge.3mf": true,
	"OrcaToleranceTest.drc": true, "3DBenchy.drc": true, "calicat.drc": true, "ksr_fdmtest_v4.drc": true, "Voron_Design_Cube_v7.drc": true,
	"Stanford_Bunny.drc": true, "Orca_stringhell.drc": true}

func allSTL(files []string) bool {
	for _, f := range files {
		if !strings.EqualFold(filepath.Ext(f), ".stl") {
			return false
		}
	}
	return true
}

var fontID = regexp.MustCompile(`^[A-Za-z0-9_ .-]{1,80}$`)

func textArgs(r *http.Request) ([]string, error) {
	text := r.FormValue("text")
	if strings.TrimSpace(text) == "" || len(text) > 4000 {
		return nil, errors.New("text must be 1 to 4000 bytes")
	}
	args := []string{"--text", text}
	if font := r.FormValue("font"); font != "" {
		if !fontID.MatchString(font) {
			return nil, errors.New("font must be a bundled font id")
		}
		args = append(args, "--font-name", font)
	}
	number := func(key string, lo, hi, def float64) (string, error) {
		raw := r.FormValue(key)
		if raw == "" {
			return strconv.FormatFloat(def, 'f', -1, 64), nil
		}
		v, err := strconv.ParseFloat(raw, 64)
		if err != nil || math.IsNaN(v) || v < lo || v > hi {
			return "", fmt.Errorf("%s must be %g to %g", key, lo, hi)
		}
		return strconv.FormatFloat(v, 'f', -1, 64), nil
	}
	for _, f := range []struct {
		key, flag   string
		lo, hi, def float64
	}{{"size", "--size", 0.1, 1000, 10}, {"depth", "--depth", 0.01, 1000, 1}, {"line_spacing", "--line-spacing", 0.1, 10, 1}, {"char_gap", "--char-gap", -100, 100, 0}} {
		v, err := number(f.key, f.lo, f.hi, f.def)
		if err != nil {
			return nil, err
		}
		args = append(args, f.flag, v)
	}
	for _, flag := range []string{"bold", "italic"} {
		if r.FormValue(flag) == "1" {
			args = append(args, "--"+flag)
		}
	}
	switch align := r.FormValue("align"); align {
	case "", "center", "left", "right":
		if align != "" {
			args = append(args, "--align", align)
		}
	default:
		return nil, errors.New("align must be left, center or right")
	}
	return args, nil
}

var vector3 = regexp.MustCompile(`^-?[0-9.]+(e-?[0-9]+)?,-?[0-9.]+(e-?[0-9]+)?,-?[0-9.]+(e-?[0-9]+)?$`)

func surfaceArgs(r *http.Request) ([]string, error) {
	args := []string{}
	for _, k := range []string{"at", "normal", "up"} {
		v := r.FormValue(k)
		if v == "" && k == "up" {
			continue
		}
		if !vector3.MatchString(v) {
			return nil, fmt.Errorf("%s must be x,y,z", k)
		}
		args = append(args, "--"+k, v)
	}
	if v := r.FormValue("angle"); v != "" {
		a, err := strconv.ParseFloat(v, 64)
		if err != nil || math.IsNaN(a) || a < -360 || a > 360 {
			return nil, errors.New("angle must be -360 to 360")
		}
		args = append(args, "--angle", strconv.FormatFloat(a, 'f', -1, 64))
	}
	if r.FormValue("outside") == "1" {
		args = append(args, "--surface-outside")
	}
	return args, nil
}

func (s *Server) embossFonts(w http.ResponseWriter, r *http.Request) {
	s.fontsOnce.Do(func() {
		ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
		defer cancel()
		cmd := exec.CommandContext(ctx, s.cfg.Tool, "--emboss-fonts", "--profiles", filepath.Join(s.cfg.Resources, "profiles"))
		cmd.Env = engineEnv(os.TempDir())
		configureKill(cmd)
		cmd.WaitDelay = 5 * time.Second
		out, err := cmd.Output()
		var catalog struct {
			Fonts []struct {
				ID   string `json:"id"`
				Name string `json:"name"`
			} `json:"fonts"`
		}
		if err == nil {
			err = json.Unmarshal(out, &catalog)
		}
		if err != nil {
			s.fontsErr = fmt.Errorf("font catalog: %w", err)
			return
		}
		s.fonts, _ = json.Marshal(map[string]any{"fonts": catalog.Fonts})
	})
	if s.fonts == nil {
		fail(w, 500, "http_internal", fmt.Sprint(s.fontsErr))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "public, max-age=3600")
	w.Write(s.fonts)
}

func (s *Server) handyModel(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !handyModels[name] {
		fail(w, 404, "unrecognized_built_sample_model")
		return
	}
	f, err := os.Open(filepath.Join(s.cfg.Resources, "handy_models", name))
	if err != nil {
		fail(w, 404, "built_sample_model_isnt")
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		fail(w, 500, "http_internal", err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "public, max-age=86400")
	http.ServeContent(w, r, name, st.ModTime(), f)
}

func (s *Server) jobTransitions(w http.ResponseWriter, r *http.Request) {
	if j := s.job(w, r); j != nil {
		writeJSON(w, 200, j.Transitions())
	}
}

func (s *Server) cancelJob(w http.ResponseWriter, r *http.Request) {
	if j := s.job(w, r); j != nil {
		s.jobs.Cancel(j)
		writeJSON(w, 200, j.View())
	}
}

func (s *Server) retryJob(w http.ResponseWriter, r *http.Request) {
	key, err := submissionKey(r.URL.Path, r.Header.Get("Idempotency-Key"))
	if err != nil {
		fail(w, 400, "http_invalid", err.Error())
		return
	}
	existing, finish, err := s.jobs.reserve(r.Context(), key)
	if err != nil {
		fail(w, 408, "http_timeout", err.Error())
		return
	}
	if existing != nil {
		w.Header().Set("Location", "/api/jobs/"+existing.ID)
		writeJSON(w, 202, existing.View())
		return
	}
	var accepted *Job
	defer func() { finish(accepted) }()
	parent := s.job(w, r)
	if parent == nil {
		return
	}
	j, err := s.jobs.Retry(parent, s.clientIP(r))
	if err != nil {
		admissionFailure(w, err)
		return
	}
	j.IdempotencyKey = key
	req := j.Request
	presets, problem := s.resolveJobProfiles(&req)
	if problem != nil {
		s.jobs.releaseUpload(j)
		os.RemoveAll(j.Dir)
		problem.write(w)
		return
	}
	if problem := s.validateJobMethods(j, req, presets); problem != nil {
		s.jobs.releaseUpload(j)
		os.RemoveAll(j.Dir)
		writeJSON(w, 409, problem)
		return
	}
	if err := s.captureSubmission(j, &req, presets); err != nil {
		s.jobs.releaseUpload(j)
		os.RemoveAll(j.Dir)
		fail(w, 500, "couldnt_save_retry_settings")
		return
	}
	j.Request = req
	if err := s.jobs.Start(j); err != nil {
		s.jobs.releaseUpload(j)
		os.RemoveAll(j.Dir)
		admissionFailure(w, err)
		return
	}
	accepted = j
	w.Header().Set("Location", "/api/jobs/"+j.ID)
	writeJSON(w, 202, j.View())
}

type HealthState string

const (
	HealthUnknown     HealthState = "unknown"
	HealthHealthy     HealthState = "healthy"
	HealthDegraded    HealthState = "degraded"
	HealthQualified   HealthState = "qualified"
	HealthUnqualified HealthState = "unqualified"
)

var engineTransitions = map[HealthState]map[HealthState]bool{
	HealthUnknown:  {HealthHealthy: true, HealthDegraded: true},
	HealthHealthy:  {HealthDegraded: true},
	HealthDegraded: {},
}
var artifactTransitions = map[HealthState]map[HealthState]bool{
	HealthUnknown:     {HealthUnqualified: true, HealthQualified: true},
	HealthUnqualified: {HealthQualified: true},
	HealthQualified:   {HealthUnqualified: true},
}

type engineHealth struct {
	mu            sync.Mutex
	state         HealthState
	artifactState HealthState
	reason        string
	manifest      map[string]any
	tool          os.FileInfo
	outputs       map[string]bool
}

func moveHealth(table map[HealthState]map[HealthState]bool, field *HealthState, next HealthState, what string) bool {
	if *field == next {
		return true
	}
	if !table[*field][next] {
		log.Printf("illegal %s transition %s -> %s", what, *field, next)
		return false
	}
	log.Printf("%s %s -> %s", what, *field, next)
	*field = next
	return true
}

func (h *engineHealth) setEngine(next HealthState) {
	if moveHealth(engineTransitions, &h.state, next, "engine health") && next == HealthDegraded {
		h.setArtifact(HealthUnqualified)
		h.outputs = nil
	}
}

func (h *engineHealth) setArtifact(next HealthState) {
	moveHealth(artifactTransitions, &h.artifactState, next, "artifact qualification")
}

func (h *engineHealth) wireState() HealthState {
	if h.state != HealthDegraded && h.artifactState != HealthUnknown {
		return h.artifactState
	}
	return h.state
}

func (h *engineHealth) qualifiedOutput(hash string) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.outputs[hash]
}

func bundleFileHash(name string) (string, error) {
	f, err := os.Open(name)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err = io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func bundleResourceHash(root string) (string, error) {
	h := sha256.New()
	err := filepath.WalkDir(root, func(name string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		if !d.Type().IsRegular() {
			return errors.New("nonregular resource")
		}
		digest, err := bundleFileHash(name)
		if err != nil {
			return err
		}
		relative, err := filepath.Rel(root, name)
		if err != nil {
			return err
		}
		fmt.Fprintf(h, "%s\x00%s\n", filepath.ToSlash(relative), digest)
		return nil
	})
	if err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func newEngineHealth(c Config) *engineHealth {
	h := &engineHealth{state: HealthUnknown, artifactState: HealthUnknown}
	st, err := os.Stat(c.Tool)
	if err != nil || !st.Mode().IsRegular() {
		h.reason = "engine executable unavailable"
		h.setEngine(HealthDegraded)
		return h
	}
	h.tool = st
	h.setEngine(HealthHealthy)
	raw, binary, resources := c.Engine.Manifest, c.Engine.Binary, c.Engine.Resources
	if raw == nil {
		return h
	}
	h.setArtifact(HealthUnqualified)
	var manifest struct {
		PublicationMain string            `json:"publication_main_sha"`
		Platform        *artifactPlatform `json:"platform"`
		Backend         string            `json:"backend"`
		Source          string            `json:"source_sha"`
		Available       bool              `json:"available"`
		Preview         bool              `json:"preview_ready"`
		Curved          bool              `json:"validated_curved_benchy_clearance"`
		Wave            bool              `json:"validated_wave_export"`
		Features        map[string]bool   `json:"features"`
	}
	if json.Unmarshal(raw, &manifest) != nil || !manifest.Available {
		h.reason = "artifact manifest says the engine is unavailable"
		h.setEngine(HealthDegraded)
		return h
	}
	qualified := c.Engine.Qualified
	if qualified {
		h.outputs = c.Engine.Outputs
		h.setArtifact(HealthQualified)
	} else {
		h.reason = c.Engine.Unqualified
		log.Printf("engine not qualified: %s", h.reason)
	}
	wave := qualified && manifest.Preview && manifest.Wave && manifest.Features["nonplanar_wave"]
	modes := []string{}
	if wave {
		modes = append(modes, "whole")
	}
	h.manifest = map[string]any{"backend": manifest.Backend, "source_sha": manifest.Source, "publication_main_sha": manifest.PublicationMain, "platform": manifest.Platform,
		"validated": manifest.Preview, "preview_ready": manifest.Preview, "print_qualified": qualified,
		"binary_sha256": binary, "resources_sha256": resources, "wave_modes": modes,
		"features":            map[string]bool{"curved_tops": manifest.Preview && manifest.Curved, "nonplanar_wave": wave},
		"qualification_scope": "exact audited reference outputs"}
	return h
}

type artifactPlatform struct {
	OS   string `json:"os"`
	Arch string `json:"arch"`
}

func runningPlatform() artifactPlatform {
	arch := runtime.GOARCH
	if arch == "amd64" {
		arch = "x86_64"
	}
	return artifactPlatform{OS: runtime.GOOS, Arch: arch}
}

func (h *engineHealth) snapshot(tool string) map[string]any {
	h.mu.Lock()
	defer h.mu.Unlock()
	st, err := os.Stat(tool)
	if h.state != HealthDegraded && (err != nil || h.tool == nil || !os.SameFile(st, h.tool) || st.Size() != h.tool.Size() || !st.ModTime().Equal(h.tool.ModTime())) {
		h.reason = "engine executable changed since the service started; restart the service"
		h.manifest = nil
		h.setEngine(HealthDegraded)
	}
	return map[string]any{"engine_state": h.wireState(), "artifact_state": h.artifactState, "reason": h.reason,
		"artifact_manifest": h.manifest}
}
