#pragma once

#include "ProxyHelper.h"

#include <optional>
#include <string>
#include <vector>

struct ClaudeUsageData {
    bool success = false;
    std::wstring errorMessage;
    int fiveHourPercent = -1;  // used percent
    int sevenDayPercent = -1;
    std::string fiveHourResetAt;  // ISO 8601 UTC
    std::string sevenDayResetAt;
    double lastSuccessTime = 0;  // Unix seconds

    // Credits / overage
    long long usedCredits = 0;   // raw integer from API
    std::string currency;        // e.g., "USD"
    int decimalPlaces = 2;

    // Subscription
    std::string subscriptionStatus; // "active", "canceled", etc.
};

class ClaudeUsageFetcher {
public:
    ClaudeUsageData Fetch();
    void SetProxyConfig(const ProxyConfig& config) { proxyConfig_ = config; }

private:
    struct AuthHeaders {
        std::string accessToken;
        std::string error;
        bool forceRefresh = false;
    };

    AuthHeaders GetHeaders(bool forceRefresh = false);
    std::optional<std::string> RefreshCliToken(const std::string& refreshToken);
    std::optional<std::string> HttpGet(const std::string& host, const std::string& path,
        const std::vector<std::wstring>& headers, unsigned long& statusCode, std::wstring* errorMessage);

    // Cached data
    ClaudeUsageData cachedData_;
    double nextFetchTime_ = 0;
    double backoffUntil_ = 0;
    double backoffSeconds_ = 0;
    bool refreshInProgress_ = false;
    bool profileFetched_ = false;
    std::string cachedSubscriptionStatus_;
    ProxyConfig proxyConfig_;
};
