#include "CodexUsageCore.h"

#include "JsonLite.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>

namespace {

constexpr int kFiveHourWindowSeconds = 5 * 60 * 60;
constexpr int kWeeklyWindowSeconds = 7 * 24 * 60 * 60;

bool ExtractWindow(const jsonlite::Value* windowNode, UsageWindow* output) {
    if (windowNode == nullptr || output == nullptr) {
        return false;
    }

    const jsonlite::Value* usedPercent = windowNode->Find("used_percent");
    const jsonlite::Value* limitWindowSeconds = windowNode->Find("limit_window_seconds");
    const jsonlite::Value* resetAfterSeconds = windowNode->Find("reset_after_seconds");
    const jsonlite::Value* resetAt = windowNode->Find("reset_at");
    if (usedPercent == nullptr || limitWindowSeconds == nullptr || resetAfterSeconds == nullptr || resetAt == nullptr) {
        return false;
    }

    auto used = usedPercent->AsInt();
    auto limit = limitWindowSeconds->AsInt();
    auto resetAfter = resetAfterSeconds->AsInt();
    auto resetAtValue = resetAt->AsNumber();
    if (!used.has_value() || !limit.has_value() || !resetAfter.has_value() || !resetAtValue.has_value()) {
        return false;
    }

    output->usedPercent = std::clamp(*used, 0, 100);
    output->remainingPercent = 100 - output->usedPercent;
    output->windowSeconds = std::max(*limit, 0);
    output->resetAfterSeconds = std::max(*resetAfter, 0);
    output->resetAtUnixSeconds = static_cast<long long>(*resetAtValue);
    return true;
}

void AssignWindowByDuration(const UsageWindow& window, UsageSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    if (window.windowSeconds == kFiveHourWindowSeconds) {
        snapshot->fiveHour = window;
    } else if (window.windowSeconds == kWeeklyWindowSeconds) {
        snapshot->weekly = window;
    }
}

}  // namespace

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    UsageSnapshot snapshot;
    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* email = root->Find("email");
    const jsonlite::Value* planType = root->Find("plan_type");
    const jsonlite::Value* rateLimit = root->Find("rate_limit");
    const jsonlite::Value* primaryWindow = rateLimit != nullptr ? rateLimit->Find("primary_window") : nullptr;
    const jsonlite::Value* secondaryWindow = rateLimit != nullptr ? rateLimit->Find("secondary_window") : nullptr;
    UsageWindow primaryUsageWindow;
    if (!ExtractWindow(primaryWindow, &primaryUsageWindow)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage payload missing primary rate_limit window";
        }
        return snapshot;
    }
    AssignWindowByDuration(primaryUsageWindow, &snapshot);

    if (secondaryWindow != nullptr) {
        UsageWindow secondaryUsageWindow;
        if (ExtractWindow(secondaryWindow, &secondaryUsageWindow)) {
            AssignWindowByDuration(secondaryUsageWindow, &snapshot);
        }
    }

    if (auto emailString = email != nullptr ? email->AsString() : std::nullopt; emailString.has_value()) {
        snapshot.email = Utf8ToWide(std::string(*emailString));
    }
    if (auto planTypeString = planType != nullptr ? planType->AsString() : std::nullopt; planTypeString.has_value()) {
        snapshot.planType = Utf8ToWide(std::string(*planTypeString));
    }

    snapshot.success = true;
    return snapshot;
}

