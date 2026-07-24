#include "CodexUsageFetcher.h"
#include "ClaudeUsageFetcher.h"
#include "CodexUsageVersion.h"
#include "ProxyHelper.h"

#include "PluginInterface.h"

#include <Windows.h>
#include <ole2.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "gdiplus.lib")

namespace {

// =====================================================================
// Layout metrics — exact values from design/preview_v03.html
// =====================================================================
struct LM {
    int padX;            // 10px
    int ringD;           // 24px
    int ringStroke;      // 2.5px → 3px integer
    int ringGap;         // 6px
    int ringGroupGap;    // 4px (Claude/Codex separator margin)
    int ringGroupSepH;   // 18px
    int pctNumSize;      // 9px
    int pctSignSize;     // 5.5px → 6px
    int labelSize;       // 6px
    int labelOffsetX;    // -4px from ring right edge
    int labelOffsetY;    // 0px from ring top
    int sepW;            // 1px
    int sepH;            // 20px
    int sepMargin;       // 8px each side
    int infoGap;         // 12px
    int infoPadTop;      // 4px
    int infoLabelSize;   // 7px
    int infoValueSize;   // 9.5px → 10px
    int infoSubSize;     // 6.5px → 7px
    int dotSize;         // 5px
    int dotGap;          // 3px
    int statusSize;      // 8px
    int staleSize;       // 8px
};

LM GetMetrics(int dpi) {
    float s = dpi / 96.0f;
    LM m{};
    m.padX          = (int)(10 * s);
    m.ringD         = (int)(24 * s);
    m.ringStroke    = std::max(2, (int)roundf(2.5f * s));
    m.ringGap       = (int)(8 * s);
    m.ringGroupGap  = (int)(4 * s);
    m.ringGroupSepH = (int)(18 * s);
    m.pctNumSize    = std::max(7, (int)(9 * s));
    m.pctSignSize   = std::max(5, (int)roundf(5.5f * s));
    m.labelSize     = std::max(5, (int)(6 * s));
    m.labelOffsetX  = (int)(-2 * s);
    m.labelOffsetY  = 0;
    m.sepW          = 1;
    m.sepH          = (int)(20 * s);
    m.sepMargin     = (int)(8 * s);
    m.infoGap       = (int)(12 * s);
    m.infoPadTop    = (int)(4 * s);
    m.infoLabelSize = std::max(6, (int)(7 * s));
    m.infoValueSize = std::max(8, (int)roundf(9.5f * s));
    m.infoSubSize   = std::max(5, (int)roundf(6.5f * s));
    m.dotSize       = (int)(5 * s);
    m.dotGap        = (int)(3 * s);
    m.statusSize    = std::max(7, (int)(8 * s));
    m.staleSize     = std::max(7, (int)(8 * s));
    return m;
}

// =====================================================================
// Colors — from preview_v03.css
// =====================================================================
constexpr COLORREF RING_TRACK    = RGB(40, 55, 72);    // rgba(255,255,255,0.08) on #1C2B3F
constexpr COLORREF CLAUDE_CLR    = RGB(198, 108, 50);   // #c66c32
constexpr COLORREF CODEX_CLR     = RGB(46, 168, 177);   // #2ea8b1
constexpr COLORREF TEXT_PCT      = RGB(242, 242, 242);  // #f2f2f2
constexpr COLORREF TEXT_LABEL    = RGB(192, 192, 196);  // #c0c0c4  (ring label + info label + status)
constexpr COLORREF TEXT_DIM      = RGB(124, 124, 127);  // ring label fallback
constexpr COLORREF TEXT_VALUE    = RGB(240, 240, 242);  // #f0f0f2
constexpr COLORREF SEP_COLOR     = RGB(31, 31, 31);     // rgba(255,255,255,0.12) on dark
constexpr COLORREF DOT_RED       = RGB(220, 60, 60);    // #dc3c3c
constexpr COLORREF DOT_GREEN     = RGB(46, 168, 74);    // #2ea84a
constexpr COLORREF STALE_CLR     = RGB(220, 60, 60);
constexpr COLORREF RING_NO_DATA  = RGB(60, 60, 63);

COLORREF GaugeColor(int pct, bool isClaude) {
    if (pct < 0) return RING_NO_DATA;
    if (pct < 60) return isClaude ? CLAUDE_CLR : CODEX_CLR;
    if (pct < 85) return RGB(220, 150, 40);
    return RGB(220, 60, 60);
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

std::wstring IsoToLocalTimeW(const std::string& iso) {
    if (iso.empty()) return L"--";
    SYSTEMTIME utc{};
    int n = sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
        &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond);
    if (n < 3) return L"--";
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", (int)local.wHour, (int)local.wMinute);
    return buf;
}

std::wstring IsoToLocalDateW(const std::string& iso) {
    if (iso.empty()) return L"--";
    SYSTEMTIME utc{};
    int n = sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
        &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond);
    if (n < 3) return L"--";
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t buf[16];
    swprintf_s(buf, L"%d\u6708%d\u65E5", (int)local.wMonth, (int)local.wDay);
    return buf;
}

