#include "ProxyHelper.h"

#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

ProxyConfig DetectProxy(const std::wstring& explicitProxy, bool /*requireProxy*/) {
    ProxyConfig config;
    config.explicitProxy = explicitProxy;

    // If an explicit proxy is provided, use it
    if (!explicitProxy.empty()) {
        config.proxyActive = true;
        config.statusMessage = L"Using explicit proxy: " + explicitProxy;
        return config;
    }

    // Check IE proxy config (per-user) — informational only, never blocks
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieConfig{};
    if (WinHttpGetIEProxyConfigForCurrentUser(&ieConfig)) {
        if (ieConfig.lpszProxy) {
            config.systemProxyDetected = true;
            config.proxyActive = true;
            config.statusMessage = L"System proxy detected";
            GlobalFree(ieConfig.lpszProxy);
        }
        if (ieConfig.lpszAutoConfigUrl) {
            config.systemProxyDetected = true;
            config.proxyActive = true;
            if (config.statusMessage.empty())
                config.statusMessage = L"Auto-proxy URL detected";
            GlobalFree(ieConfig.lpszAutoConfigUrl);
        }
        if (ieConfig.lpszProxyBypass) GlobalFree(ieConfig.lpszProxyBypass);
    }

    // Always allow requests — TUN/VPN/proxy tools work at network level,
    // not visible through WinHTTP proxy detection.
    // WinHttpOpen with AUTOMATIC_PROXY will route through system network stack.
    return config;
}

HINTERNET OpenHttpSession(const ProxyConfig& config, const wchar_t* userAgent) {
    if (!config.explicitProxy.empty()) {
        return WinHttpOpen(userAgent,
            WINHTTP_ACCESS_TYPE_NAMED_PROXY,
            config.explicitProxy.c_str(),
            WINHTTP_NO_PROXY_BYPASS, 0);
    }
    // AUTOMATIC_PROXY respects system proxy settings AND routes through
    // the system network stack, which includes TUN/VPN adapters.
    return WinHttpOpen(userAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
}

// Helper: try a lightweight HEAD request to a host, return true if reachable
static bool ProbeHost(const wchar_t* host, INTERNET_PORT port, const wchar_t* path,
                      DWORD accessType, const wchar_t* proxy, DWORD* outError) {
    if (outError) *outError = 0;
    HINTERNET session = WinHttpOpen(L"AIUsageProbe/1.0", accessType,
        proxy ? proxy : WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { if (outError) *outError = GetLastError(); return false; }

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (connect) {
        HINTERNET request = WinHttpOpenRequest(connect, L"HEAD", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request) {
            WinHttpSetTimeouts(request, 8000, 8000, 8000, 10000);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                if (WinHttpReceiveResponse(request, nullptr)) {
                    DWORD status = 0, sz = sizeof(status);
                    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                    // Any HTTP response (even 401/403) means the host is reachable
                    ok = (status > 0);
                }
            }
            if (!ok && outError) *outError = GetLastError();
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    } else {
        if (outError) *outError = GetLastError();
    }
    WinHttpCloseHandle(session);
    return ok;
}

ConnectivityResult TestConnectivity() {
    ConnectivityResult result;

    // Test 1: Direct connection (no proxy, bypasses system proxy)
    result.directReachable = ProbeHost(
        L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT,
        L"/api/oauth/usage", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, &result.directError);

    // Test 2: Via system proxy (automatic)
    result.proxyReachable = ProbeHost(
        L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT,
        L"/api/oauth/usage", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr);

    if (result.directReachable) {
        result.statusMessage = L"Direct connection OK (no proxy needed)";
    } else if (result.proxyReachable) {
        result.statusMessage = L"Reachable via proxy only";
    } else {
        result.statusMessage = L"Cannot reach API (check network/proxy)";
    }

    return result;
}
