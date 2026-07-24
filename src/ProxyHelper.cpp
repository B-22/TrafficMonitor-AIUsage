#include "ProxyHelper.h"

#include <winhttp.h>

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
