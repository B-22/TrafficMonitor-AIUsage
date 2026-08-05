package main

import (
	"bufio"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Antigravity (Google Cloud Code) OAuth uses the same installed-app client
// credentials as the Antigravity IDE itself. Google explicitly documents that
// installed-app / desktop client secrets are NOT confidential -- they ship
// inside every copy of the client -- so the value is hard-coded here rather
// than demanded from the operator. It can still be overridden via env.
const (
	agAuthEndpoint       = "https://accounts.google.com/o/oauth2/v2/auth"
	agTokenEndpoint      = "https://oauth2.googleapis.com/token"
	agDeviceCodeEndpoint = "https://oauth2.googleapis.com/device/code"
	agDeviceGrantType    = "urn:ietf:params:oauth:grant-type:device_code"
	// Verified against AntigravityQuotaWatcher src/auth/constants.ts
	// (CLOUD_CODE_API_BASE). Note the repo's own api-endpoints.md claims a
	// "cloudcode-pa.clients6.google.com" host -- that doc is stale; the code
	// uses the googleapis.com host below.
	agCloudCodeHost = "cloudcode-pa.googleapis.com"

	agDefaultClientID = "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"
	// The installed-app client secret is intentionally NOT embedded: supply it
	// via the AIUSAGE_AG_CLIENT_SECRET environment variable (same value the
	// desktop clients ship in plain text; Google treats it as non-confidential,
	// but keeping it out of git avoids GitHub secret-scan push blocks).
	agDefaultClientSecret = ""

	// Loopback redirect used by the manual paste flow. The page will never
	// actually load (the server has no browser and the operator's browser is
	// on a different machine) -- we only need Google to put ?code= into the
	// address bar so it can be copied across.
	agRedirectURI = "http://localhost:8721"

	agScopes = "https://www.googleapis.com/auth/cloud-platform " +
		"https://www.googleapis.com/auth/userinfo.email " +
		"https://www.googleapis.com/auth/userinfo.profile " +
		"https://www.googleapis.com/auth/cclog " +
		"https://www.googleapis.com/auth/experimentsandconfigs"
)

// agTokenRecord persists the OAuth tokens server-side so the headless server
// does not need to re-authorize on every restart.
type agTokenRecord struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresAt    int64  `json:"expires_at"`
}

// ---- credentials accessors ----

func (s *server) setAntigravity(access, refresh string, expiresAt int64) {
	s.creds.mu.Lock()
	defer s.creds.mu.Unlock()
	s.creds.antigravityAccess = access
	if refresh != "" {
		s.creds.antigravityRefresh = refresh
	}
	s.creds.antigravityExpiresAt = expiresAt
}

func (s *server) antigravity() (access, refresh string, expiresAt int64) {
	s.creds.mu.RLock()
	defer s.creds.mu.RUnlock()
	return s.creds.antigravityAccess, s.creds.antigravityRefresh, s.creds.antigravityExpiresAt
}

// ---- startup ----

// bootstrapAntigravity loads a persisted refresh token if one exists. It never
// blocks and never exits the process: if no token is present the Antigravity
// endpoint simply reports "not authorized" while Claude/Codex keep working.
// Authorization is performed out-of-band via `aiusage-server ag-login`.
func (s *server) bootstrapAntigravity() {
	if s.cfg.antigravityTokenFile == "" {
		return
	}
	rec, err := s.loadAGTokenFile()
	if err != nil {
		log.Printf("antigravity: no stored token (%v); run `aiusage-server ag-login` to authorize", err)
		return
	}
	if rec.RefreshToken == "" {
		log.Printf("antigravity: stored token has no refresh_token; run `aiusage-server ag-login`")
		return
	}
	s.setAntigravity(rec.AccessToken, rec.RefreshToken, rec.ExpiresAt)
	log.Printf("antigravity: loaded credentials from %s", s.cfg.antigravityTokenFile)
}

// ---- interactive login (headless: no browser on this machine) ----

