#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <winrt/base.h>
#include "app/Localization.h"
#include "device/Engine.h"

// Popup window next to the tray icon (like the Windows 11 system flyouts):
// borderless, rounded corners, closes on focus loss or Esc.
// Drawn by hand with Direct2D/DirectWrite: there are few elements, a GUI framework is not needed.
class Flyout {
public:
    struct Callbacks {
        std::function<void(BudsConstants::Xiaomi::Buds6Lite::AncModes)> onAncSelected;
        std::function<void(uint8_t level)> onAncLevelSelected; // level for the current mode
        std::function<void(XiaomiMma::EqPreset)> onEqSelected;
        std::function<void()> onReconnect;
    };

    Flyout() = default;
    ~Flyout();
    Flyout(Flyout const&) = delete;
    Flyout& operator=(Flyout const&) = delete;

    bool Create(HINSTANCE instance, Callbacks callbacks);

    void Toggle(RECT const& anchor); // anchor = the tray icon rectangle (screen pixels)
    void Show(RECT const& anchor);
    void Hide();
    bool IsVisible() const;

    void SetState(EarbudsState const& state);
    void OnLanguageChanged(); // recreates the fonts for the new language's locale

private:
    struct Palette {
        bool dark = true;
        D2D1_COLOR_F background, card, border, hover;
        D2D1_COLOR_F text, textSecondary, textDisabled;
        D2D1_COLOR_F accent, onAccent;
        D2D1_COLOR_F good, warning, bad;
    };

    // Clickable areas: HitAncBase + i = ANC mode segment, HitStatus = status,
    // HitLevelBase + i = chip of level i for the current mode, HitEqBase + i = EQ preset
    enum Hit : int { HitNone = -1, HitAncBase = 0, HitStatus = 3, HitLevelBase = 10, HitEqBase = 20 };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void LoadPalette();
    void ApplyWindowTheme();
    void CreateDeviceIndependentResources();
    void CreateTextFormats();
    void CreateDeviceResources();
    void SetDpi(UINT dpi);
    int Px(float dip) const;

    void Paint();
    void DrawHeader();
    void DrawBattery();
    void DrawAncSelector();
    void DrawLevelSelector();
    void DrawEqSelector();
    void DrawSectionHeader(float top, Str title, bool showError, Str error);
    // Segmented control: a track with segments, the selected one is filled with the accent colour
    void DrawSegmented(float top, std::span<Str const> labels, std::optional<size_t> selected, int hitBase, bool pending);
    void DrawLabel(std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F const& rect, D2D1_COLOR_F const& color);
    void FillRound(D2D1_RECT_F const& rect, float radius, D2D1_COLOR_F const& color);
    void StrokeRound(D2D1_RECT_F const& rect, float radius, D2D1_COLOR_F const& color);

    void UpdateAnimation();

    int HitTest(LPARAM lParam) const;
    bool IsEnabled(int hit) const;
    void Activate(int hit);
    void SetHover(int hit);

    HWND m_hwnd = nullptr;
    Callbacks m_callbacks;
    EarbudsState m_state;
    Palette m_palette{};
    UINT m_dpi = 96;
    int m_hover = HitNone;
    int m_pressed = HitNone;
    bool m_trackingMouse = false;
    ULONGLONG m_lastHideTick = 0;

    using AnimationClock = std::chrono::steady_clock;
    bool m_animating = false;
    AnimationClock::time_point m_animationStart{};
    D2D1_RECT_F m_statusRect{}; // in DIPs, computed while painting

    winrt::com_ptr<ID2D1Factory> m_d2dFactory;
    winrt::com_ptr<IDWriteFactory> m_dwriteFactory;
    winrt::com_ptr<ID2D1HwndRenderTarget> m_target;
    winrt::com_ptr<ID2D1SolidColorBrush> m_brush;

    winrt::com_ptr<IDWriteTextFormat> m_titleFormat;
    winrt::com_ptr<IDWriteTextFormat> m_bodyFormat;
    winrt::com_ptr<IDWriteTextFormat> m_bodyRightFormat;
    winrt::com_ptr<IDWriteTextFormat> m_captionFormat;
    winrt::com_ptr<IDWriteTextFormat> m_valueFormat;
    winrt::com_ptr<IDWriteTextFormat> m_segmentFormat;
    winrt::com_ptr<IDWriteTextFormat> m_segmentSelectedFormat;
};
