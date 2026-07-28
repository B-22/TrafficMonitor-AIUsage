#pragma once

#include <optional>
#include <string>

struct UsageWindow {
    int usedPercent = 0;
    int remainingPercent = -1;
    int windowSeconds = 0;
    int resetAfterSeconds = 0;
    long long resetAtUnixSeconds = 0;
};

struct UsageSnapshot {
    bool success = false;
    std::wstring email;
    std::wstring planType;
    std::wstring errorMessage;
    UsageWindow fiveHour;
    UsageWindow weekly;
};

enum class FreshnessLevel {
    Fresh,
    Warning,
    Stale,
};

std::wstring Utf8ToWide(const std::string& input);
std::wstring FormatIsoDateAsMonthDay(const std::string& text);
UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage);
std::optional<long long> ParseIso8601UtcSeconds(const std::string& text);
FreshnessLevel ClassifyFreshness(double ageSeconds);
FreshnessLevel ClassifyFreshnessForDisplay(double ageSeconds, bool refreshInProgress);
bool ShouldReplaceResetWithFreshness(double ageSeconds);
bool ShouldShowCountdown(long long secondsRemaining, int showBeforeHours);
std::wstring FormatRemainingPercent(int remainingPercent);
std::wstring FormatResetAfter(int seconds);
std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot);
