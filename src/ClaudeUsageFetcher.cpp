#include "ClaudeUsageFetcher.h"
#include "ClaudeCredentialReader.h"
#include "CodexUsageCore.h"
#include "DpapiHelper.h"
#include "JsonLite.h"
#include "ProxyHelper.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr const char* API_HOST = "api.anthropic.com";
constexpr const char* API_PATH = "/api/oauth/usage";
constexpr const char* TOKEN_URL_HOST1 = "platform.claude.com";
constexpr const char* TOKEN_URL_PATH1 = "/v1/oauth/token";
constexpr const char* TOKEN_URL_HOST2 = "console.anthropic.com";
constexpr const char* TOKEN_URL_PATH2 = "/v1/oauth/token";
constexpr const char* CLAUDE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const char* FALLBACK_UA = "claude-code/2.1.85";

constexpr double MIN_INTERVAL = 60.0;
constexpr double BACKOFF_MIN = 120.0;
constexpr double BACKOFF_MAX = 900.0;
constexpr double STALE_AFTER = 900.0; // 15 minutes

static std::mutex s_fetchMutex;

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
        output.data(), size, nullptr, nullptr);
    return output;
}

std::string FormatTimestampLocal(double unixSeconds) {
    FILETIME ft{};
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<ULONGLONG>((unixSeconds * 10000000.0) + 116444736000000000ULL);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    SYSTEMTIME utcTime{};
    FileTimeToSystemTime(&ft, &utcTime);

    SYSTEMTIME localTime{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime);

    char buf[64];
    sprintf_s(buf, "%02d:%02d", localTime.wHour, localTime.wMinute);
    return buf;
}

std::string FormatResetDateLocal(double unixSeconds) {
    FILETIME ft{};
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<ULONGLONG>((unixSeconds * 10000000.0) + 116444736000000000ULL);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    SYSTEMTIME utcTime{};
    FileTimeToSystemTime(&ft, &utcTime);

    SYSTEMTIME localTime{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime);

    char buf[64];
    sprintf_s(buf, "%02d-%02d %02d:%02d", localTime.wMonth, localTime.wDay, localTime.wHour, localTime.wMinute);
    return buf;
}

} // namespace

std::optional<std::string> ClaudeUsageFetcher::HttpGet(
    const std::string& host, const std::string& path,
    const std::vector<std::wstring>& headers, unsigned long& statusCode, std::wstring* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    statusCode = 0;

    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET session = OpenHttpSession(
        proxyConfig_, L"ClaudeUsageFetcher/1.0", whost.c_str(), errorMessage);
    if (!session) {
        if (errorMessage && errorMessage->empty()) {
            *errorMessage = L"WinHttpOpen failed";
        }
        return std::nullopt;
    }

    std::optional<std::string> result;
    HINTERNET connect = nullptr, request = nullptr;

    do {
        connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { if (errorMessage) *errorMessage = L"WinHttpConnect failed"; break; }

        request = WinHttpOpenRequest(connect, L"GET", wpath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) { if (errorMessage) *errorMessage = L"WinHttpOpenRequest failed"; break; }

        for (const auto& h : headers) {
            WinHttpAddRequestHeaders(request, h.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
        }

        WinHttpSetTimeouts(request, 10000, 10000, 10000, 15000);

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            if (errorMessage) *errorMessage = L"WinHttpSendRequest failed"; break;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            if (errorMessage) *errorMessage = L"WinHttpReceiveResponse failed"; break;
        }

        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        std::string body;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) { result = std::move(body); break; }
            std::string chunk(available, '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &downloaded)) break;
            chunk.resize(downloaded);
            body.append(chunk);
        }
    } while (false);

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

