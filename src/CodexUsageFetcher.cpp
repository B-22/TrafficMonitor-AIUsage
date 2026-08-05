#include "CodexUsageFetcher.h"

#include "JsonLite.h"
#include "ProxyHelper.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

std::wstring JoinPath(const std::wstring& base, const std::wstring& child) {
    std::wstring result = base;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result += child;
    return result;
}

std::optional<std::wstring> ReadEnv(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }

    std::wstring value(size - 1, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    return value;
}

std::optional<std::string> HttpGetJson(const ProxyConfig& proxyConfig,
                                       const std::wstring& userAgent,
                                       const std::wstring& host,
                                       const std::wstring& path,
                                       const std::vector<std::wstring>& headers,
                                       std::wstring* errorMessage,
                                       DWORD timeoutMilliseconds = 5000) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    HINTERNET session = OpenHttpSession(
        proxyConfig, userAgent.c_str(), host.c_str(), errorMessage);
    if (session == nullptr) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = L"WinHttpOpen failed";
        }
        return std::nullopt;
    }

    std::optional<std::string> responseBody;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    do {
        connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connect == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpConnect failed";
            }
            break;
        }

        request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpOpenRequest failed";
            }
            break;
        }

        for (const std::wstring& header : headers) {
            if (!WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpAddRequestHeaders failed";
                }
                break;
            }
        }
        if (errorMessage != nullptr && !errorMessage->empty()) {
            break;
        }

        WinHttpSetTimeouts(
            request,
            timeoutMilliseconds,
            timeoutMilliseconds,
            timeoutMilliseconds,
            timeoutMilliseconds);

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpSendRequest failed";
            }
            break;
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpReceiveResponse failed";
            }
            break;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 200) {
            if (errorMessage != nullptr) {
                *errorMessage = host + path + L" returned HTTP " + std::to_wstring(statusCode);
                if (statusCode == 401) {
                    *errorMessage += L"; auth.json access_token may be expired";
                }
            }
            break;
        }

        std::string body;
        constexpr size_t kMaximumResponseBytes = 4 * 1024 * 1024;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpQueryDataAvailable failed";
                }
                break;
            }
            if (available == 0) {
                responseBody = std::move(body);
                break;
            }

            std::string chunk(static_cast<size_t>(available), '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &downloaded)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpReadData failed";
                }
                break;
            }

            chunk.resize(downloaded);
            if (body.size() + chunk.size() > kMaximumResponseBytes) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"HTTP response exceeded 4 MiB";
                }
                break;
            }
            body.append(chunk);
        }
    } while (false);

    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connect != nullptr) {
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);

    return responseBody;
}

}  // namespace

void CodexUsageFetcher::SetRadarRefreshMinutes(int minutes) {
    radarRefreshMinutes_ = std::clamp(minutes, 5, 120);
    lastRadarFetch_ = {};
}

void CodexUsageFetcher::SetResetTodayRefreshMinutes(int minutes) {
    resetTodayRefreshMinutes_ = std::clamp(minutes, 15, 240);
    lastResetTodayFetch_ = {};
}

UsageSnapshot CodexUsageFetcher::Fetch() const {
    UsageSnapshot snapshot;
    const ResetRadarSnapshot resetRadar = FetchResetRadar();
    snapshot.resetRadar = resetRadar;

    std::wstring errorMessage;
    std::optional<AuthData> auth = ReadAuthData(&errorMessage);
    if (!auth.has_value()) {
        snapshot.errorMessage = errorMessage;
        snapshot.resetCredits.errorMessage = errorMessage;
        return snapshot;
    }

    std::optional<std::string> usageJson = HttpGetUsageJson(*auth, &errorMessage);
    if (usageJson.has_value()) {
        snapshot = ParseUsageJson(*usageJson, &errorMessage);
        snapshot.resetRadar = resetRadar;
        if (!snapshot.success) {
            snapshot.errorMessage = errorMessage;
        }
    } else {
        snapshot.errorMessage = errorMessage;
    }

    std::wstring creditsError;
    std::optional<std::string> creditsJson =
        HttpGetResetCreditsJson(*auth, &creditsError);
    if (creditsJson.has_value()) {
        snapshot.resetCredits =
            ParseResetCreditsJson(*creditsJson, &creditsError);
        if (snapshot.resetCredits.success) {
            snapshot.resetCredits.fetchedAtUnixSeconds =
                static_cast<long long>(time(nullptr));
        } else {
            snapshot.resetCredits.errorMessage = creditsError;
        }
    } else {
        snapshot.resetCredits.errorMessage = creditsError;
    }
    return snapshot;
}