// runAntigravityLogin performs a one-shot authorization from the terminal and
// writes the refresh token to disk. Two strategies are attempted in order:
//
//  1. OAuth 2.0 Device Authorization Grant -- zero copy/paste, but Google only
//     enables it for "TVs and Limited Input devices" client types, so an
//     Antigravity desktop client id will usually be rejected.
//  2. Authorization Code + PKCE with manual paste -- always works for desktop
//     clients. The operator opens the URL on any machine that has a browser,
//     approves, then copies the (failed-to-load) redirect URL back here.
func (s *server) runAntigravityLogin() error {
	if dc, err := s.requestDeviceCode(); err == nil {
		fmt.Println("== Antigravity authorization (device code) ==")
		fmt.Printf("1. Open on any device with a browser: %s\n", dc.VerificationURL)
		fmt.Printf("2. Enter code: %s\n", dc.UserCode)
		fmt.Println("   Waiting for approval...")
		if err := s.awaitDeviceApproval(dc); err == nil {
			return nil
		} else {
			fmt.Printf("device flow failed (%v), falling back to manual paste\n\n", err)
		}
	} else {
		fmt.Printf("device flow unavailable for this client (%v), using manual paste\n\n", err)
	}
	return s.runManualPKCELogin()
}

type deviceCodeResp struct {
	DeviceCode      string `json:"device_code"`
	UserCode        string `json:"user_code"`
	VerificationURL string `json:"verification_url"`
	ExpiresIn       int    `json:"expires_in"`
	Interval        int    `json:"interval"`
}

func (s *server) requestDeviceCode() (deviceCodeResp, error) {
	form := url.Values{}
	form.Set("client_id", s.cfg.antigravityClientID)
	form.Set("scope", agScopes)
	resp, err := s.http.Post(agDeviceCodeEndpoint, "application/x-www-form-urlencoded",
		strings.NewReader(form.Encode()))
	if err != nil {
		return deviceCodeResp{}, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return deviceCodeResp{}, fmt.Errorf("status %d: %s", resp.StatusCode, oauthErrOf(body))
	}
	var dc deviceCodeResp
	if err := json.Unmarshal(body, &dc); err != nil {
		return deviceCodeResp{}, err
	}
	if dc.DeviceCode == "" {
		return deviceCodeResp{}, fmt.Errorf("empty device_code")
	}
	if dc.Interval <= 0 {
		dc.Interval = 5
	}
	return dc, nil
}

func (s *server) awaitDeviceApproval(dc deviceCodeResp) error {
	deadline := time.Now().Add(time.Duration(max(dc.ExpiresIn, 300)) * time.Second)
	interval := time.Duration(dc.Interval) * time.Second
	for time.Now().Before(deadline) {
		at, rt, exp, err := s.exchangeDeviceCode(dc.DeviceCode)
		if err == nil {
			return s.persistLogin(at, rt, exp)
		}
		switch {
		case strings.Contains(err.Error(), "authorization_pending"):
			time.Sleep(interval)
		case strings.Contains(err.Error(), "slow_down"):
			interval += 5 * time.Second
			time.Sleep(interval)
		default:
			return err
		}
	}
	return fmt.Errorf("device code expired")
}

func (s *server) exchangeDeviceCode(deviceCode string) (string, string, int64, error) {
	form := url.Values{}
	form.Set("client_id", s.cfg.antigravityClientID)
	form.Set("client_secret", s.cfg.antigravityClientSecret)
	form.Set("device_code", deviceCode)
	form.Set("grant_type", agDeviceGrantType)
	return s.postTokenForm(form)
}

// runManualPKCELogin drives the copy/paste authorization-code flow.
func (s *server) runManualPKCELogin() error {
	verifier, challenge, err := newPKCEPair()
	if err != nil {
		return err
	}
	q := url.Values{}
	q.Set("client_id", s.cfg.antigravityClientID)
	q.Set("redirect_uri", agRedirectURI)
	q.Set("response_type", "code")
	q.Set("scope", agScopes)
	q.Set("access_type", "offline")
	q.Set("prompt", "consent")
	q.Set("code_challenge", challenge)
	q.Set("code_challenge_method", "S256")
	authURL := agAuthEndpoint + "?" + q.Encode()

	fmt.Println("== Antigravity authorization (manual paste) ==")
	fmt.Println("1. Open this URL in a browser on ANY machine:")
	fmt.Println()
	fmt.Println("   " + authURL)
	fmt.Println()
	fmt.Println("2. Approve access. The browser will then try to open")
	fmt.Println("   " + agRedirectURI + "/?code=... and show a connection error.")
	fmt.Println("   THAT IS EXPECTED -- the page does not need to load.")
	fmt.Println("3. Copy the ENTIRE address bar URL (or just the code= value)")
	fmt.Println("   and paste it below, then press Enter.")
	fmt.Println()
	fmt.Print("Paste redirect URL or code: ")

	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return fmt.Errorf("read input: %w", err)
	}
	code := extractAuthCode(strings.TrimSpace(line))
	if code == "" {
		return fmt.Errorf("no authorization code found in input")
	}

	form := url.Values{}
	form.Set("client_id", s.cfg.antigravityClientID)
	form.Set("client_secret", s.cfg.antigravityClientSecret)
	form.Set("code", code)
	form.Set("code_verifier", verifier)
	form.Set("redirect_uri", agRedirectURI)
	form.Set("grant_type", "authorization_code")
	at, rt, exp, err := s.postTokenForm(form)
	if err != nil {
		return err
	}
	return s.persistLogin(at, rt, exp)
}

