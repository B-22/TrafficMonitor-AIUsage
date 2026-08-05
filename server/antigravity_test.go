package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestParseAvailableModelsFiltersAndSorts(t *testing.T) {
	body := `{"models":{
		"gemini-3-pro":            {"quotaInfo":{"remainingFraction":0.42,"resetTime":"2026-01-01T00:00:00Z"}},
		"claude-3-5-sonnet":       {"quotaInfo":{"remainingFraction":0,"resetTime":"2026-01-02T00:00:00Z"}},
		"gemini-2.5-flash":        {"quotaInfo":{"remainingFraction":0.9}},
		"gemini-pro":              {"quotaInfo":{"remainingFraction":0.9}},
		"text-embedding-004":      {"quotaInfo":{"remainingFraction":1}},
		"gpt-4o":                  {"quotaInfo":{"resetTime":"2026-01-03T00:00:00Z"}},
		"gemini-3-flash-noquota":  {}
	}}`

	got, err := parseAvailableModels(body)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}

	// Expected survivors, sorted by modelName:
	//   claude-3-5-sonnet, gemini-3-pro, gpt-4o
	// Dropped: gemini-2.5-flash + gemini-pro (version < 3.0),
	//          text-embedding-004 (name filter),
	//          gemini-3-flash-noquota (no quotaInfo).
	want := []string{"claude-3-5-sonnet", "gemini-3-pro", "gpt-4o"}
	if len(got) != len(want) {
		t.Fatalf("expected %d models, got %d: %+v", len(want), len(got), got)
	}
	for i, name := range want {
		if got[i].ModelName != name {
			t.Fatalf("index %d: expected %q, got %q (order must be stable)", i, name, got[i].ModelName)
		}
	}

	claude := got[0]
	if claude.DisplayName != "Claude 3.5 Sonnet" {
		t.Fatalf("display name: got %q", claude.DisplayName)
	}
	if !claude.IsExhausted || claude.Percent != 0 {
		t.Fatalf("zero fraction must be exhausted: %+v", claude)
	}

	gem := got[1]
	if gem.Percent != 42 {
		t.Fatalf("expected 42%%, got %d", gem.Percent)
	}
	if gem.IsExhausted {
		t.Fatalf("0.42 must not be exhausted")
	}

	// Missing remainingFraction -> 0 / exhausted, and a synthesised resetTime.
	gpt := got[2]
	if !gpt.IsExhausted || gpt.RemainingFraction != 0 {
		t.Fatalf("absent remainingFraction must mean exhausted: %+v", gpt)
	}
}

func TestParseAvailableModelsSynthesisesResetTime(t *testing.T) {
	got, err := parseAvailableModels(`{"models":{"gemini-3-pro":{"quotaInfo":{"remainingFraction":0.5}}}}`)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(got) != 1 {
		t.Fatalf("expected 1 model, got %d", len(got))
	}
	ts, err := time.Parse(time.RFC3339, got[0].ResetTime)
	if err != nil {
		t.Fatalf("resetTime not RFC3339: %q (%v)", got[0].ResetTime, err)
	}
	if d := time.Until(ts); d < 23*time.Hour || d > 25*time.Hour {
		t.Fatalf("synthesised resetTime should be ~24h out, got %v", d)
	}
}

