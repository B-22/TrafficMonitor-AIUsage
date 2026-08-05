// Local-credential reuse for Claude Code and Codex CLI on the server host.
//
// The forwarder normally relies on credentials uploaded by the Windows
// plugin. On a Linux box where `claude` / `codex` are already logged in, this
// module reads their credential files directly (mirroring the semantics of
// github.com/huanchong-99/claude-usage-assistant):
//
//   - Claude:  $CLAUDE_CONFIG_DIR/.credentials.json (default ~/.claude),
//     section claudeAiOauth {accessToken, refreshToken, expiresAt}.
//   - Codex:   $CODEX_HOME/auth.json (default ~/.codex),
//     section tokens {access_token, refresh_token, id_token, account_id}.
//
// Tokens that are close to expiry (or rejected with 401 by the upstream) are
// refreshed through the public OAuth endpoints and atomically written back to
// the same files with mode 0600, so the server behaves exactly like the CLI:
// login once in the terminal, and the forwarder keeps itself fresh.
//
// Local credentials take priority over uploaded ones; if the local file is
// missing/unparseable the caller falls back to the uploaded credentials.
package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	// Same OAuth clients as the reference assistant / the CLIs themselves.
	codexLocalClientID = "app_EMoamEEZ73f0CkXaXp7hrann"
	codexLocalUA       = "codex-cli"

	// Refresh is attempted when the access token expires within this window.
	localRefreshLeadTime = 120 * time.Second
)

// claudeLocalOauth is the claudeAiOauth section of .credentials.json.
type claudeLocalOauth struct {
	AccessToken      string `json:"accessToken"`
	RefreshToken     string `json:"refreshToken"`
	ExpiresAt        any    `json:"expiresAt"` // unix seconds or ms (>=1e12)
	SubscriptionType string `json:"subscriptionType"`
}

type claudeLocalCreds struct {
	ClaudeAiOauth claudeLocalOauth `json:"claudeAiOauth"`
}

type codexLocalTokens struct {
	IDToken      string `json:"id_token"`
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	AccountID    string `json:"account_id"`
}

type codexLocalAuth struct {
	OPENAIAPIKey string           `json:"OPENAI_API_KEY"`
	Tokens       codexLocalTokens `json:"tokens"`
	LastRefresh  string           `json:"last_refresh"`
}

// localCreds reads and refreshes the CLI credential files on the server.
// All operations are serialized through mu; refresh results are cached in
// memory so concurrent requests reuse the freshly exchanged token.
type localCreds struct {
	mu     sync.Mutex
	http   *http.Client
	claude *localCredFile
	codex  *localCredFile
	// claudeTokenURLs are tried in order (primary first).
	claudeTokenURLs []string
	codexTokenURL   string
	claudeClientID  string
	codexClientID   string

	claudeState *claudeLocalOauth
	codexState  *codexLocalTokens
}

type localCredFile struct {
	path string
}

func newLocalCreds(cfg config, hc *http.Client) *localCreds {
	if hc == nil {
		hc = &http.Client{Timeout: 25 * time.Second}
	}
	claudeDir := os.Getenv("CLAUDE_CONFIG_DIR")
	if claudeDir == "" {
		home, err := os.UserHomeDir()
		if err != nil || home == "" {
			home = "."
		}
		claudeDir = filepath.Join(home, ".claude")
	}
	codexHome := os.Getenv("CODEX_HOME")
	if codexHome == "" {
		home, err := os.UserHomeDir()
		if err != nil || home == "" {
			home = "."
		}
		codexHome = filepath.Join(home, ".codex")
	}
	return &localCreds{
		http: hc,
		claude: &localCredFile{
			path: filepath.Join(claudeDir, ".credentials.json"),
		},
		codex: &localCredFile{
			path: filepath.Join(codexHome, "auth.json"),
		},
		claudeTokenURLs: []string{
			"https://platform.claude.com/v1/oauth/token",
			"https://console.anthropic.com/v1/oauth/token",
		},
		codexTokenURL:  "https://auth.openai.com/oauth/token",
		claudeClientID: cfg.claudeClientID,
		codexClientID:  codexLocalClientID,
	}
}

// ---- Claude ----

// claudeToken returns the freshest local Claude access token. It refreshes
// eagerly when the cached token is close to expiry. Returns ("", false) when
// no usable local credential exists.
func (l *localCreds) claudeToken() (string, bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.loadClaudeLocked(); err != nil {
		return "", false
	}
	st := l.claudeState
	if st == nil || st.AccessToken == "" {
		return "", false
	}
	exp := normalizeExpiry(st.ExpiresAt)
	if exp > 0 && time.Now().Add(localRefreshLeadTime).Unix() >= exp {
		l.refreshClaudeLocked() // failure: keep using the old token
		if st := l.claudeState; st != nil && st.AccessToken != "" {
			return st.AccessToken, true
		}
		return "", false
	}
	return st.AccessToken, true
}