std::wstring FormatStaleAge(double sec) {
    if (sec < 60) return L"\u8FC7\u671F<1m";
    if (sec < 3600) {
        wchar_t b[16]; swprintf_s(b, L"\u8FC7\u671F%dm", (int)(sec / 60));
        return b;
    }
    wchar_t b[24];
    swprintf_s(b, L"\u8FC7\u671F%dh%02dm", (int)(sec / 3600), ((int)sec % 3600) / 60);
    return b;
}

// ISO 8601 date -> weekday in Chinese (周一, 周二, ... 周日)
std::wstring IsoToWeekday(const std::string& iso) {
    if (iso.empty()) return L"--";
    SYSTEMTIME utc{};
    int n = sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
        &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond);
    if (n < 3) return L"--";
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    // Day of week: 0=Sun, 1=Mon, ... 6=Sat
    static const wchar_t* weekdays[] = { L"\u5468\u65E5", L"\u5468\u4E00", L"\u5468\u4E8C",
        L"\u5468\u4E09", L"\u5468\u56DB", L"\u5468\u4E94", L"\u5468\u516D" };
    // Use SystemTime day of week if available, otherwise calculate
    if (local.wDayOfWeek <= 6) return weekdays[local.wDayOfWeek];
    return L"--";
}

// Format 7d reset countdown: "周三 14:32" or "5h30m" or "23m"
// Returns {text, isUrgent} where isUrgent = true if < 2h
std::pair<std::wstring, bool> Format7dCountdown(const std::string& iso, long long unixSec) {
    double now = (double)time(nullptr);
    double resetAt = 0;

    if (!iso.empty()) {
        SYSTEMTIME utc{};
        int n = sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
            &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond);
        if (n >= 3) {
            SYSTEMTIME local{};
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
            FILETIME ft{};
            SystemTimeToFileTime(&local, &ft);
            ULARGE_INTEGER uli;
            uli.LowPart = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            resetAt = (double)(uli.QuadPart - 116444736000000000ULL) / 10000000.0;
        }
    } else if (unixSec > 0) {
        resetAt = (double)unixSec;
    }

    if (resetAt <= now) return { L"--", false };
    double diff = resetAt - now;

    // If > 24h, show weekday + time
    if (diff > 86400) {
        // Parse the time to get weekday and HH:MM
        if (!iso.empty()) {
            SYSTEMTIME utc{};
            int n = sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
                &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond);
            if (n >= 5) {
                SYSTEMTIME local{};
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
                static const wchar_t* wd[] = { L"\u5468\u65E5", L"\u5468\u4E00", L"\u5468\u4E8C",
                    L"\u5468\u4E09", L"\u5468\u56DB", L"\u5468\u4E94", L"\u5468\u516D" };
                wchar_t buf[32];
                swprintf_s(buf, L"%s %02d:%02d", wd[local.wDayOfWeek], local.wHour, local.wMinute);
                return { buf, false };
            }
        }
        if (unixSec > 0) {
            FILETIME ft{};
            ULARGE_INTEGER uli;
            uli.QuadPart = (ULONGLONG)((double)unixSec * 1e7 + 116444736000000000ULL);
            ft.dwLowDateTime = uli.LowPart; ft.dwHighDateTime = uli.HighPart;
            SYSTEMTIME utc{}, local{};
            FileTimeToSystemTime(&ft, &utc);
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
            static const wchar_t* wd[] = { L"\u5468\u65E5", L"\u5468\u4E00", L"\u5468\u4E8C",
                L"\u5468\u4E09", L"\u5468\u56DB", L"\u5468\u4E94", L"\u5468\u516D" };
            wchar_t buf[32];
            swprintf_s(buf, L"%s %02d:%02d", wd[local.wDayOfWeek], local.wHour, local.wMinute);
            return { buf, false };
        }
        return { L"--", false };
    }

    // < 24h: show countdown
    int h = (int)(diff / 3600);
    int m = ((int)diff % 3600) / 60;
    wchar_t buf[32];
    if (h > 0)
        swprintf_s(buf, L"%dh%02dm", h, m);
    else
        swprintf_s(buf, L"%dm", m);
    return { buf, diff < 7200 }; // urgent if < 2h
}

