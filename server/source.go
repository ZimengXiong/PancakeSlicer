package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
)

var buildSourceSHA256 string

var errSourceUnconfigured = errors.New("Source code archive isn't configured on this server.")

func sourceSHA256() string {
	if buildSourceSHA256 != "" {
		return buildSourceSHA256
	}
	return env("ORCA_BUILD_SOURCE_SHA256", "")
}

func openSourceArchive(c Config) (*os.File, error) {
	if c.SourceArchive == "" {
		return nil, errSourceUnconfigured
	}
	expected := sourceSHA256()
	if digest, err := hex.DecodeString(expected); err != nil || len(digest) != sha256.Size {
		return nil, errors.New("This build doesn't have a valid source code archive checksum.")
	}
	file, err := os.Open(c.SourceArchive)
	if err != nil {
		return nil, errors.New("Source code archive isn't available on this server.")
	}
	fail := func(err error) (*os.File, error) { file.Close(); return nil, err }
	stat, err := file.Stat()
	if err != nil || !stat.Mode().IsRegular() {
		return fail(errors.New("source archive is not a regular file"))
	}
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		return fail(errors.New("Oops! Couldn't read the source code archive."))
	}
	if hex.EncodeToString(hash.Sum(nil)) != expected {
		return fail(errors.New("The source code archive on disk doesn't match this build."))
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return fail(err)
	}
	return file, nil
}

func (s *Server) source(w http.ResponseWriter, r *http.Request) {
	file, err := openSourceArchive(s.cfg)
	if err != nil {
		status := http.StatusServiceUnavailable
		if errors.Is(err, errSourceUnconfigured) {
			status = http.StatusNotFound
		}
		fail(w, status, "source_failed", err.Error())
		return
	}
	defer file.Close()
	stat, err := file.Stat()
	if err != nil {
		fail(w, http.StatusServiceUnavailable, "source_code_archive_isnt")
		return
	}
	w.Header().Set("Content-Type", "application/gzip")
	_, version := buildVersion()
	name := "pancakeslicer-source-" + version + ".tar.gz"
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("ETag", fmt.Sprintf(`"sha256:%s"`, sourceSHA256()))
	http.ServeContent(w, r, name, stat.ModTime(), file)
}
