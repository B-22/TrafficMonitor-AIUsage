package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"time"
)

// refreshClaudeToken exchanges the stored refresh token for a fresh access
// token, updates the in-memory credentials and invalidates cached upstream
// data so the next request uses the new token. Returns the new access token,
// the (possibly rotated) refresh token and its expiry time.
//
// Local CLI credentials (when enabled) are refreshed first; uploaded
// credentials are used otherwise.
func refreshClaudeToken(s *server) (string, string, int64, error) {
	if s.local != nil {
		if _, ok := s.local.claudeToken(); ok {
			access, newRefresh, expiresAt, err := s.local.refreshClaude()
			if err == nil {
				s.invalidateCache()
				return access, newRefresh, expiresAt, nil
			}
			// Local refresh failed: fall back to uploaded credentials below.
		}
	}

	_, refresh := s.creds.claude()
	if refresh == "" {
		return "", "", 0, errors.New("claude: no refresh token uploaded")
	}

	body := map[string]string{
		"grant_type":    "refresh_token",
		"refresh_token": refresh,
		"client_id":     s.cfg.claudeClientID,
	}
	payload, err := json.Marshal(body)
	if err != nil {
		return "", "", 0, err
	}

	var lastErr error
	for _, u := range s.claudeTokenURLs {
		token, newRefresh, expiresAt, err := s.claudeTokenExchange(u, payload)
		if err != nil {
			lastErr = err
			continue
		}
		s.creds.setClaude(token, newRefresh)
		s.invalidateCache()
		return token, newRefresh, expiresAt, nil
	}
	if lastErr == nil {
		lastErr = errors.New("claude: token exchange failed")
	}
	return "", "", 0, lastErr
}

// refreshCodexToken refreshes the Codex access token. Local CLI credentials
// (when enabled) are refreshed first and written back to auth.json; uploaded
// credentials are the fallback.
func refreshCodexToken(s *server) (string, string, int64, error) {
	if s.local != nil {
		if _, ok := s.local.codexToken(); ok {
			access, newRefresh, expiresAt, err := s.local.refreshCodex()
			if err == nil {
				s.invalidateCache()
				return access, newRefresh, expiresAt, nil
			}
			// Local refresh failed: fall back to uploaded credentials below.
		}
	}

	access, refresh, accountID := s.creds.codex()
	if access == "" || refresh == "" {
		return "", "", 0, errors.New("codex: no refresh token uploaded")
	}
	payload, err := json.Marshal(map[string]string{
		"client_id":     codexLocalClientID,
		"grant_type":    "refresh_token",
		"refresh_token": refresh,
	})
	if err != nil {
		return "", "", 0, err
	}
	newAccess, newRefresh, expiresAt, err := tokenExchangeWith(
		s.http, s.codexTokenURL, payload, codexLocalUA)
	if err != nil {
		return "", "", 0, err
	}
	s.creds.setCodex(newAccess, newRefresh, accountID)
	s.invalidateCache()
	return newAccess, newRefresh, expiresAt, nil
}

func (s *server) claudeTokenExchange(endpoint string, payload []byte) (string, string, int64, error) {
	req, err := http.NewRequest(http.MethodPost,
		endpoint, bytes.NewReader(payload))
	if err != nil {
		return "", "", 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "claude-code/2.1.85")

	resp, err := s.http.Do(req)
	if err != nil {
		return "", "", 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", "", 0, fmt.Errorf("claude: token endpoint returned %d", resp.StatusCode)
	}

	var out struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresIn    int64  `json:"expires_in"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return "", "", 0, err
	}
	if out.AccessToken == "" {
		return "", "", 0, errors.New("claude: token endpoint returned empty access_token")
	}
	expiresAt := time.Now().Unix()
	if out.ExpiresIn > 0 {
		expiresAt += out.ExpiresIn
	}
	return out.AccessToken, out.RefreshToken, expiresAt, nil
}
