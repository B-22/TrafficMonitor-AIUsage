#pragma once

#include "CodexUsageCore.h"
#include "ProxyHelper.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

class CodexUsageFetcher {
public:
    UsageSnapshot Fetch() const;
    void SetProxyConfig(const ProxyConfig& config) {
        proxyConfig_ = config;
        lastRadarFetch_ = {};
        lastResetTodayFetch_ = {};
    }
    void SetRadarEnabled(bool enabled) {
        radarEnabled_ = enabled;
        lastRadarFetch_ = {};
        lastResetTodayFetch_ = {};
    }
    void SetRadarRefreshMinutes(int minutes);
    void SetResetTodayRefreshMinutes(int minutes);

private:
    struct AuthData {
        std::string accessToken;
        std::string accountId;
    };

    std::wstring ResolveAuthJsonPath() const;
    std::optional<AuthData> ReadAuthData(std::wstring* errorMessage) const;
    std::optional<std::string> LoadFileUtf8(const std::wstring& path, std::wstring* errorMessage) const;
    std::optional<std::string> HttpGetUsageJson(
        const AuthData& auth,
        std::wstring* errorMessage) const;
    std::optional<std::string> HttpGetResetCreditsJson(
        const AuthData& auth,
        std::wstring* errorMessage) const;
    ResetRadarSnapshot FetchResetRadar() const;

    mutable ProxyConfig proxyConfig_;
    mutable bool radarEnabled_ = true;
    mutable int radarRefreshMinutes_ = 15;
    mutable int resetTodayRefreshMinutes_ = 60;
    mutable std::chrono::steady_clock::time_point lastRadarFetch_{};
    mutable std::chrono::steady_clock::time_point lastResetTodayFetch_{};
    mutable std::string cachedRunwayJson_;
    mutable ResetRadarSnapshot cachedCommunityRadar_;
    mutable ResetRadarSnapshot cachedRadar_;
};
