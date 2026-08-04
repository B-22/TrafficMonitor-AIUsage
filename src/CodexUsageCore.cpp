#include "CodexUsageCore.h"

#include "JsonLite.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <utility>

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

const jsonlite::Value* FindEither(
    const jsonlite::Value* object,
    std::string_view first,
    std::string_view second) {
    if (object == nullptr) return nullptr;
    if (const jsonlite::Value* value = object->Find(first); value != nullptr) {
        return value;
    }
    return object->Find(second);
}

std::optional<long long> ReadTimestamp(
    const jsonlite::Value* object,
    std::string_view snakeCase,
    std::string_view camelCase) {
    const jsonlite::Value* value = FindEither(object, snakeCase, camelCase);
    if (value == nullptr) return std::nullopt;
    if (auto text = value->AsString(); text.has_value()) {
        return ParseIso8601UtcSeconds(std::string(*text));
    }
    if (auto number = value->AsNumber(); number.has_value()) {
        return static_cast<long long>(*number);
    }
    return std::nullopt;
}

std::wstring ReadWideString(
    const jsonlite::Value* object,
    std::string_view snakeCase,
    std::string_view camelCase = {}) {
    if (object == nullptr) return {};
    const jsonlite::Value* value = camelCase.empty()
        ? object->Find(snakeCase)
        : FindEither(object, snakeCase, camelCase);
    if (auto text = value != nullptr ? value->AsString() : std::nullopt;
        text.has_value()) {
        return Utf8ToWide(std::string(*text));
    }
    return {};
}

std::optional<std::string> ReadString(
    const jsonlite::Value* object,
    std::string_view key) {
    if (object == nullptr) return std::nullopt;
    const jsonlite::Value* value = object->Find(key);
    if (value == nullptr) return std::nullopt;
    if (auto text = value->AsString(); text.has_value()) {
        return std::string(*text);
    }
    return std::nullopt;
}

bool IsSameLocalCalendarDay(long long first, long long second) {
    if (first <= 0 || second <= 0) return false;
    const time_t firstTime = static_cast<time_t>(first);
    const time_t secondTime = static_cast<time_t>(second);
    tm firstLocal{};
    tm secondLocal{};
    if (localtime_s(&firstLocal, &firstTime) != 0
        || localtime_s(&secondLocal, &secondTime) != 0) {
        return false;
    }
    return firstLocal.tm_year == secondLocal.tm_year
        && firstLocal.tm_yday == secondLocal.tm_yday;
}

bool IsAsciiDigits(std::string_view text) {
    return !text.empty() && text.size() <= 30
        && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        });
}

bool ReadStringArray(
    const jsonlite::Value* object,
    std::string_view key,
    std::vector<std::wstring>* output) {
    if (object == nullptr || output == nullptr) return false;
    const jsonlite::Value* value = object->Find(key);
    const jsonlite::Value::Array* values =
        value != nullptr ? value->AsArray() : nullptr;
    if (values == nullptr) return false;
    output->clear();
    for (const jsonlite::Value& entry : *values) {
        auto text = entry.AsString();
        if (!text.has_value()) return false;
        output->push_back(Utf8ToWide(std::string(*text)));
    }
    return true;
}

const char* ExpectedRunwayRationale(std::string_view kind) {
    if (kind == "reset_completed") {
        return "Explicit Codex quota reset announcement.";
    }
    if (kind == "reset_scheduled") {
        return "Explicit Codex quota reset schedule.";
    }
    if (kind == "banked_reset") {
        return "Banked reset announcement; not a completed reset.";
    }
    if (kind == "limit_increase") {
        return "Quota limit increase announcement; not a reset.";
    }
    if (kind == "uncertain") {
        return "Relevant announcement could not be classified safely.";
    }
    return nullptr;
}