// Unix timestamp -> weekday in Chinese
std::wstring UnixToWeekday(long long unixSec) {
    if (unixSec <= 0) return L"--";
    FILETIME ft{};
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<ULONGLONG>((double)unixSec * 10000000.0 + 116444736000000000ULL);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    static const wchar_t* weekdays[] = { L"\u5468\u65E5", L"\u5468\u4E00", L"\u5468\u4E8C",
        L"\u5468\u4E09", L"\u5468\u56DB", L"\u5468\u4E94", L"\u5468\u516D" };
    if (local.wDayOfWeek <= 6) return weekdays[local.wDayOfWeek];
    return L"--";
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
// Drawing primitives (exact mapping from HTML/SVG)
// =====================================================================
void DrawArc(Gdiplus::Graphics& g, int cx, int cy, int r, int stroke,
             int pct, COLORREF track, COLORREF fill) {
    float R = (float)r, SW = (float)stroke;
    float L = cx - R, T = cy - R, D = R * 2;
    Gdiplus::Pen tp(Gdiplus::Color(255, GetRValue(track), GetGValue(track), GetBValue(track)), SW);
    tp.SetStartCap(Gdiplus::LineCapRound);
    tp.SetEndCap(Gdiplus::LineCapRound);
    g.DrawArc(&tp, L, T, D, D, 0, 360);
    if (pct > 0 && pct <= 100) {
        Gdiplus::Pen fp(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)), SW);
        fp.SetStartCap(Gdiplus::LineCapRound);
        fp.SetEndCap(Gdiplus::LineCapRound);
        g.DrawArc(&fp, L, T, D, D, -90, pct * 3.6f);
    }
}

void DrawTextAt(Gdiplus::Graphics& g, const wchar_t* txt, float x, float y,
                const wchar_t* ff, float sz, Gdiplus::Color c, bool bold,
                Gdiplus::StringAlignment halign = Gdiplus::StringAlignmentNear) {
    HDC hdc = g.GetHDC();
    HFONT hFont = CreateFontW(-(int)sz, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, ff);
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(c.GetR(), c.GetG(), c.GetB()));
    RECT rc = { (int)x, (int)y, (int)x + 400, (int)y + (int)sz + 8 };
    DrawTextW(hdc, txt, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    g.ReleaseHDC(hdc);
}

void DrawTextCentered(Gdiplus::Graphics& g, const wchar_t* txt, int cx, int cy,
                      const wchar_t* ff, float sz, Gdiplus::Color c, bool bold) {
    HDC hdc = g.GetHDC();
    HFONT hFont = CreateFontW(-(int)sz, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, ff);
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(c.GetR(), c.GetG(), c.GetB()));
    SIZE textSize{};
    GetTextExtentPoint32W(hdc, txt, (int)wcslen(txt), &textSize);
    int dx = cx - textSize.cx / 2;
    int dy = cy - (int)sz / 2;
    RECT rc = { dx, dy, dx + textSize.cx + 4, dy + (int)sz + 4 };
    DrawTextW(hdc, txt, -1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    g.ReleaseHDC(hdc);
}

int TextWidth(Gdiplus::Graphics& g, const wchar_t* txt, const wchar_t* ff, float sz, bool bold) {
    HDC hdc = g.GetHDC();
    HFONT hFont = CreateFontW(-(int)sz, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, ff);
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SIZE textSize{};
    GetTextExtentPoint32W(hdc, txt, (int)wcslen(txt), &textSize);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
    g.ReleaseHDC(hdc);
    return textSize.cx;
}

void DrawLine(Gdiplus::Graphics& g, int x, int y1, int y2, COLORREF c) {
    Gdiplus::Pen p(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)), 1.0f);
    g.DrawLine(&p, (float)x, (float)y1, (float)x, (float)y2);
}

