#include "CodexUsageFetcher.h"
#include "ClaudeUsageFetcher.h"
#include "AntigravityUsageFetcher.h"
#include "KiroCreditsFetcher.h"
#include "CodexUsageVersion.h"
#include "DashboardRenderer.h"
#include "ProxyHelper.h"

#include "PluginInterface.h"

#include <Windows.h>
#include <ole2.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <utility>

#pragma comment(lib, "gdiplus.lib")

namespace {

// =====================================================================
// Layout metrics.
//
// docs/design/taskbar_preview.html draws the reference on a 50 px canvas,
// but TrafficMonitor hard-codes its taskbar window height:
//
//     TaskBarDlg.h:  #define TASKBAR_WND_HEIGHT DPI(32)
//
// CalculateWindowSize() assigns that constant directly, so the slot is 32
// logical px regardless of taskbar height, font size or vertical margin, and
// PluginInterface.h offers no way for a plugin to ask for more. Anchoring the
// vertical layout to a fixed 50 px frame therefore pushed the "5h"/"7d"
// corner tags above the top edge and the provider captions below the bottom
// edge, where the host clipped both away. Every vertical position below is
// derived from the slot the host actually hands us instead.
// =====================================================================
namespace TaskbarVisualSpec {
constexpr float kHostSlotHeight = 32.0f;  // TASKBAR_WND_HEIGHT, logical px
constexpr float kPluginPadLeft = 10.0f;
constexpr float kRingGap = 6.0f;
constexpr float kProviderGroupGap = 11.0f;
constexpr float kInfoDividerMargin = 5.0f;
constexpr float kInfoGap = 11.0f;
constexpr float kMaxRingBox = 28.0f;      // ring box of the 50 px reference
constexpr float kTagFont = 7.0f;
constexpr float kProviderFont = 6.5f;
constexpr float kInfoLabelFont = 8.0f;
constexpr float kInfoValueFont = 11.0f;
constexpr float kInfoSubFont = 6.5f;
constexpr float kRingStrokeRatio = 2.2f / 28.0f;
constexpr float kNumberRatio = 0.45f;     // ring number vs. ring box
constexpr float kBottomInset = 1.0f;
}  // namespace TaskbarVisualSpec

// Horizontal metrics only. Vertical positions live in DashLayout because they
// depend on the slot height and on real font metrics.
struct LM {
    int padX;            // 10px
    int padRight;        // 0px
    int ringGap;         // 6px within a provider pair
    int ringGroupGap;    // 11px between Claude and Codex pairs
    int sepMargin;       // 5px each side of the info divider
    int infoGap;         // 11px between info blocks
    int dotSize;         // 5px
    int dotGap;          // 3px
};

LM GetMetrics(int dpi) {
    float s = dpi / 96.0f;
    LM m{};
    m.padX          = (int)roundf(TaskbarVisualSpec::kPluginPadLeft * s);
    m.padRight      = 0;
    m.ringGap       = std::max(
        4, (int)roundf(TaskbarVisualSpec::kRingGap * s));
    m.ringGroupGap  = std::max(
        8, (int)roundf(TaskbarVisualSpec::kProviderGroupGap * s));
    m.sepMargin     = std::max(
        4, (int)roundf(TaskbarVisualSpec::kInfoDividerMargin * s));
    m.infoGap       = std::max(
        8, (int)roundf(TaskbarVisualSpec::kInfoGap * s));
    m.dotSize       = std::max(4, (int)roundf(5.0f * s));
    m.dotGap        = std::max(2, (int)roundf(3.0f * s));
    return m;
}

// Vertical layout derived from the slot the host hands us.
//
// The corner tags own the band between the top of the slot and their own
// baseline: "5h" and "7d" have no descenders, so all of their ink fits there
// and the gauges can start immediately below without any clipping.
struct DashLayout {
    float slotTop = 0.0f;
    float slotHeight = 0.0f;
    float tagFont = 7.0f;
    float tagTop = 0.0f;          // text layout box top
    float ringBox = 24.0f;        // also the horizontal cell width
    float ringStroke = 2.2f;
    float ringRadius = 11.0f;
    float ringCenterY = 0.0f;
    float numberFont = 11.0f;
    bool showProviderLabel = false;
    float providerFont = 6.5f;
    float providerTop = 0.0f;
    float dividerTop = 0.0f;
    float dividerBottom = 0.0f;
    float infoLabelFont = 8.0f;
    float infoLabelCenterY = 0.0f;
    float infoValueFont = 11.0f;
    float infoValueCenterY = 0.0f;
    float infoSubFont = 6.5f;
};

DashLayout ComputeLayout(
    HDC hdc,
    const wchar_t* fontFamily,
    int dpi,
    float slotTop,
    float slotHeight) {
    const float s = dpi / 96.0f;
    DashLayout layout;
    layout.slotTop = slotTop;
    layout.slotHeight = std::max(16.0f * s, slotHeight);

    layout.tagFont = std::clamp(
        TaskbarVisualSpec::kTagFont * s,
        6.0f * s,
        layout.slotHeight * 0.26f);
    layout.tagTop = slotTop;
    const float tagBand = DashboardRenderer::MeasureBaseline(
        hdc, fontFamily, layout.tagFont, false);

    const float bottomInset = TaskbarVisualSpec::kBottomInset * s;
    float ringSpace = layout.slotHeight - tagBand - bottomInset;

    // The provider caption under each pair needs a taller slot than
    // TrafficMonitor provides. Keep the gauges legible rather than shrinking
    // them to squeeze it in; the info strip already carries Claude / Codex
    // columns, and the group divider separates the two pairs.
    layout.providerFont = TaskbarVisualSpec::kProviderFont * s;
    const float providerBand = DashboardRenderer::MeasureLineHeight(
        hdc, fontFamily, layout.providerFont, false);
    layout.showProviderLabel =
        ringSpace - providerBand >= TaskbarVisualSpec::kMaxRingBox * s * 0.9f;
    if (layout.showProviderLabel) {
        ringSpace -= providerBand;
    }

    layout.ringBox = std::min(
        ringSpace, TaskbarVisualSpec::kMaxRingBox * s);
    layout.ringStroke = std::max(
        2.0f, layout.ringBox * TaskbarVisualSpec::kRingStrokeRatio);
    layout.ringRadius =
        layout.ringBox / 2.0f - layout.ringStroke / 2.0f;
    layout.ringCenterY = slotTop + tagBand + layout.ringBox / 2.0f;
    layout.numberFont = std::max(
        8.0f, layout.ringBox * TaskbarVisualSpec::kNumberRatio);
    layout.providerTop = slotTop + tagBand + layout.ringBox;

    layout.dividerTop = slotTop + layout.slotHeight * 0.14f;
    layout.dividerBottom = slotTop + layout.slotHeight * 0.90f;

    layout.infoLabelFont = std::max(
        7.0f, TaskbarVisualSpec::kInfoLabelFont * s);
    layout.infoValueFont = std::max(
        9.0f, TaskbarVisualSpec::kInfoValueFont * s);
    layout.infoSubFont = std::max(
        5.0f, TaskbarVisualSpec::kInfoSubFont * s);
    layout.infoLabelCenterY = slotTop + layout.slotHeight * 0.225f;
    layout.infoValueCenterY = slotTop + layout.slotHeight * 0.725f;
    return layout;
}

// =====================================================================
// Colors
// =====================================================================
struct DashboardPalette {
    COLORREF ringTrack;
    COLORREF normal;
    COLORREF warning;
    COLORREF elevated;
    COLORREF danger;
    COLORREF textPct;
    COLORREF textLabel;
    COLORREF textDim;
    COLORREF textValue;
    COLORREF separator;
    COLORREF dotRed;
    COLORREF dotGreen;
    COLORREF dotYellow;
    COLORREF ringNoData;
};

DashboardPalette ResolveDashboardPalette(
    bool dark,
    const std::optional<COLORREF>& hostLabelColor,
    const std::optional<COLORREF>& hostValueColor) {
    DashboardPalette palette = dark
        ? DashboardPalette{
            RGB(72, 80, 92), RGB(46, 168, 120),
            RGB(224, 170, 45), RGB(220, 120, 40), RGB(220, 60, 60),
            RGB(242, 242, 242), RGB(203, 204, 208), RGB(150, 151, 155),
            RGB(240, 240, 242), RGB(84, 88, 95),
            RGB(220, 60, 60), RGB(46, 168, 74), RGB(224, 170, 45),
            RGB(60, 60, 63)}
        // The reference mock sits on a #f6f6f6 page, but the plugin never
        // paints on that: TrafficMonitor keys its configured background out
        // of the layered taskbar window, so the pixels behind the gauges are
        // the Windows 11 taskbar itself (measured ~RGB(240, 242, 244) in
        // light mode). The mock's #e6e6e6 track was only ~10 levels away from
        // that and read as invisible, so the track and divider are darker
        // here than in the HTML reference. Secondary text is darkened for the
        // same reason: at 7-8 px it never reaches full coverage, so a mid
        // grey washes out completely.
        : DashboardPalette{
            RGB(203, 206, 210), RGB(100, 202, 120),
            RGB(238, 134, 62), RGB(221, 81, 16), RGB(223, 61, 69),
            RGB(26, 28, 31), RGB(66, 68, 72), RGB(100, 102, 106),
            RGB(26, 28, 31), RGB(193, 196, 200),
            RGB(223, 61, 69), RGB(100, 202, 120), RGB(238, 134, 62),
            RGB(216, 218, 220)};

    // TrafficMonitor already resolves the active taskbar preset, including
    // per-item colors and automatic Windows light/dark switching. Prefer
    // those colors when the host provides them; the palette remains the
    // fallback for older hosts and for non-text dashboard elements.
    // Keep secondary text intentionally quieter than values. TrafficMonitor
    // presets often provide the same dark color for both, which makes this
    // compact two-line layout look heavier than the approved mockup.
    (void)hostLabelColor;
    if (hostValueColor) {
        palette.textPct = *hostValueColor;
        palette.textValue = *hostValueColor;
    }
    return palette;
}

COLORREF GaugeColor(int pct, const DashboardPalette& palette) {
    if (pct < 0) return palette.ringNoData;
    if (pct < 60) return palette.normal;
    if (pct < 80) return palette.warning;
    if (pct < 90) return palette.elevated;
    return palette.danger;
}

std::optional<COLORREF> ParseHostColor(const wchar_t* data) {
    if (!data) return std::nullopt;
    while (iswspace(*data)) ++data;
    if (*data == L'\0' || *data == L'-') return std::nullopt;

    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(data, &end, 10);
    if (errno == ERANGE || end == data || value > 0x00FFFFFFUL) {
        return std::nullopt;
    }
    while (end && iswspace(*end)) ++end;
    if (!end || *end != L'\0') return std::nullopt;
    return static_cast<COLORREF>(value);
}

// =====================================================================
// Helpers
// =====================================================================
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

bool UnixToLocalSystemTime(long long unixSeconds, SYSTEMTIME* local) {
    if (unixSeconds < 0 || local == nullptr) return false;
    constexpr ULONGLONG kWindowsEpoch = 116444736000000000ULL;
    constexpr ULONGLONG kTicksPerSecond = 10000000ULL;
    const ULONGLONG seconds = static_cast<ULONGLONG>(unixSeconds);
    if (seconds > (std::numeric_limits<ULONGLONG>::max() - kWindowsEpoch) / kTicksPerSecond) {
        return false;
    }

    ULARGE_INTEGER value{};
    value.QuadPart = seconds * kTicksPerSecond + kWindowsEpoch;
    FILETIME fileTime{value.LowPart, value.HighPart};
    SYSTEMTIME utc{};
    return FileTimeToSystemTime(&fileTime, &utc)
        && SystemTimeToTzSpecificLocalTime(nullptr, &utc, local);
}

bool IsoToLocalSystemTime(const std::string& iso, SYSTEMTIME* local) {
    const auto unixSeconds = ParseIso8601UtcSeconds(iso);
    return unixSeconds.has_value() && UnixToLocalSystemTime(*unixSeconds, local);
}

std::wstring IsoToLocalTimeW(const std::string& iso) {
    SYSTEMTIME local{};
    if (!IsoToLocalSystemTime(iso, &local)) return L"--";
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", (int)local.wHour, (int)local.wMinute);
    return buf;
}

std::wstring IsoToLocalDateTimeW(const std::string& iso) {
    SYSTEMTIME local{};
    if (!IsoToLocalSystemTime(iso, &local)) return L"--";
    wchar_t buffer[48];
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buffer;
}

std::wstring IsoToCompactDateW(const std::string& iso) {
    return FormatIsoDateAsMonthDay(iso);
}

// ISO 8601 date -> single-character Chinese weekday (一, 二, ... 日)
std::wstring IsoToWeekday(const std::string& iso) {
    SYSTEMTIME local{};
    if (!IsoToLocalSystemTime(iso, &local)) return L"--";
    // Day of week: 0=Sun, 1=Mon, ... 6=Sat
    static const wchar_t* weekdays[] = {
        L"\u65E5", L"\u4E00", L"\u4E8C", L"\u4E09", L"\u56DB", L"\u4E94", L"\u516D" };
    return weekdays[local.wDayOfWeek];
}

// Format one source's 7d reset countdown: "5h30m" or "23m".
// Returns an empty string outside the configured display window.
std::wstring Format7dCountdown(
    const std::string& iso, long long unixSec, int showBeforeHours) {
    const long long now = static_cast<long long>(time(nullptr));
    long long resetAt = 0;
    const auto claudeReset = ParseIso8601UtcSeconds(iso);
    if (claudeReset.has_value() && *claudeReset > now) resetAt = *claudeReset;
    if (unixSec > now && (resetAt == 0 || unixSec < resetAt)) resetAt = unixSec;
    if (resetAt <= now) return {};
    const long long diff = resetAt - now;
    if (!ShouldShowCountdown(diff, showBeforeHours)) return {};

    // < 24h: show countdown
    const int h = static_cast<int>(diff / 3600);
    const int m = static_cast<int>((diff % 3600) / 60);
    wchar_t buf[32];
    if (h > 0)
        swprintf_s(buf, L"%dh%02dm", h, m);
    else
        swprintf_s(buf, L"%dm", m);
    return buf;
}

// Unix timestamp -> weekday in Chinese
std::wstring UnixToWeekday(long long unixSec) {
    SYSTEMTIME local{};
    if (!UnixToLocalSystemTime(unixSec, &local)) return L"--";
    static const wchar_t* weekdays[] = {
        L"\u65E5", L"\u4E00", L"\u4E8C", L"\u4E09", L"\u56DB", L"\u4E94", L"\u516D" };
    return weekdays[local.wDayOfWeek];
}

std::wstring UnixToLocalDateTimeW(long long unixSec) {
    SYSTEMTIME local{};
    if (!UnixToLocalSystemTime(unixSec, &local)) return L"--";
    wchar_t buffer[48];
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buffer;
}

// =====================================================================
// GDI+ init
// =====================================================================
ULONG_PTR g_gdiToken = 0;
void InitGdiplus() {
    if (g_gdiToken) return;
    Gdiplus::GdiplusStartupInput in{};
    Gdiplus::GdiplusStartup(&g_gdiToken, &in, nullptr);
}
void ShutdownGdiplus() {
    if (g_gdiToken) { Gdiplus::GdiplusShutdown(g_gdiToken); g_gdiToken = 0; }
}

// =====================================================================
// Plugin options (persisted via registry/file)
// =====================================================================
struct PluginOptions {
    bool showPctSign = false;       // default: hide %
    bool showCredits = true;
    bool showReset = true;
    bool showSubscription = false;
    bool showCustomExpiry = false;
    bool showStatus = true;
    bool showClaude7dReset = true;  // show Claude 7d reset weekday
    bool showCodex7dReset = true;   // show Codex 7d reset weekday
    bool show7dCountdown = true;    // show the nearest 7d reset
    bool showResetCreditWarning = true;
    bool showResetRadar = true;
    bool showAntigravity = true;    // show Antigravity ring + tier block
    bool showKiro = true;           // show Kiro ring + credits block
    int countdownShowBeforeHours = 24;
    int resetCreditWarningHours = 48;
    int resetRadarRefreshMinutes = 15;
    int resetTodayRefreshMinutes = 60;
    std::string customSubExpiry;
    std::string antigravityPrimaryModel;  // featured model override
};

// =====================================================================
// Snapshot
// =====================================================================
struct DashData {
    int c5 = -1, c7 = -1, x5 = -1, x7 = -1;
    std::string c5Reset;
    std::string c7Reset;
    long long x7ResetUnix = 0;   // Codex 7d reset unix timestamp
    long long usedCredits = 0;
    std::string currency;
    int decPlaces = 2;
    std::string subStatus;
    double lastClaudeOk = 0;
    double lastCodexOk = 0;
    bool claudeAvailable = false;
    bool codexAvailable = false;
    bool refreshInProgress = false;
    bool resetCreditsAvailable = false;
    int resetCreditCount = 0;
    long long resetCreditExpiryUnix = 0;
    bool resetRadarWindowOpen = false;
    int resetProbability24h = -1;
    int resetProbability48h = -1;
    long long resetRadarUpdatedUnix = 0;
    // Antigravity featured model (used %), tier and freshness.
    int agPct = -1;
    std::string agTier;
    bool agAvailable = false;
    double lastAgOk = 0;
    // Kiro credits: used %, remaining credits and freshness.
    int kiroPct = -1;
    long long kiroRemaining = 0;
    long long kiroLimit = 0;
    bool kiroAvailable = false;
    double lastKiroOk = 0;
};

std::wstring FormatCredits(const DashData& data) {
    if (!data.claudeAvailable) return L"--";
    const int decimalPlaces = std::clamp(data.decPlaces, 0, 9);
    const double amount = data.usedCredits / pow(10.0, decimalPlaces);
    wchar_t number[32];
    swprintf_s(number, L"%.2f", amount);
    std::wstring currency = ToWide(data.currency);
    std::transform(currency.begin(), currency.end(), currency.begin(), towupper);
    if (currency.empty() || currency == L"USD") return L"$" + std::wstring(number);
    return currency + L" " + number;
}

std::wstring FormatSubscriptionStatus(const DashData& data) {
    if (data.subStatus == "active") return L"\u6B63\u5E38";
    if (data.subStatus == "trialing") return L"\u8BD5\u7528";
    if (data.subStatus == "canceled") return L"\u5DF2\u53D6\u6D88";
    if (data.subStatus == "past_due") return L"\u903E\u671F";
    if (data.subStatus == "paused") return L"\u5DF2\u6682\u505C";
    return L"--";
}

// Normalize an Antigravity tier id to a display label. Internal identifiers
// (e.g. "g1-pro-tier") are not user-meaningful and return empty, so callers
// hide them instead of showing raw API values.
std::wstring FormatAgTier(const std::string& tier) {
    std::string t = tier;
    std::transform(t.begin(), t.end(), t.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t == "free") return L"FREE";
    if (t == "pro") return L"PRO";
    if (t == "teams" || t == "team") return L"TEAMS";
    if (t == "enterprise") return L"ENT";
    return L"";
}

// Pick the model that drives the Antigravity ring. When PrimaryModel is
// configured it wins (substring match on the raw key); falling back to the
// highest-priority pool (Gemini 3.x > Claude > GPT), broken to the
// most-exhausted model so the ring reflects the tightest quota.
// Gemini version comparison helpers for the tooltip's per-family dedup.
// "Gemini 3.6 Flash High" -> version 36, family "Flash".
// "Gemini 3 Flash"       -> version 30, family "Flash".
// Non-Gemini models return version -1.
int GeminiModelVersion(const std::string& displayName) {
    std::smatch m;
    if (std::regex_search(displayName, m,
            std::regex(R"(^Gemini\s+(\d+)(?:\.(\d+))?)"))) {
        const int major = std::atoi(m[1].str().c_str());
        const int minor = m[2].matched ? std::atoi(m[2].str().c_str()) : 0;
        return major * 10 + minor;
    }
    return -1;
}

// The variant family is the FIRST word after the version ("Flash High" and
// "Flash Lite" both belong to "Flash"), so dedup keeps only the newest
// version of each family (user preference: Flash/Pro 各留最新版).
std::string GeminiModelFamily(const std::string& displayName) {
    std::smatch m;
    if (std::regex_search(displayName, m,
            std::regex(R"(^Gemini\s+\d+(?:\.\d+)?\s*([A-Za-z]+))"))) {
        return m[1].str();
    }
    return displayName;
}

int AntigravityModelPriority(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("gemini-3") != std::string::npos) return 3;
    if (lower.find("claude") != std::string::npos) return 2;
    if (lower.find("gpt") != std::string::npos) return 1;
    return 0;
}