std::wstring RunwayEventMessage(std::string_view kind) {
    if (kind == "reset_completed") return L"\u5DF2\u786E\u8BA4\u5168\u5C40\u989D\u5EA6\u91CD\u7F6E";
    if (kind == "reset_scheduled") return L"\u5DF2\u516C\u5E03\u5168\u5C40\u91CD\u7F6E\u8BA1\u5212";
    if (kind == "banked_reset") return L"\u65B0\u589E\u53EF\u7528\u91CD\u7F6E\u5361";
    if (kind == "limit_increase") return L"\u5DF2\u516C\u5E03\u989D\u5EA6\u63D0\u5347";
    return L"\u76F8\u5173\u516C\u544A\u5C1A\u65E0\u6CD5\u786E\u8BA4";
}

bool IsAllowedRunwayMonitorError(std::string_view errorCode) {
    return errorCode == "configuration_error"
        || errorCode == "request_failed"
        || errorCode == "invalid_response"
        || errorCode == "uncited_source";
}

void SetParseError(std::wstring* errorMessage, const wchar_t* message) {
    if (errorMessage != nullptr) *errorMessage = message;
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

ResetCreditsSnapshot ParseResetCreditsJson(
    const std::string& jsonText,
    std::wstring* errorMessage) {
    if (errorMessage != nullptr) errorMessage->clear();

    ResetCreditsSnapshot snapshot;
    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset-credit JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* availableCount =
        FindEither(&*root, "available_count", "availableCount");
    const auto count = availableCount != nullptr
        ? availableCount->AsInt()
        : std::nullopt;
    if (!count.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset-credit payload missing available_count";
        }
        return snapshot;
    }

    snapshot.availableCount = std::max(0, *count);
    if (const jsonlite::Value* credits = root->Find("credits");
        credits != nullptr && credits->IsArray()) {
        for (const jsonlite::Value& node : *credits->AsArray()) {
            if (!node.IsObject()) continue;
            ResetCredit credit;
            credit.status = ReadWideString(&node, "status");
            credit.title = ReadWideString(&node, "title");
            if (auto granted = ReadTimestamp(
                    &node, "granted_at", "grantedAt"); granted.has_value()) {
                credit.grantedAtUnixSeconds = *granted;
            }
            if (auto expires = ReadTimestamp(
                    &node, "expires_at", "expiresAt"); expires.has_value()) {
                credit.expiresAtUnixSeconds = *expires;
            }
            snapshot.credits.push_back(std::move(credit));
        }
    }

    snapshot.success = true;
    return snapshot;
}

ResetRadarSnapshot ParseResetRadarSummaryJson(
    const std::string& jsonText,
    std::wstring* errorMessage) {
    if (errorMessage != nullptr) errorMessage->clear();

    ResetRadarSnapshot snapshot;
    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"radar summary JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* windowOpen = root->Find("window_open");
    auto open = windowOpen != nullptr ? windowOpen->AsBool() : std::nullopt;
    if (!open.has_value()) {
        const jsonlite::Value* window = root->Find("window");
        const jsonlite::Value* nestedOpen =
            window != nullptr ? window->Find("open") : nullptr;
        open = nestedOpen != nullptr ? nestedOpen->AsBool() : std::nullopt;
    }
    if (!open.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"radar summary payload missing window_open";
        }
        return snapshot;
    }

    snapshot.summaryWindowOpen = *open;
    snapshot.windowOpen = *open;
    if (auto updated = ReadTimestamp(
            &*root, "monitored_at", "monitoredAt"); updated.has_value()) {
        snapshot.updatedAtUnixSeconds = *updated;
    }

    const jsonlite::Value* window = root->Find("window");
    if (auto opened = ReadTimestamp(
            window, "opened_at", "openedAt"); opened.has_value()) {
        snapshot.openedAtUnixSeconds = *opened;
    }
    snapshot.message = ReadWideString(window, "message");

    const jsonlite::Value* prediction = root->Find("prediction");
    if (prediction != nullptr) {
        if (const jsonlite::Value* probability = prediction->Find("probability_24h");
            probability != nullptr) {
            if (auto value = probability->AsNumber(); value.has_value()) {
                snapshot.probability24h = std::clamp(
                    static_cast<int>(std::lround(*value * 100.0)), 0, 100);
            }
        }
        if (const jsonlite::Value* probability = prediction->Find("probability_48h");
            probability != nullptr) {
            if (auto value = probability->AsNumber(); value.has_value()) {
                snapshot.probability48h = std::clamp(
                    static_cast<int>(std::lround(*value * 100.0)), 0, 100);
            }
        }
    }

    snapshot.success = true;
    return snapshot;
}