ClaudeUsageFetcher::AuthHeaders ClaudeUsageFetcher::GetHeaders(bool forceRefresh) {
    AuthHeaders result;
    double now = static_cast<double>(time(nullptr));

    if (!forceRefresh) {
        auto desktopToken = GetDesktopToken();
        if (desktopToken && desktopToken->expiresAt > now + 60) {
            result.accessToken = desktopToken->accessToken;
            return result;
        }
    }

    // Try CLI credentials
    auto cliToken = GetCliAccessToken();
    if (cliToken) {
        result.accessToken = *cliToken;
        return result;
    }

    // Fall back to desktop token even if expired
    auto desktopToken = GetDesktopToken();
    if (desktopToken) {
        result.accessToken = desktopToken->accessToken;
        return result;
    }

    result.error = "No Claude credentials found";
    return result;
}

std::optional<std::string> ClaudeUsageFetcher::RefreshCliToken(const std::string& refreshToken) {
    // Read CLI credentials to get current state
    auto jsonText = ReadCliCredentials();
    if (!jsonText) return std::nullopt;

    jsonlite::Parser parser(*jsonText);
    auto root = parser.Parse();
    if (!root) return std::nullopt;

    auto oauth = root->Find("claudeAiOauth");
    if (!oauth) return std::nullopt;

    auto* rt = oauth->Find("refreshToken");
    if (!rt || !rt->IsString()) return std::nullopt;
    auto rtStr = rt->AsString();
    if (!rtStr || rtStr->empty()) return std::nullopt;

    // Build refresh request body
    std::string body = "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"" +
        std::string(*rtStr) + "\",\"client_id\":\"" + CLAUDE_CLIENT_ID + "\"}";

    // Try both token URLs
    for (auto& [host, path] : {std::make_pair(TOKEN_URL_HOST1, TOKEN_URL_PATH1),
                                 std::make_pair(TOKEN_URL_HOST2, TOKEN_URL_PATH2)}) {
        std::wstring whost(host, host + strlen(host));
        std::wstring wpath(path, path + strlen(path));

        HINTERNET session = OpenHttpSession(
            proxyConfig_, L"ClaudeUsageFetcher/1.0", whost.c_str());
        if (!session) continue;

        HINTERNET connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { WinHttpCloseHandle(session); continue; }

        HINTERNET request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); continue; }

        std::wstring wua = Utf8ToWide(std::string(FALLBACK_UA));
        std::wstring headers = L"Content-Type: application/json\r\nUser-Agent: " + wua;
        WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpSetTimeouts(request, 10000, 10000, 10000, 15000);

        std::wstring wbody(body.begin(), body.end());
        BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)wbody.c_str(), static_cast<DWORD>(wbody.size()), static_cast<DWORD>(wbody.size()), 0);

        if (sent && WinHttpReceiveResponse(request, nullptr)) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

            if (statusCode == 200) {
                std::string respBody;
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                    std::string chunk(available, '\0');
                    DWORD downloaded = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &downloaded)) break;
                    chunk.resize(downloaded);
                    respBody.append(chunk);
                }

                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);

                jsonlite::Parser respParser(respBody);
                auto respRoot = respParser.Parse();
                if (!respRoot) continue;

                auto* at = respRoot->Find("access_token");
                if (!at || !at->IsString()) continue;
                auto atStr = at->AsString();
                if (!atStr || atStr->empty()) continue;

                // Get new refresh token if provided
                auto* newRt = respRoot->Find("refresh_token");
                std::string newRefreshToken;
                if (newRt && newRt->IsString()) {
                    auto nrs = newRt->AsString();
                    if (nrs) newRefreshToken = std::string(*nrs);
                }

                auto* expiresIn = respRoot->Find("expires_in");
                double expiresAt = time(nullptr) + (expiresIn ? expiresIn->AsNumber().value_or(3600) : 3600);

                // Persist
                PersistCliCredentials(std::string(*atStr), newRefreshToken.empty() ? std::string(*rtStr) : newRefreshToken, expiresAt);

                return std::string(*atStr);
            }
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
    }

    return std::nullopt;
}