// extractAuthCode accepts either a bare code or a full redirect URL.
func extractAuthCode(in string) string {
	if in == "" {
		return ""
	}
	if strings.Contains(in, "code=") {
		if u, err := url.Parse(in); err == nil {
			if c := u.Query().Get("code"); c != "" {
				return c
			}
		}
		// Not a parseable URL but still contains code=<...>
		part := in[strings.Index(in, "code=")+len("code="):]
		if i := strings.IndexAny(part, "&\r\n "); i >= 0 {
			part = part[:i]
		}
		if d, err := url.QueryUnescape(part); err == nil {
			return d
		}
		return part
	}
	if strings.HasPrefix(in, "http") {
		return ""
	}
	return in
}

func (s *server) persistLogin(access, refresh string, expiresAt int64) error {
	if refresh == "" {
		return fmt.Errorf("google returned no refresh_token (retry with prompt=consent)")
	}
	s.setAntigravity(access, refresh, expiresAt)
	if err := s.saveAGTokenFile(access, refresh, expiresAt); err != nil {
		return fmt.Errorf("authorized but could not save token: %w", err)
	}
	fmt.Printf("\nAuthorized. Token saved to %s\n", s.cfg.antigravityTokenFile)
	return nil
}

func newPKCEPair() (verifier, challenge string, err error) {
	buf := make([]byte, 48)
	if _, err = rand.Read(buf); err != nil {
		return "", "", err
	}
	verifier = base64.RawURLEncoding.EncodeToString(buf)
	sum := sha256.Sum256([]byte(verifier))
	challenge = base64.RawURLEncoding.EncodeToString(sum[:])
	return verifier, challenge, nil
}

// ---- token refresh ----

func (s *server) refreshAntigravityToken(refresh string) (string, string, int64, error) {
	form := url.Values{}
	form.Set("client_id", s.cfg.antigravityClientID)
	form.Set("client_secret", s.cfg.antigravityClientSecret)
	form.Set("refresh_token", refresh)
	form.Set("grant_type", "refresh_token")
	return s.postTokenForm(form)
}

func (s *server) postTokenForm(form url.Values) (string, string, int64, error) {
	resp, err := s.http.Post(agTokenEndpoint, "application/x-www-form-urlencoded",
		strings.NewReader(form.Encode()))
	if err != nil {
		return "", "", 0, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return "", "", 0, fmt.Errorf("%s", oauthErrOf(body))
	}
	var ok struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresIn    int64  `json:"expires_in"`
	}
	if err := json.Unmarshal(body, &ok); err != nil {
		return "", "", 0, err
	}
	if ok.AccessToken == "" {
		return "", "", 0, fmt.Errorf("token response has no access_token")
	}
	return ok.AccessToken, ok.RefreshToken, time.Now().Unix() + ok.ExpiresIn, nil
}

func oauthErrOf(body []byte) string {
	var fail struct {
		Error            string `json:"error"`
		ErrorDescription string `json:"error_description"`
	}
	if err := json.Unmarshal(body, &fail); err == nil && fail.Error != "" {
		if fail.ErrorDescription != "" {
			return fail.Error + ": " + fail.ErrorDescription
		}
		return fail.Error
	}
	if len(body) > 200 {
		body = body[:200]
	}
	return string(body)
}

// ---- quota fetch ----

type agModelQuota struct {
	ModelName         string  `json:"modelName"`
	DisplayName       string  `json:"displayName"`
	RemainingFraction float64 `json:"remainingFraction"`
	ResetTime         string  `json:"resetTime"`
	Percent           int     `json:"percent"`
	IsExhausted       bool    `json:"isExhausted"`
}

type agQuotaResponse struct {
	Tier      string         `json:"tier"`
	ProjectID string         `json:"projectId"`
	Models    []agModelQuota `json:"models"`
}