ResetRadarSnapshot ParseResetForecastJson(
    const std::string& jsonText,
    std::wstring* errorMessage) {
    if (errorMessage != nullptr) errorMessage->clear();

    ResetRadarSnapshot snapshot;
    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset forecast JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* officialSignal = root->Find("official_signal");
    if (officialSignal == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset forecast payload missing official_signal";
        }
        return snapshot;
    }

    // The forecast API represents a real announcement as an object and the
    // quiet state as null. Treat every other unexpected type as quiet.
    snapshot.forecastWindowOpen = officialSignal->IsObject();
    snapshot.windowOpen = snapshot.forecastWindowOpen;
    if (auto updated = ReadTimestamp(
            &*root, "updated_at", "updatedAt"); updated.has_value()) {
        snapshot.updatedAtUnixSeconds = *updated;
    }
    if (officialSignal->IsObject()) {
        if (auto opened = ReadTimestamp(
                officialSignal, "at", "openedAt"); opened.has_value()) {
            snapshot.openedAtUnixSeconds = *opened;
        }
        snapshot.message = ReadWideString(officialSignal, "summary");
    }

    const jsonlite::Value* probabilities = root->Find("probabilities");
    if (probabilities != nullptr) {
        if (const jsonlite::Value* value = probabilities->Find("rounded_24h");
            value != nullptr) {
            if (auto probability = value->AsInt(); probability.has_value()) {
                snapshot.probability24h = std::clamp(*probability, 0, 100);
            }
        }
        if (const jsonlite::Value* value = probabilities->Find("rounded_48h");
            value != nullptr) {
            if (auto probability = value->AsInt(); probability.has_value()) {
                snapshot.probability48h = std::clamp(*probability, 0, 100);
            }
        }
    }

    snapshot.success = true;
    return snapshot;
}

