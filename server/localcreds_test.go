package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// ---- helpers ----

func testLocalCreds(claudePath, codexPath, tokenURL string) *localCreds {
	return &localCreds{
		http:            &http.Client{},
		claude:          &localCredFile{path: claudePath},
		codex:           &localCredFile{path: codexPath},
		claudeTokenURLs: []string{tokenURL},
		codexTokenURL:   tokenURL,
		claudeClientID:  "test-claude-client",
		codexClientID:   "test-codex-client",
	}
}

func makeJWT(exp int64) string {
	h := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"none","typ":"JWT"}`))
	p := base64.RawURLEncoding.EncodeToString([]byte(fmt.Sprintf(`{"exp":%d}`, exp)))
	return h + "." + p + ".sig"
}

func newTokenServer(t *testing.T, wantBody map[string]string, respJSON string) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		var got map[string]string
		_ = json.Unmarshal(body, &got)
		for k, v := range wantBody {
			if got[k] != v {
				t.Errorf("token request body[%s] = %q, want %q (body=%s)", k, got[k], v, body)
			}
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(respJSON))
	}))
}

// ---- Claude parsing / expiry ----

func TestClaudeLoadMsExpiry(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, ".credentials.json")
	ms := fmt.Sprintf("%d", time.Now().Add(time.Hour).UnixMilli())
	os.WriteFile(p, []byte(`{"claudeAiOauth":{"accessToken":"at-1","refreshToken":"rt-1","expiresAt":`+ms+`}}`), 0600)
	l := testLocalCreds(p, filepath.Join(dir, "auth.json"), "http://x")
	tok, ok := l.claudeToken()
	if !ok || tok != "at-1" {
		t.Fatalf("claudeToken() = %q, %v", tok, ok)
	}
}

func TestClaudeLoadSecondsExpiry(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, ".credentials.json")
	secs := time.Now().Add(time.Hour).Unix()
	os.WriteFile(p, []byte(`{"claudeAiOauth":{"accessToken":"at-1","refreshToken":"rt-1","expiresAt":`+fmt.Sprint(secs)+`}}`), 0600)
	l := testLocalCreds(p, filepath.Join(dir, "auth.json"), "http://x")
	tok, ok := l.claudeToken()
	if !ok || tok != "at-1" {
		t.Fatalf("claudeToken() = %q, %v", tok, ok)
	}
}

func TestClaudeMissingFile(t *testing.T) {
	dir := t.TempDir()
	l := testLocalCreds(filepath.Join(dir, "nope.json"), filepath.Join(dir, "auth.json"), "http://x")
	if tok, ok := l.claudeToken(); ok || tok != "" {
		t.Fatalf("expected no local claude token, got %q %v", tok, ok)
	}
}

func TestClaudeGarbageFile(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, ".credentials.json")
	os.WriteFile(p, []byte("{not json"), 0600)
	l := testLocalCreds(p, filepath.Join(dir, "auth.json"), "http://x")
	if tok, ok := l.claudeToken(); ok || tok != "" {
		t.Fatalf("expected no local claude token on garbage, got %q %v", tok, ok)
	}
}

// ---- Codex parsing ----

func TestCodexLoadTokens(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "auth.json")
	os.WriteFile(p, []byte(`{"tokens":{"access_token":"cat","refresh_token":"crt","id_token":"cit","account_id":"acc1"}}`), 0600)
	l := testLocalCreds(filepath.Join(dir, "c.json"), p, "http://x")
	tok, ok := l.codexToken()
	if !ok || tok != "cat" {
		t.Fatalf("codexToken() = %q, %v", tok, ok)
	}
}

func TestCodexApiKeyModeNoQuota(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "auth.json")
	os.WriteFile(p, []byte(`{"OPENAI_API_KEY":"sk-xxx"}`), 0600)
	l := testLocalCreds(filepath.Join(dir, "c.json"), p, "http://x")
	if tok, ok := l.codexToken(); ok || tok != "" {
		t.Fatalf("API-key mode must report no local token, got %q %v", tok, ok)
	}
}

func TestCodexMissingFile(t *testing.T) {
	dir := t.TempDir()
	l := testLocalCreds(filepath.Join(dir, "c.json"), filepath.Join(dir, "auth.json"), "http://x")
	if tok, ok := l.codexToken(); ok || tok != "" {
		t.Fatalf("expected no local codex token, got %q %v", tok, ok)
	}
}

// ---- JWT exp ----

func TestJWTExp(t *testing.T) {
	want := time.Now().Add(2 * time.Hour).Unix()
	tok := makeJWT(want)
	if got := jwtExp(tok); got != want {
		t.Fatalf("jwtExp() = %d, want %d", got, want)
	}
	if got := jwtExp("not-a-jwt"); got != 0 {
		t.Fatalf("jwtExp(non-jwt) = %d, want 0", got)
	}
	if got := jwtExp("a." + base64.RawURLEncoding.EncodeToString([]byte(`{"noexp":1}`)) + ".c"); got != 0 {
		t.Fatalf("jwtExp(no-exp) = %d, want 0", got)
	}
}

// ---- normalizeExpiry ----

func TestNormalizeExpiry(t *testing.T) {
	now := time.Now().Unix()
	cases := []struct {
		in   any
		want int64
	}{
		{float64(now), now},
		{float64(now * 1000), now},
		{json.Number(fmt.Sprint(now * 1000)), now},
		{int64(now), now},
		{fmt.Sprint(now), now},
		{nil, 0},
		{"garbage", 0},
	}
	for _, c := range cases {
		if got := normalizeExpiry(c.in); got != c.want {
			t.Errorf("normalizeExpiry(%v) = %d, want %d", c.in, got, c.want)
		}
	}
}

// ---- atomic write ----

func TestAtomicWriteFile(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "target.json")
	if err := atomicWriteFile(p, []byte(`{"a":1}`), 0o600); err != nil {
		t.Fatalf("atomicWriteFile: %v", err)
	}
	raw, err := os.ReadFile(p)
	if err != nil || string(raw) != `{"a":1}` {
		t.Fatalf("content = %q, err=%v", raw, err)
	}
	st, err := os.Stat(p)
	if err != nil {
		t.Fatal(err)
	}
	// Windows does not honor POSIX chmod; the permission assertion targets the
	// Linux server where this module actually runs.
	if runtime.GOOS != "windows" && st.Mode().Perm() != 0o600 {
		t.Fatalf("perm = %o, want 600", st.Mode().Perm())
	}
	// Temp files must be cleaned up.
	entries, _ := os.ReadDir(dir)
	for _, e := range entries {
		if strings.Contains(e.Name(), ".tmp-") {
			t.Fatalf("leftover temp file: %s", e.Name())
		}
	}
	// Overwrite existing file (Windows rename fallback path).
	if err := atomicWriteFile(p, []byte(`{"a":2}`), 0o600); err != nil {
		t.Fatalf("atomicWriteFile overwrite: %v", err)
	}
	raw, _ = os.ReadFile(p)
	if string(raw) != `{"a":2}` {
		t.Fatalf("overwrite content = %q", raw)
	}
}

// ---- refresh + write-back ----

func TestClaudeRefreshWritesBack(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, ".credentials.json")
	expired := time.Now().Add(-time.Minute).UnixMilli()
	os.WriteFile(p, []byte(`{
		"claudeAiOauth":{"accessToken":"old-at","refreshToken":"old-rt","expiresAt":`+fmt.Sprint(expired)+`},
		"primaryDomain":"anthropic.com"
	}`), 0600)

	ts := newTokenServer(t,
		map[string]string{
			"grant_type":    "refresh_token",
			"refresh_token": "old-rt",
			"client_id":     "test-claude-client",
		},
		`{"access_token":"new-at","refresh_token":"new-rt","expires_in":3600}`)
	defer ts.Close()

	l := testLocalCreds(p, filepath.Join(dir, "auth.json"), ts.URL)
	tok, ok := l.claudeToken()
	if !ok || tok != "new-at" {
		t.Fatalf("claudeToken() after refresh = %q, %v", tok, ok)
	}

	// File must contain the rotated tokens and preserve other fields.
	var doc map[string]json.RawMessage
	raw, _ := os.ReadFile(p)
	json.Unmarshal(raw, &doc)
	if string(doc["primaryDomain"]) != `"anthropic.com"` {
		t.Fatalf("top-level field lost: %s", raw)
	}
	var oauth claudeLocalOauth
	json.Unmarshal(doc["claudeAiOauth"], &oauth)
	if oauth.AccessToken != "new-at" || oauth.RefreshToken != "new-rt" {
		t.Fatalf("rotated tokens not written back: %+v", oauth)
	}
	if normalizeExpiry(oauth.ExpiresAt) <= time.Now().Unix() {
		t.Fatalf("expiresAt not updated: %+v", oauth)
	}
}

func TestCodexRefreshWritesBack(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "auth.json")
	expiredJWT := makeJWT(time.Now().Add(-time.Minute).Unix())
	os.WriteFile(p, []byte(`{
		"tokens":{"access_token":"`+expiredJWT+`","refresh_token":"old-crt","id_token":"old-idt","account_id":"acc1"}
	}`), 0600)

	ts := newTokenServer(t,
		map[string]string{
			"grant_type":    "refresh_token",
			"refresh_token": "old-crt",
			"client_id":     "test-codex-client",
		},
		`{"access_token":"new-cat","refresh_token":"new-crt","id_token":"new-idt","expires_in":3600}`)
	defer ts.Close()

	l := testLocalCreds(filepath.Join(dir, "c.json"), p, ts.URL)
	tok, ok := l.codexToken()
	if !ok || tok != "new-cat" {
		t.Fatalf("codexToken() after refresh = %q, %v", tok, ok)
	}

	var auth codexLocalAuth
	raw, _ := os.ReadFile(p)
	if err := json.Unmarshal(raw, &auth); err != nil {
		t.Fatalf("parse written file: %v", err)
	}
	if auth.Tokens.AccessToken != "new-cat" || auth.Tokens.RefreshToken != "new-crt" {
		t.Fatalf("codex tokens not written back: %+v", auth.Tokens)
	}
	if auth.Tokens.IDToken != "old-idt" {
		t.Fatalf("id_token should be preserved from file, got %q", auth.Tokens.IDToken)
	}
	if auth.LastRefresh == "" {
		t.Fatalf("last_refresh not set")
	}
}

// ---- priority: local vs uploaded ----

func TestLocalPriorityOverUploaded(t *testing.T) {
	s := newTestServer(t)
	s.local = testLocalCreds("", "", "http://x")
	s.creds.setClaude("uploaded-at", "uploaded-rt")
	s.creds.setCodex("uploaded-cat", "uploaded-crt", "acc")

	// Local files exist -> local wins.
	dir := t.TempDir()
	cp := filepath.Join(dir, ".credentials.json")
	os.WriteFile(cp, []byte(`{"claudeAiOauth":{"accessToken":"local-at","refreshToken":"local-rt","expiresAt":`+
		fmt.Sprint(time.Now().Add(time.Hour).UnixMilli())+`}}`), 0600)
	s.local.claude.path = cp
	ap := filepath.Join(dir, "auth.json")
	os.WriteFile(ap, []byte(`{"tokens":{"access_token":"local-cat","refresh_token":"local-crt"}}`), 0600)
	s.local.codex.path = ap

	tok, src := s.accessTokenWithSource("claude")
	if tok != "local-at" || src != credSourceLocal {
		t.Fatalf("claude: got %q src=%v, want local-at/local", tok, src)
	}
	tok, src = s.accessTokenWithSource("codex")
	if tok != "local-cat" || src != credSourceLocal {
		t.Fatalf("codex: got %q src=%v, want local-cat/local", tok, src)
	}
}

func TestLocalFallbackToUploaded(t *testing.T) {
	s := newTestServer(t)
	s.local = testLocalCreds("", "", "http://x")
	s.creds.setClaude("uploaded-at", "uploaded-rt")
	s.creds.setCodex("uploaded-cat", "uploaded-crt", "acc")

	dir := t.TempDir() // no files exist there
	s.local.claude.path = filepath.Join(dir, ".credentials.json")
	s.local.codex.path = filepath.Join(dir, "auth.json")

	tok, src := s.accessTokenWithSource("claude")
	if tok != "uploaded-at" || src != credSourceUploaded {
		t.Fatalf("claude fallback: got %q src=%v", tok, src)
	}
	tok, src = s.accessTokenWithSource("codex")
	if tok != "uploaded-cat" || src != credSourceUploaded {
		t.Fatalf("codex fallback: got %q src=%v", tok, src)
	}
}

// ---- refresh in uploaded mode (no local creds) ----

func TestRefreshCodexUploadedMode(t *testing.T) {
	s := newTestServer(t)
	s.creds.setCodex("at", "rt", "acc1")

	ts := newTokenServer(t,
		map[string]string{
			"client_id":     codexLocalClientID,
			"grant_type":    "refresh_token",
			"refresh_token": "rt",
		},
		`{"access_token":"new-at","refresh_token":"new-rt","expires_in":3600}`)
	defer ts.Close()
	s.codexTokenURL = ts.URL

	access, newRefresh, expiresAt, err := refreshCodexToken(s)
	if err != nil {
		t.Fatalf("refreshCodexToken: %v", err)
	}
	if access != "new-at" || newRefresh != "new-rt" || expiresAt <= time.Now().Unix() {
		t.Fatalf("bad refresh result: %q %q %d", access, newRefresh, expiresAt)
	}
	caccess, crefresh, _ := s.creds.codex()
	if caccess != "new-at" || crefresh != "new-rt" {
		t.Fatalf("uploaded creds not updated: %q %q", caccess, crefresh)
	}
}

func TestRefreshClaudeUploadedMode(t *testing.T) {
	s := newTestServer(t)
	s.creds.setClaude("at", "rt")

	ts := newTokenServer(t,
		map[string]string{
			"grant_type":    "refresh_token",
			"refresh_token": "rt",
			"client_id":     s.cfg.claudeClientID,
		},
		`{"access_token":"new-at","refresh_token":"new-rt","expires_in":3600}`)
	defer ts.Close()
	s.claudeTokenURLs = []string{ts.URL}

	access, newRefresh, expiresAt, err := refreshClaudeToken(s)
	if err != nil {
		t.Fatalf("refreshClaudeToken: %v", err)
	}
	if access != "new-at" || newRefresh != "new-rt" || expiresAt <= time.Now().Unix() {
		t.Fatalf("bad refresh result: %q %q %d", access, newRefresh, expiresAt)
	}
	caccess, crefresh := s.creds.claude()
	if caccess != "new-at" || crefresh != "new-rt" {
		t.Fatalf("uploaded creds not updated: %q %q", caccess, crefresh)
	}
}

func TestRefreshClaudeUploadedModeFailsAll(t *testing.T) {
	s := newTestServer(t)
	s.creds.setClaude("at", "rt")
	// Both endpoints fail.
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "bad", http.StatusBadRequest)
	}))
	defer ts.Close()
	s.claudeTokenURLs = []string{ts.URL, ts.URL}
	if _, _, _, err := refreshClaudeToken(s); err == nil {
		t.Fatal("expected error when both token endpoints fail")
	}
	// Existing creds must be untouched.
	caccess, crefresh := s.creds.claude()
	if caccess != "at" || crefresh != "rt" {
		t.Fatalf("creds mutated on failure: %q %q", caccess, crefresh)
	}
}
