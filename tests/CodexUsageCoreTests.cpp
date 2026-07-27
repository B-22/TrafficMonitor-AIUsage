#include "CodexUsageCore.h"
#include "CodexUsageVersion.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void AssertTrue(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void AssertEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void AssertEqual(const std::wstring& actual, const std::wstring& expected, const char* message) {
    if (actual != expected) {
        std::wcerr << message << L": expected [" << expected << L"], got [" << actual << L"]\n";
        std::exit(1);
    }
}

void AssertNotContains(const std::wstring& actual, wchar_t needle, const char* message) {
    if (actual.find(needle) != std::wstring::npos) {
        std::wcerr << message << L": unexpected character [" << needle << L"] in [" << actual << L"]\n";
        std::exit(1);
    }
}

void ParsesUsageWindows() {
    const std::string json = R"json({
        "email": "user@example.com",
        "plan_type": "pro",
        "rate_limit": {
            "primary_window": {
                "used_percent": 23,
                "limit_window_seconds": 18000,
                "reset_after_seconds": 3600,
                "reset_at": 1780980000
            },
            "secondary_window": {
                "used_percent": 61,
                "limit_window_seconds": 604800,
                "reset_after_seconds": 86400,
                "reset_at": 1781066400
            }
        }
    })json";

    std::wstring error;
    UsageSnapshot snapshot = ParseUsageJson(json, &error);

    AssertTrue(snapshot.success, "snapshot should parse");
    AssertEqual(snapshot.fiveHour.remainingPercent, 77, "five-hour remaining percent");
    AssertEqual(snapshot.weekly.remainingPercent, 39, "weekly remaining percent");
    AssertEqual(snapshot.email, L"user@example.com", "email");
    AssertEqual(snapshot.planType, L"pro", "plan type");
}

void ClassifiesPrimaryWindowWithoutSecondaryWindow() {
    const std::string json = R"json({
        "rate_limit": {
            "primary_window": {
                "used_percent": 4,
                "limit_window_seconds": 604800,
                "reset_after_seconds": 580302,
                "reset_at": 1780980000
            }
        }
    })json";

    std::wstring error;
    UsageSnapshot snapshot = ParseUsageJson(json, &error);

    AssertTrue(snapshot.success, "snapshot should parse without secondary window");
    AssertTrue(error.empty(), "missing secondary window should not set an error");
    AssertEqual(snapshot.fiveHour.remainingPercent, -1, "missing five-hour window should be unavailable");
    AssertEqual(snapshot.weekly.remainingPercent, 96, "primary weekly remaining percent");
    AssertEqual(
        BuildUsageTooltip(snapshot),
        L"Codex: 5\u5c0f\u65f6\u5269\u4f59 --\nCodex: \u672c\u5468\u5269\u4f59 96% (6d 17h\u540e\u91cd\u7f6e)",
        "tooltip should mark missing five-hour window unavailable");
}

void FormatsDisplayText() {
    AssertEqual(FormatRemainingPercent(77), L"77%", "remaining percent text");
    AssertEqual(FormatRemainingPercent(-1), L"--", "negative percent text");
    AssertEqual(FormatRemainingPercent(101), L"--", "out-of-range percent text");
}

void ParsesIso8601Timestamps() {
    const auto utc = ParseIso8601UtcSeconds("2026-07-27T08:30:45Z");
    const auto offset = ParseIso8601UtcSeconds("2026-07-27T16:30:45.123+08:00");
    const auto compactOffset = ParseIso8601UtcSeconds("2026-07-27T03:00:45-0530");
    const auto equivalentUtc = ParseIso8601UtcSeconds("2026-07-27T08:30:45Z");

    AssertTrue(utc.has_value(), "UTC ISO timestamp should parse");
    AssertTrue(offset.has_value(), "offset ISO timestamp should parse");
    AssertTrue(compactOffset.has_value(), "compact offset ISO timestamp should parse");
    AssertEqual(static_cast<int>(*offset - *utc), 0, "offset timestamp should normalize to UTC");
    AssertEqual(static_cast<int>(*compactOffset - *equivalentUtc), 0, "negative offset should normalize to UTC");
    AssertTrue(!ParseIso8601UtcSeconds("2026-02-30T08:00:00Z").has_value(), "invalid date should fail");
    AssertTrue(!ParseIso8601UtcSeconds("2026-07-27T25:00:00Z").has_value(), "invalid time should fail");
}

void ClassifiesFreshnessAndCountdownWindow() {
    AssertTrue(ClassifyFreshness(59) == FreshnessLevel::Fresh, "59 seconds should be fresh");
    AssertTrue(ClassifyFreshness(60) == FreshnessLevel::Warning, "one minute should warn");
    AssertTrue(ClassifyFreshness(299) == FreshnessLevel::Warning, "under five minutes should warn");
    AssertTrue(ClassifyFreshness(300) == FreshnessLevel::Stale, "five minutes should be stale");

    AssertTrue(ShouldShowCountdown(12 * 3600, 12), "countdown should show at threshold");
    AssertTrue(!ShouldShowCountdown(12 * 3600 + 1, 12), "countdown should hide before threshold");
    AssertTrue(!ShouldShowCountdown(0, 12), "expired countdown should hide");
}

void BuildsMultiLineSuccessTooltip() {
    UsageSnapshot snapshot;
    snapshot.success = true;
    snapshot.fiveHour.remainingPercent = 77;
    snapshot.fiveHour.resetAfterSeconds = 3600;
    snapshot.weekly.remainingPercent = 39;
    snapshot.weekly.resetAfterSeconds = 86400;

    const std::wstring tooltip = BuildUsageTooltip(snapshot);

    AssertEqual(
        tooltip,
        L"Codex: 5\u5c0f\u65f6\u5269\u4f59 77% (1h 0m\u540e\u91cd\u7f6e)\nCodex: \u672c\u5468\u5269\u4f59 39% (1d 0h\u540e\u91cd\u7f6e)",
        "multi-line success tooltip");
}

void HasGeneratedVersion() {
    AssertTrue(std::wstring(CODEX_USAGE_VERSION_WIDE).size() > 0, "generated version should not be empty");
}

}  // namespace

int main() {
    ParsesUsageWindows();
    ClassifiesPrimaryWindowWithoutSecondaryWindow();
    FormatsDisplayText();
    ParsesIso8601Timestamps();
    ClassifiesFreshnessAndCountdownWindow();
    BuildsMultiLineSuccessTooltip();
    HasGeneratedVersion();
    return 0;
}
