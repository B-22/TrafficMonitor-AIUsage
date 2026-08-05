// ag-login: one-time Google OAuth authorizer for the AIUsage plugin's
// Antigravity (Google Cloud Code) quota fetcher.
//
// The plugin lives in a Windows tray host with no browser, so it cannot run
// the interactive consent flow itself. This standalone helper does the whole
// flow on the local machine where a browser IS available:
//
//   1. Start a tiny HTTP listener on 127.0.0.1:<port> (default 8721, auto
//      picks the next free port up to +9).
//   2. Open the browser at Google's consent page (PKCE S256, offline access
//      so a refresh_token is issued).
//   3. Google redirects to http://localhost:<port>/callback?code=...
//   4. Exchange the code for access_token + refresh_token.
//   5. Write the tokens back into [Antigravity] of AIUsage.ini, preserving
//      every other line byte-for-byte.
//
// After running this once, the plugin refreshes the access token itself
// through the public token endpoint; no further manual steps are needed.
//
// Usage:
//   ag-login.exe [-ini <path>] [-port <n>] [-client-id <id>] [-client-secret <s>]
//
// The ini file is auto-located: -ini flag, then the directory of this exe,
// then %APPDATA%\TrafficMonitor, then the current working directory.
package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const (
	defaultClientID     = "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com"
	defaultClientSecret = "" // kept out of git (GitHub secret-scan); use -client-secret or AIUSAGE_AG_CLIENT_SECRET
	authEndpoint        = "https://accounts.google.com/o/oauth2/v2/auth"
	tokenEndpoint       = "https://oauth2.googleapis.com/token"
	defaultScopes       = "https://www.googleapis.com/auth/cloud-platform " +
		"https://www.googleapis.com/auth/userinfo.email " +
		"https://www.googleapis.com/auth/userinfo.profile " +
		"https://www.googleapis.com/auth/cclog " +
		"https://www.googleapis.com/auth/experimentsandconfigs"
	consentTimeout = 5 * time.Minute
)

func fatalf(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	fmt.Fprintf(os.Stderr, "错误: %s\n", msg)
	// Modal box keeps double-clicked runs visible; without a console the
	// process would otherwise appear to "crash-close" instantly.
	msgBox("ag-login 错误", msg, 0x00000010 /* MB_ICONERROR */)
	os.Exit(1)
}

// inform shows a modal success/info box so a double-clicked run ends with
// visible feedback instead of a silent exit.
func inform(title, text string) {
	fmt.Printf("%s\n", text)
	msgBox(title, text, 0x00000040 /* MB_ICONINFORMATION */)
}

// ---- PKCE ----

func randBytes(n int) ([]byte, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return nil, err
	}
	return b, nil
}

func b64url(b []byte) string {
	return base64.RawURLEncoding.EncodeToString(b)
}

func newPKCEPair() (verifier, challenge string, err error) {
	raw, err := randBytes(32)
	if err != nil {
		return "", "", err
	}
	verifier = b64url(raw)
	sum := sha256.Sum256([]byte(verifier))
	challenge = b64url(sum[:])
	return verifier, challenge, nil
}

// ---- ini handling (byte-preserving) ----

func locateIni(explicit string) (string, error) {
	candidates := []string{explicit}
	if exe, err := os.Executable(); err == nil {
		candidates = append(candidates, filepath.Join(filepath.Dir(exe), "AIUsage.ini"))
	}
	if ad := os.Getenv("APPDATA"); ad != "" {
		candidates = append(candidates, filepath.Join(ad, "TrafficMonitor", "AIUsage.ini"))
	}
	if cwd, err := os.Getwd(); err == nil {
		candidates = append(candidates, filepath.Join(cwd, "AIUsage.ini"))
	}
	for _, c := range candidates {
		if c == "" {
			continue
		}
		if st, err := os.Stat(c); err == nil && !st.IsDir() {
			return c, nil
		}
	}
	return "", fmt.Errorf("找不到 AIUsage.ini。\n\n"+
		"已尝试:\n  %s\n\n"+
		"解决办法（任选其一）:\n"+
		"  1. 把本程序复制到 TrafficMonitor.exe 所在目录后双击\n"+
		"     （与 AIUsage.ini 同目录，自动识别）\n"+
		"  2. 在命令行指定路径运行:\n"+
		"     ag-login.exe -ini \"<AIUsage.ini 完整路径>\"",
		strings.Join(nonEmpty(candidates), "\n  "))
}

func nonEmpty(in []string) []string {
	var out []string
	for _, s := range in {
		if s != "" {
			out = append(out, s)
		}
	}
	return out
}