func TestFriendlyModelName(t *testing.T) {
	cases := map[string]string{
		"claude-3-5-sonnet": "Claude 3.5 Sonnet",
		"gemini-3-pro":      "Gemini 3 Pro",
		"gpt-4o":            "Gpt 4o",
		"gemini-3-flash":    "Gemini 3 Flash",
	}
	for in, want := range cases {
		if got := friendlyModelName(in); got != want {
			t.Errorf("friendlyModelName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestModelVersionSupported(t *testing.T) {
	cases := map[string]bool{
		"gemini-3-pro":      true,
		"gemini-3.5-pro":    true,
		"gemini-2.5-flash":  false,
		"gemini-1.5-pro":    false,
		"gemini-pro":        false, // unversioned == 1.0
		"claude-3-5-sonnet": true,  // non-Gemini always allowed
		"gpt-4o":            true,
	}
	for in, want := range cases {
		if got := modelVersionSupported(in); got != want {
			t.Errorf("modelVersionSupported(%q) = %v, want %v", in, got, want)
		}
	}
}

func TestExtractAuthCode(t *testing.T) {
	cases := map[string]string{
		"http://localhost:8721/?code=4%2F0AbCd&scope=email": "4/0AbCd",
		"http://localhost:8721/?code=plain&state=x":         "plain",
		"4/0AbCdEf":     "4/0AbCdEf",
		"  4/0AbCdEf  ": "4/0AbCdEf",
		"http://localhost:8721/?error=access_denied": "",
		"": "",
	}
	for in, want := range cases {
		got := extractAuthCode(trimForTest(in))
		if got != want {
			t.Errorf("extractAuthCode(%q) = %q, want %q", in, got, want)
		}
	}
}

func trimForTest(s string) string {
	for len(s) > 0 && (s[0] == ' ' || s[0] == '\t') {
		s = s[1:]
	}
	for len(s) > 0 && (s[len(s)-1] == ' ' || s[len(s)-1] == '\t') {
		s = s[:len(s)-1]
	}
	return s
}

func TestAntigravityQuotaRequiresAuth(t *testing.T) {
	s := newTestServer(t)

	req := httptest.NewRequest(http.MethodGet, "/api/v1/antigravity/quota", nil)
	rec := httptest.NewRecorder()
	s.handleAntigravityQuota(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 without auth, got %d", rec.Code)
	}

	req = httptest.NewRequest(http.MethodPost, "/api/v1/antigravity/quota", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	rec = httptest.NewRecorder()
	s.handleAntigravityQuota(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("expected 405 for POST, got %d", rec.Code)
	}
}

func TestAntigravityQuotaUnauthorizedWhenNoToken(t *testing.T) {
	s := newTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/antigravity/quota", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	rec := httptest.NewRecorder()
	s.handleAntigravityQuota(rec, req)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("expected 502 when unauthorized upstream, got %d", rec.Code)
	}
	var out struct {
		Error string `json:"error"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &out); err != nil {
		t.Fatalf("error body must be JSON: %v (%s)", err, rec.Body.String())
	}
	if out.Error == "" {
		t.Fatalf("expected an error message, got %s", rec.Body.String())
	}
}

func TestAGTokenFileRoundTrip(t *testing.T) {
	dir := t.TempDir()
	s := newTestServer(t)
	s.cfg.antigravityTokenFile = filepath.Join(dir, "ag.json")

	if err := s.saveAGTokenFile("at", "rt", 1234); err != nil {
		t.Fatalf("save: %v", err)
	}
	rec, err := s.loadAGTokenFile()
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if rec.AccessToken != "at" || rec.RefreshToken != "rt" || rec.ExpiresAt != 1234 {
		t.Fatalf("roundtrip mismatch: %+v", rec)
	}

	// bootstrap should pick the stored refresh token up without any network.
	s2 := newTestServer(t)
	s2.cfg.antigravityTokenFile = s.cfg.antigravityTokenFile
	s2.bootstrapAntigravity()
	_, refresh, _ := s2.antigravity()
	if refresh != "rt" {
		t.Fatalf("bootstrap did not load refresh token, got %q", refresh)
	}

	// A missing file must not panic or block.
	s3 := newTestServer(t)
	s3.cfg.antigravityTokenFile = filepath.Join(dir, "does-not-exist.json")
	s3.bootstrapAntigravity()
	if _, refresh, _ := s3.antigravity(); refresh != "" {
		t.Fatalf("expected no credentials, got %q", refresh)
	}
}

func TestValidAccessTokenUsesUnexpiredCache(t *testing.T) {
	s := newTestServer(t)
	s.setAntigravity("cached-at", "rt", time.Now().Add(10*time.Minute).Unix())
	got, err := s.validAccessToken()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != "cached-at" {
		t.Fatalf("expected cached token, got %q", got)
	}
}

func TestValidAccessTokenErrorsWithoutRefresh(t *testing.T) {
	s := newTestServer(t)
	if _, err := s.validAccessToken(); err == nil {
		t.Fatalf("expected error when not authorized")
	}
}

func TestSaveAGTokenFileNoPathIsNoop(t *testing.T) {
	s := newTestServer(t)
	s.cfg.antigravityTokenFile = ""
	if err := s.saveAGTokenFile("a", "b", 1); err != nil {
		t.Fatalf("expected no-op, got %v", err)
	}
}

func TestAGTokenFilePermissions(t *testing.T) {
	dir := t.TempDir()
	s := newTestServer(t)
	s.cfg.antigravityTokenFile = filepath.Join(dir, "ag.json")
	if err := s.saveAGTokenFile("at", "rt", 1); err != nil {
		t.Fatalf("save: %v", err)
	}
	st, err := os.Stat(s.cfg.antigravityTokenFile)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	if st.Size() == 0 {
		t.Fatalf("token file is empty")
	}
}
