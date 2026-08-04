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
    AssertEqual(
        FormatIsoDateAsMonthDay("2099-08-13"), L"8.13",
        "date-only subscription expiry should use month.day");
    AssertEqual(
        FormatIsoDateAsMonthDay("2026-02-30"), L"--",
        "invalid subscription expiry should be rejected");
    AssertEqual(
        FormatIsoDateAsMonthDay("2024-02-29"), L"2.29",
        "leap-day subscription expiry should be accepted");
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
    AssertTrue(
        ClassifyFreshnessForDisplay(60, true) == FreshnessLevel::Fresh,
        "in-flight refresh should suppress the one-minute boundary flash");
    AssertTrue(
        ClassifyFreshnessForDisplay(119, true) == FreshnessLevel::Fresh,
        "short in-flight refresh should remain visually fresh");
    AssertTrue(
        ClassifyFreshnessForDisplay(120, true) == FreshnessLevel::Warning,
        "long in-flight refresh should still warn");
    AssertTrue(
        ClassifyFreshnessForDisplay(60, false) == FreshnessLevel::Warning,
        "completed failed refresh should warn immediately");
    AssertTrue(!ShouldReplaceResetWithFreshness(600), "ten minutes should retain reset weekday");
    AssertTrue(ShouldReplaceResetWithFreshness(601), "over ten minutes should replace reset weekday");

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

void ParsesResetCreditsAndExpiryWarning() {
    const std::string json = R"json({
        "available_count": 3,
        "credits": [
            {
                "status": "available",
                "title": "Full reset",
                "granted_at": "2026-07-29T00:00:00Z",
                "expires_at": "2026-07-31T00:00:00Z"
            },
            {
                "status": "available",
                "granted_at": "2026-07-29T01:00:00Z",
                "expires_at": "2026-07-30T12:00:00Z"
            },
            {
                "status": "redeemed",
                "expires_at": "2026-07-30T01:00:00Z"
            }
        ]
    })json";

    std::wstring error;
    const ResetCreditsSnapshot snapshot =
        ParseResetCreditsJson(json, &error);
    const long long now =
        *ParseIso8601UtcSeconds("2026-07-30T00:00:00Z");
    const long long expectedExpiry =
        *ParseIso8601UtcSeconds("2026-07-30T12:00:00Z");

    AssertTrue(snapshot.success, "reset credits should parse");
    AssertEqual(snapshot.availableCount, 3, "available reset-credit count");
    AssertEqual(
        static_cast<int>(
            EarliestAvailableResetCreditExpiry(snapshot, now)
            - expectedExpiry),
        0,
        "earliest available reset-credit expiry");
    AssertTrue(
        IsResetCreditExpiringSoon(snapshot, now, 48),
        "credit within 48 hours should warn");
    AssertTrue(
        !IsResetCreditExpiringSoon(snapshot, now, 11),
        "credit outside 11 hours should not warn");
    AssertEqual(
        FormatResetCreditWarning(3, 12 * 3600),
        L"3\u536112h",
        "reset-credit warning text");
}

void ParsesAndMergesResetRadarSignals() {
    const std::string summaryJson = R"json({
        "monitored_at": "2026-07-30T08:00:00+08:00",
        "window_open": true,
        "window": {
            "open": true,
            "message": "Lands in the next hour",
            "opened_at": "2026-07-30T07:50:00+08:00"
        },
        "prediction": {
            "probability_24h": 0.55,
            "probability_48h": 0.75
        }
    })json";
    const std::string quietForecastJson = R"json({
        "updated_at": "2026-07-30T00:05:00Z",
        "official_signal": null,
        "probabilities": {
            "rounded_24h": 15,
            "rounded_48h": 30
        }
    })json";
    const std::string activeForecastJson = R"json({
        "updated_at": "2026-07-30T00:05:00Z",
        "official_signal": {
            "at": "2026-07-30T00:01:00Z",
            "summary": "Reset announced"
        },
        "probabilities": {
            "rounded_24h": 90,
            "rounded_48h": 95
        }
    })json";
    const std::string malformedSignalForecastJson = R"json({
        "updated_at": "2026-07-30T00:05:00Z",
        "official_signal": false,
        "probabilities": {
            "rounded_24h": 15,
            "rounded_48h": 30
        }
    })json";

    std::wstring error;
    const ResetRadarSnapshot summary =
        ParseResetRadarSummaryJson(summaryJson, &error);
    const ResetRadarSnapshot quiet =
        ParseResetForecastJson(quietForecastJson, &error);
    const ResetRadarSnapshot active =
        ParseResetForecastJson(activeForecastJson, &error);
    const ResetRadarSnapshot malformedSignal =
        ParseResetForecastJson(malformedSignalForecastJson, &error);
    const long long now =
        *ParseIso8601UtcSeconds("2026-07-30T00:10:00Z");

    AssertTrue(summary.success, "radar summary should parse");
    AssertTrue(quiet.success, "quiet reset forecast should parse");
    AssertTrue(active.success, "active reset forecast should parse");
    AssertTrue(!quiet.forecastWindowOpen, "null official signal should be quiet");
    AssertTrue(active.forecastWindowOpen, "object official signal should open window");
    AssertTrue(
        !malformedSignal.forecastWindowOpen,
        "unexpected official signal types must fail quiet");

    const ResetRadarSnapshot merged =
        MergeResetRadarSnapshots(summary, quiet, now);
    AssertTrue(merged.windowOpen, "fresh summary signal should open merged window");
    AssertEqual(merged.probability24h, 15, "forecast probability should win");
    AssertEqual(merged.probability48h, 30, "forecast 48h probability should win");

    const ResetRadarSnapshot activeMerged =
        MergeResetRadarSnapshots({}, active, now);
    AssertTrue(
        activeMerged.windowOpen,
        "fresh official signal should open merged window");
    AssertEqual(
        activeMerged.message,
        L"Reset announced",
        "official signal summary");

    const long long staleNow = now + 13 * 60 * 60;
    const ResetRadarSnapshot stale =
        MergeResetRadarSnapshots(summary, active, staleNow);
    AssertTrue(
        !stale.windowOpen,
        "signals older than the maximum age should be ignored");
}

