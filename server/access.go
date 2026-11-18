package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/url"
	"strings"
)

const accessCookie = "orca_access"

func (s *Server) accessProof() string {
	mac := hmac.New(sha256.New, []byte(s.cfg.BearerToken))
	mac.Write([]byte("orca-web-access-v1"))
	return hex.EncodeToString(mac.Sum(nil))
}

func (s *Server) authorized(r *http.Request) bool {
	if auth := r.Header.Get("Authorization"); strings.HasPrefix(auth, "Bearer ") {
		return subtle.ConstantTimeCompare([]byte(strings.TrimPrefix(auth, "Bearer ")), []byte(s.cfg.BearerToken)) == 1
	}
	c, err := r.Cookie(accessCookie)
	if err != nil || subtle.ConstantTimeCompare([]byte(c.Value), []byte(s.accessProof())) != 1 {
		return false
	}
	if site := r.Header.Get("Sec-Fetch-Site"); site != "" && site != "same-origin" {
		return false
	}
	if origin := r.Header.Get("Origin"); origin != "" {
		u, err := url.Parse(origin)
		return err == nil && strings.EqualFold(u.Host, r.Host)
	}
	return true
}

func setAccessCookie(w http.ResponseWriter, r *http.Request, value string, maxAge int) {
	http.SetCookie(w, &http.Cookie{Name: accessCookie, Value: value, Path: "/", MaxAge: maxAge, HttpOnly: true,
		SameSite: http.SameSiteStrictMode, Secure: r.TLS != nil || strings.EqualFold(r.Header.Get("X-Forwarded-Proto"), "https")})
}

func (s *Server) saveAccess(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Token string `json:"token"`
	}
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&in) != nil {
		fail(w, 400, "access_token_body")
		return
	}
	if s.cfg.BearerToken == "" {
		w.WriteHeader(204)
		return
	}
	if subtle.ConstantTimeCompare([]byte(strings.TrimSpace(in.Token)), []byte(s.cfg.BearerToken)) != 1 {
		fail(w, 401, "access_token_wrong")
		return
	}
	setAccessCookie(w, r, s.accessProof(), 400*24*3600)
	w.WriteHeader(204)
}

func (s *Server) forgetAccess(w http.ResponseWriter, r *http.Request) {
	setAccessCookie(w, r, "", -1)
	w.WriteHeader(204)
}
