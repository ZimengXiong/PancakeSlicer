package main

import (
	"cmp"
	"net/http"
	"slices"
	"strings"
)

type libraryReceipt struct {
	Digest   string `json:"digest"`
	Revision uint64 `json:"revision"`
	Seq      uint64 `json:"seq,omitempty"`
}

const maxReceipts = 256

func putReceipt[R any](ops map[string]R, op string, receipt R, seq func(R) uint64, withSeq func(R, uint64) R) {
	next := uint64(0)
	for _, r := range ops {
		next = max(next, seq(r))
	}
	ops[op] = withSeq(receipt, next+1)
	if len(ops) <= maxReceipts {
		return
	}
	keys := make([]string, 0, len(ops))
	for k := range ops {
		keys = append(keys, k)
	}
	slices.SortFunc(keys, func(a, b string) int { return cmp.Or(cmp.Compare(seq(ops[a]), seq(ops[b])), strings.Compare(a, b)) })
	for _, k := range keys[:len(ops)-maxReceipts] {
		delete(ops, k)
	}
}

type libraryError struct {
	Message  string `json:"error"`
	Code     string `json:"code"`
	Recovery string `json:"recovery"`
	Revision uint64 `json:"revision,omitempty"`
}

func libraryFailure(w http.ResponseWriter, status int, code, message, recovery string, revision uint64) {
	writeJSON(w, status, libraryError{message, code, recovery, revision})
}
