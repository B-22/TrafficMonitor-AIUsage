#pragma once
#include <Windows.h>
#include <winhttp.h>
#include <string>

struct ProxyConfig {
    std::wstring explicitProxy;       // user-specified proxy from AIUsage.ini
    bool systemProxyDetected = false; // whether system proxy was found (informational)
    bool proxyActive = false;         // whether a proxy is configured
    std::wstring statusMessage;       // status description for tooltip
};

// Detect proxy configuration (informational, never blocks requests)
ProxyConfig DetectProxy(const std::wstring& explicitProxy, bool requireProxy);

// Create HTTP session with appropriate proxy settings
HINTERNET OpenHttpSession(const ProxyConfig& config, const wchar_t* userAgent);

// Connectivity test result
struct ConnectivityResult {
    bool directReachable = false;      // can reach api.anthropic.com directly (no proxy)
    bool proxyReachable = false;       // can reach via system proxy
    DWORD directError = 0;             // WinHTTP error code for direct attempt
    std::wstring statusMessage;        // human-readable summary
};

// Test connectivity to Claude/Codex API endpoints
// Helps users with VPN/TUN determine if they need a proxy
ConnectivityResult TestConnectivity();