// refreshClaude refreshes the local Claude credential file using its
// refresh_token, writing the rotated tokens back atomically. Returns the new
// access token, refresh token and expiry (unix seconds).
func (l *localCreds) refreshClaude() (string, string, int64, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.loadClaudeLocked(); err != nil {
		return "", "", 0, err
	}
	return l.refreshClaudeLocked()
}

func (l *localCreds) loadClaudeLocked() error {
	raw, err := os.ReadFile(l.claude.path)
	if err != nil {
		l.claudeState = nil
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return err
	}
	var creds claudeLocalCreds
	if err := json.Unmarshal(raw, &creds); err != nil {
		l.claudeState = nil
		return fmt.Errorf("claude: parse %s: %w", l.claude.path, err)
	}
	l.claudeState = &creds.ClaudeAiOauth
	return nil
}

func (l *localCreds) refreshClaudeLocked() (string, string, int64, error) {
	st := l.claudeState
	if st == nil || st.RefreshToken == "" {
		return "", "", 0, errors.New("claude: local credential has no refreshToken")
	}
	payload, err := json.Marshal(map[string]string{
		"grant_type":    "refresh_token",
		"refresh_token": st.RefreshToken,
		"client_id":     l.claudeClientID,
	})
	if err != nil {
		return "", "", 0, err
	}

	var lastErr error
	for _, u := range l.claudeTokenURLs {
		access, newRefresh, expiresAt, err := l.tokenExchange(u, payload, "claude-code/2.1.85")
		if err != nil {
			lastErr = err
			continue
		}
		st.AccessToken = access
		if newRefresh != "" {
			st.RefreshToken = newRefresh
		}
		st.ExpiresAt = float64(expiresAt * 1000) // persist in ms like the CLI does
		if err := l.writeClaudeLocked(); err != nil {
			// Still usable in memory for this process.
			return access, newRefresh, expiresAt, err
		}
		return access, newRefresh, expiresAt, nil
	}
	if lastErr == nil {
		lastErr = errors.New("claude: token exchange failed")
	}
	return "", "", 0, lastErr
}

func (l *localCreds) writeClaudeLocked() error {
	// Re-read the original file and only replace the claudeAiOauth object,
	// preserving any other top-level fields the CLI may keep there.
	raw, err := os.ReadFile(l.claude.path)
	if err != nil {
		return err
	}
	var doc map[string]json.RawMessage
	if err := json.Unmarshal(raw, &doc); err != nil {
		return err
	}
	oauthData, err := json.Marshal(l.claudeState)
	if err != nil {
		return err
	}
	doc["claudeAiOauth"] = oauthData
	data, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return err
	}
	return atomicWriteFile(l.claude.path, data, 0o600)
}

// ---- Codex ----

// codexToken returns the freshest local Codex access token, refreshing
// eagerly near expiry. ("", false) when the local file is missing or has no
// usable token (e.g. API-key billing mode, where there is no subscription
// quota to read).
func (l *localCreds) codexToken() (string, bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.loadCodexLocked(); err != nil {
		return "", false
	}
	st := l.codexState
	if st == nil || st.AccessToken == "" {
		return "", false
	}
	if exp := jwtExp(st.AccessToken); exp > 0 && time.Now().Add(localRefreshLeadTime).Unix() >= exp {
		l.refreshCodexLocked() // failure: keep using the old token
		if st := l.codexState; st != nil && st.AccessToken != "" {
			return st.AccessToken, true
		}
		return "", false
	}
	return st.AccessToken, true
}

// refreshCodex refreshes the local Codex auth.json via its refresh_token,
// writing back atomically. Returns the new access token, refresh token and
// expiry (unix seconds).
func (l *localCreds) refreshCodex() (string, string, int64, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if err := l.loadCodexLocked(); err != nil {
		return "", "", 0, err
	}
	return l.refreshCodexLocked()
}

func (l *localCreds) loadCodexLocked() error {
	raw, err := os.ReadFile(l.codex.path)
	if err != nil {
		l.codexState = nil
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return err
	}
	var auth codexLocalAuth
	if err := json.Unmarshal(raw, &auth); err != nil {
		l.codexState = nil
		return fmt.Errorf("codex: parse %s: %w", l.codex.path, err)
	}
	if auth.Tokens.AccessToken == "" {
		l.codexState = nil // API-key billing mode or incomplete login
		return nil
	}
	l.codexState = &auth.Tokens
	return nil
}

