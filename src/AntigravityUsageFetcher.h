#pragma once

#include "ProxyHelper.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

// One Antigravity model's quota as returned by fetchAvailableModels.
struct AntigravityModelQuota {
    std::string modelName;     // raw key, e.g. "gemini-3.5-pro"
    std::string displayName;   // friendly, e.g. "Gemini 3.5 Pro"
    double remainingFraction = 0.0;  // 0..1
    std::string resetTime;     // ISO 8601 UTC
    bool isExhausted = false;
    int percent() const {
        return static_cast<int>(std::lround(remainingFraction * 100.0));
    }
};

struct AntigravityUsageData {
    bool success = false;
    std::wstring errorMessage;
    std::string tier;          // FREE / PRO / TEAMS / ...
    std::string projectId;
    std::string userEmail;
    std::vector<AntigravityModelQuota> models;
    double lastSuccessTime = 0;
};

// Fetches Antigravity (Google Cloud Code) model quotas.
//
// Auth: a Google OAuth access token (+ refresh token) supplied via config.
// The plugin refreshes the access token automatically through the public
// Google token endpoint using the Cloud Code OAuth client id/secret (the
// same installed-app credentials the reference AntigravityQuotaWatcher /
// Float desktop app use). Token never leaves the machine.
class AntigravityUsageFetcher {
public:
    AntigravityUsageData Fetch();

    void SetProxyConfig(const ProxyConfig& config) { proxyConfig_ = config; }
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool IsEnabled() const { return enabled_; }

    // Credentials from AIUsage.ini [Antigravity]. Empty ClientId falls back to
    // the built-in Cloud Code installed-app client id. The client secret is
    // NOT embedded in the binary: it is taken from the ini value, or from the
    // AIUSAGE_AG_CLIENT_SECRET environment variable as a fallback (kept out
    // of git to avoid GitHub secret-scan push blocks; Google treats
    // installed-app secrets as non-confidential).
    void SetCredentials(const std::string& accessToken,
                        const std::string& refreshToken,
                        const std::string& clientId,
                        const std::string& clientSecret) {
        accessToken_ = accessToken;
        refreshToken_ = refreshToken;
        clientId_ = clientId.empty() ? kDefaultClientId : clientId;
        clientSecret_ = clientSecret;
    }

    // Override which model is featured in the dashboard ring. Empty = auto.
    void SetPrimaryModel(const std::string& model) { primaryModel_ = model; }

private:
    static constexpr const char* kApiHost = "cloudcode-pa.googleapis.com";
    static constexpr const char* kLoadCodeAssistPath = "/v1internal:loadCodeAssist";
    static constexpr const char* kFetchModelsPath = "/v1internal:fetchAvailableModels";
    static constexpr const char* kTokenHost = "oauth2.googleapis.com";
    static constexpr const char* kTokenPath = "/token";
    // Public Cloud Code OAuth client id (installed-app). Google treats the
    // installed-app client_secret as non-confidential and desktop clients ship
    // it in plain text; we still keep it out of the binary/git to avoid
    // GitHub secret-scan push blocks — supply it via AIUsage.ini ClientSecret
    // or the AIUSAGE_AG_CLIENT_SECRET environment variable.
    static constexpr const char* kDefaultClientId =
        "1071006060591-tmhssin2h21lcre235vtolojh4g403ep.apps.googleusercontent.com";

    static constexpr double kMinInterval = 60.0;
    static constexpr double kBackoffMin = 120.0;
    static constexpr double kBackoffMax = 900.0;

    std::optional<std::string> HttpPost(const std::string& host, const std::string& path,
        const std::vector<std::wstring>& headers, const std::string& body,
        unsigned long& statusCode, std::wstring* errorMessage);

    // Ensure a valid (non-expired) access token, refreshing if needed.
    bool EnsureAccessToken(std::wstring* errorMessage);

    bool RefreshAccessToken(std::wstring* errorMessage);

    bool ParseModels(const std::string& body, AntigravityUsageData& out);

    ProxyConfig proxyConfig_;
    bool enabled_ = false;
    std::string accessToken_;
    std::string refreshToken_;
    std::string clientId_;   // from AIUsage.ini [Antigravity] ClientId
    std::string clientSecret_; // from AIUsage.ini [Antigravity] ClientSecret
    std::string primaryModel_;
    double tokenExpiry_ = 0.0;     // unix seconds
    double nextFetchTime_ = 0.0;
    double backoffUntil_ = 0.0;
    double backoffSeconds_ = 0.0;
    AntigravityUsageData cachedData_;
};