const AntigravityModelQuota* SelectFeaturedModel(
    const AntigravityUsageData& data, const std::string& primary) {
    if (data.models.empty()) return nullptr;
    if (!primary.empty()) {
        const AntigravityModelQuota* match = nullptr;
        for (const auto& m : data.models) {
            if (m.modelName.find(primary) != std::string::npos) {
                if (!match || m.remainingFraction < match->remainingFraction) {
                    match = &m;
                }
            }
        }
        if (match) return match;
    }
    const AntigravityModelQuota* best = nullptr;
    int bestPriority = -1;
    for (const auto& m : data.models) {
        const int p = AntigravityModelPriority(m.modelName);
        if (p > bestPriority
            || (p == bestPriority && best
                && m.remainingFraction < best->remainingFraction)) {
            best = &m;
            bestPriority = p;
        }
    }
    return best;
}

std::wstring FormatKiroRemaining(const DashData& data) {
    if (!data.kiroAvailable || data.kiroRemaining < 0) return L"--";
    return std::to_wstring(data.kiroRemaining);
}

double SourceAge(bool available, double lastSuccess) {
    if (!available || lastSuccess <= 0) return 0;
    return std::max(0.0, static_cast<double>(time(nullptr)) - lastSuccess);
}

FreshnessLevel DisplayFreshness(bool available, double lastSuccess, bool refreshInProgress) {
    return ClassifyFreshnessForDisplay(
        SourceAge(available, lastSuccess), refreshInProgress);
}