void DrawDot(Gdiplus::Graphics& g, int x, int y, int r, COLORREF c) {
    Gdiplus::SolidBrush b(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    g.FillEllipse(&b, x - r, y - r, r * 2, r * 2);
}

// =====================================================================
// Plugin options (persisted via registry/file)
// =====================================================================
struct PluginOptions {
    bool showPctSign = false;       // default: hide %
    bool showCredits = true;
    bool showReset = true;
    bool showSubscription = true;
    bool showStatus = true;
    bool showClaude7dReset = false; // show Claude 7d reset weekday
    bool showCodex7dReset = false;  // show Codex 7d reset weekday
    bool show7dCountdown = false;    // 7d reset countdown (穿透显示)
    std::string customSubExpiry;    // user-set date, e.g. "2026-12-20"
};

// =====================================================================
// Snapshot
// =====================================================================
struct DashData {
    int c5 = -1, c7 = -1, x7 = -1;
    std::string c5Reset;
    std::string c7Reset;
    long long x7ResetUnix = 0;   // Codex 7d reset unix timestamp
    double lastOk = 0;
    bool stale = false;
    double staleAge = 0;
    long long usedCredits = 0;
    std::string currency;
    int decPlaces = 2;
    std::string subStatus;
};

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

    int GetItemWidth() const override { return cachedW_ > 0 ? cachedW_ : 360; }

    int GetItemWidthEx(void* hDC) const override {
        // After first draw, return actual measured width
        if (cachedW_ > 0) return cachedW_;
        // Minimal initial estimate — will be corrected after first draw
        if (!hDC) return 200;
        HDC hdc = (HDC)hDC;
        int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        if (dpi < 72) dpi = 96;
        return 200 * dpi / 96;
    }

    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark) override {
        HDC hdc = (HDC)hDC;
        if (!hdc) return;
        int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        if (dpi < 72) dpi = 96;
        LM m = GetMetrics(dpi);

        DashData snap;
        PluginOptions opts;
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap = data_;
            opts = opts_;
        }

        // Transparent background — taskbar shows through
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const wchar_t* ff = L"Segoe UI";
        int cy = y + h / 2;
        int curX = x + m.padX;
        Gdiplus::Color cPct(TEXT_PCT);
        Gdiplus::Color cLabel(TEXT_LABEL);
        Gdiplus::Color cValue(TEXT_VALUE);
        Gdiplus::Color cSub(TEXT_LABEL);

        // ── Ring gauges ──
        auto ring = [&](int pct, COLORREF clr, const wchar_t* tag) {
            int rcx = curX + m.ringD / 2;
            int R = m.ringD / 2 - m.ringStroke;
            DrawArc(g, rcx, cy, R, m.ringStroke, std::max(0, pct), RING_TRACK,
                    pct < 0 ? RING_NO_DATA : clr);

            if (pct >= 0) {
                wchar_t nb[8];
                if (opts.showPctSign)
                    swprintf_s(nb, L"%d%%", pct);
                else
                    swprintf_s(nb, L"%d", pct);
                DrawTextCentered(g, nb, rcx, cy - 2, ff, (float)m.pctNumSize, cPct, true);
            } else {
                DrawTextCentered(g, L"--", rcx, cy - 2, ff, (float)m.pctNumSize,
                                 Gdiplus::Color(100, 100, 103), false);
            }

            // Label top-right, outside ring
            float lx = (float)(curX + m.ringD + m.labelOffsetX);
            float ly = (float)(cy - m.ringD / 2 + m.labelOffsetY);
            DrawTextAt(g, tag, lx, ly, ff, (float)m.labelSize,
                       Gdiplus::Color(178, 178, 178), true);
            curX += m.ringD + m.ringGap;
        };

        ring(snap.c5, GaugeColor(snap.c5, true), L"5h");
        ring(snap.c7, GaugeColor(snap.c7, true), L"7d");
        ring(snap.x7, GaugeColor(snap.x7, false), L"7d");

        // ── Separator 1 ──
        curX += m.sepMargin;
        DrawLine(g, curX, cy - m.sepH / 2, cy + m.sepH / 2, SEP_COLOR);
        curX += m.sepMargin;

        // ── Info blocks ──
        auto block = [&](const wchar_t* label, const wchar_t* value,
                         const wchar_t* sub = nullptr, COLORREF subClr = TEXT_LABEL) {
            int by = cy - h / 2 + m.infoPadTop;
            DrawTextAt(g, label, (float)curX, (float)by, ff, (float)m.infoLabelSize, cLabel, false);
            DrawTextAt(g, value, (float)curX, (float)(by + m.infoLabelSize + 3), ff,
                       (float)m.infoValueSize, cValue, true);
            if (sub) {
                DrawTextAt(g, sub, (float)curX, (float)(by + m.infoLabelSize + m.infoValueSize + 5),
                           ff, (float)m.infoSubSize, Gdiplus::Color(GetRValue(subClr), GetGValue(subClr), GetBValue(subClr)), false);
            }
            int bw = std::max(TextWidth(g, value, ff, (float)m.infoValueSize, true),
                              TextWidth(g, label, ff, (float)m.infoLabelSize, false));
            if (sub) bw = std::max(bw, TextWidth(g, sub, ff, (float)m.infoSubSize, false));
            curX += bw + m.infoGap;
        };

        if (opts.showCredits) {
            std::wstring cv = L"--";
            if (snap.usedCredits > 0) {
                double amt = snap.usedCredits / pow(10.0, snap.decPlaces);
                wchar_t cb[24];
                swprintf_s(cb, L"$%.2f", amt);
                cv = cb;
            }
            block(L"Credits", cv.c_str());
        }

        if (opts.showReset) {
            std::wstring rs = snap.c5Reset.empty() ? L"--" : IsoToLocalTimeW(snap.c5Reset);
            block(L"5h\u91CD\u7F6E", rs.c_str());
        }

        if (opts.showSubscription) {
            // Use custom date if set, otherwise use API date
            std::wstring dateStr = L"--";
            if (!opts.customSubExpiry.empty()) {
                dateStr = IsoToLocalDateW(opts.customSubExpiry);
            } else if (!snap.c7Reset.empty()) {
                dateStr = IsoToLocalDateW(snap.c7Reset);
            }
            // Status dot: red = canceled/expired, green = active
            bool isCanceled = (snap.subStatus == "canceled" || snap.subStatus == "past_due"
                            || snap.subStatus == "paused");
            bool isActive = (snap.subStatus == "active" || snap.subStatus == "trialing");
            const wchar_t* subLine = nullptr;
            COLORREF dotClr = 0;
            if (isCanceled) { dotClr = DOT_RED; }
            else if (isActive) { dotClr = DOT_GREEN; }
            // We draw the dot separately
            int by = cy - h / 2 + m.infoPadTop;
            DrawTextAt(g, L"\u8BA2\u9605", (float)curX, (float)by, ff,
                       (float)m.infoLabelSize, cLabel, false);
            // Value with dot
            int vx = curX;
            int vy = by + m.infoLabelSize + 3;
            if (dotClr) {
                DrawDot(g, vx + m.dotSize / 2, vy + m.infoValueSize / 2, m.dotSize / 2, dotClr);
                vx += m.dotSize + m.dotGap;
            }
            DrawTextAt(g, dateStr.c_str(), (float)vx, (float)vy, ff,
                       (float)m.infoValueSize, cValue, true);
            int bw = TextWidth(g, dateStr.c_str(), ff, (float)m.infoValueSize, true)
                   + (dotClr ? m.dotSize + m.dotGap : 0);
            bw = std::max(bw, TextWidth(g, L"\u8BA2\u9605", ff, (float)m.infoLabelSize, false));
            curX += bw + m.infoGap;
        }

        // ── 7d reset weekday blocks ──
        if (opts.showClaude7dReset) {
            std::wstring wd = snap.c7Reset.empty() ? L"--" : IsoToWeekday(snap.c7Reset);
            block(L"Claude\u91CD\u7F6E", wd.c_str());
        }
        if (opts.showCodex7dReset) {
            std::wstring wd = UnixToWeekday(snap.x7ResetUnix);
            block(L"Codex\u91CD\u7F6E", wd.c_str());
        }

        // ── 7d reset countdown (穿透显示) ──
        if (opts.show7dCountdown) {
            // Use Claude 7d reset if available, otherwise Codex
            auto [countdown, urgent] = Format7dCountdown(snap.c7Reset, snap.x7ResetUnix);
            if (countdown != L"--") {
                Gdiplus::Color valColor = urgent
                    ? Gdiplus::Color(STALE_CLR)  // red when < 2h
                    : cValue;
                int by = cy - h / 2 + m.infoPadTop;
                DrawTextAt(g, L"7d\u91CD\u7F6E", (float)curX, (float)by, ff,
                           (float)m.infoLabelSize, cLabel, false);
                DrawTextAt(g, countdown.c_str(), (float)curX, (float)(by + m.infoLabelSize + 3),
                           ff, (float)m.infoValueSize, valColor, true);
                int bw = std::max(TextWidth(g, countdown.c_str(), ff, (float)m.infoValueSize, true),
                                  TextWidth(g, L"7d\u91CD\u7F6E", ff, (float)m.infoLabelSize, false));
                curX += bw + m.infoGap;
            }
        }

        // ── Separator 2 + status (only show when stale) ──
        if (opts.showStatus && snap.stale && snap.staleAge > 0) {
            curX += m.sepMargin;
            DrawLine(g, curX, cy - m.sepH / 2, cy + m.sepH / 2, SEP_COLOR);
            curX += m.sepMargin;
            std::wstring st = FormatStaleAge(snap.staleAge);
            DrawTextAt(g, st.c_str(), (float)curX, (float)(cy - m.staleSize / 2),
                       ff, (float)m.staleSize, Gdiplus::Color(STALE_CLR), true);
            curX += TextWidth(g, st.c_str(), ff, (float)m.staleSize, true);
        }

        cachedW_ = curX + m.padX - x;
    }

    void SetSnapshot(const DashData& d) { std::lock_guard<std::mutex> lk(mu_); data_ = d; }
    void SetOptions(const PluginOptions& o) { std::lock_guard<std::mutex> lk(mu_); opts_ = o; }
    const PluginOptions& GetOptions() const { std::lock_guard<std::mutex> lk(mu_); return opts_; }
    DashData GetSnapshot() const { std::lock_guard<std::mutex> lk(mu_); return data_; }

