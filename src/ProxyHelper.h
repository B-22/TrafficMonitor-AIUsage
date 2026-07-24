#pragma once
#include <Windows.h>
#include <winhttp.h>
#include <string>

struct ProxyConfig {
    bool requireProxy = true;
    std::wstring explicitProxy;
    bool systemProxyDetected = false;
    bool proxyActive = false;
    std::wstring statusMessage;
};

// 检测代理配置
ProxyConfig DetectProxy(const std::wstring& explicitProxy, bool requireProxy);

// 创建 HTTP 会话，自动应用代理配置
HINTERNET OpenHttpSession(const ProxyConfig& config, const wchar_t* userAgent);
