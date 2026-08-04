#pragma once

#include "ProxyHelper.h"

#include <optional>
#include <string>
#include <vector>

// Kiro IDE credits (AWS SSO backed).
struct KiroCreditsData {
    bool success = false;
    std::wstring errorMessage;
    long long used = 0;
    long long limit = 500;       // default monthly credit allowance
    long long remaining = 0;
    int percent = -1;            // remaining percentage 0..100
    std::string profileArn;
    double lastSuccessTime = 0;
};

// Fetches Kiro Credits from the Kiro desktop auth/usage endpoints.
//
// Auth: reads the local AWS SSO cache written by the Kiro IDE
// (%USERPROFILE%\.aws\sso\cache\kiro-auth-token.json), refreshes the access
// token via the Kiro auth endpoint, then GETs getUsageLimits. No browser
// flow required; works whenever Kiro has been logged in on this machine.
class KiroCreditsFetcher {
public:
    KiroCreditsData Fetch();

    void SetProxyConfig(const ProxyConfig& config) { proxyConfig_ = config; }
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool IsEnabled() const { return enabled_; }

    // Optional override of the token cache path (default:
    // %USERPROFILE%\.aws\sso\cache\kiro-auth-token.json).
    void SetTokenFilePath(const std::wstring& path) { tokenPathOverride_ = path; }

private:
    static constexpr const char* kAuthHost = "prod.us-east-1.auth.desktop.kiro.dev";
    static constexpr const char* kAuthPath = "/refreshToken";
    static constexpr const char* kUsageHost = "q.us-east-1.amazonaws.com";
    static constexpr const char* kUsagePath = "/getUsageLimits";

    static constexpr double kMinInterval = 60.0;
    static constexpr double kBackoffMin = 120.0;
    static constexpr double kBackoffMax = 900.0;

    std::wstring ResolveTokenPath() const;
    std::optional<std::string> ReadFileUtf8(const std::wstring& path);

    std::optional<std::string> HttpPost(const std::string& host, const std::string& path,
        const std::vector<std::wstring>& headers, const std::string& body,
        unsigned long& statusCode, std::wstring* errorMessage);
    std::optional<std::string> HttpGet(const std::string& host, const std::wstring& path,
        const std::vector<std::wstring>& headers, unsigned long& statusCode, std::wstring* errorMessage);

    bool LoadCredentials(std::string& accessToken, std::string& refreshToken,
        std::string& profileArn, std::wstring* errorMessage);
    std::optional<std::string> RefreshAccessToken(const std::string& refreshToken, std::wstring* errorMessage);

    ProxyConfig proxyConfig_;
    bool enabled_ = false;
    std::wstring tokenPathOverride_;
    double nextFetchTime_ = 0.0;
    double backoffUntil_ = 0.0;
    double backoffSeconds_ = 0.0;
    KiroCreditsData cachedData_;
};
