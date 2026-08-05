#include "ProxyHelper.h"

#include <winhttp.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

std::wstring Trim(const std::wstring& value) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return first < last ? std::wstring(first, last) : std::wstring();
}

std::vector<std::wstring> SplitIpList(const std::wstring& value) {
    std::vector<std::wstring> result;
    std::wstring token;
    for (wchar_t ch : value) {
        if (ch == L',' || ch == L';' || std::iswspace(ch)) {
            token = Trim(token);
            if (!token.empty()) result.push_back(token);
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    token = Trim(token);
    if (!token.empty()) result.push_back(token);
    return result;
}

bool ParseIp(const std::wstring& text, int* family, std::vector<unsigned char>* bytes) {
    IN_ADDR ipv4{};
    if (InetPtonW(AF_INET, text.c_str(), &ipv4) == 1) {
        *family = AF_INET;
        const auto* first = reinterpret_cast<const unsigned char*>(&ipv4);
        bytes->assign(first, first + sizeof(ipv4));
        return true;
    }

    IN6_ADDR ipv6{};
    if (InetPtonW(AF_INET6, text.c_str(), &ipv6) == 1) {
        *family = AF_INET6;
        const auto* first = reinterpret_cast<const unsigned char*>(&ipv6);
        bytes->assign(first, first + sizeof(ipv6));
        return true;
    }
    return false;
}

bool IpEquals(const std::wstring& left, const std::wstring& right) {
    int leftFamily = 0;
    int rightFamily = 0;
    std::vector<unsigned char> leftBytes;
    std::vector<unsigned char> rightBytes;
    return ParseIp(left, &leftFamily, &leftBytes)
        && ParseIp(right, &rightFamily, &rightBytes)
        && leftFamily == rightFamily
        && leftBytes == rightBytes;
}

HINTERNET OpenRawSession(const ProxyConfig& config, const wchar_t* userAgent) {
    if (!config.explicitProxy.empty()) {
        return WinHttpOpen(userAgent,
            WINHTTP_ACCESS_TYPE_NAMED_PROXY,
            config.explicitProxy.c_str(),
            WINHTTP_NO_PROXY_BYPASS, 0);
    }
    return WinHttpOpen(userAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
}

} // namespace

ProxyConfig DetectProxy(const std::wstring& explicitProxy, bool requireProxy,
                        const std::wstring& allowedExitIps,
                        const std::wstring& exitIpCheckUrl) {
    ProxyConfig config;
    config.explicitProxy = Trim(explicitProxy);
    config.requireProxy = requireProxy;
    config.allowedExitIps = SplitIpList(allowedExitIps);
    config.exitIpCheckUrl = Trim(exitIpCheckUrl);
    if (config.exitIpCheckUrl.empty()) {
        config.exitIpCheckUrl = L"https://api.ipify.org/";
    }

    // If an explicit proxy is provided, use it
    if (!config.explicitProxy.empty()) {
        config.proxyActive = true;
        config.statusMessage = L"Using explicit proxy: " + config.explicitProxy;
        if (!config.allowedExitIps.empty()) {
            config.statusMessage += L" (exit IP locked)";
        }
        return config;
    }

    // Check IE proxy config (per-user). TUN/VPN adapters are intentionally not
    // treated as system proxies; use AllowedExitIPs to validate those paths.
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieConfig{};
    if (WinHttpGetIEProxyConfigForCurrentUser(&ieConfig)) {
        if (ieConfig.fAutoDetect) {
            config.systemProxyDetected = true;
            config.proxyActive = true;
            config.statusMessage = L"System auto-proxy detected";
        }
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

    if (!config.allowedExitIps.empty()) {
        config.statusMessage = L"TUN/VPN exit IP lock enabled";
    } else if (config.requireProxy && !config.proxyActive) {
        config.statusMessage = L"BLOCKED: required proxy not detected";
    }
    return config;
}

std::wstring ExtractExitIp(const std::wstring& responseBody) {
    std::wstring candidate = Trim(responseBody);
    int family = 0;
    std::vector<unsigned char> bytes;
    if (ParseIp(candidate, &family, &bytes)) return candidate;

    size_t lineStart = 0;
    while (lineStart < responseBody.size()) {
        const size_t lineEnd = responseBody.find_first_of(L"\r\n", lineStart);
        const std::wstring line = Trim(responseBody.substr(
            lineStart, lineEnd - lineStart));
        if (line.rfind(L"ip=", 0) == 0) {
            candidate = Trim(line.substr(3));
            if (ParseIp(candidate, &family, &bytes)) return candidate;
        }
        if (lineEnd == std::wstring::npos) break;
        lineStart = lineEnd + 1;
    }
    return {};
}

// Outcome of a single exit-IP probe.
//   Match       - the endpoint answered and the exit IP is whitelisted.
//   Mismatch    - the endpoint answered but the exit IP is NOT whitelisted.
//   Unavailable - the endpoint could not be used at all (non-200, unparsable
//                 body, network failure). This is distinct from Mismatch so
//                 the caller can fall back to another probe URL instead of
//                 blocking outright.
enum class ExitIpProbe { Match, Mismatch, Unavailable };

static ExitIpProbe ProbeExitIpOnce(const ProxyConfig& config,
                                   const std::wstring& checkUrl,
                                   std::wstring* observedIp) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(checkUrl.c_str(), 0, 0, &parts)
        || parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0) {
        return ExitIpProbe::Unavailable;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    HINTERNET session = OpenRawSession(config, L"AIUsageExitIpCheck/1.0");
    if (!session) return ExitIpProbe::Unavailable;

    ExitIpProbe outcome = ExitIpProbe::Unavailable;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = nullptr;
    if (connect) {
        request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request) {
        constexpr DWORD timeout = 5000;
        WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
        DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
            &disableRedirects, sizeof(disableRedirects));

        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX);

            if (status == 200) {
                std::string body;
                while (body.size() <= 2048) {
                    char chunk[513]{};
                    DWORD downloaded = 0;
                    if (!WinHttpReadData(request, chunk, 512, &downloaded)) break;
                    if (downloaded == 0) {
                        const int wideLength = MultiByteToWideChar(
                            CP_UTF8, MB_ERR_INVALID_CHARS, body.data(),
                            static_cast<int>(body.size()), nullptr, 0);
                        if (wideLength > 0) {
                            std::wstring current(static_cast<size_t>(wideLength), L'\0');
                            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                body.data(), static_cast<int>(body.size()),
                                current.data(), wideLength);
                            current = ExtractExitIp(current);
                            int family = 0;
                            std::vector<unsigned char> bytes;
                            if (ParseIp(current, &family, &bytes)) {
                                if (observedIp) *observedIp = current;
                                const bool whitelisted = std::any_of(
                                    config.allowedExitIps.begin(),
                                    config.allowedExitIps.end(),
                                    [&](const std::wstring& expected) {
                                        return IpEquals(current, expected);
                                    });
                                outcome = whitelisted ? ExitIpProbe::Match
                                                      : ExitIpProbe::Mismatch;
                            }
                        }
                        break;
                    }
                    body.append(chunk, downloaded);
                }
            }
        }
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return outcome;
}