std::wstring CodexUsageFetcher::ResolveAuthJsonPath() const {
    if (auto codexHome = ReadEnv(L"CODEX_HOME"); codexHome.has_value() && !codexHome->empty()) {
        return JoinPath(*codexHome, L"auth.json");
    }

    if (auto userProfile = ReadEnv(L"USERPROFILE"); userProfile.has_value() && !userProfile->empty()) {
        return JoinPath(JoinPath(*userProfile, L".codex"), L"auth.json");
    }

    return L".codex\\auth.json";
}

std::optional<CodexUsageFetcher::AuthData> CodexUsageFetcher::ReadAuthData(
    std::wstring* errorMessage) const {
    const std::wstring authPath = ResolveAuthJsonPath();
    std::optional<std::string> jsonText = LoadFileUtf8(authPath, errorMessage);
    if (!jsonText.has_value()) {
        return std::nullopt;
    }

    jsonlite::Parser parser(*jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"auth.json parse failed: " + Utf8ToWide(parser.Error());
        }
        return std::nullopt;
    }

    const jsonlite::Value* tokens = root->Find("tokens");
    const jsonlite::Value* accessToken = tokens != nullptr ? tokens->Find("access_token") : nullptr;
    auto token = accessToken != nullptr ? accessToken->AsString() : std::nullopt;
    if (!token.has_value() || token->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"auth.json missing tokens.access_token";
        }
        return std::nullopt;
    }

    AuthData auth;
    auth.accessToken = std::string(*token);
    const jsonlite::Value* accountId =
        tokens != nullptr ? tokens->Find("account_id") : nullptr;
    if (auto value = accountId != nullptr
            ? accountId->AsString()
            : std::nullopt;
        value.has_value()) {
        auth.accountId = std::string(*value);
    }
    return auth;
}

std::optional<std::string> CodexUsageFetcher::LoadFileUtf8(const std::wstring& path, std::wstring* errorMessage) const {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot open Codex auth.json";
        }
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot read Codex auth.json";
        }
        return std::nullopt;
    }

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = contents.empty() || ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != contents.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot read Codex auth.json";
        }
        return std::nullopt;
    }

    return contents;
}

std::optional<std::string> CodexUsageFetcher::HttpGetUsageJson(
    const AuthData& auth,
    std::wstring* errorMessage) const {
    return HttpGetJson(
        proxyConfig_,
        L"TrafficMonitorCodexUsage/0.1",
        L"chatgpt.com",
        L"/backend-api/wham/usage",
        { L"Authorization: Bearer " + Utf8ToWide(auth.accessToken) },
        errorMessage);
}

std::optional<std::string> CodexUsageFetcher::HttpGetResetCreditsJson(
    const AuthData& auth,
    std::wstring* errorMessage) const {
    std::vector<std::wstring> headers = {
        L"Authorization: Bearer " + Utf8ToWide(auth.accessToken),
        L"OpenAI-Beta: codex-1",
        L"originator: Codex Desktop",
    };
    if (!auth.accountId.empty()) {
        headers.push_back(
            L"ChatGPT-Account-ID: " + Utf8ToWide(auth.accountId));
    }
    return HttpGetJson(
        proxyConfig_,
        L"TrafficMonitorCodexUsage/0.5",
        L"chatgpt.com",
        L"/backend-api/wham/rate-limit-reset-credits",
        headers,
        errorMessage);
}

