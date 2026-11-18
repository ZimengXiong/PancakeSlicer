//go:build js && wasm

package main

import (
	"encoding/json"
	"errors"
	"strings"
	"syscall/js"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

var catalog *resolve.Catalog

type tables struct {
	Rules, VariantKeys, Feeds, Options json.RawMessage
	Schemas                            struct {
		Library int `json:"library_schema"`
	}
}

func configure(raw, printers, filaments, prints string) error {
	var t tables
	if err := json.Unmarshal([]byte(raw), &t); err != nil {
		return err
	}
	if err := resolve.Configure(resolve.Data{Rules: t.Rules, VariantKeys: t.VariantKeys, Options: t.Options, Feeds: t.Feeds, LibrarySchema: t.Schemas.Library}); err != nil {
		return err
	}
	var err error
	catalog, err = resolve.LoadCatalog(strings.NewReader(printers), strings.NewReader(filaments), strings.NewReader(prints))
	return err
}

func library(document string) (*resolve.Library, error) {
	if catalog == nil {
		return nil, errors.New("the demo resolver is not configured")
	}
	var doc map[string]json.RawMessage
	if err := json.Unmarshal([]byte(document), &doc); err != nil {
		return nil, err
	}
	return catalog.ReadLibrary(doc)
}

func answer(document, query string) (any, error) {
	lib, err := library(document)
	if err != nil {
		return nil, err
	}
	var q resolve.Query
	if err := json.Unmarshal([]byte(query), &q); err != nil {
		return nil, err
	}
	return catalog.Answer(lib, q), nil
}

func slotOffer(document, request string) (any, error) {
	lib, err := library(document)
	if err != nil {
		return nil, err
	}
	var in resolve.SlotOfferRequest
	if err := json.Unmarshal([]byte(request), &in); err != nil {
		return nil, err
	}
	return catalog.SlotOfferFor(lib, in), nil
}

func reply(v any, err error) any {
	if err != nil {
		b, _ := json.Marshal(map[string]string{"error": err.Error()})
		return string(b)
	}
	b, _ := json.Marshal(map[string]any{"answer": v})
	return string(b)
}

func main() {
	js.Global().Set("pancakeResolver", js.ValueOf(map[string]any{
		"configure": js.FuncOf(func(_ js.Value, a []js.Value) any {
			if err := configure(a[0].String(), a[1].String(), a[2].String(), a[3].String()); err != nil {
				return err.Error()
			}
			return ""
		}),
		"answer":    js.FuncOf(func(_ js.Value, a []js.Value) any { return reply(answer(a[0].String(), a[1].String())) }),
		"slotOffer": js.FuncOf(func(_ js.Value, a []js.Value) any { return reply(slotOffer(a[0].String(), a[1].String())) }),
	}))
	select {}
}
