#include "AntigravityUsageFetcher.h"
#include "JsonLite.h"
#include "ProxyHelper.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <regex>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

// "gemini-3-5-pro" -> "Gemini 3.5 Pro"  (mirrors reference display logic)
std::string FormatModelDisplayName(const std::string& raw) {
    std::string name = std::regex_replace(raw, std::regex(R"((\d+)-(\d+))"), "$1.$2");
    std::istringstream iss(name);
    std::string part, out;
    while (std::getline(iss, part, '-')) {
        if (part.empty()) continue;
        if (!out.empty()) out += ' ';
        if (std::isdigit((unsigned char)part[0])) {
            out += part;
        } else {
            part[0] = (char)std::toupper((unsigned char)part[0]);
            out += part;
        }
    }
    return out;
}

bool ModelAllowed(const std::string& name) {
    if (!std::regex_search(name, std::regex("gemini|claude|gpt", std::regex_constants::icase)))
        return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("gemini") == std::string::npos) return true;
    auto m = std::regex_search(lower, std::regex("gemini-(\\d+(?:\\.\\d+)?)"));
    std::smatch sm;
    if (std::regex_search(lower, sm, std::regex("gemini-(\\d+(?:\\.\\d+)?)"))) {
        float v = std::stof(sm[1].str());
        return v >= 3.0f;
    }
    return false;  // gemini without a >=3.0 version is filtered out
}

// Priority for the featured dashboard ring: Gemini 3.x > Claude > GPT > others.
int ModelPriority(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("gemini-3") != std::string::npos) return 3;
    if (lower.find("claude") != std::string::npos) return 2;
    if (lower.find("gpt") != std::string::npos) return 1;
    return 0;
}

static std::mutex s_mutex;

} // namespace

std::optional<std::string> AntigravityUsageFetcher::HttpPost(
    const std::string& host, const std::string& path,
    const std::vector<std::wstring>& headers, const std::string& body,
    unsigned long& statusCode, std::wstring* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    statusCode = 0;

    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET session = OpenHttpSession(
        proxyConfig_, L"AntigravityUsageFetcher/1.0", whost.c_str(), errorMessage);
    if (!session) {
        if (errorMessage && errorMessage->empty()) *errorMessage = L"WinHttpOpen failed";
        return std::nullopt;
    }

    std::optional<std::string> result;
    HINTERNET connect = nullptr, request = nullptr;
    do {
        connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { if (errorMessage) *errorMessage = L"WinHttpConnect failed"; break; }

        request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) { if (errorMessage) *errorMessage = L"WinHttpOpenRequest failed"; break; }

        for (const auto& h : headers) {
            WinHttpAddRequestHeaders(request, h.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        }
        WinHttpSetTimeouts(request, 10000, 10000, 10000, 15000);

        std::wstring wbody(body.begin(), body.end());
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                (LPVOID)wbody.c_str(), (DWORD)wbody.size(), (DWORD)wbody.size(), 0)) {
            if (errorMessage) *errorMessage = L"WinHttpSendRequest failed"; break;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            if (errorMessage) *errorMessage = L"WinHttpReceiveResponse failed"; break;
        }

        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        std::string respBody;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) { result = std::move(respBody); break; }
            std::string chunk(available, '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &downloaded)) break;
            chunk.resize(downloaded);
            respBody.append(chunk);
        }
    } while (false);

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

bool AntigravityUsageFetcher::RefreshAccessToken(std::wstring* errorMessage) {
    if (refreshToken_.empty()) {
        if (errorMessage) *errorMessage = L"No Antigravity refresh token configured";
        return false;
    }
    std::string body = "grant_type=refresh_token"
        "&refresh_token=" + refreshToken_
        + "&client_id=" + clientId_
        + "&client_secret=" + clientSecret_;

    std::vector<std::wstring> headers = {
        L"Content-Type: application/x-www-form-urlencoded",
        L"User-Agent: AntigravityUsageFetcher/1.0"
    };
    unsigned long status = 0;
    auto resp = HttpPost(kTokenHost, kTokenPath, headers, body, status, errorMessage);
    if (!resp || status != 200) return false;

    jsonlite::Parser parser(*resp);
    auto root = parser.Parse();
    if (!root) return false;
    auto* at = root->Find("access_token");
    if (!at || !at->IsString()) return false;
    auto atStr = at->AsString();
    if (!atStr || atStr->empty()) return false;

    accessToken_ = std::string(*atStr);
    double expiresIn = 3600.0;
    if (auto* ei = root->Find("expires_in")) {
        if (auto v = ei->AsNumber()) expiresIn = *v;
    }
    tokenExpiry_ = static_cast<double>(time(nullptr)) + expiresIn;
    return true;
}

bool AntigravityUsageFetcher::EnsureAccessToken(std::wstring* errorMessage) {
    if (clientId_.empty() || clientSecret_.empty()) {
        if (errorMessage) *errorMessage = L"Antigravity ClientId/ClientSecret not set in AIUsage.ini";
        return false;
    }
    double now = static_cast<double>(time(nullptr));
    if (!accessToken_.empty() && tokenExpiry_ > now + 60.0) return true;
    if (!refreshToken_.empty()) return RefreshAccessToken(errorMessage);
    // No refresh token: trust the provided access token (refreshed on 401).
    if (!accessToken_.empty()) return true;
    if (errorMessage) *errorMessage = L"No Antigravity credentials configured";
    return false;
}