ResetRadarSnapshot CodexUsageFetcher::FetchResetRadar() const {
    ResetRadarSnapshot disabled;
    if (!radarEnabled_) return disabled;

    // These endpoints expose anonymous community status data and receive
    // no account headers. Keep any configured proxy route, but do not apply
    // the credential-bearing API policy (RequireProxy / AllowedExitIPs) to
    // them. Some community hosts do not expose a usable /cdn-cgi/trace check.
    ProxyConfig radarProxyConfig = proxyConfig_;
    radarProxyConfig.requireProxy = false;
    radarProxyConfig.allowedExitIps.clear();

    const auto nowSteady = std::chrono::steady_clock::now();
    const long long nowUnix = static_cast<long long>(time(nullptr));

    ResetRadarSnapshot runway;
    const bool refreshRunway =
        lastResetTodayFetch_.time_since_epoch().count() == 0
        || nowSteady - lastResetTodayFetch_
            >= std::chrono::minutes(resetTodayRefreshMinutes_);
    if (refreshRunway) {
        lastResetTodayFetch_ = nowSteady;
        std::wstring runwayError;
        const auto runwayJson = HttpGetJson(
            radarProxyConfig,
            L"TrafficMonitorCodexResetToday/0.6",
            L"codexreset.gitcdn.top",
            L"/api/status.json",
            {L"Accept: application/json"},
            &runwayError,
            5000);
        if (runwayJson.has_value()) {
            runway = ParseCodexRunwayResetStatusJson(
                *runwayJson, nowUnix, &runwayError);
            if (runway.success) {
                cachedRunwayJson_ = *runwayJson;
            } else {
                runway.errorMessage = runwayError;
            }
        }
        if (!runway.success && !cachedRunwayJson_.empty()) {
            std::wstring cachedError;
            runway = ParseCodexRunwayResetStatusJson(
                cachedRunwayJson_, nowUnix, &cachedError);
            if (runway.success) {
                runway.errorMessage = runwayError.empty()
                    ? L"Codex Runway refresh failed; using cached feed"
                    : L"Codex Runway refresh failed; using cached feed: "
                        + runwayError;
            } else if (runwayError.empty()) {
                runwayError = cachedError;
            }
        }
        if (!runway.success && runway.errorMessage.empty()) {
            runway.errorMessage = runwayError;
        }
    } else if (!cachedRunwayJson_.empty()) {
        std::wstring runwayError;
        runway = ParseCodexRunwayResetStatusJson(
            cachedRunwayJson_, nowUnix, &runwayError);
        if (!runway.success) runway.errorMessage = runwayError;
    }

    const bool refreshCommunity =
        lastRadarFetch_.time_since_epoch().count() == 0
        || nowSteady - lastRadarFetch_
            >= std::chrono::minutes(radarRefreshMinutes_);
    if (refreshCommunity) {
        lastRadarFetch_ = nowSteady;
        ResetRadarSnapshot summary;
        ResetRadarSnapshot forecast;
        std::wstring error;
        const auto summaryJson = HttpGetJson(
            radarProxyConfig,
            L"TrafficMonitorCodexResetRadar/0.6",
            L"codex-reset-radar.pages.dev",
            L"/current.json",
            {},
            &error,
            5000);
        if (summaryJson.has_value()) {
            summary = ParseResetRadarSummaryJson(*summaryJson, &error);
            if (!summary.success) summary.errorMessage = error;
        } else {
            summary.errorMessage = error;
        }

        error.clear();
        const auto forecastJson = HttpGetJson(
            radarProxyConfig,
            L"TrafficMonitorCodexResetRadar/0.6",
            L"codex-reset.com",
            L"/api/forecast",
            {},
            &error,
            5000);
        if (forecastJson.has_value()) {
            forecast = ParseResetForecastJson(*forecastJson, &error);
            if (!forecast.success) forecast.errorMessage = error;
        } else {
            forecast.errorMessage = error;
        }
        cachedCommunityRadar_ =
            MergeResetRadarSnapshots(summary, forecast, nowUnix);
    }

    ResetRadarSnapshot merged = MergeResetRadarSnapshots(
        runway, cachedCommunityRadar_, {}, nowUnix);
    if (merged.success) {
        cachedRadar_ = merged;
        return merged;
    }

    if (cachedRadar_.success) {
        ResetRadarSnapshot cached = cachedRadar_;
        cached.checkedAtUnixSeconds = nowUnix;
        if (cached.updatedAtUnixSeconds <= 0
            || nowUnix - cached.updatedAtUnixSeconds > 12 * 60 * 60) {
            cached.windowOpen = false;
            cached.summaryWindowOpen = false;
            cached.forecastWindowOpen = false;
        }
        cached.errorMessage = L"radar refresh failed; using last successful status";
        cachedRadar_ = cached;
        return cached;
    }
    cachedRadar_ = merged;
    return merged;
}