std::optional<long long> ParseIso8601UtcSeconds(const std::string& text) {
    // Accept the forms used by the Claude API:
    // YYYY-MM-DD, YYYY-MM-DDTHH:MM[:SS[.fraction]][Z|+HH:MM|-HH:MM].
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }

    const auto parseDigits = [&](size_t offset, size_t count) -> std::optional<int> {
        if (offset + count > text.size()) return std::nullopt;
        int value = 0;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char ch = static_cast<unsigned char>(text[offset + i]);
            if (!std::isdigit(ch)) return std::nullopt;
            value = value * 10 + (ch - '0');
        }
        return value;
    };

    const auto yearValue = parseDigits(0, 4);
    const auto monthValue = parseDigits(5, 2);
    const auto dayValue = parseDigits(8, 2);
    if (!yearValue || !monthValue || !dayValue) return std::nullopt;

    int hour = 0;
    int minute = 0;
    int second = 0;
    size_t pos = 10;
    if (pos < text.size()) {
        if (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ') return std::nullopt;
        if (pos + 6 > text.size() || text[pos + 3] != ':') return std::nullopt;
        const auto hourValue = parseDigits(pos + 1, 2);
        const auto minuteValue = parseDigits(pos + 4, 2);
        if (!hourValue || !minuteValue) return std::nullopt;
        hour = *hourValue;
        minute = *minuteValue;
        pos += 6;

        if (pos < text.size() && text[pos] == ':') {
            const auto secondValue = parseDigits(pos + 1, 2);
            if (!secondValue) return std::nullopt;
            second = *secondValue;
            pos += 3;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            const size_t fractionStart = pos;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
            if (pos == fractionStart) return std::nullopt;
        }
    }

    int offsetSeconds = 0;
    if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
        ++pos;
    } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        const int sign = text[pos] == '+' ? 1 : -1;
        ++pos;
        const auto offsetHour = parseDigits(pos, 2);
        if (!offsetHour) return std::nullopt;
        pos += 2;
        if (pos < text.size() && text[pos] == ':') ++pos;
        const auto offsetMinute = parseDigits(pos, 2);
        if (!offsetMinute || *offsetHour > 23 || *offsetMinute > 59) return std::nullopt;
        pos += 2;
        offsetSeconds = sign * (*offsetHour * 3600 + *offsetMinute * 60);
    }
    if (pos != text.size() || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }

    using namespace std::chrono;
    const year_month_day date{
        year{*yearValue}, month{static_cast<unsigned>(*monthValue)}, day{static_cast<unsigned>(*dayValue)}};
    if (!date.ok()) return std::nullopt;

    const auto utc = sys_days{date}
        + hours{hour} + minutes{minute} + seconds{second} - seconds{offsetSeconds};
    return duration_cast<seconds>(utc.time_since_epoch()).count();
}

FreshnessLevel ClassifyFreshness(double ageSeconds) {
    if (ageSeconds >= 5 * 60) return FreshnessLevel::Stale;
    if (ageSeconds >= 60) return FreshnessLevel::Warning;
    return FreshnessLevel::Fresh;
}

bool ShouldShowCountdown(long long secondsRemaining, int showBeforeHours) {
    if (secondsRemaining <= 0 || showBeforeHours <= 0) return false;
    return secondsRemaining <= static_cast<long long>(showBeforeHours) * 60 * 60;
}

std::wstring FormatRemainingPercent(int remainingPercent) {
    if (remainingPercent < 0 || remainingPercent > 100) {
        return L"--";
    }
    return std::to_wstring(remainingPercent) + L"%";
}

std::wstring FormatResetAfter(int seconds) {
    if (seconds <= 0) {
        return L"now";
    }

    const int days = seconds / 86400;
    seconds %= 86400;
    const int hours = seconds / 3600;
    seconds %= 3600;
    const int minutes = seconds / 60;

    if (days > 0) {
        return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h";
    }
    if (hours > 0) {
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }
    return std::to_wstring(minutes) + L"m";
}

std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot) {
    if (!snapshot.success) {
        if (snapshot.errorMessage.empty()) {
            return L"Codex Usage: no data";
        }
        return L"Codex Usage error: " + snapshot.errorMessage;
    }

    const auto formatWindow = [](const UsageWindow& window) {
        const std::wstring remaining = FormatRemainingPercent(window.remainingPercent);
        if (window.remainingPercent < 0) {
            return remaining;
        }
        return remaining + L" (" + FormatResetAfter(window.resetAfterSeconds) + L"\u540e\u91cd\u7f6e)";
    };

    std::wstring text = L"Codex: 5\u5c0f\u65f6\u5269\u4f59 " + formatWindow(snapshot.fiveHour);
    text += L"\nCodex: \u672c\u5468\u5269\u4f59 " + formatWindow(snapshot.weekly);
    return text;
}