// updateIniSection rewrites key=value lines inside [section] (case-insensitive
// section header), preserving every other line byte-for-byte. Keys already
// present are replaced in place; missing keys are appended at the end of the
// section. If the section does not exist it is created at the end of the file.
// The returned text is UTF-8; the caller writes it back as-is.
func updateIniSection(data []byte, section string, kv map[string]string) []byte {
	text := string(data)
	newline := "\n"
	if strings.Contains(text, "\r\n") {
		newline = "\r\n"
	}
	lines := strings.Split(text, newline)

	inSection := false
	sectionHeader := -1
	sectionEnd := -1 // index of last non-blank content line inside section
	replaced := map[string]bool{}
	for i := 0; i < len(lines); i++ {
		ln := strings.TrimSpace(lines[i])
		trimmed := strings.TrimRight(ln, "\r")
		if strings.HasPrefix(trimmed, "[") && strings.HasSuffix(trimmed, "]") {
			inSection = strings.EqualFold(trimmed, "["+section+"]")
			if inSection {
				sectionHeader = i
				sectionEnd = i
			}
			continue
		}
		if !inSection {
			continue
		}
		if trimmed == "" {
			continue // blank separators are not section content
		}
		sectionEnd = i
		indent := lines[i][:len(lines[i])-len(strings.TrimLeft(lines[i], " \t"))]
		for k, v := range kv {
			if strings.HasPrefix(trimmed, k+"=") || strings.HasPrefix(trimmed, k+" =") {
				lines[i] = indent + k + "=" + v
				replaced[k] = true
				break
			}
		}
	}

	// Append missing keys just before the section's closing point.
	var missing []string
	for k, v := range kv {
		if !replaced[k] {
			missing = append(missing, k+"="+v)
		}
	}
	if len(missing) > 0 {
		if sectionHeader == -1 {
			// Section was never found: append a fresh one.
			lines = append(lines, "")
			lines = append(lines, "["+section+"]")
			lines = append(lines, missing...)
		} else {
			// Section exists; insert after its last content line.
			insertAt := sectionEnd + 1
			var out []string
			out = append(out, lines[:insertAt]...)
			out = append(out, missing...)
			out = append(out, lines[insertAt:]...)
			lines = out
		}
	}
	return []byte(strings.Join(lines, newline))
}

func sectionExists(text, section string) bool {
	for _, ln := range strings.Split(text, "\n") {
		t := strings.TrimSpace(strings.TrimRight(ln, "\r"))
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			if strings.EqualFold(t, "["+section+"]") {
				return true
			}
		}
	}
	return false
}

// ---- browser ----

func openBrowser(url string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	return nil
}

// ---- OAuth token exchange ----

type tokenResponse struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresIn    int64  `json:"expires_in"`
	TokenType    string `json:"token_type"`
	Error        string `json:"error"`
	ErrorDesc    string `json:"error_description"`
}