ResetRadarSnapshot ParseCodexRunwayResetStatusJson(
    const std::string& jsonText,
    long long nowUnixSeconds,
    std::wstring* errorMessage) {
    if (errorMessage != nullptr) errorMessage->clear();

    ResetRadarSnapshot snapshot;
    snapshot.checkedAtUnixSeconds = nowUnixSeconds;

    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value() || !root->IsObject()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Codex Runway JSON parse failed: "
                + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* schemaNode = root->Find("schemaVersion");
    const auto schemaVersion =
        schemaNode != nullptr ? schemaNode->AsInt() : std::nullopt;
    if (!schemaVersion.has_value() || *schemaVersion != 1) {
        SetParseError(errorMessage, L"Codex Runway schemaVersion is not supported");
        return snapshot;
    }

    const auto generatedAtText = ReadString(&*root, "generatedAt");
    const auto generatedAt = generatedAtText.has_value()
        ? ParseIso8601UtcSeconds(*generatedAtText)
        : std::nullopt;
    if (!generatedAt.has_value()) {
        SetParseError(errorMessage, L"Codex Runway generatedAt is invalid");
        return snapshot;
    }

    long long lastSuccessfulCheckAt = 0;
    const jsonlite::Value* lastCheckNode =
        root->Find("lastSuccessfulCheckAt");
    if (lastCheckNode == nullptr) {
        SetParseError(
            errorMessage,
            L"Codex Runway payload is missing lastSuccessfulCheckAt");
        return snapshot;
    }
    if (!lastCheckNode->IsNull()) {
        auto lastCheckText = lastCheckNode->AsString();
        const auto parsed = lastCheckText.has_value()
            ? ParseIso8601UtcSeconds(std::string(*lastCheckText))
            : std::nullopt;
        if (!parsed.has_value()) {
            SetParseError(
                errorMessage,
                L"Codex Runway lastSuccessfulCheckAt is invalid");
            return snapshot;
        }
        lastSuccessfulCheckAt = *parsed;
    }

    const jsonlite::Value* monitor = root->Find("monitor");
    const auto monitorStatus = ReadString(monitor, "status");
    if (!monitorStatus.has_value()
        || (*monitorStatus != "ok" && *monitorStatus != "degraded")) {
        SetParseError(errorMessage, L"Codex Runway monitor status is invalid");
        return snapshot;
    }
    const jsonlite::Value* monitorError =
        monitor != nullptr ? monitor->Find("errorCode") : nullptr;
    if (monitorError == nullptr) {
        SetParseError(errorMessage, L"Codex Runway monitor errorCode is missing");
        return snapshot;
    }
    if (*monitorStatus == "ok") {
        if (!monitorError->IsNull()) {
            SetParseError(
                errorMessage,
                L"healthy Codex Runway monitor published an error code");
            return snapshot;
        }
    } else {
        const auto monitorErrorText = monitorError->AsString();
        if (!monitorErrorText.has_value()
            || !IsAllowedRunwayMonitorError(*monitorErrorText)) {
            SetParseError(
                errorMessage,
                L"degraded Codex Runway monitor error code is invalid");
            return snapshot;
        }
    }

    struct RunwayEvent {
        std::string kind;
        long long announcedAt = 0;
        long long effectiveAt = 0;
        double confidence = 0;
        std::wstring url;
        std::vector<std::wstring> plans;
        std::vector<std::wstring> windows;
    };

    const jsonlite::Value* eventsNode = root->Find("events");
    const jsonlite::Value::Array* events =
        eventsNode != nullptr ? eventsNode->AsArray() : nullptr;
    if (events == nullptr || events->size() > 50) {
        SetParseError(
            errorMessage,
            L"Codex Runway events must be an array with at most 50 entries");
        return snapshot;
    }

    std::vector<RunwayEvent> parsedEvents;
    parsedEvents.reserve(events->size());
    for (const jsonlite::Value& eventNode : *events) {
        if (!eventNode.IsObject()) {
            SetParseError(errorMessage, L"Codex Runway event is not an object");
            return snapshot;
        }

        RunwayEvent event;
        const auto kind = ReadString(&eventNode, "kind");
        const char* expectedRationale = kind.has_value()
            ? ExpectedRunwayRationale(*kind)
            : nullptr;
        if (!kind.has_value() || expectedRationale == nullptr) {
            SetParseError(errorMessage, L"Codex Runway event kind is invalid");
            return snapshot;
        }
        event.kind = *kind;

        const auto announcedAtText = ReadString(&eventNode, "announcedAt");
        const auto announcedAt = announcedAtText.has_value()
            ? ParseIso8601UtcSeconds(*announcedAtText)
            : std::nullopt;
        if (!announcedAt.has_value()) {
            SetParseError(errorMessage, L"Codex Runway announcedAt is invalid");
            return snapshot;
        }
        event.announcedAt = *announcedAt;

        const jsonlite::Value* effectiveNode = eventNode.Find("effectiveAt");
        if (effectiveNode == nullptr) {
            SetParseError(errorMessage, L"Codex Runway effectiveAt is missing");
            return snapshot;
        }
        if (!effectiveNode->IsNull()) {
            const auto effectiveText = effectiveNode->AsString();
            const auto effectiveAt = effectiveText.has_value()
                ? ParseIso8601UtcSeconds(std::string(*effectiveText))
                : std::nullopt;
            if (!effectiveAt.has_value()) {
                SetParseError(errorMessage, L"Codex Runway effectiveAt is invalid");
                return snapshot;
            }
            event.effectiveAt = *effectiveAt;
        }
        if (event.kind == "reset_scheduled" && event.effectiveAt <= 0) {
            SetParseError(
                errorMessage,
                L"Codex Runway scheduled event is missing effectiveAt");
            return snapshot;
        }
        if (event.kind != "reset_scheduled"
            && event.kind != "reset_completed"
            && event.effectiveAt > 0) {
            SetParseError(
                errorMessage,
                L"Codex Runway non-reset event unexpectedly has effectiveAt");
            return snapshot;
        }

        const jsonlite::Value* confidenceNode = eventNode.Find("confidence");
        const auto confidence = confidenceNode != nullptr
            ? confidenceNode->AsNumber()
            : std::nullopt;
        if (!confidence.has_value() || !std::isfinite(*confidence)
            || *confidence < 0.0 || *confidence > 1.0) {
            SetParseError(errorMessage, L"Codex Runway confidence is invalid");
            return snapshot;
        }
        event.confidence = *confidence;

        const auto rationale = ReadString(&eventNode, "rationale");
        if (!rationale.has_value() || *rationale != expectedRationale) {
            SetParseError(errorMessage, L"Codex Runway rationale is invalid");
            return snapshot;
        }

        const jsonlite::Value* source = eventNode.Find("source");
        const auto handle = ReadString(source, "handle");
        const auto postId = ReadString(source, "postId");
        const auto sourceUrl = ReadString(source, "url");
        if (!handle.has_value() || *handle != "thsottiaux"
            || !postId.has_value() || !IsAsciiDigits(*postId)
            || !sourceUrl.has_value()
            || *sourceUrl != "https://x.com/thsottiaux/status/" + *postId) {
            SetParseError(errorMessage, L"Codex Runway event source is invalid");
            return snapshot;
        }
        event.url = Utf8ToWide(*sourceUrl);

        const jsonlite::Value* scope = eventNode.Find("scope");
        if (!ReadStringArray(scope, "plans", &event.plans)
            || !ReadStringArray(scope, "windows", &event.windows)) {
            SetParseError(errorMessage, L"Codex Runway event scope is invalid");
            return snapshot;
        }
        parsedEvents.push_back(std::move(event));
    }

    snapshot.success = true;
    snapshot.runwaySourceAvailable = true;
    snapshot.updatedAtUnixSeconds =
        lastSuccessfulCheckAt > 0 ? lastSuccessfulCheckAt : *generatedAt;

    if (*monitorStatus != "ok") {
        snapshot.errorMessage = L"Codex Runway reset source is degraded";
        snapshot.todayState = ResetTodayState::Unknown;
        return snapshot;
    }
    if (lastSuccessfulCheckAt <= 0) {
        snapshot.errorMessage =
            L"Codex Runway reset source has no successful check";
        snapshot.todayState = ResetTodayState::Unknown;
        return snapshot;
    }
    const long long sourceAge = nowUnixSeconds - lastSuccessfulCheckAt;
    if (sourceAge < -5 * 60 || sourceAge > 30 * 60 * 60) {
        snapshot.errorMessage =
            L"Codex Runway reset source is stale";
        snapshot.todayState = ResetTodayState::Unknown;
        return snapshot;
    }

    int latestEventIndex = -1;
    int latestResetIndex = -1;
    int nextScheduledIndex = -1;
    int sameDayCompletedIndex = -1;
    int sameDayUncertainIndex = -1;
    long long latestEventTime = 0;
    long long latestResetTime = 0;
    long long nextScheduledTime = 0;
    long long sameDayCompletedAnnouncement = 0;
    long long sameDayUncertainAnnouncement = 0;

    for (size_t index = 0; index < parsedEvents.size(); ++index) {
        const RunwayEvent& event = parsedEvents[index];
        if (event.announcedAt > latestEventTime) {
            latestEventTime = event.announcedAt;
            latestEventIndex = static_cast<int>(index);
        }
        if (event.kind == "reset_scheduled"
            && event.effectiveAt > nowUnixSeconds
            && (nextScheduledTime == 0
                || event.effectiveAt < nextScheduledTime)) {
            nextScheduledTime = event.effectiveAt;
            nextScheduledIndex = static_cast<int>(index);
        }

        long long occurredAt = 0;
        if (event.kind == "reset_completed") {
            occurredAt =
                event.effectiveAt > 0 ? event.effectiveAt : event.announcedAt;
        } else if (event.kind == "reset_scheduled"
                   && event.effectiveAt <= nowUnixSeconds) {
            occurredAt = event.effectiveAt;
        }
        if (occurredAt > 0 && occurredAt <= nowUnixSeconds) {
            if (occurredAt > latestResetTime) {
                latestResetTime = occurredAt;
                latestResetIndex = static_cast<int>(index);
            }
            if (IsSameLocalCalendarDay(occurredAt, nowUnixSeconds)
                && event.announcedAt >= sameDayCompletedAnnouncement) {
                sameDayCompletedAnnouncement = event.announcedAt;
                sameDayCompletedIndex = static_cast<int>(index);
            }
        }
        if (event.kind == "uncertain"
            && IsSameLocalCalendarDay(event.announcedAt, nowUnixSeconds)
            && event.announcedAt >= sameDayUncertainAnnouncement) {
            sameDayUncertainAnnouncement = event.announcedAt;
            sameDayUncertainIndex = static_cast<int>(index);
        }
    }

    snapshot.nextScheduledAtUnixSeconds = nextScheduledTime;
    snapshot.latestResetAtUnixSeconds = latestResetTime;
    const bool nextScheduleIsToday =
        nextScheduledTime > 0
        && IsSameLocalCalendarDay(nextScheduledTime, nowUnixSeconds);
    if (sameDayCompletedIndex >= 0 || nextScheduleIsToday) {
        snapshot.todayState = ResetTodayState::Yes;
    } else if (sameDayUncertainIndex >= 0) {
        snapshot.todayState = ResetTodayState::Unknown;
    } else {
        snapshot.todayState = ResetTodayState::No;
    }
    snapshot.windowOpen = snapshot.todayState == ResetTodayState::Yes;

    int primaryEventIndex = latestEventIndex;
    if (snapshot.todayState == ResetTodayState::Yes) {
        primaryEventIndex = nextScheduleIsToday
            ? nextScheduledIndex
            : sameDayCompletedIndex;
    } else if (snapshot.todayState == ResetTodayState::No
               && nextScheduledIndex >= 0) {
        primaryEventIndex = nextScheduledIndex;
    } else if (snapshot.todayState == ResetTodayState::Unknown
               && sameDayUncertainIndex >= 0) {
        primaryEventIndex = sameDayUncertainIndex;
    }

    if (primaryEventIndex >= 0) {
        const RunwayEvent& primary =
            parsedEvents[static_cast<size_t>(primaryEventIndex)];
        snapshot.confidencePercent = std::clamp(
            static_cast<int>(std::lround(primary.confidence * 100.0)),
            0,
            100);
        snapshot.message = RunwayEventMessage(primary.kind);
        snapshot.evidenceKind = Utf8ToWide(primary.kind);
        snapshot.evidenceUrl = primary.url;
        snapshot.scopePlans = primary.plans;
        snapshot.scopeWindows = primary.windows;
    }
    if (latestResetIndex >= 0) {
        snapshot.openedAtUnixSeconds = latestResetTime;
    }
    return snapshot;
}