private:
    mutable std::mutex mu_;
    DashData data_;
    PluginOptions opts_;
    mutable int cachedW_ = 0;
};

// =====================================================================
// Main plugin
// =====================================================================
class AIUsagePlugin final : public ITMPlugin {
public:
    static AIUsagePlugin& Instance() { static AIUsagePlugin p; return p; }

    IPluginItem* GetItem(int i) override { return i == 0 ? &dash_ : nullptr; }

    void DataRequired() override {
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (busy_) return;
            if (lastFetch_.time_since_epoch().count() && now - lastFetch_ < interval_) return;
            busy_ = true;
            lastFetch_ = now;
        }
        std::thread([this]() {
            auto claude = claude_.Fetch();
            auto codex = codex_.Fetch();
            DashData d;
            if (claude.success) {
                d.c5 = std::clamp(claude.fiveHourPercent, -1, 100);
                d.c7 = std::clamp(claude.sevenDayPercent, -1, 100);
                d.c5Reset = claude.fiveHourResetAt;
                d.c7Reset = claude.sevenDayResetAt;
                d.lastOk = claude.lastSuccessTime;
                d.usedCredits = claude.usedCredits;
                d.currency = claude.currency;
                d.decPlaces = claude.decimalPlaces;
                d.subStatus = claude.subscriptionStatus;
            }
            if (codex.success && codex.weekly.remainingPercent >= 0 && codex.weekly.remainingPercent <= 100) {
                d.x7 = 100 - codex.weekly.remainingPercent;
                d.x7ResetUnix = codex.weekly.resetAtUnixSeconds;
            }
            double t = (double)time(nullptr);
            d.stale = (!claude.success && !codex.success) || (d.lastOk > 0 && t - d.lastOk > 900);
            if (d.lastOk > 0) d.staleAge = t - d.lastOk;
            dash_.SetSnapshot(d);
            std::lock_guard<std::mutex> lk(mu_);
            tip_ = BuildTip(claude, codex);
            busy_ = false;
        }).detach();
    }

    const wchar_t* GetInfo(PluginInfoIndex i) override {
        switch (i) {
            case TMI_NAME:        return L"AI Usage";
            case TMI_DESCRIPTION: return L"Claude & Codex usage dashboard.";
            case TMI_AUTHOR:      return L"Claude+Codex";
            case TMI_COPYRIGHT:   return L"MIT";
            case TMI_VERSION:     return CODEX_USAGE_VERSION_WIDE;
            case TMI_URL:         return L"https://github.com/HCLonely/TrafficMonitor_Codex_Plugin";
            default:              return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override {
        std::lock_guard<std::mutex> lk(mu_);
        return tip_.c_str();
    }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        HWND parent = (HWND)hParent;
        PluginOptions o = dash_.GetOptions();
        DashData snap = dash_.GetSnapshot();

        // Run connectivity test
        auto conn = TestConnectivity();

        // Read proxy settings from config
        std::wstring proxyServer;
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
        if (conn.directReachable && conn.proxyReachable) {
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

        wchar_t msg[3072];
        swprintf_s(msg,
            L"AI Usage Plugin Options\n"
            L"========================================\n\n"
            L"Display Options:\n"
            L"  1. Show %% sign:          %s\n"
            L"  2. Show Credits:          %s\n"
            L"  3. Show 5h Reset:         %s\n"
            L"  4. Show Subscription:     %s\n"
            L"  5. Show Status:           %s\n"
            L"  6. Show Claude 7d Reset:  %s\n"
            L"  7. Show Codex 7d Reset:   %s\n"
            L"  8. Show 7d Countdown:     %s\n"
            L"  9. Custom Sub Expiry:     %s\n\n"
            L"Proxy Settings:\n"
            L"  Proxy Server:   %s\n"
            L"  Require Proxy:  %s\n\n"
            L"Connectivity Test:\n"
            L"  Direct:  %s\n"
            L"  Proxy:   %s\n"
            L"  Status:  %s\n\n"
            L"Data Diagnostics:\n"
            L"  Config Path:    %s\n"
            L"  Claude 5h%%:    %d\n"
            L"  Claude 7d%%:    %d\n"
            L"  Claude 7d Reset: %s\n"
            L"  Codex 7d%%:     %d\n"
            L"  Codex 7d Reset:  %s\n"
            L"  Data OK:         %s\n\n"
            L"To change settings, edit AIUsage.ini:\n"
            L"  [AIUsage]\n"
            L"  ShowPctSign=0\n"
            L"  ShowCredits=1\n"
            L"  ShowReset=1\n"
            L"  ShowSubscription=1\n"
            L"  ShowStatus=1\n"
            L"  ShowClaude7dReset=1\n"
            L"  ShowCodex7dReset=1\n"
            L"  Show7dCountdown=1\n"
            L"  CustomSubExpiry=2026-12-20\n"
            L"  ProxyServer=127.0.0.1:7890\n"
            L"  RequireProxy=0",
            o.showPctSign ? L"Yes" : L"No",
            o.showCredits ? L"Yes" : L"No",
            o.showReset ? L"Yes" : L"No",
            o.showSubscription ? L"Yes" : L"No",
            o.showStatus ? L"Yes" : L"No",
            o.showClaude7dReset ? L"Yes" : L"No",
            o.showCodex7dReset ? L"Yes" : L"No",
            o.show7dCountdown ? L"Yes" : L"No",
            o.customSubExpiry.empty() ? L"(auto)" : std::wstring(o.customSubExpiry.begin(), o.customSubExpiry.end()).c_str(),
            proxyServer.empty() ? L"(system)" : proxyServer.c_str(),
            requireProxy ? L"Yes" : L"No",
            conn.directReachable ? L"Yes" : L"No",
            conn.proxyReachable ? L"Yes" : L"No",
            connStatus.c_str(),
            iniActualPath.empty() ? L"(not set)" : iniActualPath.c_str(),
            snap.c5, snap.c7,
            c7ResetBuf,
            snap.x7,
            x7ResetBuf,
            snap.stale ? L"Stale" : L"OK");
        MessageBoxW(parent, msg, L"AI Usage Options", MB_OK | MB_ICONINFORMATION);
        return OR_OPTION_UNCHANGED;
    }

    void OnExtenedInfo(ExtendedInfoIndex idx, const wchar_t* data) override {
        if (idx == EI_CONFIG_DIR && data) {
            configDir_ = data;
            LoadConfig();
        }
    }

private:
    AIUsagePlugin() = default;

    void LoadConfig() {
        if (configDir_.empty()) return;
        std::wstring iniPath = configDir_ + L"\\AIUsage.ini";
        PluginOptions o;
        wchar_t buf[256];

        GetPrivateProfileStringW(L"AIUsage", L"ShowPctSign", L"0", buf, 256, iniPath.c_str());
        o.showPctSign = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowCredits", L"1", buf, 256, iniPath.c_str());
        o.showCredits = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowReset", L"1", buf, 256, iniPath.c_str());
        o.showReset = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowSubscription", L"1", buf, 256, iniPath.c_str());
        o.showSubscription = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowStatus", L"1", buf, 256, iniPath.c_str());
        o.showStatus = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowClaude7dReset", L"0", buf, 256, iniPath.c_str());
        o.showClaude7dReset = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"ShowCodex7dReset", L"0", buf, 256, iniPath.c_str());
        o.showCodex7dReset = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"Show7dCountdown", L"0", buf, 256, iniPath.c_str());
        o.show7dCountdown = (buf[0] == L'1');

        GetPrivateProfileStringW(L"AIUsage", L"CustomSubExpiry", L"", buf, 256, iniPath.c_str());
        // Convert wide to narrow
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            o.customSubExpiry.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &o.customSubExpiry[0], len, nullptr, nullptr);
        }

        dash_.SetOptions(o);

        // Read proxy settings
        GetPrivateProfileStringW(L"AIUsage", L"ProxyServer", L"", buf, 256, iniPath.c_str());
        std::wstring proxyServer = buf;

        GetPrivateProfileStringW(L"AIUsage", L"RequireProxy", L"0", buf, 256, iniPath.c_str());
        bool requireProxy = (buf[0] == L'1');

        proxyConfig_ = DetectProxy(proxyServer, requireProxy);
        claude_.SetProxyConfig(proxyConfig_);
        codex_.SetProxyConfig(proxyConfig_);
    }

    std::wstring BuildTip(const ClaudeUsageData& c, const UsageSnapshot& x) const {
        std::wstring t;

        // Proxy status (always show for user awareness)
        if (!proxyConfig_.statusMessage.empty()) {
            t += L"Proxy: ";
            t += proxyConfig_.statusMessage;
            t += L"\n";
        } else if (proxyConfig_.systemProxyDetected) {
            t += L"Proxy: System proxy detected\n";
        } else {
            t += L"Proxy: None (direct/auto)\n";
        }

        auto addReset = [&](const std::string& iso) -> std::wstring {
            if (iso.empty()) return L"--";
            SYSTEMTIME utc{};
            int n = sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
                &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond);
            if (n < 3) return L"--";
            SYSTEMTIME loc{};
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);
            wchar_t b[48];
            if (n >= 5)
                swprintf_s(b, L"%04d-%02d-%02d %02d:%02d", loc.wYear, loc.wMonth, loc.wDay, loc.wHour, loc.wMinute);
            else
                swprintf_s(b, L"%04d-%02d-%02d", loc.wYear, loc.wMonth, loc.wDay);
            return b;
        };
        if (c.success) {
            auto line = [&](const wchar_t* n, int pct, const std::string& iso) {
                t += n; t += L": ";
                if (pct >= 0) {
                    t += std::to_wstring(pct) + L"% used";
                    auto r = addReset(iso);
                    if (r != L"--") t += L", resets at " + r;
                } else t += L"--";
                t += L"\n";
            };
            line(L"Claude 5h", c.fiveHourPercent, c.fiveHourResetAt);
            line(L"Claude 7d", c.sevenDayPercent, c.sevenDayResetAt);
            if (c.usedCredits > 0) {
                double a = c.usedCredits / pow(10.0, c.decimalPlaces);
                wchar_t b[32]; swprintf_s(b, L"Credits: $%.2f\n", a);
                t += b;
            }
            if (!c.subscriptionStatus.empty()) {
                t += L"Subscription: " + ToWide(c.subscriptionStatus) + L"\n";
            }
            if (!c.errorMessage.empty()) t += L"Claude: " + c.errorMessage + L"\n";
        } else {
            t += L"Claude: ";
            t += c.errorMessage.empty() ? L"waiting" : c.errorMessage;
            t += L"\n";
        }
        if (x.success) {
            auto cx = [&](const UsageWindow& w, const wchar_t* n, bool date) {
                if (w.remainingPercent < 0) { t += std::wstring(n) + L": --\n"; return; }
                t += std::wstring(n) + L": " + std::to_wstring(100 - w.remainingPercent) + L"% used";
                if (date && w.resetAtUnixSeconds > 0) {
                    FILETIME ft{};
                    ULARGE_INTEGER ul;
                    ul.QuadPart = (ULONGLONG)((double)w.resetAtUnixSeconds * 1e7 + 116444736000000000ULL);
                    ft.dwLowDateTime = ul.LowPart; ft.dwHighDateTime = ul.HighPart;
                    SYSTEMTIME u{}, l{};
                    FileTimeToSystemTime(&ft, &u);
                    SystemTimeToTzSpecificLocalTime(nullptr, &u, &l);
                    wchar_t b[48];
                    swprintf_s(b, L", resets at %04d-%02d-%02d %02d:%02d", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute);
                    t += b;
                } else if (w.resetAfterSeconds > 0) {
                    wchar_t b[32];
                    swprintf_s(b, L", in %dh%dm", w.resetAfterSeconds / 3600, (w.resetAfterSeconds % 3600) / 60);
                    t += b;
                }
                t += L"\n";
            };
            cx(x.fiveHour, L"Codex 5h", false);
            cx(x.weekly, L"Codex 7d", true);
            if (!x.errorMessage.empty()) t += L"Codex: " + x.errorMessage + L"\n";
        } else {
            t += L"Codex: "; t += x.errorMessage.empty() ? L"waiting" : x.errorMessage; t += L"\n";
        }
        return t;
    }

    AIDashboardItem dash_;
    ClaudeUsageFetcher claude_;
    CodexUsageFetcher codex_;
    ProxyConfig proxyConfig_;
    std::wstring configDir_;
    mutable std::mutex mu_;
    std::wstring tip_ = L"AI Usage: waiting for data";
    bool busy_ = false;
    std::chrono::steady_clock::time_point lastFetch_{};
    static constexpr std::chrono::minutes interval_{1};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) InitGdiplus();
    if (reason == DLL_PROCESS_DETACH) ShutdownGdiplus();
    return TRUE;
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &AIUsagePlugin::Instance();
}