COLORREF FreshnessMarkerColor(
    bool available,
    double lastSuccess,
    bool refreshInProgress,
    const DashboardPalette& palette) {
    const FreshnessLevel level = DisplayFreshness(available, lastSuccess, refreshInProgress);
    if (level == FreshnessLevel::Warning) return palette.dotYellow;
    if (level == FreshnessLevel::Stale) return palette.dotRed;
    return 0;
}

std::wstring FormatFreshnessMinutes(bool available, double lastSuccess, bool refreshInProgress) {
    const double ageSeconds = SourceAge(available, lastSuccess);
    if (DisplayFreshness(available, lastSuccess, refreshInProgress) == FreshnessLevel::Fresh
        || !ShouldReplaceResetWithFreshness(ageSeconds)) {
        return {};
    }
    const int minutes = std::max(1, static_cast<int>(ageSeconds / 60));
    if (minutes > 99) return L"99+\u5206";
    return std::to_wstring(minutes) + L"\u5206";
}

std::wstring FormatSourceAge(const wchar_t* source, double ageSeconds) {
    if (ClassifyFreshness(ageSeconds) == FreshnessLevel::Fresh) return {};
    wchar_t buffer[32];
    if (ageSeconds < 3600) {
        swprintf_s(buffer, L"%s\u8FC7\u671F%dm", source, std::max(1, static_cast<int>(ageSeconds / 60)));
    } else {
        swprintf_s(buffer, L"%s\u8FC7\u671F%dh%02dm", source,
            static_cast<int>(ageSeconds / 3600), (static_cast<int>(ageSeconds) % 3600) / 60);
    }
    return buffer;
}

std::wstring BuildFreshnessText(const DashData& data) {
    const std::wstring claude = FormatSourceAge(
        L"C", SourceAge(data.claudeAvailable, data.lastClaudeOk));
    const std::wstring codex = FormatSourceAge(
        L"X", SourceAge(data.codexAvailable, data.lastCodexOk));
    if (claude.empty()) return codex;
    if (codex.empty()) return claude;
    return claude + L" " + codex;
}

struct CodexColumnDisplay {
    std::wstring value;
    bool gradient = false;
};

CodexColumnDisplay BuildCodexColumnDisplay(
    const DashData& data,
    const PluginOptions& options) {
    CodexColumnDisplay display;
    const long long now = static_cast<long long>(time(nullptr));
    if (options.showResetCreditWarning
        && data.resetCreditsAvailable
        && data.resetCreditCount > 0
        && data.resetCreditExpiryUnix > now
        && data.resetCreditExpiryUnix - now
            <= static_cast<long long>(options.resetCreditWarningHours) * 60 * 60) {
        display.value = FormatResetCreditWarning(
            data.resetCreditCount, data.resetCreditExpiryUnix - now);
        display.gradient = !display.value.empty();
        if (display.gradient) return display;
    }
    if (options.show7dCountdown) {
        display.value = Format7dCountdown(
            {}, data.x7ResetUnix, options.countdownShowBeforeHours);
    }
    if (display.value.empty() && options.showStatus) {
        display.value = FormatFreshnessMinutes(
            data.codexAvailable, data.lastCodexOk, data.refreshInProgress);
    }
    if (display.value.empty()) {
        display.value = UnixToWeekday(data.x7ResetUnix);
    }
    return display;
}

// =====================================================================
// Main custom-drawn item
// =====================================================================
class AIDashboardItem final : public IPluginItem {
public:
    const wchar_t* GetItemName() const override { return L"AI Usage Dashboard"; }
    const wchar_t* GetItemId() const override { return L"AIUsageDashboard"; }
    const wchar_t* GetItemLableText() const override { return L""; }
    const wchar_t* GetItemValueText() const override { return L""; }
    const wchar_t* GetItemValueSampleText() const override { return L""; }
    bool IsCustomDraw() const override { return true; }