void ParsesCodexRunwayResetTodayFeed() {
    const long long now =
        *ParseIso8601UtcSeconds("2026-07-31T12:00:00Z");
    const std::string completedJson = R"json({
        "schemaVersion": 1,
        "generatedAt": "2026-07-31T12:00:00Z",
        "lastSuccessfulCheckAt": "2026-07-31T12:00:00Z",
        "monitor": {
            "status": "ok",
            "errorCode": null
        },
        "events": [{
            "kind": "reset_completed",
            "announcedAt": "2026-07-31T12:00:00Z",
            "effectiveAt": "2026-07-31T12:00:00Z",
            "confidence": 0.9,
            "rationale": "Explicit Codex quota reset announcement.",
            "source": {
                "handle": "thsottiaux",
                "postId": "1951234567890123456",
                "url": "https://x.com/thsottiaux/status/1951234567890123456"
            },
            "scope": {
                "plans": ["all"],
                "windows": ["weekly"]
            }
        }]
    })json";

    std::wstring error;
    const ResetRadarSnapshot completed =
        ParseCodexRunwayResetStatusJson(completedJson, now, &error);
    AssertTrue(completed.success, "Runway feed should parse");
    AssertTrue(
        completed.todayState == ResetTodayState::Yes,
        "same-day completed reset should resolve to yes");
    AssertTrue(completed.windowOpen, "Runway yes should open reset window");
    AssertEqual(completed.confidencePercent, 90, "Runway confidence");
    AssertEqual(
        completed.evidenceUrl,
        L"https://x.com/thsottiaux/status/1951234567890123456",
        "Runway evidence URL");
    AssertTrue(
        completed.latestResetAtUnixSeconds == now,
        "completed reset timestamp should be preserved");

    const std::string quietJson = R"json({
        "schemaVersion": 1,
        "generatedAt": "2026-07-31T12:00:00Z",
        "lastSuccessfulCheckAt": "2026-07-31T12:00:00Z",
        "monitor": {
            "status": "ok",
            "errorCode": null
        },
        "events": []
    })json";
    const ResetRadarSnapshot quiet =
        ParseCodexRunwayResetStatusJson(quietJson, now, &error);
    AssertTrue(quiet.success, "quiet Runway feed should parse");
    AssertTrue(
        quiet.todayState == ResetTodayState::No,
        "fresh feed without same-day evidence should resolve to no");

    ResetRadarSnapshot forecast;
    forecast.success = true;
    forecast.windowOpen = true;
    forecast.forecastWindowOpen = true;
    forecast.probability24h = 88;
    forecast.updatedAtUnixSeconds = now;
    const ResetRadarSnapshot merged =
        MergeResetRadarSnapshots(quiet, {}, forecast, now);
    AssertTrue(
        merged.runwayPrimary,
        "fresh conclusive Runway state should be primary");
    AssertTrue(
        !merged.windowOpen,
        "Runway no should override a secondary open-window forecast");
    AssertEqual(
        merged.probability24h,
        88,
        "secondary probability should remain available");

    const long long staleNow = now + 31 * 60 * 60;
    const ResetRadarSnapshot stale =
        ParseCodexRunwayResetStatusJson(quietJson, staleNow, &error);
    AssertTrue(stale.success, "stale Runway payload should remain parseable");
    AssertTrue(
        stale.todayState == ResetTodayState::Unknown,
        "stale Runway payload should fail to unknown");

    std::string invalidSourceJson = completedJson;
    const std::string validHost = "https://x.com/thsottiaux/status/";
    const size_t sourcePosition = invalidSourceJson.find(validHost);
    AssertTrue(
        sourcePosition != std::string::npos,
        "test fixture should contain source host");
    invalidSourceJson.replace(
        sourcePosition, validHost.size(), "https://example.com/status/");
    const ResetRadarSnapshot invalid =
        ParseCodexRunwayResetStatusJson(invalidSourceJson, now, &error);
    AssertTrue(
        !invalid.success,
        "non-canonical Runway evidence URL should be rejected");
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
    ParsesResetCreditsAndExpiryWarning();
    ParsesAndMergesResetRadarSignals();
    ParsesCodexRunwayResetTodayFeed();
    HasGeneratedVersion();
    return 0;
}