func (l *localCreds) refreshCodexLocked() (string, string, int64, error) {
	st := l.codexState
	if st == nil || st.RefreshToken == "" {
		return "", "", 0, errors.New("codex: local credential has no refresh_token")
	}
	payload, err := json.Marshal(map[string]string{
		"client_id":     l.codexClientID,
		"grant_type":    "refresh_token",
		"refresh_token": st.RefreshToken,
	})
	if err != nil {
		return "", "", 0, err
	}
	access, newRefresh, expiresAt, err := l.tokenExchange(l.codexTokenURL, payload, codexLocalUA)
	if err != nil {
		return "", "", 0, err
	}
	// Update the in-memory state so the current call already returns the
	// fresh token; id_token and other fields are preserved from the file.
	st.AccessToken = access
	if newRefresh != "" {
		st.RefreshToken = newRefresh
	}
	// auth.openai.com returns id_token/access_token/refresh_token; the file
	// keeps all fields, mirroring the reference assistant.
	if err := l.writeCodexLocked(); err != nil {
		return access, newRefresh, expiresAt, err
	}
	return access, newRefresh, expiresAt, nil
}

func (l *localCreds) writeCodexLocked() error {
	raw, err := os.ReadFile(l.codex.path)
	if err != nil {
		return err
	}
	var auth codexLocalAuth
	if err := json.Unmarshal(raw, &auth); err != nil {
		return err
	}
	auth.Tokens = *l.codexState
	auth.LastRefresh = time.Now().UTC().Format("2006-01-02T15:04:05Z")
	data, err := json.MarshalIndent(auth, "", "  ")
	if err != nil {
		return err
	}
	return atomicWriteFile(l.codex.path, data, 0o600)
}

// ---- shared helpers ----

// tokenExchange POSTs a JSON body to the token endpoint and parses the
// standard OAuth response. Returns (access, refresh, expiresAt unix, err).
func (l *localCreds) tokenExchange(endpoint string, payload []byte, ua string) (string, string, int64, error) {
	return tokenExchangeWith(l.http, endpoint, payload, ua)
}

func tokenExchangeWith(hc *http.Client, endpoint string, payload []byte, ua string) (string, string, int64, error) {
	req, err := http.NewRequest(http.MethodPost, endpoint, strings.NewReader(string(payload)))
	if err != nil {
		return "", "", 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", ua)
	resp, err := hc.Do(req)
	if err != nil {
		return "", "", 0, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return "", "", 0, err
	}
	if resp.StatusCode != http.StatusOK {
		return "", "", 0, fmt.Errorf("token endpoint %s returned %d: %s",
			endpoint, resp.StatusCode, truncate(string(body), 200))
	}
	var out struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresIn    int64  `json:"expires_in"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return "", "", 0, err
	}
	if out.AccessToken == "" {
		return "", "", 0, errors.New("token endpoint returned empty access_token")
	}
	expiresAt := time.Now().Unix()
	if out.ExpiresIn > 0 {
		expiresAt += out.ExpiresIn
	}
	return out.AccessToken, out.RefreshToken, expiresAt, nil
}

// jwtExp decodes the `exp` claim of a JWT without verifying the signature.
// Returns 0 when the token is not a JWT or has no exp claim.
func jwtExp(token string) int64 {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return 0
	}
	seg := parts[1]
	if pad := len(seg) % 4; pad != 0 {
		seg += strings.Repeat("=", 4-pad)
	}
	raw, err := base64.RawURLEncoding.DecodeString(strings.TrimRight(seg, "="))
	if err != nil {
		return 0
	}
	var claims struct {
		Exp float64 `json:"exp"`
	}
	if err := json.Unmarshal(raw, &claims); err != nil || claims.Exp == 0 {
		return 0
	}
	return int64(claims.Exp)
}

// normalizeExpiry converts a Claude expiresAt (unix seconds or ms) to unix
// seconds. Returns 0 when unknown.
func normalizeExpiry(v any) int64 {
	switch t := v.(type) {
	case float64:
		if t > 1e12 {
			return int64(t / 1000)
		}
		return int64(t)
	case json.Number:
		f, err := t.Float64()
		if err != nil {
			return 0
		}
		if f > 1e12 {
			return int64(f / 1000)
		}
		return int64(f)
	case int64:
		if t > 1e12 {
			return t / 1000
		}
		return t
	case string:
		var f float64
		if _, err := fmt.Sscanf(t, "%f", &f); err != nil {
			return 0
		}
		if f > 1e12 {
			return int64(f / 1000)
		}
		return int64(f)
	}
	return 0
}

// atomicWriteFile writes data to a temp file in the same directory, chmods it
// to perm, fsyncs and renames it over the target (atomic on POSIX). On
// Windows os.Rename cannot replace an existing file, so it falls back to a
// remove-then-rename.
func atomicWriteFile(path string, data []byte, perm os.FileMode) error {
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, filepath.Base(path)+".tmp-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer func() {
		if tmpName != "" {
			_ = os.Remove(tmpName)
		}
	}()
	if err := tmp.Chmod(perm); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpName, path); err != nil {
		// Windows: rename cannot overwrite; remove then rename (best-effort).
		if rmErr := os.Remove(path); rmErr == nil {
			if err2 := os.Rename(tmpName, path); err2 == nil {
				tmpName = ""
				return nil
			}
		}
		return err
	}
	tmpName = ""
	return nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
