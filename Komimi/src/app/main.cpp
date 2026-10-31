#include <windows.h>
#include <DispatcherQueue.h>
#include <format>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include "device/Engine.h"
#include "ui/Flyout.h"
#include "app/Autostart.h"
#include "app/Localization.h"
#include "ui/TrayProvider.h"

namespace {
    // DispatcherQueue on top of a regular Win32 message loop: Engine uses it to
    // bring results from WinRT threads back to the UI thread.
    winrt::Windows::System::DispatcherQueueController CreateUiDispatcherQueue() {
        DispatcherQueueOptions options{ sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
        winrt::Windows::System::DispatcherQueueController controller{ nullptr };
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
        return controller;
    }

    std::wstring BuildTooltip(EarbudsState const& state) {
        std::wstring tip = L"Komimi\n";
        tip += BudsConstants::Xiaomi::Buds6Lite::DisplayName;
        tip += L'\n';
        switch (state.link) {
        case LinkState::Connected:    tip += Tr(Str::TipConnected); break;
        case LinkState::Connecting:   tip += Tr(Str::TipConnecting); break;
        case LinkState::Disconnected: tip += Tr(Str::TipDisconnected); break;
        case LinkState::NotFound:     tip += Tr(Str::TipNotFound); break;
        }
        auto const& left = state.battery.left.percent;
        auto const& right = state.battery.right.percent;
        if (left || right) {
            auto percent = [](std::optional<uint8_t> const& p) { return p ? std::format(L"{}%", *p) : L"—"; };
            tip += std::format(L"\nL {}  R {}", percent(left), percent(right));
        }
        return tip;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Two copies in the tray make no sense, and the earbuds have one RFCOMM channel. So a running
    // copy gives way to the new one: F5 in Visual Studio always runs the fresh build,
    // and starting it again from Explorer simply restarts the app.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\KomimiTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS && !TrayProvider::CloseOtherInstance(5000)) return 0;

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Localization::Init();
    auto uiQueueController = CreateUiDispatcherQueue();

    // The order matters: Engine is declared last and destroyed first,
    // because the window and tray callbacks refer to it.
    TrayProvider tray;
    Flyout flyout;
    Engine engine{ uiQueueController.DispatcherQueue() };

    if (!flyout.Create(instance, {
            .onAncSelected = [&](BudsConstants::Xiaomi::Buds6Lite::AncModes mode) { engine.SetAncMode(mode); },
            .onAncLevelSelected = [&](uint8_t level) { engine.SetAncLevel(level); },
            .onEqSelected = [&](XiaomiMma::EqPreset preset) { engine.SetEqPreset(preset); },
            .onReconnect = [&] { engine.Reconnect(); },
        })) {
        return 1;
    }

    if (!tray.Create(instance, {
            .onActivate = [&](RECT const& iconRect) { flyout.Toggle(iconRect); },
            .onContextMenu = [&](POINT pt) {
                flyout.Hide();

                // Language names are always in that language, so they can be found from any UI language
                auto languageItem = [&](Language language, std::wstring_view name) {
                    return TrayProvider::MenuItem{
                        .text = std::wstring(name),
                        .action = [&, language] {
                            Localization::SetLanguage(language);
                            flyout.OnLanguageChanged();
                            engine.onStateChanged(); // updates the icon tooltip
                        },
                        .checked = Localization::GetLanguage() == language,
                    };
                };

                tray.ShowMenu(pt, {
                    {
                        .text = std::wstring(Tr(Str::MenuReconnect)),
                        .action = [&] { engine.Reconnect(); },
                        .enabled = engine.State().link != LinkState::Connecting,
                    },
                    {
                        // Read the state each time the menu opens: it may have been changed in Task Manager
                        .text = std::wstring(Tr(Str::MenuStartWithWindows)),
                        .action = [enabled = Autostart::IsEnabled()] { Autostart::SetEnabled(!enabled); },
                        .checked = Autostart::IsEnabled(),
                    },
                    {
                        .text = std::wstring(Tr(Str::MenuLanguage)),
                        .children = {
                            languageItem(Language::System, Tr(Str::MenuLanguageSystem)),
                            {},
                            languageItem(Language::English, L"English"),
                            languageItem(Language::Russian, L"Русский"),
                            languageItem(Language::Japanese, L"日本語"),
                        },
                    },
                    {},
                    { .text = std::wstring(Tr(Str::MenuExit)), .action = [] { PostQuitMessage(0); } },
                });
            },
        })) {
        return 1;
    }

    engine.onStateChanged = [&] {
        auto const& state = engine.State();
        flyout.SetState(state);
        tray.SetTooltip(BuildTooltip(state));
        tray.SetDimmed(state.link != LinkState::Connected);
    };
    engine.onStateChanged();
    engine.Initialize();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