ResetRadarSnapshot MergeResetRadarSnapshots(
    const ResetRadarSnapshot& summary,
    const ResetRadarSnapshot& forecast,
    long long nowUnixSeconds,
    long long maximumSignalAgeSeconds) {
    ResetRadarSnapshot merged;
    merged.success = summary.success || forecast.success;
    merged.checkedAtUnixSeconds = nowUnixSeconds;

    const auto isFresh = [&](const ResetRadarSnapshot& source) {
        if (!source.success || source.updatedAtUnixSeconds <= 0) return false;
        const long long age = nowUnixSeconds - source.updatedAtUnixSeconds;
        return age >= -5 * 60 && age <= maximumSignalAgeSeconds;
    };
    const bool summaryFresh = isFresh(summary);
    const bool forecastFresh = isFresh(forecast);

    merged.summaryWindowOpen =
        summary.summaryWindowOpen && summaryFresh;
    merged.forecastWindowOpen =
        (summary.forecastWindowOpen && summaryFresh)
        || (forecast.forecastWindowOpen && forecastFresh);
    merged.windowOpen = merged.summaryWindowOpen || merged.forecastWindowOpen;
    merged.updatedAtUnixSeconds = std::max(
        summary.updatedAtUnixSeconds, forecast.updatedAtUnixSeconds);
    merged.openedAtUnixSeconds =
        forecast.forecastWindowOpen && forecastFresh
        ? forecast.openedAtUnixSeconds
        : (merged.summaryWindowOpen ? summary.openedAtUnixSeconds : 0);
    if (summary.forecastWindowOpen && summaryFresh) {
        merged.openedAtUnixSeconds = summary.openedAtUnixSeconds;
    }

    if (forecast.probability24h >= 0) {
        merged.probability24h = forecast.probability24h;
    } else {
        merged.probability24h = summary.probability24h;
    }
    if (forecast.probability48h >= 0) {
        merged.probability48h = forecast.probability48h;
    } else {
        merged.probability48h = summary.probability48h;
    }
    if (forecast.forecastWindowOpen
        && forecastFresh
        && !forecast.message.empty()) {
        merged.message = forecast.message;
    } else if (summary.forecastWindowOpen
               && summaryFresh
               && !summary.message.empty()) {
        merged.message = summary.message;
    } else if (merged.summaryWindowOpen && !summary.message.empty()) {
        merged.message = summary.message;
    }

    if (!summary.success && !forecast.success) {
        merged.errorMessage = summary.errorMessage;
        if (!forecast.errorMessage.empty()) {
            if (!merged.errorMessage.empty()) merged.errorMessage += L"; ";
            merged.errorMessage += forecast.errorMessage;
        }
    } else if ((summary.windowOpen && !summaryFresh)
               || (forecast.windowOpen && !forecastFresh)) {
        merged.errorMessage = L"stale reset-window signal ignored";
    }
    return merged;
}

