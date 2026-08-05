// aiUsage-server is a small credential-holding forwarder for the
// AIUsage TrafficMonitor plugin. The Windows plugin uploads Claude/Codex
// OAuth credentials once, then polls usage/credits endpoints on this
// server. The server fetches from the official APIs over its own network
// path (e.g. residential IPv6 egress), caches responses briefly, and
// transparently refreshes expired Claude tokens.
package main

import (
	"crypto/subtle"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

type config struct {
	listen         string
	token          string
	cacheUsage     time.Duration
	cacheCredits   time.Duration
	cacheProfile   time.Duration
	claudeClientID string
	tlsCert        string
	tlsKey         string
}

func loadConfig() (config, error) {
	cfg := config{
		listen:         envOr("AIUSAGE_LISTEN", "127.0.0.1:8444"),
		cacheUsage:     time.Duration(envIntOr("AIUSAGE_CACHE_USAGE", 45)) * time.Second,
		cacheCredits:   time.Duration(envIntOr("AIUSAGE_CACHE_CREDITS", 300)) * time.Second,
		cacheProfile:   time.Duration(envIntOr("AIUSAGE_CACHE_PROFILE", 3600)) * time.Second,
		claudeClientID: envOr("AIUSAGE_CLAUDE_CLIENT_ID", "9d1c250a-e61b-44d9-88ed-5944d1962f5e"),
		tlsCert:        os.Getenv("AIUSAGE_TLS_CERT"),
		tlsKey:         os.Getenv("AIUSAGE_TLS_KEY"),
	}
	cfg.token = os.Getenv("AIUSAGE_TOKEN")
	if cfg.token == "" {
		return cfg, fmt.Errorf("AIUSAGE_TOKEN must be set")
	}
	return cfg, nil
}

func envOr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

func envIntOr(name string, def int) int {
	v := os.Getenv(name)
	if v == "" {
		return def
	}
	var n int
	if _, err := fmt.Sscanf(v, "%d", &n); err != nil {
		return def
	}
	return n
}

// credentials holds the OAuth tokens uploaded by the plugin.
type credentials struct {
	mu             sync.RWMutex
	claudeAccess   string
	claudeRefresh  string
	codexAccess    string
	codexRefresh   string
	codexAccountID string
}

func (c *credentials) setClaude(access, refresh string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.claudeAccess = access
	if refresh != "" {
		c.claudeRefresh = refresh
	}
}

func (c *credentials) setCodex(access, refresh, accountID string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.codexAccess = access
	if refresh != "" {
		c.codexRefresh = refresh
	}
	if accountID != "" {
		c.codexAccountID = accountID
	}
}

func (c *credentials) claude() (string, string) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.claudeAccess, c.claudeRefresh
}

func (c *credentials) codex() (string, string, string) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.codexAccess, c.codexRefresh, c.codexAccountID
}

// credentialPayload is the JSON body accepted by POST /api/v1/credentials.
type credentialPayload struct {
	Claude *struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
	} `json:"claude"`
	Codex *struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		AccountID    string `json:"account_id"`
	} `json:"codex"`
}

// cachedResponse is a single upstream response kept briefly in memory.
type cachedResponse struct {
	body       []byte
	statusCode int
	fetchedAt  time.Time
}

// tokenRefreshInfo is returned to the plugin via response header after a
// server-side token refresh, so the plugin can persist the new tokens.
type tokenRefreshInfo struct {
	Provider     string `json:"provider"`
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token,omitempty"`
	ExpiresAt    int64  `json:"expires_at,omitempty"`
}

type server struct {
	cfg   config
	creds credentials
	mu    sync.Mutex
	cache map[string]*cachedResponse
	http  *http.Client
}

func newServer(cfg config) *server {
	return &server{
		cfg:   cfg,
		cache: make(map[string]*cachedResponse),
		http: &http.Client{
			Timeout: 25 * time.Second,
			Transport: &http.Transport{
				Proxy: nil, // force direct egress (system route, e.g. residential IPv6)
			},
		},
	}
}

// checkAuth rejects requests without a valid server bearer token.
func (s *server) checkAuth(r *http.Request) bool {
	const prefix = "Bearer "
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, prefix) {
		return false
	}
	got := strings.TrimPrefix(header, prefix)
	if subtle.ConstantTimeCompare([]byte(got), []byte(s.cfg.token)) != 1 {
		return false
	}
	return true
}