// validAccessToken returns a usable access token, refreshing when the cached
// one is missing or within 60s of expiry.
func (s *server) validAccessToken() (string, error) {
	access, refresh, expiresAt := s.antigravity()
	fresh := access != "" && (expiresAt == 0 || time.Now().Unix() < expiresAt-60)
	if fresh {
		return access, nil
	}
	if refresh == "" {
		return "", fmt.Errorf("antigravity not authorized; run `aiusage-server ag-login`")
	}
	at, rt, exp, err := s.refreshAntigravityToken(refresh)
	if err != nil {
		return "", err
	}
	if rt == "" {
		rt = refresh
	}
	s.setAntigravity(at, rt, exp)
	_ = s.saveAGTokenFile(at, rt, exp)
	return at, nil
}

func (s *server) fetchAntigravityQuota() (agQuotaResponse, error) {
	access, err := s.validAccessToken()
	if err != nil {
		return agQuotaResponse{}, err
	}
	tier, project, models, err := s.agFetch(access)
	if err == nil {
		return agQuotaResponse{Tier: tier, ProjectID: project, Models: models}, nil
	}
	// One forced-refresh retry covers a token revoked out from under us.
	_, refresh, _ := s.antigravity()
	if refresh == "" {
		return agQuotaResponse{}, err
	}
	at, rt, exp, rerr := s.refreshAntigravityToken(refresh)
	if rerr != nil {
		return agQuotaResponse{}, err
	}
	if rt == "" {
		rt = refresh
	}
	s.setAntigravity(at, rt, exp)
	_ = s.saveAGTokenFile(at, rt, exp)
	tier, project, models, err = s.agFetch(at)
	if err != nil {
		return agQuotaResponse{}, err
	}
	return agQuotaResponse{Tier: tier, ProjectID: project, Models: models}, nil
}

func (s *server) agFetch(access string) (string, string, []agModelQuota, error) {
	tier, project, err := s.agCallLoadCodeAssist(access)
	if err != nil {
		return "", "", nil, err
	}
	models, err := s.agCallFetchAvailableModels(access, project)
	if err != nil {
		return "", "", nil, err
	}
	return tier, project, models, nil
}

func (s *server) agCallLoadCodeAssist(access string) (string, string, error) {
	status, body, err := s.agPost(agCloudCodeHost, "/v1internal:loadCodeAssist", access,
		`{"metadata":{"ideType":"ANTIGRAVITY"}}`)
	if err != nil {
		return "", "", err
	}
	if status != http.StatusOK {
		return "", "", fmt.Errorf("loadCodeAssist status %d", status)
	}
	var out struct {
		CurrentTier struct {
			ID string `json:"id"`
		} `json:"currentTier"`
		PaidTier struct {
			ID string `json:"id"`
		} `json:"paidTier"`
		CloudaicompanionProject string `json:"cloudaicompanionProject"`
	}
	if err := json.Unmarshal([]byte(body), &out); err != nil {
		return "", "", err
	}
	// Reference implementation prefers paidTier over currentTier.
	tier := out.PaidTier.ID
	if tier == "" {
		tier = out.CurrentTier.ID
	}
	if tier == "" {
		tier = "FREE"
	}
	return tier, out.CloudaicompanionProject, nil
}

func (s *server) agCallFetchAvailableModels(access, project string) ([]agModelQuota, error) {
	payload := map[string]string{}
	if project != "" {
		payload["project"] = project
	}
	b, _ := json.Marshal(payload)
	status, body, err := s.agPost(agCloudCodeHost, "/v1internal:fetchAvailableModels", access, string(b))
	if err != nil {
		return nil, err
	}
	if status != http.StatusOK {
		return nil, fmt.Errorf("fetchAvailableModels status %d", status)
	}
	return parseAvailableModels(body)
}