AntigravityUsageData AntigravityUsageFetcher::Fetch() {
    std::lock_guard<std::mutex> lock(s_mutex);
    double now = static_cast<double>(time(nullptr));

    if (!enabled_) {
        AntigravityUsageData d;
        d.success = false;
        d.errorMessage = L"Antigravity disabled";
        return d;
    }

    if (now < backoffUntil_ && cachedData_.success) {
        AntigravityUsageData stale = cachedData_;
        stale.errorMessage = L"Rate limited, retrying later";
        return stale;
    }
    if (now < nextFetchTime_ && cachedData_.success) return cachedData_;
    nextFetchTime_ = now + kMinInterval;

    std::wstring werr;
    if (!EnsureAccessToken(&werr)) {
        AntigravityUsageData d;
        d.success = false;
        d.errorMessage = werr.empty() ? L"Antigravity auth failed" : werr;
        if (cachedData_.success) { AntigravityUsageData stale = cachedData_; stale.errorMessage = d.errorMessage; return stale; }
        return d;
    }

    auto Bearer = [&](const std::string& path, const std::string& bodyJson) {
        std::vector<std::wstring> headers = {
            L"Authorization: Bearer " + Utf8ToWide(accessToken_),
            L"Content-Type: application/json",
            L"User-Agent: AntigravityUsageFetcher/1.0"
        };
        unsigned long status = 0;
        return std::make_pair(
            HttpPost(kApiHost, path, headers, bodyJson, status, &werr), status);
    };

    // 1) loadCodeAssist -> projectId + tier
    AntigravityUsageData data;
    data.success = false;

    auto load = Bearer(kLoadCodeAssistPath, R"({"metadata":{"ideType":"ANTIGRAVITY"}})");
    if (!load.first) {
        data.errorMessage = werr.empty() ? L"Antigravity loadCodeAssist failed" : werr;
        if (cachedData_.success) { AntigravityUsageData s = cachedData_; s.errorMessage = data.errorMessage; return s; }
        return data;
    }
    {
        jsonlite::Parser p(*load.first);
        auto root = p.Parse();
        if (root) {
            if (auto* proj = root->Find("cloudaicompanionProject")) {
                if (auto s = proj->AsString()) data.projectId = std::string(*s);
            }
            std::string tier;
            if (auto* paid = root->Find("paidTier")) {
                if (auto* id = paid->Find("id")) if (auto s = id->AsString()) tier = std::string(*s);
            }
            if (tier.empty()) {
                if (auto* cur = root->Find("currentTier")) {
                    if (auto* id = cur->Find("id")) if (auto s = id->AsString()) tier = std::string(*s);
                }
            }
            data.tier = tier.empty() ? "FREE" : tier;
        }
    }

    // 2) fetchAvailableModels -> quota map
    std::string modelsBody = data.projectId.empty()
        ? "{}"
        : R"({"project":")" + data.projectId + R"("})";
    auto models = Bearer(kFetchModelsPath, modelsBody);
    if (!models.first) {
        data.errorMessage = werr.empty() ? L"Antigravity fetchAvailableModels failed" : werr;
        if (cachedData_.success) { AntigravityUsageData s = cachedData_; s.errorMessage = data.errorMessage; return s; }
        return data;
    }

    if (!ParseModels(*models.first, data)) {
        if (cachedData_.success) { AntigravityUsageData s = cachedData_; s.errorMessage = data.errorMessage; return s; }
        return data;
    }

    if (data.models.empty()) {
        data.errorMessage = L"No Antigravity models returned";
        if (cachedData_.success) { AntigravityUsageData s = cachedData_; s.errorMessage = data.errorMessage; return s; }
        return data;
    }

    data.success = true;
    data.lastSuccessTime = now;
    backoffSeconds_ = 0;
    cachedData_ = data;
    return data;
}

bool AntigravityUsageFetcher::ParseModels(const std::string& body, AntigravityUsageData& out) {
    jsonlite::Parser parser(body);
    auto root = parser.Parse();
    if (!root) { out.errorMessage = L"Antigravity JSON parse failed"; return false; }
    auto* modelsObj = root->Find("models");
    if (!modelsObj || !modelsObj->IsObject()) { out.errorMessage = L"Antigravity: missing models map"; return false; }
    const auto* obj = modelsObj->AsObject();
    if (!obj) { out.errorMessage = L"Antigravity: models not an object"; return false; }

    for (const auto& [name, info] : *obj) {
        if (!ModelAllowed(name)) continue;
        const auto* infoObj = info.AsObject();
        if (!infoObj) continue;
        auto* quota = infoObj->Find("quotaInfo");
        if (!quota) continue;
        const auto* quotaObj = quota->AsObject();
        if (!quotaObj) continue;

        double remaining = 0.0;
        if (auto* rf = quotaObj->Find("remainingFraction")) {
            if (auto v = rf->AsNumber()) remaining = *v;
        }
        std::string reset;
        if (auto* rt = quotaObj->Find("resetTime")) {
            if (auto s = rt->AsString()) reset = std::string(*s);
        }
        AntigravityModelQuota mq;
        mq.modelName = name;
        mq.displayName = FormatModelDisplayName(name);
        mq.remainingFraction = remaining;
        mq.resetTime = reset;
        mq.isExhausted = remaining <= 0.0;
        out.models.push_back(std::move(mq));
    }
    return true;
}