bool VerifyExitIp(const ProxyConfig& config, const wchar_t* targetHost,
                  std::wstring* observedIp, std::wstring* errorMessage) {
    if (observedIp) observedIp->clear();
    if (errorMessage) errorMessage->clear();
    if (config.allowedExitIps.empty()) return true;

    // Preferred probe: ask the very host we are about to call, so the check
    // follows the same proxy/TUN split-routing rule as the real request.
    // Only Cloudflare-fronted hosts serve /cdn-cgi/trace though - Google
    // (Antigravity) and AWS (Kiro) answer 404.
    if (config.verifyTargetHost && targetHost && targetHost[0] != L'\0') {
        std::wstring traceUrl = L"https://";
        traceUrl += targetHost;
        traceUrl += L"/cdn-cgi/trace";
        switch (ProbeExitIpOnce(config, traceUrl, observedIp)) {
            case ExitIpProbe::Match:
                return true;
            case ExitIpProbe::Mismatch:
                if (errorMessage) {
                    *errorMessage = L"BLOCKED: exit IP " + *observedIp
                        + L" is not in AllowedExitIPs";
                }
                return false;
            case ExitIpProbe::Unavailable:
                // LENIENT policy (user preference): when the target host does
                // not expose /cdn-cgi/trace (Google/Antigravity, AWS/Kiro),
                // trust its own split-routing rule and allow instead of
                // falling back to the generic exit-IP service. That fallback
                // follows a DIFFERENT proxy rule (different hostname) and
                // would misreport the real egress of the API request.
                if (observedIp) observedIp->clear();
                if (errorMessage) errorMessage->clear();
                return true;
        }
    }

    // Only reached when verifyTargetHost is disabled or no target host was
    // given: probe the generic exit-IP service explicitly.
    switch (ProbeExitIpOnce(config, config.exitIpCheckUrl, observedIp)) {
        case ExitIpProbe::Match:
            return true;
        case ExitIpProbe::Mismatch:
            if (errorMessage) {
                *errorMessage = L"BLOCKED: exit IP " + *observedIp
                    + L" is not in AllowedExitIPs";
            }
            return false;
        case ExitIpProbe::Unavailable:
            break;
    }

    if (errorMessage) {
        *errorMessage = L"BLOCKED: could not verify the public exit IP";
    }
    return false;
}

HINTERNET OpenHttpSession(const ProxyConfig& config, const wchar_t* userAgent,
                          const wchar_t* targetHost,
                          std::wstring* errorMessage) {
    if (errorMessage) errorMessage->clear();

    if (config.requireProxy && !config.proxyActive
        && config.allowedExitIps.empty()) {
        if (errorMessage) *errorMessage = L"BLOCKED: required proxy not detected";
        SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
        return nullptr;
    }

    std::wstring observedIp;
    if (!VerifyExitIp(config, targetHost, &observedIp, errorMessage)) {
        SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
        return nullptr;
    }

    HINTERNET session = OpenRawSession(config, userAgent);
    if (!session && errorMessage && errorMessage->empty()) {
        *errorMessage = L"WinHttpOpen failed";
    }
    return session;
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
            WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);
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

ConnectivityResult TestConnectivity(const ProxyConfig& config) {
    ConnectivityResult result;

    // In exit-IP-lock mode, never run a deliberate no-proxy probe against the
    // provider. Validate the TUN/proxy exit first and only use that path.
    if (!config.allowedExitIps.empty()) {
        std::wstring error;
        result.exitIpVerified = VerifyExitIp(
            config, L"api.anthropic.com", &result.observedExitIp, &error);
        if (!result.exitIpVerified) {
            result.statusMessage = error;
            return result;
        }

        const DWORD accessType = config.explicitProxy.empty()
            ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
            : WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        const wchar_t* proxy = config.explicitProxy.empty()
            ? nullptr : config.explicitProxy.c_str();
        result.proxyReachable = ProbeHost(
            L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT,
            L"/api/oauth/usage", accessType, proxy, nullptr);
        result.statusMessage = result.proxyReachable
            ? L"Exit IP verified; protected API path reachable"
            : L"Exit IP verified; API path unreachable";
        return result;
    }

    if (config.requireProxy && !config.proxyActive) {
        result.statusMessage = L"BLOCKED: required proxy not detected";
        return result;
    }

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