ClaudeUsageData ClaudeUsageFetcher::Fetch() {
    std::lock_guard<std::mutex> lock(s_fetchMutex);

    double now = static_cast<double>(time(nullptr));

    // Rate limiting: respect backoff
    if (now < backoffUntil_) {
        if (cachedData_.success) {
            ClaudeUsageData stale = cachedData_;
            stale.errorMessage = L"Rate limited, retrying later";
            return stale;
        }
        ClaudeUsageData err;
        err.errorMessage = L"Rate limited, retrying later";
        return err;
    }

    // Don't fetch too frequently
    if (now < nextFetchTime_ && cachedData_.success) {
        return cachedData_;
    }

    nextFetchTime_ = now + MIN_INTERVAL;

    // Get auth headers
    auto auth = GetHeaders(false);
    if (auth.accessToken.empty()) {
        ClaudeUsageData err;
        err.errorMessage = Utf8ToWide(auth.error);
        if (cachedData_.success) {
            ClaudeUsageData stale = cachedData_;
            stale.errorMessage = err.errorMessage;
            return stale;
        }
        return err;
    }

    // Make request
    std::wstring wError;
    DWORD statusCode = 0;
    std::wstring ua = Utf8ToWide(std::string(FALLBACK_UA));
    std::vector<std::wstring> headers = {
        L"Authorization: Bearer " + std::wstring(auth.accessToken.begin(), auth.accessToken.end()),
        L"Content-Type: application/json",
        L"User-Agent: " + ua,
        L"anthropic-beta: oauth-2025-04-20"
    };

    auto response = HttpGet(API_HOST, API_PATH, headers, statusCode, &wError);

    // Handle 401: retry with force refresh
    if (statusCode == 401) {
        auth = GetHeaders(true);
        if (!auth.accessToken.empty()) {
            headers[0] = L"Authorization: Bearer " + std::wstring(auth.accessToken.begin(), auth.accessToken.end());
            response = HttpGet(API_HOST, API_PATH, headers, statusCode, &wError);
        }
    }

    // Handle errors
    if (statusCode == 429) {
        backoffSeconds_ = std::min(std::max(backoffSeconds_ * 2, BACKOFF_MIN), BACKOFF_MAX);
        backoffUntil_ = now + backoffSeconds_;
        ClaudeUsageData err;
        err.errorMessage = L"API rate limited";
        if (cachedData_.success) {
            ClaudeUsageData stale = cachedData_;
            stale.errorMessage = err.errorMessage;
            return stale;
        }
        return err;
    }

    if (statusCode == 403) {
        ClaudeUsageData err;
        err.errorMessage = L"Access denied (403)";
        if (cachedData_.success) {
            ClaudeUsageData stale = cachedData_;
            stale.errorMessage = err.errorMessage;
            return stale;
        }
        return err;
    }

    if (statusCode != 200 || !response) {
        ClaudeUsageData err;
        err.errorMessage = !wError.empty()
            ? wError
            : L"HTTP " + std::to_wstring(statusCode);
        if (cachedData_.success) {
            ClaudeUsageData stale = cachedData_;
            stale.errorMessage = err.errorMessage;
            return stale;
        }
        return err;
    }

    // Parse response
    backoffSeconds_ = 0;
    jsonlite::Parser parser(*response);
    auto root = parser.Parse();
    if (!root) {
        ClaudeUsageData err;
        err.errorMessage = L"JSON parse failed";
        if (cachedData_.success) {
            ClaudeUsageData stale = cachedData_;
            stale.errorMessage = err.errorMessage;
            return stale;
        }
        return err;
    }

    ClaudeUsageData data;
    data.success = true;
    data.lastSuccessTime = now;

    // Parse top-level windows (legacy format)
    auto* fiveHour = root->Find("five_hour");
    if (fiveHour) {
        auto* util = fiveHour->Find("utilization");
        auto* resetAt = fiveHour->Find("resets_at");
        if (util) {
            auto val = util->AsNumber();
            if (val) data.fiveHourPercent = static_cast<int>(std::round(*val));
        }
        if (resetAt && resetAt->IsString()) {
            auto rs = resetAt->AsString();
            if (rs) data.fiveHourResetAt = std::string(*rs);
        }
    }

    auto* sevenDay = root->Find("seven_day");
    if (sevenDay) {
        auto* util = sevenDay->Find("utilization");
        auto* resetAt = sevenDay->Find("resets_at");
        if (util) {
            auto val = util->AsNumber();
            if (val) data.sevenDayPercent = static_cast<int>(std::round(*val));
        }
        if (resetAt && resetAt->IsString()) {
            auto rs = resetAt->AsString();
            if (rs) data.sevenDayResetAt = std::string(*rs);
        }
    }

    // Parse limits array (new API format)
    auto* limits = root->Find("limits");
    if (limits && limits->IsArray()) {
        auto* arr = limits->AsArray();
        if (arr) {
            for (const auto& entry : *arr) {
                auto* kind = entry.Find("kind");
                auto* percent = entry.Find("percent");
                auto* resetsAt = entry.Find("resets_at");
                if (!kind || !percent || !resetsAt) continue;
                auto kindStr = kind->AsString();
                auto pctVal = percent->AsNumber();
                auto resetStr = resetsAt->AsString();
                if (!kindStr || !pctVal || !resetStr) continue;

                int pct = static_cast<int>(std::round(*pctVal));
                std::string reset = std::string(*resetStr);

                if (*kindStr == "session" && data.fiveHourPercent < 0) {
                    data.fiveHourPercent = pct;
                    data.fiveHourResetAt = reset;
                } else if (*kindStr == "weekly_all" && data.sevenDayPercent < 0) {
                    data.sevenDayPercent = pct;
                    data.sevenDayResetAt = reset;
                }
            }
        }
    }

    // Parse extra_usage (credits/overage)
    auto* extraUsage = root->Find("extra_usage");
    if (extraUsage && extraUsage->IsObject()) {
        auto* usedCredits = extraUsage->Find("used_credits");
        if (usedCredits) {
            auto val = usedCredits->AsNumber();
            if (val) data.usedCredits = static_cast<long long>(*val);
        }
        auto* cur = extraUsage->Find("currency");
        if (cur && cur->IsString()) {
            auto s = cur->AsString();
            if (s) data.currency = std::string(*s);
        }
        auto* dp = extraUsage->Find("decimal_places");
        if (dp) {
            auto val = dp->AsNumber();
            if (val) data.decimalPlaces = static_cast<int>(*val);
        }
    }

    // Fetch profile once for subscription status (to avoid extra API calls)
    if (!profileFetched_ && !auth.accessToken.empty()) {
        profileFetched_ = true;
        std::wstring profileErr;
        unsigned long profileStatus = 0;
        std::vector<std::wstring> profileHeaders = {
            L"Authorization: Bearer " + std::wstring(auth.accessToken.begin(), auth.accessToken.end()),
            L"Content-Type: application/json",
            L"User-Agent: " + ua,
            L"anthropic-beta: oauth-2025-04-20"
        };
        auto profileResp = HttpGet("api.anthropic.com", "/api/oauth/profile", profileHeaders, profileStatus, &profileErr);
        if (profileStatus == 200 && profileResp) {
            jsonlite::Parser pParser(*profileResp);
            auto pRoot = pParser.Parse();
            if (pRoot) {
                auto* org = pRoot->Find("organization");
                if (org) {
                    auto* subStatus = org->Find("subscription_status");
                    if (subStatus && subStatus->IsString()) {
                        auto s = subStatus->AsString();
                        if (s) cachedSubscriptionStatus_ = std::string(*s);
                    }
                }
            }
        }
    }
    data.subscriptionStatus = cachedSubscriptionStatus_;

    data.lastSuccessTime = now;
    cachedData_ = data;
    return data;
}
