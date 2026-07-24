#include "ProxyHelper.h"

#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

ProxyConfig DetectProxy(const std::wstring& explicitProxy, bool requireProxy) {
    ProxyConfig config;
    config.requireProxy = requireProxy;
    config.explicitProxy = explicitProxy;

    // If an explicit proxy is provided, mark it active immediately
    if (!explicitProxy.empty()) {
        config.proxyActive = true;
        config.statusMessage = L"Using explicit proxy: " + explicitProxy;
        return config;
    }

    // Check IE proxy config (per-user)
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieConfig{};
    if (WinHttpGetIEProxyConfigForCurrentUser(&ieConfig)) {
        if (ieConfig.lpszProxy) {
            config.systemProxyDetected = true;
            config.proxyActive = true;
            config.statusMessage = L"System proxy detected: ";
            config.statusMessage += ieConfig.lpszProxy;
            GlobalFree(ieConfig.lpszProxy);
        }
        if (ieConfig.lpszAutoConfigUrl) {
            config.systemProxyDetected = true;
            config.proxyActive = true;
            if (config.statusMessage.empty()) {
                config.statusMessage = L"System auto-proxy URL detected";
            }
            GlobalFree(ieConfig.lpszAutoConfigUrl);
        }
        if (ieConfig.lpszProxyBypass) {
            GlobalFree(ieConfig.lpszProxyBypass);
        }
    }

    // If still not active, check default (system-wide) proxy
    if (!config.proxyActive) {
        WINHTTP_PROXY_INFO defProxy{};
        if (WinHttpGetDefaultProxyConfiguration(&defProxy)) {
            if (defProxy.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && defProxy.lpszProxy) {
                config.systemProxyDetected = true;
                config.proxyActive = true;
                config.statusMessage = L"Default system proxy detected: ";
                config.statusMessage += defProxy.lpszProxy;
            }
            if (defProxy.lpszProxy) GlobalFree(defProxy.lpszProxy);
            if (defProxy.lpszProxyBypass) GlobalFree(defProxy.lpszProxyBypass);
        }
    }

    // Warn if proxy is required but none was found
    if (requireProxy && !config.proxyActive) {
        config.statusMessage = L"Proxy required but not configured. Set ProxyServer in AIUsage.ini";
    }

    return config;
}

HINTERNET OpenHttpSession(const ProxyConfig& config, const wchar_t* userAgent) {
    if (!config.explicitProxy.empty()) {
        // Named proxy mode
        return WinHttpOpen(userAgent,
            WINHTTP_ACCESS_TYPE_NAMED_PROXY,
            config.explicitProxy.c_str(),
            WINHTTP_NO_PROXY_BYPASS, 0);
    }

    // Automatic proxy detection (WPAD + IE settings)
    return WinHttpOpen(userAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
}
