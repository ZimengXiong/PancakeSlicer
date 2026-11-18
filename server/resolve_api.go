package main

import (
	_ "embed"
	"net/http"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

var (
	//go:embed profile_rules.json
	profileRulesData []byte
	//go:embed filament_variant_keys.json
	filamentVariantKeysData []byte
)

func configureResolve(options []byte) error {
	return resolve.Configure(resolve.Data{Rules: profileRulesData, VariantKeys: filamentVariantKeysData, Options: options, Feeds: printerFeedsJSON, LibrarySchema: librarySchema})
}

func (s *Server) postResolve(w http.ResponseWriter, r *http.Request) {
	var in resolve.Query
	if !workspaceDecode(w, r, &in) {
		return
	}
	lib, err := s.libraryDocument()
	if err != nil {
		libraryFailure(w, 503, "library_unavailable", "Oops! Couldn't read profiles.", "retry_storage", 0)
		return
	}
	writeJSON(w, 200, s.catalog.Answer(lib, in))
}