ResetRadarSnapshot MergeResetRadarSnapshots(
    const ResetRadarSnapshot& runway,
    const ResetRadarSnapshot& summary,
    const ResetRadarSnapshot& forecast,
    long long nowUnixSeconds,
    long long maximumSignalAgeSeconds) {
    ResetRadarSnapshot merged = MergeResetRadarSnapshots(
        summary, forecast, nowUnixSeconds, maximumSignalAgeSeconds);
    merged.success = merged.success || runway.success;
    merged.runwaySourceAvailable = runway.runwaySourceAvailable;
    merged.checkedAtUnixSeconds = nowUnixSeconds;

    if (runway.runwaySourceAvailable) {
        merged.todayState = runway.todayState;
        merged.confidencePercent = runway.confidencePercent;
        merged.nextScheduledAtUnixSeconds =
            runway.nextScheduledAtUnixSeconds;
        merged.latestResetAtUnixSeconds = runway.latestResetAtUnixSeconds;
        merged.evidenceKind = runway.evidenceKind;
        merged.evidenceUrl = runway.evidenceUrl;
        merged.scopePlans = runway.scopePlans;
        merged.scopeWindows = runway.scopeWindows;
    }

    const long long runwayAge =
        nowUnixSeconds - runway.updatedAtUnixSeconds;
    const bool runwayFresh =
        runway.success
        && runway.runwaySourceAvailable
        && runway.updatedAtUnixSeconds > 0
        && runwayAge >= -5 * 60
        && runwayAge <= 30 * 60 * 60;
    if (runwayFresh && runway.todayState != ResetTodayState::Unknown) {
        merged.runwayPrimary = true;
        merged.windowOpen =
            runway.todayState == ResetTodayState::Yes;
        merged.updatedAtUnixSeconds = runway.updatedAtUnixSeconds;
        merged.openedAtUnixSeconds =
            runway.openedAtUnixSeconds;
        merged.message = runway.message;
        merged.errorMessage = runway.errorMessage;
        return merged;
    }

    merged.runwayPrimary = false;
    if (runway.updatedAtUnixSeconds > merged.updatedAtUnixSeconds) {
        merged.updatedAtUnixSeconds = runway.updatedAtUnixSeconds;
    }
    if (!runway.errorMessage.empty()) {
        if (!merged.errorMessage.empty()) merged.errorMessage += L"; ";
        merged.errorMessage += runway.errorMessage;
    }
    return merged;
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

long long EarliestAvailableResetCreditExpiry(
    const ResetCreditsSnapshot& snapshot,
    long long nowUnixSeconds) {
    if (!snapshot.success || snapshot.availableCount <= 0) return 0;
    long long earliest = 0;
    for (const ResetCredit& credit : snapshot.credits) {
        std::wstring status = credit.status;
        std::transform(status.begin(), status.end(), status.begin(), towlower);
        if (!status.empty() && status != L"available") continue;
        if (credit.expiresAtUnixSeconds <= nowUnixSeconds) continue;
        if (earliest == 0 || credit.expiresAtUnixSeconds < earliest) {
            earliest = credit.expiresAtUnixSeconds;
        }
    }
    return earliest;
}

bool IsResetCreditExpiringSoon(
    const ResetCreditsSnapshot& snapshot,
    long long nowUnixSeconds,
    int warningHours) {
    if (warningHours <= 0) return false;
    const long long expiry =
        EarliestAvailableResetCreditExpiry(snapshot, nowUnixSeconds);
    return expiry > nowUnixSeconds
        && expiry - nowUnixSeconds
            <= static_cast<long long>(warningHours) * 60 * 60;
}

std::wstring FormatResetCreditWarning(
    int availableCount,
    long long secondsRemaining) {
    if (availableCount <= 0 || secondsRemaining <= 0) return {};
    const std::wstring prefix = std::to_wstring(availableCount) + L"\u5361";
    if (secondsRemaining >= 3600) {
        const long long hours = (secondsRemaining + 3599) / 3600;
        return prefix + std::to_wstring(hours) + L"h";
    }
    const long long minutes = std::max(1LL, (secondsRemaining + 59) / 60);
    return prefix + std::to_wstring(minutes) + L"m";
}

FreshnessLevel ClassifyFreshness(double ageSeconds) {
    if (ageSeconds >= 5 * 60) return FreshnessLevel::Stale;
    if (ageSeconds >= 60) return FreshnessLevel::Warning;
    return FreshnessLevel::Fresh;
}

std::wstring FormatIsoDateAsMonthDay(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return L"--";
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return L"--";
    }

    const int year = std::stoi(text.substr(0, 4));
    const int month = std::stoi(text.substr(5, 2));
    const int day = std::stoi(text.substr(8, 2));
    if (year < 1601 || month < 1 || month > 12) return L"--";

    static constexpr int daysInMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int maxDay = daysInMonth[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) maxDay = 29;
    if (day < 1 || day > maxDay) return L"--";

    return std::to_wstring(month) + L"." + std::to_wstring(day);
}

FreshnessLevel ClassifyFreshnessForDisplay(double ageSeconds, bool refreshInProgress) {
    // A normal one-minute refresh briefly crosses the warning threshold while
    // its replacement request is still in flight. Suppress only that boundary.
    if (refreshInProgress && ageSeconds < 2 * 60) return FreshnessLevel::Fresh;
    return ClassifyFreshness(ageSeconds);
}

bool ShouldReplaceResetWithFreshness(double ageSeconds) {
    return ageSeconds > 10 * 60;
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