func (s *server) handleCredentials(w http.ResponseWriter, r *http.Request) {
	if !s.checkAuth(r) {
		w.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	body := http.MaxBytesReader(w, r.Body, 64<<10)
	var payload credentialPayload
	if err := json.NewDecoder(body).Decode(&payload); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}
	if payload.Claude != nil {
		if payload.Claude.AccessToken == "" {
			http.Error(w, "claude.access_token required", http.StatusBadRequest)
			return
		}
		s.creds.setClaude(payload.Claude.AccessToken, payload.Claude.RefreshToken)
	}
	if payload.Codex != nil {
		if payload.Codex.AccessToken == "" {
			http.Error(w, "codex.access_token required", http.StatusBadRequest)
			return
		}
		s.creds.setCodex(payload.Codex.AccessToken, payload.Codex.RefreshToken, payload.Codex.AccountID)
	}
	s.invalidateCache()
	w.WriteHeader(http.StatusNoContent)
}

// upstreamEndpoint describes one forwarded API call.
type upstreamEndpoint struct {
	cacheKey    string
	provider    string // "claude" or "codex"
	host        string
	path        string
	cacheTTL    time.Duration
	extraHeader map[string]string
	// needsRefresh is invoked on 401 to obtain a fresh access token.
	// Returns (access token, new refresh token, expires_at unix, error).
	needsRefresh func(s *server) (string, string, int64, error)
}

func (s *server) handleEndpoint(e upstreamEndpoint) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if !s.checkAuth(r) {
			w.Header().Set("WWW-Authenticate", "Bearer")
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}

		if cached, ok := s.getCached(e.cacheKey); ok {
			w.Header().Set("X-AIUsage-Cache", "HIT")
			s.writeResponse(w, cached.statusCode, cached.body)
			return
		}

		status, body, refreshed := s.fetchWithRetry(e)
		if status == 0 {
			http.Error(w, "upstream unreachable", http.StatusBadGateway)
			return
		}
		s.putCached(e.cacheKey, status, body, e.cacheTTL)
		w.Header().Set("X-AIUsage-Cache", "MISS")
		if refreshed != nil {
			if enc, err := json.Marshal(refreshed); err == nil {
				w.Header().Set("X-AIUsage-Token-Refresh", base64.RawURLEncoding.EncodeToString(enc))
			}
		}
		s.writeResponse(w, status, body)
	}
}

func (s *server) fetchWithRetry(e upstreamEndpoint) (int, []byte, *tokenRefreshInfo) {
	access := s.accessToken(e.provider)
	if access == "" {
		return http.StatusUnauthorized, []byte(`{"error":"no credentials uploaded"}`), nil
	}

	status, body := s.doUpstream(e, access)
	if status != http.StatusUnauthorized || e.needsRefresh == nil {
		return status, body, nil
	}

	newAccess, newRefresh, expiresAt, err := e.needsRefresh(s)
	if err != nil {
		return status, body, nil
	}
	status, body = s.doUpstream(e, newAccess)
	return status, body, &tokenRefreshInfo{
		Provider:     e.provider,
		AccessToken:  newAccess,
		RefreshToken: newRefresh,
		ExpiresAt:    expiresAt,
	}
}

func (s *server) accessToken(provider string) string {
	if provider == "claude" {
		access, _ := s.creds.claude()
		return access
	}
	access, _, _ := s.creds.codex()
	return access
}

func (s *server) doUpstream(e upstreamEndpoint, access string) (int, []byte) {
	req, err := http.NewRequest(http.MethodGet, "https://"+e.host+e.path, nil)
	if err != nil {
		return 0, nil
	}
	req.Header.Set("Authorization", "Bearer "+access)
	for k, v := range e.extraHeader {
		req.Header.Set(k, v)
	}
	resp, err := s.http.Do(req)
	if err != nil {
		return 0, nil
	}
	defer resp.Body.Close()
	body := make([]byte, 0, 4096)
	buf := make([]byte, 32<<10)
	for {
		n, err := resp.Body.Read(buf)
		body = append(body, buf[:n]...)
		if len(body) > 4<<20 {
			return http.StatusBadGateway, []byte(`{"error":"upstream response too large"}`)
		}
		if err != nil {
			break
		}
	}
	return resp.StatusCode, body
}

func (s *server) getCached(key string) (*cachedResponse, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c, ok := s.cache[key]
	if !ok {
		return nil, false
	}
	return c, true
}

