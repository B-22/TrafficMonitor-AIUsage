#include "KiroCreditsFetcher.h"
#include "JsonLite.h"
#include "ProxyHelper.h"

#include <Windows.h>
#include <winhttp.h>
#include <objbase.h>

#include <algorithm>
#include <ctime>
#include <mutex>
#include <sstream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

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

// Percent-encode a URL query value (RFC 3986 unreserved set kept literal).
std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string GenerateUuid() {
    GUID g;
    if (FAILED(CoCreateGuid(&g))) return "00000000-0000-4000-8000-000000000000";
    char buf[40];
    sprintf_s(buf, "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
        g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

static std::mutex s_mutex;

} // namespace

std::wstring KiroCreditsFetcher::ResolveTokenPath() const {
    if (!tokenPathOverride_.empty()) return tokenPathOverride_;
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    if (!profile || !*profile) profile = _wgetenv(L"HOME");
    if (!profile || !*profile) return {};
    std::wstring p = profile;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    p += L".aws\\sso\\cache\\kiro-auth-token.json";
    return p;
}

std::optional<std::string> KiroCreditsFetcher::ReadFileUtf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string out;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
        out.append(buf, read);
    }
    CloseHandle(h);
    return out;
}

std::optional<std::string> KiroCreditsFetcher::HttpPost(
    const std::string& host, const std::string& path,
    const std::vector<std::wstring>& headers, const std::string& body,
    unsigned long& statusCode, std::wstring* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    statusCode = 0;
    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET session = OpenHttpSession(proxyConfig_, L"KiroCreditsFetcher/1.0", whost.c_str(), errorMessage);
    if (!session) { if (errorMessage && errorMessage->empty()) *errorMessage = L"WinHttpOpen failed"; return std::nullopt; }

    std::optional<std::string> result;
    HINTERNET connect = nullptr, request = nullptr;
    do {
        connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { if (errorMessage) *errorMessage = L"WinHttpConnect failed"; break; }
        request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) { if (errorMessage) *errorMessage = L"WinHttpOpenRequest failed"; break; }
        for (const auto& h : headers)
            WinHttpAddRequestHeaders(request, h.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);
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

std::optional<std::string> KiroCreditsFetcher::HttpGet(
    const std::string& host, const std::wstring& path,
    const std::vector<std::wstring>& headers, unsigned long& statusCode, std::wstring* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    statusCode = 0;
    std::wstring whost(host.begin(), host.end());

    HINTERNET session = OpenHttpSession(proxyConfig_, L"KiroCreditsFetcher/1.0", whost.c_str(), errorMessage);
    if (!session) { if (errorMessage && errorMessage->empty()) *errorMessage = L"WinHttpOpen failed"; return std::nullopt; }

    std::optional<std::string> result;
    HINTERNET connect = nullptr, request = nullptr;
    do {
        connect = WinHttpConnect(session, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { if (errorMessage) *errorMessage = L"WinHttpConnect failed"; break; }
        request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) { if (errorMessage) *errorMessage = L"WinHttpOpenRequest failed"; break; }
        for (const auto& h : headers)
            WinHttpAddRequestHeaders(request, h.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
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

bool KiroCreditsFetcher::LoadCredentials(std::string& accessToken, std::string& refreshToken,
    std::string& profileArn, std::wstring* errorMessage)
{
    std::wstring path = ResolveTokenPath();
    if (path.empty()) { if (errorMessage) *errorMessage = L"USERPROFILE not set"; return false; }
    auto text = ReadFileUtf8(path);
    if (!text) { if (errorMessage) *errorMessage = L"Kiro token file not found (is Kiro logged in?)"; return false; }

    jsonlite::Parser parser(*text);
    auto root = parser.Parse();
    if (!root) { if (errorMessage) *errorMessage = L"Kiro token file JSON parse failed"; return false; }

    auto* rt = root->Find("refreshToken");
    auto* arn = root->Find("profileArn");
    if (!rt || !rt->IsString() || !arn || !arn->IsString()) {
        if (errorMessage) *errorMessage = L"Kiro token file missing refreshToken/profileArn";
        return false;
    }
    auto rtStr = rt->AsString();
    auto arnStr = arn->AsString();
    if (!rtStr || !arnStr || rtStr->empty()) {
        if (errorMessage) *errorMessage = L"Kiro token file missing refreshToken/profileArn";
        return false;
    }
    refreshToken = std::string(*rtStr);
    profileArn = std::string(*arnStr);
    if (auto* at = root->Find("accessToken")) {
        if (auto s = at->AsString()) accessToken = std::string(*s);
    }
    return true;
}

std::optional<std::string> KiroCreditsFetcher::RefreshAccessToken(const std::string& refreshToken, std::wstring* errorMessage) {
    std::string body = R"({"refreshToken":")" + refreshToken + R"("})";
    std::vector<std::wstring> headers = {
        L"Content-Type: application/json",
        L"User-Agent: KiroCreditsFetcher/1.0"
    };
    unsigned long status = 0;
    auto resp = HttpPost(kAuthHost, kAuthPath, headers, body, status, errorMessage);
    if (!resp || status != 200) return std::nullopt;
    jsonlite::Parser p(*resp);
    auto root = p.Parse();
    if (!root) return std::nullopt;
    auto* at = root->Find("accessToken");
    if (!at || !at->IsString()) return std::nullopt;
    auto s = at->AsString();
    if (!s || s->empty()) return std::nullopt;
    return std::string(*s);
}

KiroCreditsData KiroCreditsFetcher::Fetch() {
    std::lock_guard<std::mutex> lock(s_mutex);
    double now = static_cast<double>(time(nullptr));

    if (!enabled_) {
        KiroCreditsData d;
        d.success = false;
        d.errorMessage = L"Kiro disabled";
        return d;
    }
    if (now < backoffUntil_ && cachedData_.success) {
        KiroCreditsData stale = cachedData_;
        stale.errorMessage = L"Rate limited, retrying later";
        return stale;
    }
    if (now < nextFetchTime_ && cachedData_.success) return cachedData_;
    nextFetchTime_ = now + kMinInterval;

    std::wstring werr;
    std::string accessToken, refreshToken, profileArn;
    if (!LoadCredentials(accessToken, refreshToken, profileArn, &werr)) {
        KiroCreditsData d; d.success = false; d.errorMessage = werr.empty() ? L"Kiro auth failed" : werr;
        if (cachedData_.success) { KiroCreditsData s = cachedData_; s.errorMessage = d.errorMessage; return s; }
        return d;
    }

    auto newToken = RefreshAccessToken(refreshToken, &werr);
    if (!newToken) {
        KiroCreditsData d; d.success = false;
        d.errorMessage = werr.empty() ? L"Kiro token refresh failed" : werr;
        if (cachedData_.success) { KiroCreditsData s = cachedData_; s.errorMessage = d.errorMessage; return s; }
        return d;
    }
    accessToken = *newToken;

    std::string encodedArn = UrlEncode(profileArn);
    std::wstring path = Utf8ToWide(
        std::string("/getUsageLimits?isEmailRequired=true&origin=AI_EDITOR")
        + "&resourceType=AGENTIC_REQUEST&profileArn=" + encodedArn);

    std::wstring uuid = Utf8ToWide(GenerateUuid());
    std::vector<std::wstring> headers = {
        L"Authorization: Bearer " + Utf8ToWide(accessToken),
        L"x-amz-user-agent: aws-sdk-js/1.0.0 KiroIDE-0.7.5",
        L"user-agent: aws-sdk-js/1.0.0 ua/2.1 os/windows lang/js api/codewhispereruntime#1.0.0 KiroIDE-0.7.5",
        L"amz-sdk-invocation-id: " + uuid,
        L"amz-sdk-request: attempt=1; max=1"
    };

    unsigned long status = 0;
    auto resp = HttpGet(kUsageHost, path, headers, status, &werr);
    if (!resp || status != 200) {
        KiroCreditsData d; d.success = false;
        d.errorMessage = werr.empty() ? (L"Kiro usage HTTP " + std::to_wstring(status)) : werr;
        if (cachedData_.success) { KiroCreditsData s = cachedData_; s.errorMessage = d.errorMessage; return s; }
        return d;
    }

    jsonlite::Parser p(*resp);
    auto root = p.Parse();
    KiroCreditsData data;
    data.success = false;
    if (!root) { data.errorMessage = L"Kiro usage JSON parse failed"; }
    else {
        long long used = 0, limit = 500;
        if (auto* list = root->Find("usageBreakdownList")) {
            if (list->IsArray()) {
                if (const auto* arr = list->AsArray()) {
                    for (const auto& item : *arr) {
                        const auto* io = item.AsObject();
                        if (!io) continue;
                        auto* rt = io->Find("resourceType");
                        auto rs = rt ? rt->AsString() : std::nullopt;
                        if (!rs || *rs != "CREDIT") continue;
                        if (auto* cu = io->Find("currentUsage")) if (auto v = cu->AsNumber()) used = (long long)*v;
                        if (auto* lu = io->Find("usageLimit")) if (auto v = lu->AsNumber()) limit = (long long)*v;
                        break;
                    }
                }
            }
        } else {
            if (auto* u = root->Find("used")) if (auto v = u->AsNumber()) used = (long long)*v;
            if (auto* l = root->Find("limit")) if (auto v = l->AsNumber()) limit = (long long)*v;
            else if (auto* l = root->Find("totalCredits")) if (auto v = l->AsNumber()) limit = (long long)*v;
        }
        data.used = used;
        data.limit = limit;
        data.remaining = limit - used;
        data.percent = limit > 0 ? static_cast<int>(std::lround((double)data.remaining / (double)limit * 100.0)) : -1;
        data.profileArn = profileArn;
        data.success = true;
        data.lastSuccessTime = now;
        backoffSeconds_ = 0;
    }

    if (!data.success) {
        if (cachedData_.success) { KiroCreditsData s = cachedData_; s.errorMessage = data.errorMessage; return s; }
        return data;
    }
    cachedData_ = data;
    return data;
}
