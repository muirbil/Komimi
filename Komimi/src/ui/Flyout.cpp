#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <ShellScalingApi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "ui/Flyout.h"
#include "app/Localization.h"
#include "ui/Theme.h"

using BudsConstants::Xiaomi::Buds6Lite::AncModes;

namespace {
    constexpr wchar_t kWindowClass[] = L"KomimiFlyout";

    // Layout in DIPs (1 DIP = 1 pixel at 100% scale)
    constexpr float kWidth = 340.f;
    constexpr float kPadding = 16.f;
    constexpr float kHeaderTop = 14.f, kHeaderHeight = 24.f;
    constexpr float kBatteryTop = 50.f, kBatteryHeight = 64.f;
    constexpr float kSectionTop = 128.f, kSectionHeight = 18.f;
    constexpr float kSelectorTop = 152.f, kSelectorHeight = 38.f, kSelectorInset = 3.f;
    constexpr float kLevelSectionTop = 202.f;
    constexpr float kLevelTop = 226.f, kLevelChipHeight = 30.f, kLevelGap = 6.f;
    constexpr int kLevelRows = 2; // room for two rows of chips is always kept, so the window height does not jump
    constexpr float kLevelBottom = kLevelTop + kLevelRows * kLevelChipHeight + (kLevelRows - 1) * kLevelGap;
    constexpr float kEqSectionTop = kLevelBottom + 12.f;
    constexpr float kEqTop = kEqSectionTop + 24.f;
    constexpr float kHeight = kEqTop + kSelectorHeight + kPadding;
    constexpr float kScreenMargin = 12.f; // gap to the taskbar and the screen edge

    // If the window hid because of a click on the tray icon, that same click must not reopen it
    constexpr ULONGLONG kReopenGuardMs = 400;

    // Status dot animation
    constexpr UINT_PTR kAnimationTimerId = 1;
    constexpr UINT kAnimationFrameMs = 16;     // ~60 frames/s
    constexpr float kPulsePeriodMs = 1800.f;   // Connected: an expanding ring
    constexpr float kBlinkPeriodMs = 1100.f;   // Connecting: blinking

    // Selector segments: the protocol value and the label share an index
    constexpr std::array kAncModes{ AncModes::Off, AncModes::NoiseCancelling, AncModes::Transparency };
    constexpr std::array kAncLabels{ Str::AncOff, Str::AncOn, Str::AncTransparency };

    constexpr auto& kEqPresets = XiaomiMma::Buds6LitePresets;
    constexpr std::array kEqLabels{ Str::EqBalanced, Str::EqVocal, Str::EqBass, Str::EqTreble, Str::EqCustom };
    static_assert(kEqPresets.size() == kEqLabels.size());

    // Index of a value among the segments; nullopt = no value, or it is not in the list
    template <typename T, size_t N>
    std::optional<size_t> IndexOf(std::array<T, N> const& values, std::optional<T> value) {
        if (!value) return std::nullopt;
        auto it = std::find(values.begin(), values.end(), *value);
        if (it == values.end()) return std::nullopt;
        return static_cast<size_t>(it - values.begin());
    }

    // Level labels; the index is the level value in the protocol (XiaomiMma::NoiseCancellingLevel / TransparencyLevel)
    constexpr std::array kNoiseCancellingLevels{
        Str::AncLevelBalanced, Str::AncLevelLight, Str::AncLevelDeep,
        Str::AncLevelAdaptive, Str::AncLevelAntiWind, Str::AncLevelDeepPlus,
    };
    constexpr std::array kTransparencyLevels{
        Str::TransparencyLevelNormal, Str::TransparencyLevelVoice,
        Str::TransparencyLevelAmbient, Str::TransparencyLevelVoicePlus,
    };

    // Levels available in the given mode (empty for Off)
    std::span<Str const> LevelsFor(std::optional<AncModes> mode) {
        if (mode == AncModes::NoiseCancelling) return kNoiseCancellingLevels;
        if (mode == AncModes::Transparency) return kTransparencyLevels;
        return {};
    }

