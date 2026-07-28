#pragma once
#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

struct ProxyConfig {
    std::wstring explicitProxy;       // user-specified proxy from AIUsage.ini
    bool systemProxyDetected = false; // whether system proxy was found (informational)
    bool proxyActive = false;         // whether a proxy is configured
    bool requireProxy = false;        // fail closed when no explicit/system proxy is detected
    std::vector<std::wstring> allowedExitIps; // exact public IPv4/IPv6 allowlist
    std::wstring exitIpCheckUrl;       // HTTPS endpoint returning the caller IP as plain text
    bool verifyTargetHost = true;      // use target-host /cdn-cgi/trace when available
    std::wstring statusMessage;       // status description for tooltip
};

// Detect proxy configuration and build fail-closed policy.
ProxyConfig DetectProxy(const std::wstring& explicitProxy, bool requireProxy,
                        const std::wstring& allowedExitIps = L"",
                        const std::wstring& exitIpCheckUrl = L"https://api.ipify.org/");

// Create an HTTP session with appropriate proxy settings. When a policy check
// fails, returns nullptr and writes a human-readable reason to errorMessage.
HINTERNET OpenHttpSession(const ProxyConfig& config, const wchar_t* userAgent,
                          const wchar_t* targetHost = nullptr,
                          std::wstring* errorMessage = nullptr);

// Validate the public exit IP through the same target-domain routing policy.
bool VerifyExitIp(const ProxyConfig& config, const wchar_t* targetHost,
                  std::wstring* observedIp, std::wstring* errorMessage);

// Parse either a plain IP response or a Cloudflare trace body containing ip=.
std::wstring ExtractExitIp(const std::wstring& responseBody);

// Connectivity test result
struct ConnectivityResult {
    bool directReachable = false;      // can reach api.anthropic.com directly (no proxy)
    bool proxyReachable = false;       // can reach via system proxy
    bool exitIpVerified = false;       // current public IP matched AllowedExitIPs
    std::wstring observedExitIp;       // public IP returned by the check endpoint
    DWORD directError = 0;             // WinHTTP error code for direct attempt
    std::wstring statusMessage;        // human-readable summary
};

// Test connectivity to Claude/Codex API endpoints
// Helps users with VPN/TUN determine if they need a proxy
ConnectivityResult TestConnectivity(const ProxyConfig& config);