func (s *server) agPost(host, path, access, body string) (int, string, error) {
	req, err := http.NewRequest(http.MethodPost, "https://"+host+path, strings.NewReader(body))
	if err != nil {
		return 0, "", err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer "+access)
	req.Header.Set("User-Agent", "AIUsage/1.0")
	resp, err := s.http.Do(req)
	if err != nil {
		return 0, "", err
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return resp.StatusCode, "", err
	}
	return resp.StatusCode, string(b), nil
}

// Keep only models the UI cares about, mirroring the reference client.
var (
	agAllowedModel  = regexp.MustCompile(`(?i)gemini|claude|gpt`)
	agGeminiVersion = regexp.MustCompile(`gemini-(\d+(?:\.\d+)?)`)
	agVersionPair   = regexp.MustCompile(`(\d+)-(\d+)`)
)

func parseAvailableModels(body string) ([]agModelQuota, error) {
	var root struct {
		Models map[string]struct {
			QuotaInfo *struct {
				// Pointer so a missing remainingFraction is distinguishable;
				// the reference treats absent as 0 (quota exhausted).
				RemainingFraction *float64 `json:"remainingFraction"`
				ResetTime         string   `json:"resetTime"`
			} `json:"quotaInfo"`
		} `json:"models"`
	}
	if err := json.Unmarshal([]byte(body), &root); err != nil {
		return nil, err
	}
	out := make([]agModelQuota, 0, len(root.Models))
	for name, m := range root.Models {
		if !agAllowedModel.MatchString(name) || !modelVersionSupported(name) {
			continue
		}
		if m.QuotaInfo == nil {
			continue // no quota reported -> nothing to show
		}
		frac := 0.0
		if m.QuotaInfo.RemainingFraction != nil {
			frac = *m.QuotaInfo.RemainingFraction
		}
		reset := m.QuotaInfo.ResetTime
		if reset == "" {
			reset = time.Now().Add(24 * time.Hour).UTC().Format(time.RFC3339)
		}
		out = append(out, agModelQuota{
			ModelName:         name,
			DisplayName:       friendlyModelName(name),
			RemainingFraction: frac,
			ResetTime:         reset,
			Percent:           int(math.Round(frac * 100)),
			IsExhausted:       frac <= 0,
		})
	}
	// Map iteration order is randomised in Go; sort so the plugin sees a
	// stable ordering across polls.
	sort.Slice(out, func(i, j int) bool { return out[i].ModelName < out[j].ModelName })
	return out, nil
}

// modelVersionSupported filters out pre-3.0 Gemini models. Non-Gemini models
// (Claude, GPT) are always supported.
func modelVersionSupported(name string) bool {
	lower := strings.ToLower(name)
	if !strings.Contains(lower, "gemini") {
		return true
	}
	m := agGeminiVersion.FindStringSubmatch(lower)
	if len(m) < 2 {
		return false // unversioned "gemini-pro" means 1.0
	}
	v, err := strconv.ParseFloat(m[1], 64)
	if err != nil {
		return false
	}
	return v >= 3.0
}

// friendlyModelName turns "claude-3-5-sonnet" into "Claude 3.5 Sonnet".
func friendlyModelName(raw string) string {
	fixed := agVersionPair.ReplaceAllString(raw, "$1.$2")
	parts := strings.Split(fixed, "-")
	for i, p := range parts {
		if p == "" || (p[0] >= '0' && p[0] <= '9') {
			continue // numeric segments stay as-is
		}
		parts[i] = strings.ToUpper(p[:1]) + p[1:]
	}
	return strings.Join(parts, " ")
}

// ---- token file persistence ----

func (s *server) loadAGTokenFile() (agTokenRecord, error) {
	b, err := os.ReadFile(s.cfg.antigravityTokenFile)
	if err != nil {
		return agTokenRecord{}, err
	}
	var rec agTokenRecord
	if err := json.Unmarshal(b, &rec); err != nil {
		return agTokenRecord{}, err
	}
	return rec, nil
}

func (s *server) saveAGTokenFile(access, refresh string, expiresAt int64) error {
	if s.cfg.antigravityTokenFile == "" {
		return nil
	}
	rec := agTokenRecord{AccessToken: access, RefreshToken: refresh, ExpiresAt: expiresAt}
	b, err := json.MarshalIndent(rec, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(s.cfg.antigravityTokenFile, b, 0o600)
}

// ---- HTTP handler ----

func (s *server) handleAntigravityQuota(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if !s.checkAuth(r) {
		w.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}

	const key = "antigravity:quota"
	if cached, ok := s.getCached(key); ok {
		w.Header().Set("X-AIUsage-Cache", "HIT")
		s.writeResponse(w, cached.statusCode, cached.body)
		return
	}

	q, err := s.fetchAntigravityQuota()
	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadGateway)
		_, _ = w.Write([]byte(`{"error":` + strconv.Quote(err.Error()) + `}`))
		return
	}
	body, err := json.Marshal(q)
	if err != nil {
		http.Error(w, "encode failed", http.StatusInternalServerError)
		return
	}
	s.putCached(key, http.StatusOK, body, s.cfg.cacheUsage)
	w.Header().Set("X-AIUsage-Cache", "MISS")
	s.writeResponse(w, http.StatusOK, body)
}