    // 6 levels = 3×2 grid, 4 levels = 2×2
    D2D1_RECT_F LevelChipRect(size_t index, size_t count) {
        size_t columns = count == 4 ? 2 : 3;
        float width = (kWidth - 2 * kPadding - (columns - 1) * kLevelGap) / columns;
        float left = kPadding + (index % columns) * (width + kLevelGap);
        float top = kLevelTop + (index / columns) * (kLevelChipHeight + kLevelGap);
        return D2D1::RectF(left, top, left + width, top + kLevelChipHeight);
    }

    D2D1_COLOR_F Rgb(UINT32 rgb, float alpha = 1.f) { return D2D1::ColorF(rgb, alpha); }

    D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
        color.a *= alpha;
        return color;
    }

    D2D1_RECT_F SegmentRect(float top, size_t index, size_t count) {
        float left = kPadding + kSelectorInset;
        float width = (kWidth - 2 * kPadding - 2 * kSelectorInset) / count;
        return D2D1::RectF(left + width * index, top + kSelectorInset,
            left + width * (index + 1), top + kSelectorHeight - kSelectorInset);
    }

    bool Contains(D2D1_RECT_F const& rect, float x, float y) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }
}

Flyout::~Flyout() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool Flyout::Create(HINSTANCE instance, Callbacks callbacks) {
    m_callbacks = std::move(callbacks);
    CreateDeviceIndependentResources();
    LoadPalette();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass,
        L"Komimi", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!m_hwnd) return false;

    ApplyWindowTheme();
    return true;
}

// ---------------------------------------------------------------- show / hide

bool Flyout::IsVisible() const {
    return IsWindowVisible(m_hwnd);
}

void Flyout::Toggle(RECT const& anchor) {
    if (IsVisible()) {
        Hide();
    }
    else if (GetTickCount64() - m_lastHideTick > kReopenGuardMs) {
        Show(anchor);
    }
}

void Flyout::Show(RECT const& anchor) {
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);

    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    SetDpi(dpiX);

    int width = Px(kWidth);
    int height = Px(kHeight);
    int margin = Px(kScreenMargin);
    RECT const& work = mi.rcWork;
    RECT const& screen = mi.rcMonitor;
    int centerX = (anchor.left + anchor.right) / 2;
    int centerY = (anchor.top + anchor.bottom) / 2;

    // The side comes from the icon itself: the monitor edge nearest to it. The work area cannot be
    // used: third-party bars (YASB etc.) shrink it, while an auto-hidden taskbar does not.
    int distances[4] = {
        centerX - static_cast<int>(screen.left),   // ABE_LEFT
        centerY - static_cast<int>(screen.top),    // ABE_TOP
        static_cast<int>(screen.right) - centerX,  // ABE_RIGHT
        static_cast<int>(screen.bottom) - centerY, // ABE_BOTTOM
    };
    UINT edge = static_cast<UINT>(std::min_element(std::begin(distances), std::end(distances)) - std::begin(distances));

    // Keep clear of the taskbar (even auto-hidden, like system flyouts), otherwise of the icon
    RECT bar = anchor;
    APPBARDATA taskbar{ sizeof(taskbar) };
    RECT overlap{};
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &taskbar) && taskbar.uEdge == edge &&
        IntersectRect(&overlap, &taskbar.rc, &screen)) {
        bar = taskbar.rc;
    }

    int x = centerX - width / 2;
    int y = centerY - height / 2;
    switch (edge) {
    case ABE_LEFT:   x = std::max(bar.right, work.left) + margin; break;
    case ABE_TOP:    y = std::max(bar.bottom, work.top) + margin; break;
    case ABE_RIGHT:  x = std::min(bar.left, work.right) - width - margin; break;
    case ABE_BOTTOM: y = std::min(bar.top, work.bottom) - height - margin; break;
    }
    x = std::clamp(x, static_cast<int>(work.left) + margin, std::max<int>(work.left + margin, work.right - width - margin));
    y = std::clamp(y, static_cast<int>(work.top) + margin, std::max<int>(work.top + margin, work.bottom - height - margin));

    m_hover = HitNone;
    m_pressed = HitNone;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(m_hwnd);
    UpdateAnimation();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Flyout::Hide() {
    if (!IsVisible()) return;
    ShowWindow(m_hwnd, SW_HIDE);
    m_lastHideTick = GetTickCount64();
    UpdateAnimation();
}

