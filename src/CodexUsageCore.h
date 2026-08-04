#pragma once

#include <optional>
#include <string>
#include <vector>

struct UsageWindow {
    int usedPercent = 0;
    int remainingPercent = -1;
    int windowSeconds = 0;
    int resetAfterSeconds = 0;
    long long resetAtUnixSeconds = 0;
};

struct ResetCredit {
    std::wstring status;
    std::wstring title;
    long long grantedAtUnixSeconds = 0;
    long long expiresAtUnixSeconds = 0;
};

struct ResetCreditsSnapshot {
    bool success = false;
    int availableCount = 0;
    long long fetchedAtUnixSeconds = 0;
    std::wstring errorMessage;
    std::vector<ResetCredit> credits;
};

enum class ResetTodayState {
    Unknown,
    No,
    Yes,
};

struct ResetRadarSnapshot {
    bool success = false;
    bool windowOpen = false;
    bool summaryWindowOpen = false;
    bool forecastWindowOpen = false;
    bool runwaySourceAvailable = false;
    bool runwayPrimary = false;
    ResetTodayState todayState = ResetTodayState::Unknown;
    int probability24h = -1;
    int probability48h = -1;
    int confidencePercent = -1;
    long long updatedAtUnixSeconds = 0;
    long long openedAtUnixSeconds = 0;
    long long checkedAtUnixSeconds = 0;
    long long nextScheduledAtUnixSeconds = 0;
    long long latestResetAtUnixSeconds = 0;
    std::wstring message;
    std::wstring evidenceKind;
    std::wstring evidenceUrl;
    std::vector<std::wstring> scopePlans;
    std::vector<std::wstring> scopeWindows;
    std::wstring errorMessage;
};

struct UsageSnapshot {
    bool success = false;
    std::wstring email;
    std::wstring planType;
    std::wstring errorMessage;
    UsageWindow fiveHour;
    UsageWindow weekly;
    ResetCreditsSnapshot resetCredits;
    ResetRadarSnapshot resetRadar;
};

enum class FreshnessLevel {
    Fresh,
    Warning,
    Stale,
};

std::wstring Utf8ToWide(const std::string& input);
std::wstring FormatIsoDateAsMonthDay(const std::string& text);
UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage);
ResetCreditsSnapshot ParseResetCreditsJson(const std::string& jsonText, std::wstring* errorMessage);
ResetRadarSnapshot ParseResetRadarSummaryJson(const std::string& jsonText, std::wstring* errorMessage);
ResetRadarSnapshot ParseResetForecastJson(const std::string& jsonText, std::wstring* errorMessage);
ResetRadarSnapshot ParseCodexRunwayResetStatusJson(
    const std::string& jsonText,
    long long nowUnixSeconds,
    std::wstring* errorMessage);
ResetRadarSnapshot MergeResetRadarSnapshots(
    const ResetRadarSnapshot& summary,
    const ResetRadarSnapshot& forecast,
    long long nowUnixSeconds,
    long long maximumSignalAgeSeconds = 12 * 60 * 60);
ResetRadarSnapshot MergeResetRadarSnapshots(
    const ResetRadarSnapshot& runway,
    const ResetRadarSnapshot& summary,
    const ResetRadarSnapshot& forecast,
    long long nowUnixSeconds,
    long long maximumSignalAgeSeconds = 12 * 60 * 60);
std::optional<long long> ParseIso8601UtcSeconds(const std::string& text);
long long EarliestAvailableResetCreditExpiry(
    const ResetCreditsSnapshot& snapshot,
    long long nowUnixSeconds);
bool IsResetCreditExpiringSoon(
    const ResetCreditsSnapshot& snapshot,
    long long nowUnixSeconds,
    int warningHours);
std::wstring FormatResetCreditWarning(
    int availableCount,
    long long secondsRemaining);
FreshnessLevel ClassifyFreshness(double ageSeconds);
FreshnessLevel ClassifyFreshnessForDisplay(double ageSeconds, bool refreshInProgress);
bool ShouldReplaceResetWithFreshness(double ageSeconds);
bool ShouldShowCountdown(long long secondsRemaining, int showBeforeHours);
std::wstring FormatRemainingPercent(int remainingPercent);
std::wstring FormatResetAfter(int seconds);
std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot);
