#pragma once

#include <Windows.h>

#include <memory>

// Small, taskbar-scale renderer used by the dashboard. Direct2D/DirectWrite is
// preferred so fractional font sizes and vector strokes survive the
// TrafficMonitor HDC interop path. Every operation has a GDI/GDI+ fallback for
// older Windows installations or an unavailable Direct2D runtime.
class DashboardRenderer final {
public:
    DashboardRenderer(HDC hdc, const RECT& bounds, int dpi);
    ~DashboardRenderer();

    DashboardRenderer(const DashboardRenderer&) = delete;
    DashboardRenderer& operator=(const DashboardRenderer&) = delete;

    bool UsesDirectWrite() const;

    static float MeasureTextWidth(
        HDC hdc,
        const wchar_t* text,
        const wchar_t* fontFamily,
        float fontSizePx,
        bool mediumWeight);

    // Distance from the top of a text layout box down to the baseline. The
    // taskbar slot is only 32 px tall, so bands are sized from real font
    // metrics instead of guessed multiples of the em size.
    static float MeasureBaseline(
        HDC hdc,
        const wchar_t* fontFamily,
        float fontSizePx,
        bool mediumWeight);

    static float MeasureLineHeight(
        HDC hdc,
        const wchar_t* fontFamily,
        float fontSizePx,
        bool mediumWeight);

    float TextWidth(
        const wchar_t* text,
        const wchar_t* fontFamily,
        float fontSizePx,
        bool mediumWeight) const;

    void DrawArc(
        float centerX,
        float centerY,
        float radius,
        float strokeWidth,
        int percent,
        COLORREF trackColor,
        COLORREF valueColor);

    void DrawTextAt(
        const wchar_t* text,
        float x,
        float y,
        const wchar_t* fontFamily,
        float fontSizePx,
        COLORREF color,
        bool mediumWeight);

    void DrawGradientTextAt(
        const wchar_t* text,
        float x,
        float y,
        const wchar_t* fontFamily,
        float fontSizePx,
        bool mediumWeight,
        bool darkMode);

    void DrawTextCentered(
        const wchar_t* text,
        float centerX,
        float centerY,
        const wchar_t* fontFamily,
        float fontSizePx,
        COLORREF color,
        bool mediumWeight);

    void DrawLine(
        float x1,
        float y1,
        float x2,
        float y2,
        COLORREF color,
        float strokeWidth = 1.0f);

    void DrawFreshnessUnderline(
        float centerX,
        float y,
        COLORREF color,
        int dpi);

    void DrawDot(
        float centerX,
        float centerY,
        float radius,
        COLORREF color);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