void Flyout::SetState(EarbudsState const& state) {
    m_state = state;
    UpdateAnimation();
    if (IsVisible()) InvalidateRect(m_hwnd, nullptr, FALSE);
}

// The frame timer only runs while the window is open and the dot has something to show,
// and stays off if animations are disabled in Windows (Accessibility → Visual effects)
void Flyout::UpdateAnimation() {
    BOOL animationsEnabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animationsEnabled, 0);
    bool want = IsVisible() && animationsEnabled &&
        (m_state.link == LinkState::Connected || m_state.link == LinkState::Connecting);
    if (want == m_animating) return;

    m_animating = want;
    if (want) {
        m_animationStart = AnimationClock::now();
        SetTimer(m_hwnd, kAnimationTimerId, kAnimationFrameMs, nullptr);
    }
    else {
        KillTimer(m_hwnd, kAnimationTimerId);
    }
}

// ---------------------------------------------------------------- theme and resources

void Flyout::LoadPalette() {
    Palette p;
    p.dark = !SystemUsesLightTheme();
    if (p.dark) {
        p.background = Rgb(0x2B2B2B);
        p.card = Rgb(0x343434);
        p.border = Rgb(0x404040);
        p.hover = Rgb(0x3E3E3E);
        p.text = Rgb(0xFFFFFF);
        p.textSecondary = Rgb(0xFFFFFF, 0.65f);
        p.textDisabled = Rgb(0xFFFFFF, 0.36f);
        p.onAccent = Rgb(0x000000);
        p.good = Rgb(0x6CCB5F);
        p.warning = Rgb(0xFCE100);
        p.bad = Rgb(0xFF99A4);
    }
    else {
        p.background = Rgb(0xF3F3F3);
        p.card = Rgb(0xFBFBFB);
        p.border = Rgb(0xE2E2E2);
        p.hover = Rgb(0xEAEAEA);
        p.text = Rgb(0x1B1B1B);
        p.textSecondary = Rgb(0x000000, 0.62f);
        p.textDisabled = Rgb(0x000000, 0.36f);
        p.onAccent = Rgb(0xFFFFFF);
        p.good = Rgb(0x0F7B0F);
        p.warning = Rgb(0x9D5D00);
        p.bad = Rgb(0xC42B1C);
    }

    // System accent colour; Windows 11 uses a light shade in the dark theme and a dark one in the light theme
    p.accent = p.dark ? Rgb(0x99EBFF) : Rgb(0x005FB8);
    try {
        using winrt::Windows::UI::ViewManagement::UIColorType;
        auto color = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            p.dark ? UIColorType::AccentLight2 : UIColorType::AccentDark1);
        p.accent = D2D1::ColorF(color.R / 255.f, color.G / 255.f, color.B / 255.f);
    }
    catch (winrt::hresult_error const&) {
    }

    m_palette = p;
}

void Flyout::ApplyWindowTheme() {
    BOOL dark = m_palette.dark;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    auto const& b = m_palette.border;
    COLORREF border = RGB(b.r * 255, b.g * 255, b.b * 255);
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

void Flyout::CreateDeviceIndependentResources() {
    winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put()));
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));
    CreateTextFormats();
}