func exchangeCode(code, verifier, clientID, clientSecret, redirectURI string) (*tokenResponse, error) {
	form := url.Values{
		"grant_type":    {"authorization_code"},
		"code":          {code},
		"client_id":     {clientID},
		"client_secret": {clientSecret},
		"redirect_uri":  {redirectURI},
		"code_verifier": {verifier},
	}
	req, err := http.NewRequest(http.MethodPost, tokenEndpoint, strings.NewReader(form.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("User-Agent", "ag-login/1.0 (AIUsage Antigravity authorizer)")

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, err
	}
	var tr tokenResponse
	if err := json.Unmarshal(body, &tr); err != nil {
		return nil, fmt.Errorf("token 响应解析失败 (HTTP %d): %s", resp.StatusCode, truncate(string(body), 300))
	}
	if tr.Error != "" {
		return nil, fmt.Errorf("Google 返回 %s: %s", tr.Error, tr.ErrorDesc)
	}
	if resp.StatusCode != 200 || tr.AccessToken == "" {
		return nil, fmt.Errorf("token 请求失败 (HTTP %d): %s", resp.StatusCode, truncate(string(body), 300))
	}
	return &tr, nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}

// ---- main flow ----

func main() {
	var (
		iniPath      = flag.String("ini", "", "AIUsage.ini 路径（默认自动查找）")
		port         = flag.Int("port", 8721, "localhost 回调端口（占用时自动 +1 重试）")
		clientID     = flag.String("client-id", defaultClientID, "OAuth client id（覆盖内置默认值）")
		clientSecret = flag.String("client-secret", "", "OAuth client secret（默认读环境变量 AIUSAGE_AG_CLIENT_SECRET）")
	)
	flag.Parse()

	secret := *clientSecret
	if secret == "" {
		secret = os.Getenv("AIUSAGE_AG_CLIENT_SECRET")
	}
	if secret == "" {
		fatalf("未提供 OAuth client secret：请用 -client-secret 参数，或设置环境变量 AIUSAGE_AG_CLIENT_SECRET")
	}

	ini, err := locateIni(*iniPath)
	if err != nil {
		fatalf("%v", err)
	}

	verifier, challenge, err := newPKCEPair()
	if err != nil {
		fatalf("生成 PKCE 参数失败: %v", err)
	}

	// Find a free localhost port starting at *port.
	ln, listenerPort, err := listenLocalhost(*port)
	if err != nil {
		fatalf("无法监听本地端口 %d-%d: %v", *port, *port+9, err)
	}
	defer ln.Close()

	redirectURI := fmt.Sprintf("http://localhost:%d/callback", listenerPort)

	authURL := fmt.Sprintf("%s?%s",
		authEndpoint,
		url.Values{
			"client_id":             {*clientID},
			"redirect_uri":          {redirectURI},
			"response_type":         {"code"},
			"scope":                 {defaultScopes},
			"code_challenge":        {challenge},
			"code_challenge_method": {"S256"},
			"access_type":           {"offline"},
			"prompt":                {"consent"},
		}.Encode())

	// Callback server: receives the authorization code.
	codeCh := make(chan string, 1)
	errCh := make(chan error, 1)
	mux := http.NewServeMux()
	mux.HandleFunc("/callback", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		if e := q.Get("error"); e != "" {
			errCh <- fmt.Errorf("用户在 Google 页面拒绝了授权: %s (%s)", e, q.Get("error_description"))
			w.WriteHeader(http.StatusOK)
			_, _ = io.WriteString(w, "授权已取消，可以关闭本窗口。")
			return
		}
		code := q.Get("code")
		if code == "" {
			errCh <- errors.New("回调缺少 code 参数")
			w.WriteHeader(http.StatusBadRequest)
			_, _ = io.WriteString(w, "回调缺少 code 参数")
			return
		}
		codeCh <- code
		_, _ = io.WriteString(w, "授权成功！可以关闭本窗口并返回命令行。")
	})
	srv := &http.Server{Handler: mux}
	go func() { _ = srv.Serve(ln) }()
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
	}()

	fmt.Printf("AIUsage Antigravity 授权工具\n")
	fmt.Printf("  回调地址 : %s\n", redirectURI)
	fmt.Printf("  配置文件 : %s\n", ini)
	fmt.Printf("  正在打开浏览器完成 Google 登录…（5 分钟内有效，Ctrl+C 可取消）\n")
	if err := openBrowser(authURL); err != nil {
		fmt.Printf("  无法自动打开浏览器，请手动访问:\n  %s\n", authURL)
		msgBox("ag-login — 请手动打开浏览器",
			"无法自动打开浏览器。\n\n请复制以下地址到浏览器访问（5 分钟内有效）:\n\n"+authURL,
			0x00000000 /* MB_OK */)
	}

	var code string
	select {
	case code = <-codeCh:
	case err := <-errCh:
		fatalf("%v", err)
	case <-time.After(consentTimeout):
		fatalf("等待授权超时（%v），请重新运行。", consentTimeout)
	}

	fmt.Printf("  授权成功，正在换取令牌…\n")
	tr, err := exchangeCode(code, verifier, *clientID, secret, redirectURI)
	if err != nil {
		fatalf("%v", err)
	}

	raw, err := os.ReadFile(ini)
	if err != nil {
		fatalf("读取 %s 失败: %v", ini, err)
	}
	kv := map[string]string{
		"ClientId":     *clientID,
		"AccessToken":  tr.AccessToken,
		"RefreshToken": tr.RefreshToken,
	}
	if tr.RefreshToken == "" {
		fmt.Printf("  警告: Google 未返回 refresh_token（可能此前已授权过且未过期）\n")
		delete(kv, "RefreshToken")
	}
	updated := updateIniSection(raw, "Antigravity", kv)
	if err := os.WriteFile(ini, updated, 0644); err != nil {
		fatalf("写入 %s 失败: %v", ini, err)
	}

	fmt.Printf("  完成！已写入 %s 的 [Antigravity] 段:\n", ini)
	fmt.Printf("    ClientId=%s\n", *clientID)
	fmt.Printf("    AccessToken=%s…\n", short(tr.AccessToken, 12))
	if tr.RefreshToken != "" {
		fmt.Printf("    RefreshToken=%s…\n", short(tr.RefreshToken, 12))
	}
		fmt.Printf("  令牌已持久化，插件将自动刷新 AccessToken。重启 TrafficMonitor 后即可看到 Antigravity 配额。\n")
	inform("ag-login 授权完成",
		fmt.Sprintf("已写入:\n%s\n\n[Antigravity]\nClientId=%s\nAccessToken=%s…\nRefreshToken=%s…\n\n重启 TrafficMonitor 后即可看到 Antigravity 配额。",
			ini, *clientID, short(tr.AccessToken, 12), short(tr.RefreshToken, 12)))
}

func listenLocalhost(base int) (net.Listener, int, error) {
	for i := 0; i < 10; i++ {
		p := base + i
		ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", p))
		if err == nil {
			return ln, p, nil
		}
	}
	return nil, 0, errors.New("端口全部被占用")
}

func short(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}