    int GetItemWidth() const override {
        PluginOptions opts;
        DashData snap;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (cachedW_ > 0) return cachedW_;
            opts = opts_;
            snap = data_;
        }
        int width = 162;
        if (opts.showResetRadar && snap.resetRadarWindowOpen) width += 16;
        if (opts.showAntigravity || opts.showKiro) {
            const int n = (opts.showAntigravity ? 1 : 0)
                        + (opts.showKiro ? 1 : 0);
            width += 11 + n * 28 + (n > 1 ? 6 : 0);
        }
        bool hasInfoBlock = false;
        const auto addEstimatedBlock = [&](int blockWidth) {
            width += blockWidth;
            hasInfoBlock = true;
        };
        if (opts.showCredits) addEstimatedBlock(55);
        if (opts.showReset) addEstimatedBlock(48);
        if (opts.showSubscription) addEstimatedBlock(55);
        if (opts.showCustomExpiry) addEstimatedBlock(45);
        if (opts.showAntigravity) addEstimatedBlock(38);
        if (opts.showKiro) addEstimatedBlock(45);
        if (opts.showClaude7dReset) {
            addEstimatedBlock(opts.show7dCountdown ? 55 : 45);
        }
        if (opts.showCodex7dReset) {
            addEstimatedBlock(opts.show7dCountdown ? 55 : 45);
        }
        if (hasInfoBlock) width -= 11;
        return width;
    }

    int GetItemWidthEx(void* hDC) const override {
        if (!hDC) return GetItemWidth();
        HDC hdc = (HDC)hDC;
        int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        if (dpi < 72) dpi = 96;
        const LM m = GetMetrics(dpi);
        DashData snap;
        PluginOptions opts;
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap = data_;
            opts = opts_;
        }

        const wchar_t* ff = L"Segoe UI";
        // The ring cell is square, so its width follows the same slot height
        // that DrawItem lays out against. Until the first paint tells us what
        // the host really passes, assume TASKBAR_WND_HEIGHT.
        const DashLayout layout = ComputeLayout(
            hdc, ff, dpi, 0.0f, SlotHeightHint(dpi));
        const int ringCell = std::max(
            8, static_cast<int>(std::lround(layout.ringBox)));
        const auto textWidth = [&](const wchar_t* text,
                                   const wchar_t* family,
                                   float size,
                                   bool medium) {
            return static_cast<int>(std::ceil(
                DashboardRenderer::MeasureTextWidth(
                    hdc, text, family, size, medium)));
        };
        int width = m.padX
                  + 4 * ringCell
                  + 2 * m.ringGap
                  + m.ringGroupGap
                  + 2 * m.sepMargin;
        const int extraRingCount = (opts.showAntigravity ? 1 : 0)
                                 + (opts.showKiro ? 1 : 0);
        if (extraRingCount > 0) {
            width += m.ringGroupGap
                   + extraRingCount * ringCell
                   + (extraRingCount > 1 ? m.ringGap : 0);
        }
        if (opts.showResetRadar && snap.resetRadarWindowOpen) {
            width += textWidth(
                L"\u26A1", L"Segoe UI Symbol",
                layout.infoValueFont, true)
                + std::max(3, m.ringGap / 2);
        }
        bool hasInfoBlock = false;
        const auto addBlock = [&](const wchar_t* label, const std::wstring& value, int valueExtra = 0) {
            width += std::max(textWidth(label, ff, layout.infoLabelFont, false),
                              textWidth(value.c_str(), ff, layout.infoValueFont, false) + valueExtra)
                   + m.infoGap;
            hasInfoBlock = true;
        };
        if (opts.showCredits) addBlock(L"Credits", FormatCredits(snap));
        if (opts.showReset) {
            addBlock(L"5h\u91CD\u7F6E",
                     snap.c5Reset.empty() ? L"--" : IsoToLocalTimeW(snap.c5Reset));
        }
        if (opts.showAntigravity) {
            const std::wstring tier = FormatAgTier(snap.agTier);
            if (!tier.empty()) addBlock(L"AG", tier.c_str());
        }
        if (opts.showKiro) {
            addBlock(L"Kiro", FormatKiroRemaining(snap).c_str());
        }
        if (opts.showSubscription) {
            const bool hasStatusDot = snap.subStatus == "active"
                || snap.subStatus == "trialing" || snap.subStatus == "canceled"
                || snap.subStatus == "past_due" || snap.subStatus == "paused";
            addBlock(L"\u8BA2\u9605", FormatSubscriptionStatus(snap),
                     hasStatusDot ? m.dotSize + m.dotGap : 0);
        }
        if (opts.showCustomExpiry) {
            addBlock(L"\u5230\u671F", IsoToCompactDateW(opts.customSubExpiry));
        }
        if (opts.showClaude7dReset) {
            std::wstring value;
            if (opts.show7dCountdown) {
                value = Format7dCountdown(
                    snap.c7Reset, 0, opts.countdownShowBeforeHours);
            }
            if (value.empty() && opts.showStatus) {
                value = FormatFreshnessMinutes(
                    snap.claudeAvailable, snap.lastClaudeOk, snap.refreshInProgress);
            }
            if (value.empty()) {
                value = snap.c7Reset.empty() ? L"--" : IsoToWeekday(snap.c7Reset);
            }
            addBlock(L"Claude", value);
        }
        if (opts.showCodex7dReset) {
            const CodexColumnDisplay display =
                BuildCodexColumnDisplay(snap, opts);
            addBlock(L"Codex", display.value);
        }
        if (hasInfoBlock) width -= m.infoGap;
        width += m.padRight;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cachedW_ = width;
        }
        return width;
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark) override {
        HDC hdc = (HDC)hDC;
        if (!hdc) return;
        const int savedDc = SaveDC(hdc);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        if (dpi < 72) dpi = 96;
        LM m = GetMetrics(dpi);
        RECT renderBounds{
            x,
            y,
            x + std::max(1, w),
            y + std::max(1, h)};
        if (w > 0 && h > 0) {
            // The host can leave an item-sized clip selected on the HDC.
            // Reset that transient clip and restore one bounded by the item
            // rectangle itself. Nothing is drawn outside it any more, so the
            // old vertical overscan only hid layout mistakes: whatever landed
            // in it was clipped by the 32 px window regardless.
            SelectClipRgn(hdc, nullptr);
            IntersectClipRect(
                hdc,
                renderBounds.left,
                renderBounds.top,
                renderBounds.right,
                renderBounds.bottom);
        }

        DashData snap;
        PluginOptions opts;
        std::optional<COLORREF> hostLabelColor;
        std::optional<COLORREF> hostValueColor;
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap = data_;
            opts = opts_;
            hostLabelColor = hostLabelColor_;
            hostValueColor = hostValueColor_;
        }
        const DashboardPalette colors =
            ResolveDashboardPalette(dark, hostLabelColor, hostValueColor);

        // Draw directly through Direct2D/DirectWrite at the final taskbar
        // scale. The renderer falls back to GDI/GDI+ when unavailable.
        DashboardRenderer canvas(hdc, renderBounds, dpi);
        const wchar_t* ff = L"Segoe UI";
        const float visualScale = dpi / 96.0f;
        const DashLayout layout = ComputeLayout(
            hdc,
            ff,
            dpi,
            static_cast<float>(y),
            static_cast<float>(std::max(1, h)));
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (slotHeight_ != layout.slotHeight) {
                slotHeight_ = layout.slotHeight;
                cachedW_ = 0;
            }
        }
        const int ringCell = std::max(
            8, static_cast<int>(std::lround(layout.ringBox)));
        int curX = x + m.padX;
        const COLORREF cPct = colors.textPct;
        const COLORREF cLabel = colors.textLabel;
        const COLORREF cValue = colors.textValue;

        if (opts.showResetRadar && snap.resetRadarWindowOpen) {
            const wchar_t* indicator = L"\u26A1";
            const int indicatorWidth = static_cast<int>(std::ceil(
                canvas.TextWidth(
                    indicator, L"Segoe UI Symbol",
                    layout.infoValueFont, true)));
            canvas.DrawGradientTextAt(
                indicator,
                static_cast<float>(curX),
                layout.infoValueCenterY - layout.infoValueFont * 0.7f,
                L"Segoe UI Symbol", layout.infoValueFont,
                true, dark);
            curX += indicatorWidth + std::max(3, m.ringGap / 2);
        }

        // ── Ring gauges ──
        auto ring = [&](int pct, COLORREF clr, const wchar_t* tag,
                        COLORREF freshnessMarker, int trailingGap) {
            const float rcx = curX + ringCell / 2.0f;
            canvas.DrawArc(
                rcx,
                layout.ringCenterY,
                layout.ringRadius,
                layout.ringStroke,
                std::max(0, pct),
                colors.ringTrack,
                pct < 0 ? colors.ringNoData : clr);

            if (pct >= 0) {
                wchar_t nb[8];
                if (opts.showPctSign)
                    swprintf_s(nb, L"%d%%", pct);
                else
                    swprintf_s(nb, L"%d", pct);
                canvas.DrawTextCentered(
                    nb,
                    rcx,
                    layout.ringCenterY,
                    ff,
                    layout.numberFont,
                    cPct,
                    false);
            } else {
                canvas.DrawTextCentered(
                    L"--",
                    rcx,
                    layout.ringCenterY,
                    ff,
                    layout.numberFont,
                    colors.textDim,
                    false);
            }

            // The tag lives in the band above the gauges, right-aligned to its
            // own cell so it reads as that ring's corner label and leans into
            // the gap before the next one. "5h" and "7d" have no descenders,
            // so the band down to the tag baseline holds all of their ink.
            const float tagWidth =
                canvas.TextWidth(tag, ff, layout.tagFont, false);
            const float tagLeft = std::max(
                static_cast<float>(curX),
                curX + ringCell - tagWidth + trailingGap / 2.0f);
            canvas.DrawTextAt(
                tag,
                tagLeft,
                layout.tagTop,
                ff,
                layout.tagFont,
                cLabel,
                false);
            if (opts.showStatus && freshnessMarker != 0) {
                canvas.DrawFreshnessUnderline(
                    rcx,
                    layout.ringCenterY + layout.numberFont / 2.0f
                        + std::max(1.5f, visualScale * 1.5f),
                    freshnessMarker,
                    dpi);
            }
            curX += ringCell + trailingGap;
            return static_cast<int>(std::lround(rcx));
        };
        auto providerLabel = [&](const wchar_t* text, int firstCenter, int secondCenter) {
            // Only drawn when the slot is tall enough to carry the caption
            // without shrinking the gauges. TrafficMonitor's 32 px window is
            // not, so this stays off there and the info strip's Claude /
            // Codex columns plus the group divider carry the distinction.
            if (!layout.showProviderLabel) return;
            const int center = (firstCenter + secondCenter) / 2;
            const float textW =
                canvas.TextWidth(text, ff, layout.providerFont, false);
            canvas.DrawTextAt(
                text,
                center - textW / 2.0f,
                layout.providerTop,
                ff,
                layout.providerFont,
                colors.textDim,
                false);
        };

        const COLORREF claudeFreshness = FreshnessMarkerColor(
            snap.claudeAvailable, snap.lastClaudeOk, snap.refreshInProgress, colors);
        const COLORREF codexFreshness = FreshnessMarkerColor(
            snap.codexAvailable, snap.lastCodexOk, snap.refreshInProgress, colors);
        const COLORREF agFreshness = FreshnessMarkerColor(
            snap.agAvailable, snap.lastAgOk, snap.refreshInProgress, colors);
        const COLORREF kiroFreshness = FreshnessMarkerColor(
            snap.kiroAvailable, snap.lastKiroOk, snap.refreshInProgress, colors);
        const int claude5Center = ring(
            snap.c5, GaugeColor(snap.c5, colors), L"5h",
            claudeFreshness, m.ringGap);
        const int claude7Center = ring(
            snap.c7, GaugeColor(snap.c7, colors), L"7d",
            claudeFreshness, 0);
        providerLabel(L"Claude", claude5Center, claude7Center);

        const int groupSeparatorX = curX + m.ringGroupGap / 2;
        canvas.DrawLine(
            static_cast<float>(groupSeparatorX),
            layout.dividerTop,
            static_cast<float>(groupSeparatorX),
            layout.dividerBottom,
            colors.separator);
        curX += m.ringGroupGap;

        const int codex5Center = ring(
            snap.x5, GaugeColor(snap.x5, colors), L"5h",
            codexFreshness, m.ringGap);
        const int codex7Center = ring(
            snap.x7, GaugeColor(snap.x7, colors), L"7d",
            codexFreshness, 0);
        providerLabel(L"Codex", codex5Center, codex7Center);

        // ── Antigravity + Kiro group ──
        if (opts.showAntigravity || opts.showKiro) {
            const int group2SeparatorX = curX + m.ringGroupGap / 2;
            canvas.DrawLine(
                static_cast<float>(group2SeparatorX),
                layout.dividerTop,
                static_cast<float>(group2SeparatorX),
                layout.dividerBottom,
                colors.separator);
            curX += m.ringGroupGap;
            if (opts.showAntigravity) {
                const int agCenter = ring(
                    snap.agPct, GaugeColor(snap.agPct, colors), L"AG",
                    agFreshness,
                    opts.showKiro ? m.ringGap : 0);
                (void)agCenter;
            }
            if (opts.showKiro) {
                const int kiroCenter = ring(
                    snap.kiroPct, GaugeColor(snap.kiroPct, colors), L"Kiro",
                    kiroFreshness, 0);
                (void)kiroCenter;
            }
        }

        // ── Separator 1 ──
        curX += m.sepMargin;
        canvas.DrawLine(
            static_cast<float>(curX),
            layout.dividerTop,
            static_cast<float>(curX),
            layout.dividerBottom,
            colors.separator);
        curX += m.sepMargin;

        // ── Info blocks ──
        // Both rows are centred on their band instead of stacked from a fixed
        // top, so the pair stays balanced whatever height the host gives us.
        const auto lineTopFor = [&](float centerY, float font, bool medium) {
            return centerY - DashboardRenderer::MeasureLineHeight(
                hdc, ff, font, medium) / 2.0f;
        };
        auto block = [&](const wchar_t* label, const wchar_t* value,
                         const wchar_t* sub = nullptr, COLORREF subClr = 0,
                         bool gradientValue = false) {
            const float labelW =
                canvas.TextWidth(label, ff, layout.infoLabelFont, false);
            const float valueW =
                canvas.TextWidth(value, ff, layout.infoValueFont, false);
            const float subW =
                sub ? canvas.TextWidth(sub, ff, layout.infoSubFont, false)
                    : 0.0f;
            const float bw = std::max({labelW, valueW, subW});
            canvas.DrawTextCentered(
                label,
                curX + bw / 2.0f,
                layout.infoLabelCenterY,
                ff,
                layout.infoLabelFont,
                cLabel,
                false);
            if (gradientValue) {
                canvas.DrawGradientTextAt(
                    value,
                    curX + (bw - valueW) / 2.0f,
                    lineTopFor(layout.infoValueCenterY,
                               layout.infoValueFont, true),
                    ff,
                    layout.infoValueFont,
                    true,
                    dark);
            } else {
                canvas.DrawTextCentered(
                    value,
                    curX + bw / 2.0f,
                    layout.infoValueCenterY,
                    ff,
                    layout.infoValueFont,
                    cValue,
                    false);
            }
            if (sub) {
                const COLORREF resolvedSubColor =
                    subClr == 0 ? colors.textLabel : subClr;
                canvas.DrawTextAt(
                    sub,
                    curX + (bw - subW) / 2.0f,
                    lineTopFor(layout.infoValueCenterY,
                               layout.infoValueFont, false)
                        + layout.infoValueFont + 2.0f,
                    ff,
                    layout.infoSubFont,
                    resolvedSubColor,
                    false);
            }
            curX += static_cast<int>(std::ceil(bw)) + m.infoGap;
        };

        if (opts.showCredits) {
            std::wstring cv = FormatCredits(snap);
            block(L"Credits", cv.c_str());
        }

        if (opts.showReset) {
            std::wstring rs = snap.c5Reset.empty() ? L"--" : IsoToLocalTimeW(snap.c5Reset);
            block(L"5h\u91CD\u7F6E", rs.c_str());
        }

        if (opts.showAntigravity) {
            const std::wstring tier = FormatAgTier(snap.agTier);
            if (!tier.empty()) block(L"AG", tier.c_str());
        }

        if (opts.showKiro) {
            block(L"Kiro", FormatKiroRemaining(snap).c_str());
        }

        if (opts.showSubscription) {
            std::wstring dateStr = FormatSubscriptionStatus(snap);
            // Status dot: red = canceled/expired, green = active
            bool isCanceled = (snap.subStatus == "canceled" || snap.subStatus == "past_due"
                            || snap.subStatus == "paused");
            bool isActive = (snap.subStatus == "active" || snap.subStatus == "trialing");
            COLORREF dotClr = 0;
            if (isCanceled) { dotClr = colors.dotRed; }
            else if (isActive) { dotClr = colors.dotGreen; }
            const float labelW = canvas.TextWidth(
                L"\u8BA2\u9605", ff, layout.infoLabelFont, false);
            const float valueTextW = canvas.TextWidth(
                dateStr.c_str(), ff, layout.infoValueFont, false);
            const float valueW =
                valueTextW + (dotClr ? m.dotSize + m.dotGap : 0);
            const float bw = std::max(labelW, valueW);
            canvas.DrawTextCentered(
                L"\u8BA2\u9605",
                curX + bw / 2.0f,
                layout.infoLabelCenterY,
                ff,
                layout.infoLabelFont,
                cLabel,
                false);
            // Value with dot
            float vx = curX + (bw - valueW) / 2.0f;
            const float vy = lineTopFor(
                layout.infoValueCenterY, layout.infoValueFont, false);
            if (dotClr) {
                canvas.DrawDot(
                    vx + m.dotSize / 2.0f,
                    layout.infoValueCenterY,
                    m.dotSize / 2.0f,
                    dotClr);
                vx += m.dotSize + m.dotGap;
            }
            canvas.DrawTextAt(
                dateStr.c_str(),
                vx,
                vy,
                ff,
                layout.infoValueFont,
                cValue,
                false);
            curX += static_cast<int>(std::ceil(bw)) + m.infoGap;
        }

        if (opts.showCustomExpiry) {
            const std::wstring expiry = IsoToCompactDateW(opts.customSubExpiry);
            block(L"\u5230\u671F", expiry.c_str());
        }

        // ── 7d reset weekday blocks ──
        if (opts.showClaude7dReset) {
            std::wstring wd;
            if (opts.show7dCountdown) {
                wd = Format7dCountdown(
                    snap.c7Reset, 0, opts.countdownShowBeforeHours);
            }
            if (wd.empty() && opts.showStatus) {
                wd = FormatFreshnessMinutes(
                    snap.claudeAvailable, snap.lastClaudeOk, snap.refreshInProgress);
            }
            if (wd.empty()) wd = snap.c7Reset.empty() ? L"--" : IsoToWeekday(snap.c7Reset);
            block(L"Claude", wd.c_str());
        }
        if (opts.showCodex7dReset) {
            const CodexColumnDisplay display =
                BuildCodexColumnDisplay(snap, opts);
            block(L"Codex", display.value.c_str(), nullptr, colors.textLabel,
                  display.gradient);
        }

        // ── Separator 2 + per-source freshness status ──
        if (savedDc != 0) RestoreDC(hdc, savedDc);
    }

    void SetSnapshot(const DashData& d) {
        std::lock_guard<std::mutex> lk(mu_);
        data_ = d;
        cachedW_ = 0;
    }
    void SetOptions(const PluginOptions& o) {
        std::lock_guard<std::mutex> lk(mu_);
        opts_ = o;
        cachedW_ = 0;
    }
    void SetHostLabelColor(COLORREF color) {
        std::lock_guard<std::mutex> lk(mu_);
        hostLabelColor_ = color;
    }
    void SetHostValueColor(COLORREF color) {
        std::lock_guard<std::mutex> lk(mu_);
        hostValueColor_ = color;
    }
    PluginOptions GetOptions() const { std::lock_guard<std::mutex> lk(mu_); return opts_; }
    DashData GetSnapshot() const { std::lock_guard<std::mutex> lk(mu_); return data_; }