void Flyout::OnLanguageChanged() {
    CreateTextFormats();
    if (IsVisible()) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void Flyout::CreateTextFormats() {
    // Segoe UI Variable is the Windows 11 UI font; Windows 10 does not have it.
    // Characters missing from it (Japanese) come from the locale's fallback font in DirectWrite.
    wchar_t const* family = L"Segoe UI";
    winrt::com_ptr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(m_dwriteFactory->GetSystemFontCollection(fonts.put()))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists);
        if (exists) family = L"Segoe UI Variable Text";
    }

    auto make = [&](winrt::com_ptr<IDWriteTextFormat>& format, DWRITE_FONT_WEIGHT weight, float size,
                    DWRITE_TEXT_ALIGNMENT alignment) {
        format = nullptr;
        winrt::check_hresult(m_dwriteFactory->CreateTextFormat(family, nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, Localization::LocaleName(), format.put()));
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    make(m_titleFormat, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.f, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(m_bodyFormat, DWRITE_FONT_WEIGHT_NORMAL, 12.f, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(m_bodyRightFormat, DWRITE_FONT_WEIGHT_NORMAL, 12.f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    make(m_captionFormat, DWRITE_FONT_WEIGHT_NORMAL, 11.f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(m_valueFormat, DWRITE_FONT_WEIGHT_SEMI_BOLD, 18.f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(m_segmentFormat, DWRITE_FONT_WEIGHT_NORMAL, 12.f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(m_segmentSelectedFormat, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.f, DWRITE_TEXT_ALIGNMENT_CENTER);

    // Trim a long title with an ellipsis instead of running into the status
    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    m_dwriteFactory->CreateEllipsisTrimmingSign(m_titleFormat.get(), ellipsis.put());
    m_titleFormat->SetTrimming(&trimming, ellipsis.get());
}

void Flyout::CreateDeviceResources() {
    if (m_target) return;

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(),
        static_cast<float>(m_dpi), static_cast<float>(m_dpi));
    auto hwndProps = D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(rc.right, rc.bottom));
    if (FAILED(m_d2dFactory->CreateHwndRenderTarget(props, hwndProps, m_target.put()))) return;
    m_target->CreateSolidColorBrush(m_palette.text, m_brush.put());
}

void Flyout::SetDpi(UINT dpi) {
    m_dpi = dpi;
    // We draw in DIPs, Direct2D converts them to pixels
    if (m_target) m_target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
}

int Flyout::Px(float dip) const {
    return static_cast<int>(dip * m_dpi / 96.f + 0.5f);
}

// ---------------------------------------------------------------- painting

void Flyout::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(m_hwnd, &ps);

    CreateDeviceResources();
    if (m_target) {
        m_target->BeginDraw();
        m_target->Clear(m_palette.background);
        DrawHeader();
        DrawBattery();
        DrawAncSelector();
        DrawLevelSelector();
        DrawEqSelector();
        if (m_target->EndDraw() == D2DERR_RECREATE_TARGET) {
            m_brush = nullptr;
            m_target = nullptr;
        }
    }

    EndPaint(m_hwnd, &ps);
}

void Flyout::DrawHeader() {
    std::wstring_view statusText;
    D2D1_COLOR_F dotColor{};
    switch (m_state.link) {
    case LinkState::Connected:
        statusText = Tr(Str::StatusConnected);
        dotColor = m_palette.good;
        break;
    case LinkState::Connecting:
        statusText = Tr(Str::StatusConnecting);
        dotColor = m_palette.warning;
        break;
    case LinkState::NotFound:
        statusText = Tr(Str::StatusNotFoundRetry);
        dotColor = m_palette.bad;
        break;
    case LinkState::Disconnected:
        statusText = Tr(Str::StatusDisconnectedRetry);
        dotColor = m_palette.textDisabled;
        break;
    }

    // The status is right-aligned, so measure the text width first
    winrt::com_ptr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(statusText.data(), static_cast<UINT32>(statusText.size()),
        m_bodyFormat.get(), kWidth, kHeaderHeight, layout.put());
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    float right = kWidth - kPadding;
    float textLeft = right - metrics.widthIncludingTrailingWhitespace;
    float dotX = textLeft - 10.f;
    float centerY = kHeaderTop + kHeaderHeight / 2;
    m_statusRect = D2D1::RectF(dotX - 8.f, kHeaderTop, right, kHeaderTop + kHeaderHeight);

    auto dotCenter = D2D1::Point2F(dotX, centerY);
    float dotAlpha = 1.f;
    if (m_animating) {
        float elapsedMs = std::chrono::duration<float, std::milli>(AnimationClock::now() - m_animationStart).count();
        if (m_state.link == LinkState::Connected) {
            // Pulse: a ring expands from the dot and fades
            float t = std::fmod(elapsedMs / kPulsePeriodMs, 1.f);
            float eased = 1.f - (1.f - t) * (1.f - t) * (1.f - t);
            float radius = 4.f + 6.f * eased;
            m_brush->SetColor(WithAlpha(dotColor, 0.5f * (1.f - eased)));
            m_target->FillEllipse(D2D1::Ellipse(dotCenter, radius, radius), m_brush.get());
        }
        else {
            // Connecting: the dot blinks smoothly
            float t = std::fmod(elapsedMs / kBlinkPeriodMs, 1.f);
            dotAlpha = 0.35f + 0.65f * (0.5f + 0.5f * std::cos(2.f * 3.14159265f * t));
        }
    }
    m_brush->SetColor(WithAlpha(dotColor, dotAlpha));
    m_target->FillEllipse(D2D1::Ellipse(dotCenter, 4.f, 4.f), m_brush.get());

    bool hot = m_hover == HitStatus && IsEnabled(HitStatus);
    m_brush->SetColor(hot ? m_palette.accent : m_palette.textSecondary);
    m_target->DrawTextLayout(D2D1::Point2F(textLeft, kHeaderTop), layout.get(), m_brush.get());

    DrawLabel(BudsConstants::Xiaomi::Buds6Lite::DisplayName, m_titleFormat.get(),
        D2D1::RectF(kPadding, kHeaderTop, m_statusRect.left - 8.f, kHeaderTop + kHeaderHeight), m_palette.text);
}

void Flyout::DrawBattery() {
    auto card = D2D1::RectF(kPadding, kBatteryTop, kWidth - kPadding, kBatteryTop + kBatteryHeight);
    FillRound(card, 6.f, m_palette.card);
    StrokeRound(card, 6.f, m_palette.border);

    struct Column {
        Str caption;
        XiaomiMma::BatteryLevel level;
    };
    std::array columns{
        Column{ Str::BatteryLeft, m_state.battery.left },
        Column{ Str::BatteryCase, m_state.battery.caseBox }, // the case goes in the middle, between the earbuds
        Column{ Str::BatteryRight, m_state.battery.right },
    };

    float columnWidth = (card.right - card.left) / columns.size();
    for (size_t i = 0; i < columns.size(); ++i) {
        float left = card.left + columnWidth * i;
        float right = left + columnWidth;

        if (i > 0) {
            m_brush->SetColor(m_palette.border);
            m_target->DrawLine(D2D1::Point2F(left, card.top + 14.f), D2D1::Point2F(left, card.bottom - 14.f), m_brush.get());
        }

        DrawLabel(Tr(columns[i].caption), m_captionFormat.get(),
            D2D1::RectF(left, card.top + 8.f, right, card.top + 24.f), m_palette.textSecondary);

        auto const& level = columns[i].level;
        auto const& value = level.percent;
        std::wstring text = !value ? L"—"
            : level.charging ? std::format(L"{}% ⚡", *value) // ⚡ is drawn in the brush colour, not as an emoji
            : std::format(L"{}%", *value);
        D2D1_COLOR_F color = !value ? m_palette.textDisabled
            : level.charging ? m_palette.good
            : *value <= 15 ? m_palette.bad
            : m_palette.text;
        DrawLabel(text, m_valueFormat.get(), D2D1::RectF(left, card.top + 26.f, right, card.bottom - 8.f), color);
    }
}

void Flyout::DrawAncSelector() {
    DrawSectionHeader(kSectionTop, Str::NoiseControl, m_state.error == EngineError::AncWriteFailed, Str::ErrorAncWrite);
    DrawSegmented(kSelectorTop, kAncLabels, IndexOf(kAncModes, m_state.anc), HitAncBase, m_state.ancPending);
}

void Flyout::DrawEqSelector() {
    DrawSectionHeader(kEqSectionTop, Str::Equalizer, m_state.error == EngineError::EqWriteFailed, Str::ErrorEqWrite);
    DrawSegmented(kEqTop, kEqLabels, IndexOf(kEqPresets, m_state.eq), HitEqBase, m_state.eqPending);
}

void Flyout::DrawSectionHeader(float top, Str title, bool showError, Str error) {
    DrawLabel(Tr(title), m_bodyFormat.get(),
        D2D1::RectF(kPadding + 2.f, top, kWidth - kPadding, top + kSectionHeight), m_palette.textSecondary);
    if (showError) {
        DrawLabel(Tr(error), m_bodyRightFormat.get(),
            D2D1::RectF(kPadding, top, kWidth - kPadding - 2.f, top + kSectionHeight), m_palette.bad);
    }
}

void Flyout::DrawSegmented(float top, std::span<Str const> labels, std::optional<size_t> selected, int hitBase, bool pending) {
    auto track = D2D1::RectF(kPadding, top, kWidth - kPadding, top + kSelectorHeight);
    FillRound(track, 6.f, m_palette.card);
    StrokeRound(track, 6.f, m_palette.border);

    bool connected = m_state.link == LinkState::Connected;
    for (size_t i = 0; i < labels.size(); ++i) {
        auto rect = SegmentRect(top, i, labels.size());
        int hit = hitBase + static_cast<int>(i);
        bool isSelected = selected == i;
        bool enabled = IsEnabled(hit);

        D2D1_COLOR_F textColor = enabled ? m_palette.text : m_palette.textDisabled;
        if (isSelected) {
            // The accent is translucent while the command is in flight
            D2D1_COLOR_F fill = !connected ? m_palette.border
                : pending ? WithAlpha(m_palette.accent, 0.55f)
                : m_palette.accent;
            FillRound(rect, 4.f, fill);
            textColor = connected ? m_palette.onAccent : m_palette.textDisabled;
        }
        else if (enabled && m_hover == hit) {
            FillRound(rect, 4.f, m_palette.hover);
        }

        DrawLabel(Tr(labels[i]), isSelected ? m_segmentSelectedFormat.get() : m_segmentFormat.get(), rect, textColor);
    }
}

void Flyout::DrawLevelSelector() {
    // Without a link the mode is unknown: leave the block empty rather than show stale data
    if (m_state.link != LinkState::Connected || !m_state.anc) return;

    auto mode = *m_state.anc;
    auto levels = LevelsFor(mode);
    Str header = mode == AncModes::NoiseCancelling ? Str::LevelHeaderNoiseCancelling
        : mode == AncModes::Transparency ? Str::LevelHeaderTransparency
        : Str::LevelHeaderGeneric;
    DrawLabel(Tr(header), m_bodyFormat.get(),
        D2D1::RectF(kPadding + 2.f, kLevelSectionTop, kWidth - kPadding, kLevelSectionTop + kSectionHeight), m_palette.textSecondary);

    if (levels.empty()) {
        auto area = D2D1::RectF(kPadding, kLevelTop, kWidth - kPadding, kLevelBottom);
        DrawLabel(Tr(Str::LevelUnavailableWhenOff), m_captionFormat.get(), area, m_palette.textDisabled);
        return;
    }

    auto current = m_state.ancLevels[static_cast<size_t>(mode)];
    for (size_t i = 0; i < levels.size(); ++i) {
        auto rect = LevelChipRect(i, levels.size());
        bool selected = current == i;
        bool enabled = IsEnabled(HitLevelBase + static_cast<int>(i));

        D2D1_COLOR_F textColor = enabled ? m_palette.text : m_palette.textDisabled;
        if (selected) {
            FillRound(rect, 4.f, m_state.ancPending ? WithAlpha(m_palette.accent, 0.55f) : m_palette.accent);
            textColor = m_palette.onAccent;
        }
        else {
            FillRound(rect, 4.f, enabled && m_hover == HitLevelBase + static_cast<int>(i) ? m_palette.hover : m_palette.card);
            StrokeRound(rect, 4.f, m_palette.border);
        }
        DrawLabel(Tr(levels[i]), selected ? m_segmentSelectedFormat.get() : m_segmentFormat.get(), rect, textColor);
    }
}

void Flyout::DrawLabel(std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F const& rect, D2D1_COLOR_F const& color) {
    m_brush->SetColor(color);
    m_target->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, m_brush.get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Flyout::FillRound(D2D1_RECT_F const& rect, float radius, D2D1_COLOR_F const& color) {
    m_brush->SetColor(color);
    m_target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_brush.get());
}

void Flyout::StrokeRound(D2D1_RECT_F const& rect, float radius, D2D1_COLOR_F const& color) {
    // Half-pixel offset so a one-pixel line does not blur across two
    float half = 0.5f * 96.f / m_dpi;
    auto inner = D2D1::RectF(rect.left + half, rect.top + half, rect.right - half, rect.bottom - half);
    m_brush->SetColor(color);
    m_target->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), m_brush.get(), 96.f / m_dpi);
}

// ---------------------------------------------------------------- input

int Flyout::HitTest(LPARAM lParam) const {
    float x = GET_X_LPARAM(lParam) * 96.f / m_dpi;
    float y = GET_Y_LPARAM(lParam) * 96.f / m_dpi;
    for (size_t i = 0; i < kAncModes.size(); ++i) {
        if (Contains(SegmentRect(kSelectorTop, i, kAncModes.size()), x, y)) return HitAncBase + static_cast<int>(i);
    }
    for (size_t i = 0; i < kEqPresets.size(); ++i) {
        if (Contains(SegmentRect(kEqTop, i, kEqPresets.size()), x, y)) return HitEqBase + static_cast<int>(i);
    }
    if (Contains(m_statusRect, x, y)) return HitStatus;
    auto levels = LevelsFor(m_state.anc);
    for (size_t i = 0; i < levels.size(); ++i) {
        if (Contains(LevelChipRect(i, levels.size()), x, y)) return HitLevelBase + static_cast<int>(i);
    }
    return HitNone;
}

bool Flyout::IsEnabled(int hit) const {
    bool connected = m_state.link == LinkState::Connected;
    auto inRange = [hit](int base, size_t count) { return hit >= base && hit < base + static_cast<int>(count); };

    if (inRange(HitAncBase, kAncModes.size())) return connected && !m_state.ancPending;
    if (inRange(HitLevelBase, LevelsFor(m_state.anc).size())) return connected && !m_state.ancPending;
    if (inRange(HitEqBase, kEqPresets.size())) return connected && !m_state.eqPending;
    if (hit == HitStatus) return m_state.link == LinkState::Disconnected || m_state.link == LinkState::NotFound;
    return false;
}

void Flyout::Activate(int hit) {
    if (!IsEnabled(hit)) return;
    if (hit == HitStatus) {
        if (m_callbacks.onReconnect) m_callbacks.onReconnect();
    }
    else if (hit >= HitEqBase) {
        if (m_callbacks.onEqSelected) m_callbacks.onEqSelected(kEqPresets[hit - HitEqBase]);
    }
    else if (hit >= HitLevelBase) {
        if (m_callbacks.onAncLevelSelected) m_callbacks.onAncLevelSelected(static_cast<uint8_t>(hit - HitLevelBase));
    }
    else if (m_callbacks.onAncSelected) {
        m_callbacks.onAncSelected(kAncModes[hit - HitAncBase]);
    }
}

void Flyout::SetHover(int hit) {
    if (m_hover == hit) return;
    m_hover = hit;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- messages

LRESULT CALLBACK Flyout::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<Flyout*>(create->lpCreateParams)->m_hwnd = hwnd;
    }
    auto self = reinterpret_cast<Flyout*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Flyout::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        Paint();
        return 0;

    case WM_TIMER:
        if (wParam == kAnimationTimerId) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (m_target) m_target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        return 0;

    case WM_DPICHANGED:
        // Show() computes the size and position itself, the rectangle suggested by the system is not needed
        SetDpi(HIWORD(wParam));
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) Hide();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) Hide();
        return 0;

    case WM_MOUSEMOVE:
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_trackingMouse = TrackMouseEvent(&tme);
        }
        SetHover(HitTest(lParam));
        return 0;

    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        SetHover(HitNone);
        return 0;

    case WM_LBUTTONDOWN:
        m_pressed = HitTest(lParam);
        return 0;

    case WM_LBUTTONUP: {
        int hit = HitTest(lParam);
        if (hit != HitNone && hit == m_pressed) Activate(hit);
        m_pressed = HitNone;
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            // WM_SETCURSOR arrives before WM_MOUSEMOVE, so m_hover is still stale here
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(m_hwnd, &pt);
            SetCursor(LoadCursorW(nullptr, IsEnabled(HitTest(MAKELPARAM(pt.x, pt.y))) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_SETTINGCHANGE:
        if (!IsThemeChangeMessage(lParam)) break;
        [[fallthrough]];
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        LoadPalette();
        ApplyWindowTheme();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}