func (s *server) putCached(key string, status int, body []byte, ttl time.Duration) {
	if status != http.StatusOK {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cache[key] = &cachedResponse{
		body:       append([]byte(nil), body...),
		statusCode: status,
		fetchedAt:  time.Now(),
	}
}

func (s *server) invalidateCache() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cache = make(map[string]*cachedResponse)
}

func (s *server) writeResponse(w http.ResponseWriter, status int, body []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func (s *server) handleHealth(w http.ResponseWriter, r *http.Request) {
	if !s.checkAuth(r) {
		w.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte(`{"status":"ok"}`))
}

func (s *server) handleCacheExpiry() {
	ticker := time.NewTicker(time.Minute)
	defer ticker.Stop()
	for range ticker.C {
		now := time.Now()
		s.mu.Lock()
		for k, c := range s.cache {
			ttl := s.cfg.cacheUsage
			if strings.HasPrefix(k, "credits:") {
				ttl = s.cfg.cacheCredits
			} else if strings.HasPrefix(k, "profile:") {
				ttl = s.cfg.cacheProfile
			}
			if now.Sub(c.fetchedAt) > ttl {
				delete(s.cache, k)
			}
		}
		s.mu.Unlock()
	}
}

func main() {
	log.SetFlags(log.LstdFlags | log.LUTC)
	cfg, err := loadConfig()
	if err != nil {
		log.Fatalf("config: %v", err)
	}

	s := newServer(cfg)
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/health", s.handleHealth)
	mux.HandleFunc("/api/v1/credentials", s.handleCredentials)

	mux.Handle("/api/v1/claude/usage", s.handleEndpoint(upstreamEndpoint{
		cacheKey: "claude:usage",
		provider: "claude",
		host:     "api.anthropic.com",
		path:     "/api/oauth/usage",
		cacheTTL: cfg.cacheUsage,
		extraHeader: map[string]string{
			"Content-Type":   "application/json",
			"User-Agent":     "claude-code/2.1.85",
			"anthropic-beta": "oauth-2025-04-20",
		},
		needsRefresh: refreshClaudeToken,
	}))
	mux.Handle("/api/v1/claude/profile", s.handleEndpoint(upstreamEndpoint{
		cacheKey: "claude:profile",
		provider: "claude",
		host:     "api.anthropic.com",
		path:     "/api/oauth/profile",
		cacheTTL: cfg.cacheProfile,
		extraHeader: map[string]string{
			"Content-Type":   "application/json",
			"User-Agent":     "claude-code/2.1.85",
			"anthropic-beta": "oauth-2025-04-20",
		},
		needsRefresh: refreshClaudeToken,
	}))
	mux.Handle("/api/v1/codex/usage", s.handleEndpoint(upstreamEndpoint{
		cacheKey: "codex:usage",
		provider: "codex",
		host:     "chatgpt.com",
		path:     "/backend-api/wham/usage",
		cacheTTL: cfg.cacheUsage,
		extraHeader: map[string]string{
			"OpenAI-Beta": "codex-1",
			"originator":  "Codex Desktop",
		},
	}))
	mux.Handle("/api/v1/codex/credits", s.handleEndpoint(upstreamEndpoint{
		cacheKey: "codex:credits",
		provider: "codex",
		host:     "chatgpt.com",
		path:     "/backend-api/wham/rate-limit-reset-credits",
		cacheTTL: cfg.cacheCredits,
		extraHeader: map[string]string{
			"OpenAI-Beta": "codex-1",
			"originator":  "Codex Desktop",
		},
	}))

	go s.handleCacheExpiry()

	httpServer := &http.Server{
		Addr:    cfg.listen,
		Handler: mux,
	}

	if cfg.tlsCert != "" && cfg.tlsKey != "" {
		reloader := newCertReloader(cfg.tlsCert, cfg.tlsKey)
		httpServer.TLSConfig = &tls.Config{
			GetCertificate: reloader.GetCertificate,
			MinVersion:     tls.VersionTLS12,
		}
		log.Printf("aiusage-server listening on %s (TLS, cert=%s)", cfg.listen, cfg.tlsCert)
		if err := httpServer.ListenAndServeTLS("", ""); err != nil {
			log.Fatalf("listen: %v", err)
		}
		return
	}

	log.Printf("aiusage-server listening on %s (plain HTTP)", cfg.listen)
	if err := httpServer.ListenAndServe(); err != nil {
		log.Fatalf("listen: %v", err)
	}
}