private:
    // Height of the slot the host draws into. TrafficMonitor always passes
    // TASKBAR_WND_HEIGHT, but the first width query happens before the first
    // paint, so fall back to that constant until DrawItem reports the real
    // rectangle.
    float SlotHeightHint(int dpi) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (slotHeight_ > 0.0f) return slotHeight_;
        return TaskbarVisualSpec::kHostSlotHeight * (dpi / 96.0f);
    }

    mutable std::mutex mu_;
    DashData data_;
    PluginOptions opts_;
    std::optional<COLORREF> hostLabelColor_;
    std::optional<COLORREF> hostValueColor_;
    mutable int cachedW_ = 0;
    mutable float slotHeight_ = 0.0f;
};

// =====================================================================
// Main plugin
// =====================================================================
class AIUsagePlugin final : public ITMPlugin {
public:
    static AIUsagePlugin& Instance() { static AIUsagePlugin p; return p; }

    IPluginItem* GetItem(int i) override { return i == 0 ? &dash_ : nullptr; }

    // Releases the fetch-cycle lock. Called at the end of every worker path
    // (normal completion and early-stop), so busy_ is always cleared.
    void FinishFetch() {
        std::lock_guard<std::mutex> lk(mu_);
        busy_ = false;
    }

    // Signals the background fetch worker to stop and joins it. Called from
    // DllMain on DLL_PROCESS_DETACH so a stalled worker thread cannot keep the
    // host process alive after exit (the residual-process bug).
    //
    // The wait is BOUNDED and lock-free: in DllMain (loader lock held) any
    // sync primitive (mutex/condition_variable) is risky, so we poll the
    // worker's completion flag with a pure sleep. If the worker is in the
    // middle of an in-flight request (each WinHttp call has a 5s timeout, a
    // fetch can chain several), an unbounded join() would delay process exit
    // for up to ~10-15s. On host process exit the OS reclaims every thread,
    // so we never block indefinitely.
    void Shutdown() {
        stop_.store(true);
        if (!worker_.joinable()) return;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!workerExited_.load()
               && std::chrono::steady_clock::now() < deadline) {
            ::Sleep(50);
        }
        if (workerExited_.load() && worker_.joinable()) {
            worker_.join(); // thread already finished; instant
        }
        // In-flight case: return without joining; process exit reclaims it.
    }

    void DataRequired() override {
        auto now = std::chrono::steady_clock::now();
        bool applyPendingConfig = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (busy_) return;
            if (configReloadPending_) {
                configReloadPending_ = false;
                applyPendingConfig = true;
            }
        }
        // Safe point: busy_ is false, so no background fetch thread is running
        // and nothing else reads proxyConfig_ or the fetcher credentials.
        // Both DataRequired() and OnExtenedInfo() run on the host UI thread,
        // so they cannot interleave with each other.
        if (applyPendingConfig) ApplyConfigFromIni();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (busy_) return;
            if (lastFetch_.time_since_epoch().count() && now - lastFetch_ < interval_) return;
            busy_ = true;
            lastFetch_ = now;
        }
        {
            DashData refreshing = dash_.GetSnapshot();
            refreshing.refreshInProgress = true;
            dash_.SetSnapshot(refreshing);
        }
        // Join any previously finished worker before starting a new one.
        // Assigning a new std::thread to a joinable worker would call
        // std::terminate via the move-assignment destructor, so we must join
        // first. The previous worker has already cleared busy_ by the time we
        // reach here, so joining a finished thread is instantaneous.
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this]() {
            auto markDone = [&]() {
                workerExited_.store(true);
            };
            auto claude = claude_.Fetch();
            if (stop_.load()) { FinishFetch(); markDone(); return; }
            auto codex = codex_.Fetch();
            if (stop_.load()) { FinishFetch(); markDone(); return; }
            auto ag = ag_.Fetch();
            if (stop_.load()) { FinishFetch(); markDone(); return; }
            auto kiro = kiro_.Fetch();
            if (stop_.load()) { FinishFetch(); markDone(); return; }
            DashData d = dash_.GetSnapshot();
            const double t = static_cast<double>(time(nullptr));
            if (claude.success) {
                d.c5 = std::clamp(claude.fiveHourPercent, -1, 100);
                d.c7 = std::clamp(claude.sevenDayPercent, -1, 100);
                d.c5Reset = claude.fiveHourResetAt;
                d.c7Reset = claude.sevenDayResetAt;
                d.usedCredits = claude.usedCredits;
                d.currency = claude.currency;
                d.decPlaces = claude.decimalPlaces;
                d.subStatus = claude.subscriptionStatus;
                d.claudeAvailable = true;
                d.lastClaudeOk = claude.lastSuccessTime > 0 ? claude.lastSuccessTime : t;
            }
            if (codex.success) {
                bool hasCodexUsage = false;
                if (codex.fiveHour.remainingPercent >= 0
                    && codex.fiveHour.remainingPercent <= 100) {
                    d.x5 = 100 - codex.fiveHour.remainingPercent;
                    hasCodexUsage = true;
                }
                if (codex.weekly.remainingPercent >= 0
                    && codex.weekly.remainingPercent <= 100) {
                    d.x7 = 100 - codex.weekly.remainingPercent;
                    d.x7ResetUnix = codex.weekly.resetAtUnixSeconds;
                    hasCodexUsage = true;
                }
                if (hasCodexUsage) {
                    d.codexAvailable = true;
                    d.lastCodexOk = t;
                }
            }
            if (codex.resetCredits.success) {
                d.resetCreditsAvailable = true;
                d.resetCreditCount = codex.resetCredits.availableCount;
                d.resetCreditExpiryUnix = EarliestAvailableResetCreditExpiry(
                    codex.resetCredits, static_cast<long long>(t));
            }
            if (codex.resetRadar.success) {
                d.resetRadarWindowOpen = codex.resetRadar.windowOpen;
                d.resetProbability24h = codex.resetRadar.probability24h;
                d.resetProbability48h = codex.resetRadar.probability48h;
                d.resetRadarUpdatedUnix =
                    codex.resetRadar.updatedAtUnixSeconds;
            }
            if (ag.success) {
                const PluginOptions agOpts = dash_.GetOptions();
                if (const AntigravityModelQuota* featured =
                        SelectFeaturedModel(ag, agOpts.antigravityPrimaryModel)) {
                    const int remainingPct =
                        std::clamp(featured->percent(), 0, 100);
                    d.agPct = 100 - remainingPct;
                }
                d.agTier = ag.tier;
                d.agAvailable = true;
                d.lastAgOk = ag.lastSuccessTime > 0 ? ag.lastSuccessTime : t;
            }
            if (kiro.success && kiro.percent >= 0) {
                d.kiroPct = std::clamp(100 - kiro.percent, 0, 100);
                d.kiroRemaining = kiro.remaining;
                d.kiroLimit = kiro.limit;
                d.kiroAvailable = true;
                d.lastKiroOk = kiro.lastSuccessTime > 0 ? kiro.lastSuccessTime : t;
            }
            d.refreshInProgress = false;
            dash_.SetSnapshot(d);
            {
                std::lock_guard<std::mutex> lk(mu_);
                tip_ = BuildTip(claude, codex, ag, kiro);
            }
            FinishFetch();
            markDone();
        });
    }

    const wchar_t* GetInfo(PluginInfoIndex i) override {
        switch (i) {
            case TMI_NAME:        return L"AI Usage";
            case TMI_DESCRIPTION: return L"Claude, Codex, Antigravity & Kiro usage dashboard.";
            case TMI_AUTHOR:      return L"B-22";
            case TMI_COPYRIGHT:   return L"MIT";
            case TMI_VERSION:     return CODEX_USAGE_VERSION_WIDE;
            case TMI_URL:         return L"https://github.com/B-22/TrafficMonitor-AIUsage";
            default:              return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override {
        // Copy into a FIXED-ADDRESS buffer. tooltipOut_ used to be a
        // std::wstring member that reallocated on every call; if the host
        // keeps the returned pointer across calls (instead of copying it
        // immediately), the old pointer dangles as soon as the next data
        // update refreshes the tip — reading freed memory surfaces as random
        // Win32 failures (e.g. ERROR_INVALID_PARAMETER once data arrives).
        // The fixed array never moves and bounds the length at the same time.
        std::lock_guard<std::mutex> lk(mu_);
        wcsncpy_s(tooltipBuf_, tip_.c_str(), _TRUNCATE);
        return tooltipBuf_;
    }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        HWND parent = (HWND)hParent;
        PluginOptions o = dash_.GetOptions();
        DashData snap = dash_.GetSnapshot();

        // Run connectivity test
        auto conn = TestConnectivity(proxyConfig_);

        // Read proxy settings from config
        std::wstring proxyServer;
        std::wstring allowedExitIps;
        bool requireProxy = false;
        std::wstring iniActualPath;
        if (!configDir_.empty()) {
            std::wstring iniPath = configDir_ + L"\\AIUsage.ini";
            iniActualPath = iniPath;
            wchar_t buf[256];
            GetPrivateProfileStringW(L"AIUsage", L"ProxyServer", L"", buf, 256, iniPath.c_str());
            proxyServer = buf;
            GetPrivateProfileStringW(L"AIUsage", L"RequireProxy", L"0", buf, 256, iniPath.c_str());
            requireProxy = (buf[0] == L'1');
            GetPrivateProfileStringW(L"AIUsage", L"AllowedExitIPs", L"", buf, 256, iniPath.c_str());
            allowedExitIps = buf;
        }

        // Build proxy status string
        std::wstring proxyStatus;
        if (!proxyServer.empty()) {
            proxyStatus = L"Explicit: " + proxyServer;
        } else if (proxyConfig_.systemProxyDetected) {
            proxyStatus = L"System proxy detected";
        } else {
            proxyStatus = L"None";
        }

        // Connectivity status
        std::wstring connStatus;
        if (!conn.statusMessage.empty()) {
            connStatus = conn.statusMessage;
        } else if (conn.directReachable && conn.proxyReachable) {
            connStatus = L"Direct + Proxy OK";
        } else if (conn.directReachable) {
            connStatus = L"Direct OK (no proxy needed)";
        } else if (conn.proxyReachable) {
            connStatus = L"Proxy only (direct blocked)";
        } else {
            connStatus = L"FAILED - check network";
        }

        // Build data diagnostic strings
        wchar_t c7ResetBuf[64] = L"(empty)";
        if (!snap.c7Reset.empty()) {
            std::wstring tmp(snap.c7Reset.begin(), snap.c7Reset.end());
            swprintf_s(c7ResetBuf, L"%s", tmp.c_str());
        }
        wchar_t x7ResetBuf[64];
        swprintf_s(x7ResetBuf, L"%lld", snap.x7ResetUnix);
        const std::wstring resetCreditExpiry =
            UnixToLocalDateTimeW(snap.resetCreditExpiryUnix);
        const std::wstring radarUpdated =
            UnixToLocalDateTimeW(snap.resetRadarUpdatedUnix);

        const std::wstring freshness = BuildFreshnessText(snap);
        wchar_t msg[4096];
        swprintf_s(msg,
            L"AI Usage Plugin Options\n"
            L"========================================\n\n"
            L"Display Options:\n"
            L"  1. Show %% sign:          %s\n"
            L"  2. Show Credits:          %s\n"
            L"  3. Show 5h Reset:         %s\n"
            L"  4. Show Subscription:     %s\n"
            L"  5. Show Custom Expiry:    %s\n"
            L"  6. Show Freshness Status: %s\n"
            L"  7. Show Claude Weekday:   %s\n"
            L"  8. Show Codex Weekday:    %s\n"
            L"  9. Show 7d Countdown:     %s\n"
            L" 10. Countdown Threshold:   %d hours\n"
            L" 11. Reset Card Warning:    %s\n"
            L" 12. Card Warning Threshold:%d hours\n"
            L" 13. Show Reset Radar:      %s\n"
            L" 14. Forecast Refresh:      %d minutes\n"
            L" 15. Runway Refresh:        %d minutes\n"
            L" 16. Custom Sub Expiry:     %s\n"
            L" 17. Show Antigravity:      %s\n"
            L" 18. Show Kiro Credits:     %s\n"
            L" 19. AG Primary Model:      %s\n\n"
            L"Proxy Settings:\n"
            L"  Proxy Server:   %s\n"
            L"  Require Proxy:  %s\n"
            L"  Allowed Exit IPs: %s\n"
            L"  Observed Exit IP: %s\n\n"
            L"Connectivity Test:\n"
            L"  Direct:  %s\n"
            L"  Proxy:   %s\n"
            L"  Status:  %s\n\n"
            L"Data Diagnostics:\n"
            L"  Config Path:    %s\n"
            L"  Claude 5h%%:    %d\n"
            L"  Claude 7d%%:    %d\n"
            L"  Claude 7d Reset: %s\n"
            L"  Codex 5h%%:     %d\n"
            L"  Codex 7d%%:     %d\n"
            L"  Codex 7d Reset:  %s\n"
            L"  AG Ring (used%%): %d\n"
            L"  AG Tier:         %s\n"
            L"  Kiro Ring (used%%): %d\n"
            L"  Kiro Remaining:  %lld\n"
            L"  Reset Cards:     %d\n"
            L"  Card Expiry:     %s\n"
            L"  Radar Window:    %s\n"
            L"  Radar Updated:   %s\n"
            L"  Freshness:       %s\n\n"
            L"To change settings, edit AIUsage.ini:\n"
            L"  [AIUsage]\n"
            L"  ShowPctSign=0\n"
            L"  ShowCredits=1\n"
            L"  ShowReset=1\n"
            L"  ShowSubscription=0\n"
            L"  ShowCustomExpiry=1\n"
            L"  ShowStatus=1\n"
            L"  ShowClaude7dReset=1\n"
            L"  ShowCodex7dReset=1\n"
            L"  Show7dCountdown=1\n"
            L"  CountdownShowBeforeHours=24\n"
            L"  ShowResetCreditWarning=1\n"
            L"  ResetCreditWarningHours=48\n"
            L"  ShowResetRadar=1\n"
            L"  ResetRadarRefreshMinutes=15\n"
            L"  RunwayResetRefreshMinutes=60\n"
            L"  ShowAntigravity=1\n"
            L"  ShowKiro=0\n"
            L"  CustomSubExpiry=\n"
            L"  ProxyServer=\n"
            L"  RequireProxy=1\n"
            L"  AllowedExitIPs=203.0.113.10\n"
            L"  [Antigravity]\n"
            L"  ClientId=\n"
            L"  ClientSecret=\n"
            L"  AccessToken=\n"
            L"  RefreshToken=\n"
            L"  PrimaryModel=\n"
            L"  [Kiro]\n"
            L"  TokenPath=",
            o.showPctSign ? L"Yes" : L"No",
            o.showCredits ? L"Yes" : L"No",
            o.showReset ? L"Yes" : L"No",
            o.showSubscription ? L"Yes" : L"No",
            o.showCustomExpiry ? L"Yes" : L"No",
            o.showStatus ? L"Yes" : L"No",
            o.showClaude7dReset ? L"Yes" : L"No",
            o.showCodex7dReset ? L"Yes" : L"No",
            o.show7dCountdown ? L"Yes" : L"No",
            o.countdownShowBeforeHours,
            o.showResetCreditWarning ? L"Yes" : L"No",
            o.resetCreditWarningHours,
            o.showResetRadar ? L"Yes" : L"No",
            o.resetRadarRefreshMinutes,
            o.resetTodayRefreshMinutes,
            o.customSubExpiry.empty() ? L"(auto)" : std::wstring(o.customSubExpiry.begin(), o.customSubExpiry.end()).c_str(),
            o.showAntigravity ? L"Yes" : L"No",
            o.showKiro ? L"Yes" : L"No",
            o.antigravityPrimaryModel.empty() ? L"(auto)" : std::wstring(o.antigravityPrimaryModel.begin(), o.antigravityPrimaryModel.end()).c_str(),
            proxyServer.empty() ? L"(system)" : proxyServer.c_str(),
            requireProxy ? L"Yes" : L"No",
            allowedExitIps.empty() ? L"(disabled)" : allowedExitIps.c_str(),
            conn.observedExitIp.empty() ? L"(not checked)" : conn.observedExitIp.c_str(),
            conn.directReachable ? L"Yes" : L"No",
            conn.proxyReachable ? L"Yes" : L"No",
            connStatus.c_str(),
            iniActualPath.empty() ? L"(not set)" : iniActualPath.c_str(),
            snap.c5, snap.c7,
            c7ResetBuf,
            snap.x5, snap.x7,
            x7ResetBuf,
            snap.agPct,
            FormatAgTier(snap.agTier).empty() ? L"--" : FormatAgTier(snap.agTier).c_str(),
            snap.kiroPct, snap.kiroRemaining,
            snap.resetCreditCount,
            resetCreditExpiry.c_str(),
            snap.resetRadarWindowOpen ? L"Open" : L"Closed",
            radarUpdated.c_str(),
            freshness.empty() ? L"Fresh" : freshness.c_str());
        MessageBoxW(parent, msg, L"AI Usage Options", MB_OK | MB_ICONINFORMATION);
        return OR_OPTION_UNCHANGED;
    }

    void OnExtenedInfo(ExtendedInfoIndex idx, const wchar_t* data) override {
        if (idx == EI_LABEL_TEXT_COLOR) {
            if (const auto color = ParseHostColor(data)) {
                dash_.SetHostLabelColor(*color);
            }
        } else if (idx == EI_VALUE_TEXT_COLOR) {
            if (const auto color = ParseHostColor(data)) {
                dash_.SetHostValueColor(*color);
            }
        } else if (idx == EI_CONFIG_DIR && data) {
            std::wstring candidateDir = data;
            std::wstring candidateIni = candidateDir + L"\\AIUsage.ini";
            const DWORD attributes = GetFileAttributesW(candidateIni.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES
                && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                configDir_ = std::move(candidateDir);
                LoadConfig();
            }
        }
    }

private:
    AIUsagePlugin() {
        // Some TrafficMonitor builds do not send EI_CONFIG_DIR to plugins.
        // Fall back to the directory of the running host executable so local
        // display and proxy-safety settings are always loaded.
        std::array<wchar_t, 32768> executablePath{};
        const DWORD length = GetModuleFileNameW(
            nullptr, executablePath.data(),
            static_cast<DWORD>(executablePath.size()));
        if (length > 0 && length < executablePath.size()) {
            std::wstring path(executablePath.data(), length);
            const size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                configDir_ = path.substr(0, slash);
                LoadConfig();
            }
        }
    }

    // Entry point used by the constructor and by OnExtenedInfo(). If a
    // background fetch is in flight, applying the config now would race with
    // the fetcher threads reading proxyConfig_ / credentials, so defer it to
    // the next DataRequired() tick instead.
    void LoadConfig() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (busy_) {
                configReloadPending_ = true;
                return;
            }
        }
        ApplyConfigFromIni();
    }

    void ApplyConfigFromIni() {
        if (configDir_.empty()) return;
        std::wstring iniPath = configDir_ + L"\\AIUsage.ini";
        const DWORD attributes = GetFileAttributesW(iniPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return;
        }
        PluginOptions o;
        wchar_t buf[1024];

        GetPrivateProfileStringW(L"AIUsage", L"ShowPctSign", L"0", buf, 256, iniPath.c_str());
        o.showPctSign = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowCredits", L"1", buf, 256, iniPath.c_str());
        o.showCredits = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowReset", L"1", buf, 256, iniPath.c_str());
        o.showReset = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowSubscription", L"0", buf, 256, iniPath.c_str());
        o.showSubscription = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowCustomExpiry", L"0", buf, 256, iniPath.c_str());
        o.showCustomExpiry = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowStatus", L"1", buf, 256, iniPath.c_str());
        o.showStatus = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowClaude7dReset", L"1", buf, 256, iniPath.c_str());
        o.showClaude7dReset = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowCodex7dReset", L"1", buf, 256, iniPath.c_str());
        o.showCodex7dReset = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"Show7dCountdown", L"1", buf, 256, iniPath.c_str());
        o.show7dCountdown = (buf[0] == L'1');

        GetPrivateProfileStringW(
            L"AIUsage", L"ShowResetCreditWarning", L"1",
            buf, 256, iniPath.c_str());
        o.showResetCreditWarning = (buf[0] == L'1');

        GetPrivateProfileStringW(
            L"AIUsage", L"ShowResetRadar", L"1",
            buf, 256, iniPath.c_str());
        o.showResetRadar = (buf[0] == L'1');

        GetPrivateProfileStringW(
            L"AIUsage", L"ShowAntigravity", L"1",
            buf, 256, iniPath.c_str());
        o.showAntigravity = (buf[0] == L'1');

        GetPrivateProfileStringW(
            L"AIUsage", L"ShowKiro", L"0",
            buf, 256, iniPath.c_str());
        o.showKiro = (buf[0] == L'1');

        o.countdownShowBeforeHours = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"AIUsage", L"CountdownShowBeforeHours", 24, iniPath.c_str())),
            1, 168);

        o.resetCreditWarningHours = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"AIUsage", L"ResetCreditWarningHours", 48, iniPath.c_str())),
            1, 168);

        o.resetRadarRefreshMinutes = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"AIUsage", L"ResetRadarRefreshMinutes", 15, iniPath.c_str())),
            5, 120);

        o.resetTodayRefreshMinutes = std::clamp(
            static_cast<int>(GetPrivateProfileIntW(
                L"AIUsage", L"RunwayResetRefreshMinutes", 60,
                iniPath.c_str())),
            15, 240);

        GetPrivateProfileStringW(
            L"AIUsage", L"CustomSubExpiry", L"", buf, 256, iniPath.c_str());
        // Convert wide to narrow
        o.customSubExpiry.clear();
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string utf8(static_cast<size_t>(len), '\0');
            if (WideCharToMultiByte(
                    CP_UTF8, 0, buf, -1, utf8.data(), len, nullptr, nullptr) > 0) {
                utf8.pop_back();
                o.customSubExpiry = std::move(utf8);
            }
        }

        dash_.SetOptions(o);

        // Read proxy settings
        GetPrivateProfileStringW(L"AIUsage", L"ProxyServer", L"", buf, 1024, iniPath.c_str());
        std::wstring proxyServer = buf;

        GetPrivateProfileStringW(L"AIUsage", L"RequireProxy", L"0", buf, 1024, iniPath.c_str());
        bool requireProxy = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"AllowedExitIPs", L"", buf, 1024, iniPath.c_str());
        std::wstring allowedExitIps = buf;

        GetPrivateProfileStringW(
            L"AIUsage", L"ExitIpCheckUrl", L"https://api.ipify.org/",
            buf, 1024, iniPath.c_str());
        std::wstring exitIpCheckUrl = buf;

        proxyConfig_ = DetectProxy(
            proxyServer, requireProxy, allowedExitIps, exitIpCheckUrl);
        GetPrivateProfileStringW(
            L"AIUsage", L"VerifyTargetHostExitIp", L"1",
            buf, 1024, iniPath.c_str());
        proxyConfig_.verifyTargetHost = (buf[0] != L'0');
        claude_.SetProxyConfig(proxyConfig_);
        codex_.SetProxyConfig(proxyConfig_);
        codex_.SetRadarEnabled(o.showResetRadar);
        codex_.SetRadarRefreshMinutes(o.resetRadarRefreshMinutes);
        codex_.SetResetTodayRefreshMinutes(
            o.resetTodayRefreshMinutes);

        // Antigravity credentials ([Antigravity] section).
        ag_.SetEnabled(o.showAntigravity);
        ag_.SetProxyConfig(proxyConfig_);
        std::string agClientId, agClientSecret, agAccessToken, agRefreshToken;
        {
            auto readSection = [&](const wchar_t* key, std::string& out) {
                wchar_t sbuf[2048];
                GetPrivateProfileStringW(
                    L"Antigravity", key, L"", sbuf, 2048, iniPath.c_str());
                out.clear();
                int slen = WideCharToMultiByte(
                    CP_UTF8, 0, sbuf, -1, nullptr, 0, nullptr, nullptr);
                if (slen > 1) {
                    out.resize(static_cast<size_t>(slen));
                    if (WideCharToMultiByte(
                            CP_UTF8, 0, sbuf, -1, out.data(),
                            slen, nullptr, nullptr) > 0) {
                        out.pop_back();
                    } else {
                        out.clear();
                    }
                }
            };
            readSection(L"ClientId", agClientId);
            readSection(L"ClientSecret", agClientSecret);
            readSection(L"AccessToken", agAccessToken);
            readSection(L"RefreshToken", agRefreshToken);
            std::string agPrimaryModel;
            readSection(L"PrimaryModel", agPrimaryModel);
            o.antigravityPrimaryModel = agPrimaryModel;
        }
        ag_.SetCredentials(
            agAccessToken, agRefreshToken, agClientId, agClientSecret);
        ag_.SetPrimaryModel(o.antigravityPrimaryModel);

        // Kiro token cache path override ([Kiro] section).
        kiro_.SetEnabled(o.showKiro);
        kiro_.SetProxyConfig(proxyConfig_);
        wchar_t kiroPath[1024];
        GetPrivateProfileStringW(
            L"Kiro", L"TokenPath", L"", kiroPath, 1024, iniPath.c_str());
        if (kiroPath[0] != L'\0') {
            kiro_.SetTokenFilePath(kiroPath);
        }
        dash_.SetOptions(o);
    }

    std::wstring BuildTip(const ClaudeUsageData& c, const UsageSnapshot& x,
                          const AntigravityUsageData& ag,
                          const KiroCreditsData& kiro) const {
        // 10-cell ASCII progress bar: [##########] (any font, no width math)
        auto bar = [](int remaining) -> std::wstring {
            int filled = std::clamp((remaining + 5) / 10, 0, 10); // round 0..10
            std::wstring b = L"[";
            for (int i = 0; i < 10; ++i) b += (i < filled) ? L"#" : L".";
            b += L"]";
            return b;
        };
        // Freshest successful fetch time among sources, for a staleness hint.
        double latest = std::max(c.lastSuccessTime, ag.lastSuccessTime);
        std::wstring updated;
        if (latest > 0) {
            time_t ut = static_cast<time_t>(latest);
            struct tm tmBuf{};
            localtime_s(&tmBuf, &ut);
            wchar_t timeBuf[32];
            swprintf_s(timeBuf, L"\u66F4\u65B0\u4E8E %02d:%02d:%02d", // 更新于
                       tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
            updated = timeBuf;
        }

        std::wstring t = L"===== AI Usage =====\n";
        t += L" \u914D\u989D"; // 配额
        if (!updated.empty()) t += L"  [" + updated + L"]";
        t += L"\n";

        auto addReset = [&](const std::string& iso) -> std::wstring {
            SYSTEMTIME loc{};
            if (!IsoToLocalSystemTime(iso, &loc)) return L"--";
            wchar_t b[48];
            swprintf_s(b, L"%02d-%02d %02d:%02d",
                loc.wMonth, loc.wDay, loc.wHour, loc.wMinute);
            return b;
        };
        auto addQuotaLine = [&](const wchar_t* provider,
                                const wchar_t* window,
                                int remaining,
                                const std::wstring& reset) {
            t += L" ";
            t += provider;
            t += L" ";
            t += window;
            t += L"  ";
            if (remaining >= 0) {
                t += bar(remaining);
                t += L" ";
                t += std::to_wstring(std::clamp(remaining, 0, 100));
                t += L"%";
            } else {
                t += L"[..........] --";
            }
            if (!reset.empty() && reset != L"--") {
                t += L"  ";
                t += reset;
            }
            t += L"\n";
        };

        if (c.success) {
            addQuotaLine(
                L"Claude", L"5h",
                c.fiveHourPercent >= 0
                    ? 100 - c.fiveHourPercent : -1,
                addReset(c.fiveHourResetAt));
            addQuotaLine(
                L"Claude", L"7d",
                c.sevenDayPercent >= 0
                    ? 100 - c.sevenDayPercent : -1,
                addReset(c.sevenDayResetAt));
        } else {
            t += L" Claude [!] \u672A\u83B7\u53D6\n"; // 未获取
        }
        if (x.success) {
            addQuotaLine(
                L"Codex", L"5h", x.fiveHour.remainingPercent,
                x.fiveHour.resetAtUnixSeconds > 0
                    ? UnixToLocalDateTimeW(
                        x.fiveHour.resetAtUnixSeconds)
                    : L"");
            addQuotaLine(
                L"Codex", L"7d", x.weekly.remainingPercent,
                x.weekly.resetAtUnixSeconds > 0
                    ? UnixToLocalDateTimeW(
                        x.weekly.resetAtUnixSeconds)
                    : L"");
        } else {
            t += L" Codex [!] \u672A\u83B7\u53D6\n"; // 未获取
        }

        if (ag.success && !ag.models.empty()) {
            // Dedup rule (user preference): keep every non-Gemini model
            // (Claude/GPT/others), and for Gemini keep only the LATEST
            // version of each variant ("Gemini 3.6 Flash High" wins over
            // "Gemini 3.1 Flash High"). The old wall of 13 identical rows is
            // gone and the sections below it (reset radar / account /
            // status) are no longer pushed out of the tip buffer.
            std::map<std::string, int> bestGeminiVersion; // family -> max
            for (const auto& model : ag.models) {
                const int v = GeminiModelVersion(model.displayName);
                if (v < 0) continue;
                const std::string family = GeminiModelFamily(model.displayName);
                auto& best = bestGeminiVersion[family];
                if (v > best) best = v;
            }
            std::set<std::string> shownFamilies; // one row per Gemini family
            for (const auto& model : ag.models) {
                const int v = GeminiModelVersion(model.displayName);
                if (v >= 0) {
                    const std::string family =
                        GeminiModelFamily(model.displayName);
                    if (v != bestGeminiVersion[family]) continue; // old version
                    // Same family, same version (High/Low tiers): keep one.
                    if (!shownFamilies.insert(family).second) continue;
                }
                // No progress bar here: model names are long and the rows
                // must stay compact so every tooltip section stays visible.
                t += L" ";
                t += ToWide(model.displayName);
                t += L" ";
                t += std::to_wstring(std::clamp(model.percent(), 0, 100));
                t += L"%";
                if (!model.resetTime.empty()) {
                    const std::wstring reset = IsoToLocalDateTimeW(model.resetTime);
                    if (reset.size() >= 5) {
                        t += L" ";
                        t += reset.substr(0, 5); // MM-DD
                    }
                }
                t += L"\n";
            }
            const std::wstring agTierLabel = FormatAgTier(ag.tier);
            if (!agTierLabel.empty()) {
                t += L"  \u5957\u9910  " + agTierLabel + L"\n"; // 套餐
            }
        } else {
            // 未授权 → 双击 ag-login.exe
            t += L" \u53CD\u91CD\u529B [!] \u672A\u6388\u6743 \u2192 \u53CC\u51FB ag-login.exe\n";
        }

        if (kiro.success) {
            t += L" Kiro Credits  ";
            t += std::to_wstring(kiro.remaining);
            t += L" / ";
            t += std::to_wstring(kiro.limit);
            t += L" \u5269\u4F59"; // 剩余
            if (kiro.percent >= 0) {
                t += L"  \u00B7  ";
                t += std::to_wstring(std::clamp(kiro.percent, 0, 100));
                t += L"%";
            }
            t += L"\n";
        } else {
            t += L" Kiro Credits [!] \u672A\u767B\u5F55\n"; // 未登录
        }

        if (dash_.GetOptions().showResetRadar) {
            t += L"\n\u4ECA\u65E5\u5168\u5C40\u91CD\u7F6E\n";
            if (x.resetRadar.runwaySourceAvailable) {
                if (x.resetRadar.todayState == ResetTodayState::Yes) {
                    t += L"\u25CF \u662F";
                } else if (x.resetRadar.todayState == ResetTodayState::No) {
                    t += L"\u25CB \u5426";
                } else {
                    t += L"? \u672A\u77E5";
                }
                if (!x.resetRadar.message.empty()) {
                    t += L"  \u00B7  " + x.resetRadar.message;
                }
                t += L"\n";
            } else if (x.resetRadar.windowOpen) {
                t += L"\u25B3 \u8865\u5145\u9884\u6D4B\u7A97\u53E3\u5DF2\u5F00\u542F\n";
            } else {
                t += L"? \u6682\u65E0\u53EF\u7528\u7ED3\u8BBA\n";
            }

            if (x.resetRadar.nextScheduledAtUnixSeconds > 0) {
                t += L"\u8BA1\u5212  ";
                t += UnixToLocalDateTimeW(
                    x.resetRadar.nextScheduledAtUnixSeconds);
                t += L"\n";
            } else if (x.resetRadar.latestResetAtUnixSeconds > 0) {
                t += L"\u6700\u8FD1  ";
                t += UnixToLocalDateTimeW(
                    x.resetRadar.latestResetAtUnixSeconds);
                t += L"\n";
            }

            auto scopeName = [](const std::wstring& value) {
                if (value == L"all") return std::wstring(L"\u5168\u90E8\u5957\u9910");
                if (value == L"weekly") return std::wstring(L"\u5468\u989D\u5EA6");
                if (value == L"five_hour") return std::wstring(L"5 \u5C0F\u65F6\u989D\u5EA6");
                if (value == L"unknown") return std::wstring(L"\u672A\u77E5\u989D\u5EA6");
                if (value == L"plus") return std::wstring(L"Plus");
                if (value == L"pro") return std::wstring(L"Pro");
                if (value == L"team") return std::wstring(L"Team");
                return value;
            };
            auto joinScope = [&](const std::vector<std::wstring>& values) {
                std::wstring joined;
                for (const std::wstring& value : values) {
                    if (!joined.empty()) joined += L" \u00B7 ";
                    joined += scopeName(value);
                }
                return joined;
            };
            const std::wstring plans = joinScope(x.resetRadar.scopePlans);
            const std::wstring windows = joinScope(x.resetRadar.scopeWindows);
            if (!plans.empty() || !windows.empty()) {
                t += L"\u8303\u56F4  ";
                t += plans;
                if (!plans.empty() && !windows.empty()) t += L" \u00B7 ";
                t += windows;
                t += L"\n";
            }
            if (x.resetRadar.confidencePercent >= 0) {
                t += L"\u53EF\u4FE1\u5EA6  ";
                t += std::to_wstring(x.resetRadar.confidencePercent);
                t += L"%";
            }
            if (x.resetRadar.confidencePercent >= 0) t += L"\n";
            if (x.resetRadar.probability24h >= 0
                || x.resetRadar.probability48h >= 0) {
                t += L"\u8865\u5145\u9884\u6D4B  ";
                if (x.resetRadar.probability24h >= 0) {
                    t += L"24h ";
                    t += std::to_wstring(x.resetRadar.probability24h);
                    t += L"%";
                }
                if (x.resetRadar.probability48h >= 0) {
                    if (x.resetRadar.probability24h >= 0) t += L"  \u00B7  ";
                    t += L"48h ";
                    t += std::to_wstring(x.resetRadar.probability48h);
                    t += L"%";
                }
                t += L"\n";
            }
            if (x.resetRadar.updatedAtUnixSeconds > 0) {
                t += L"\u66F4\u65B0  ";
                t += UnixToLocalDateTimeW(
                    x.resetRadar.updatedAtUnixSeconds);
                t += L"\n";
            }
            t += x.resetRadar.runwaySourceAvailable
                ? L"\u6765\u6E90  Codex Runway / @thsottiaux\n"
                : L"\u6765\u6E90  codex-reset.com / Codex Reset Radar\n";
        }

        t += L"\n\u8D26\u6237\n";
        if (x.resetCredits.success) {
            t += L"Reset credits  "
                + std::to_wstring(x.resetCredits.availableCount)
                + L" \u53EF\u7528";
            long long earliestExpiry = 0;
            for (const ResetCredit& credit : x.resetCredits.credits) {
                std::wstring status = credit.status;
                std::transform(
                    status.begin(), status.end(), status.begin(), towlower);
                if (!status.empty() && status != L"available") continue;
                if (credit.expiresAtUnixSeconds > 0
                    && (earliestExpiry == 0
                        || credit.expiresAtUnixSeconds < earliestExpiry)) {
                    earliestExpiry = credit.expiresAtUnixSeconds;
                }
            }
            if (earliestExpiry > 0) {
                t += L"  \u00B7  \u6700\u65E9 ";
                t += UnixToLocalDateTimeW(earliestExpiry);
                t += L" \u5230\u671F";
            }
            t += L"\n";
        } else if (!x.resetCredits.errorMessage.empty()) {
            t += L"Reset credits  "
                + x.resetCredits.errorMessage + L"\n";
        }
        if (c.success && c.usedCredits > 0) {
            const double amount =
                c.usedCredits / pow(10.0, c.decimalPlaces);
            wchar_t buffer[32];
            swprintf_s(buffer, L"Credits  $%.2f\n", amount);
            t += buffer;
        }
        if (!c.subscriptionStatus.empty()) {
            // Only render known lifecycle statuses. Unknown tier identifiers
            // (e.g. "g1-pro-tier") are not meaningful on the dashboard and
            // are hidden by default (user preference).
            const std::string st = c.subscriptionStatus;
            if (st == "active" || st == "trialing" || st == "canceled"
                || st == "past_due" || st == "paused") {
                t += L"Claude  " + ToWide(st) + L"\n";
            }
        }

        t += L"\n\u72B6\u6001\n"; // 状态
        if (!proxyConfig_.statusMessage.empty()) {
            t += L" [ok] \u7F51\u7EDC  "; // 网络
            std::wstring net = proxyConfig_.statusMessage;
            if (net.size() > 42) { // keep the row short for the tip budget
                net.resize(42);
                net += L"...";
            }
            t += net;
            t += L"\n";
        } else if (proxyConfig_.systemProxyDetected) {
            t += L" [ok] \u7F51\u7EDC  System proxy\n"; // 网络
        } else {
            t += L" [ok] \u7F51\u7EDC  Direct / auto\n"; // 网络
        }
        if (!c.errorMessage.empty()) {
            t += L" [!] Claude  " + c.errorMessage + L"\n";
        }
        if (!x.errorMessage.empty()) {
            t += L" [!] Codex  " + x.errorMessage + L"\n";
        }
        if (!ag.errorMessage.empty()) {
            t += L" [!] Antigravity  " + ag.errorMessage + L"\n";
        }
        if (!kiro.errorMessage.empty()) {
            t += L" [!] Kiro  " + kiro.errorMessage + L"\n";
        }
        if (!x.resetRadar.errorMessage.empty()) {
            t += L" [!] \u91CD\u7F6E\u6E90  "; // 重置源
            t += x.resetRadar.errorMessage;
            t += L"\n";
        }
        // Guard against host-side fixed-size tooltip buffers: keep the text
        // bounded (quota rows come first, truncation drops the tail).
        constexpr size_t kMaxTipChars = 768;
        if (t.size() > kMaxTipChars) {
            t.resize(kMaxTipChars - 3);
            t += L"...";
        }
        return t;
    }

    AIDashboardItem dash_;
    ClaudeUsageFetcher claude_;
    CodexUsageFetcher codex_;
    AntigravityUsageFetcher ag_;
    KiroCreditsFetcher kiro_;
    ProxyConfig proxyConfig_;
    std::wstring configDir_;
    mutable std::mutex mu_;
    std::wstring tip_ = L"AI Usage: waiting for data";
    // Fixed-address tooltip buffer handed to the host. Never reallocated, so
    // the returned pointer stays valid even if the host keeps it across calls
    // (the dangling-pointer bug: tooltipOut_ was a wstring that moved).
    static constexpr size_t kTooltipBufSize = 2048;
    wchar_t tooltipBuf_[kTooltipBufSize] = {};
    bool busy_ = false;
    bool configReloadPending_ = false;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::atomic<bool> workerExited_{false};
    std::chrono::steady_clock::time_point lastFetch_{};
    static constexpr std::chrono::minutes interval_{1};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitGdiplus();
    } else if (reason == DLL_PROCESS_DETACH) {
        // Clean shutdown: signal the background fetch worker to stop and wait
        // for it. Without this, a detached worker thread could keep the host
        // process alive after exit (the residual-process bug). The fetchers'
        // WinHttp timeouts bound a single in-flight request, so join() returns
        // promptly rather than hanging on a stalled network call.
        AIUsagePlugin::Instance().Shutdown();
        // NOTE: GdiplusShutdown() is intentionally NOT called here. Calling it
        // under the loader lock (DLL_PROCESS_DETACH) is forbidden by Microsoft
        // and can deadlock/crash on unload. The process is exiting anyway, so
        // the OS reclaims the GDI+ token; the dashboard only ever re-inits it
        // on DLL_PROCESS_ATTACH of a fresh load.
    }
    return TRUE;
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &AIUsagePlugin::Instance();
}
