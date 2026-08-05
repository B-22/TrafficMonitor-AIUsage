package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func newTestServer(t *testing.T) *server {
	t.Helper()
	s := newServer(config{
		token:          "test-token",
		cacheUsage:     45 * 1000000000,
		cacheCredits:   300 * 1000000000,
		cacheProfile:   3600 * 1000000000,
		claudeClientID: "9d1c250a-e61b-44d9-88ed-5944d1962f5e",
	})
	return s
}

func TestHealthRequiresAuth(t *testing.T) {
	s := newTestServer(t)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/health", nil)
	rec := httptest.NewRecorder()
	s.handleHealth(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rec.Code)
	}

	req = httptest.NewRequest(http.MethodGet, "/api/v1/health", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	rec = httptest.NewRecorder()
	s.handleHealth(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}
}

func TestCredentialsAuthAndValidation(t *testing.T) {
	s := newTestServer(t)

	// Missing auth -> 401.
	req := httptest.NewRequest(http.MethodPost, "/api/v1/credentials", nil)
	rec := httptest.NewRecorder()
	s.handleCredentials(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rec.Code)
	}

	// Valid auth + valid body -> 204, credentials stored.
	body := `{"codex":{"access_token":"at","refresh_token":"rt","account_id":"acc1"}}`
	req = httptest.NewRequest(http.MethodPost, "/api/v1/credentials", strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer test-token")
	rec = httptest.NewRecorder()
	s.handleCredentials(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("expected 204, got %d", rec.Code)
	}
	access, refresh, accountID := s.creds.codex()
	if access != "at" || refresh != "rt" || accountID != "acc1" {
		t.Fatalf("codex creds not stored: %q %q %q", access, refresh, accountID)
	}

	// Missing access_token -> 400.
	body = `{"codex":{"refresh_token":"rt"}}`
	req = httptest.NewRequest(http.MethodPost, "/api/v1/credentials", strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer test-token")
	rec = httptest.NewRecorder()
	s.handleCredentials(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", rec.Code)
	}

	// Invalid JSON -> 400.
	req = httptest.NewRequest(http.MethodPost, "/api/v1/credentials", strings.NewReader("{bad"))
	req.Header.Set("Authorization", "Bearer test-token")
	rec = httptest.NewRecorder()
	s.handleCredentials(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", rec.Code)
	}

	// Claude body with claude only.
	body = `{"claude":{"access_token":"cat"}}`
	req = httptest.NewRequest(http.MethodPost, "/api/v1/credentials", strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer test-token")
	rec = httptest.NewRecorder()
	s.handleCredentials(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("expected 204, got %d", rec.Code)
	}
	caccess, crefresh := s.creds.claude()
	if caccess != "cat" {
		t.Fatalf("claude creds not stored: %q", caccess)
	}
	if crefresh != "" {
		t.Fatalf("unexpected claude refresh token: %q", crefresh)
	}
}

func TestUpstreamRejectsUnsupportedMethod(t *testing.T) {
	s := newTestServer(t)
	ep := s.upstreamHandlers()
	req := httptest.NewRequest(http.MethodPost, "/api/v1/codex/usage", nil)
	req.Header.Set("Authorization", "Bearer test-token")
	rec := httptest.NewRecorder()
	ep(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("expected 405, got %d", rec.Code)
	}
}

func (s *server) upstreamHandlers() func(w http.ResponseWriter, r *http.Request) {
	ep := upstreamEndpoint{
		cacheKey: "codex:usage",
		provider: "codex",
		host:     "chatgpt.com",
		path:     "/backend-api/wham/usage",
		cacheTTL: s.cfg.cacheUsage,
		extraHeader: map[string]string{
			"OpenAI-Beta": "codex-1",
			"originator":  "Codex Desktop",
		},
	}
	return s.handleEndpoint(ep)
}

func TestCacheRoundTrip(t *testing.T) {
	s := newTestServer(t)
	key := "codex:usage"
	s.putCached(key, http.StatusOK, []byte(`{"ok":true}`), s.cfg.cacheUsage)
	c, ok := s.getCached(key)
	if !ok {
		t.Fatalf("expected cache hit")
	}
	if string(c.body) != `{"ok":true}` || c.statusCode != http.StatusOK {
		t.Fatalf("unexpected cached value")
	}
	s.invalidateCache()
	if _, ok := s.getCached(key); ok {
		t.Fatalf("expected cache miss after invalidate")
	}
}

func TestTokenRefreshInfoJSON(t *testing.T) {
	info := tokenRefreshInfo{
		Provider:    "claude",
		AccessToken: "at",
		ExpiresAt:   123,
	}
	data, err := json.Marshal(info)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var back tokenRefreshInfo
	if err := json.Unmarshal(data, &back); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if back.Provider != "claude" || back.AccessToken != "at" || back.ExpiresAt != 123 {
		t.Fatalf("roundtrip mismatch: %+v", back)
	}
}