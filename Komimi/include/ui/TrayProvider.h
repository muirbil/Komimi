#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Notification area icon. Owns a hidden window that receives
// messages from the Shell. The picture is the IDI_APP icon from the resources (res/app.ico).
class TrayProvider {
public:
    struct Callbacks {
        std::function<void(RECT const& iconRect)> onActivate; // left click / Enter
        std::function<void(POINT pt)> onContextMenu;           // right click
    };

    struct MenuItem {
        std::wstring text; // empty string = separator
        std::function<void()> action;
        bool enabled = true;
        bool checked = false;
        std::vector<MenuItem> children; // non-empty = the item opens a submenu
    };

    TrayProvider() = default;
    ~TrayProvider();
    TrayProvider(TrayProvider const&) = delete;
    TrayProvider& operator=(TrayProvider const&) = delete;

    // Asks an already running copy of the app to close and waits for it to exit.
    // false = it did not close within timeoutMs.
    static bool CloseOtherInstance(DWORD timeoutMs);

    bool Create(HINSTANCE instance, Callbacks callbacks);

    void SetTooltip(std::wstring_view text);
    void SetDimmed(bool dimmed); // grey translucent icon while the earbuds are not connected
    void ShowMenu(POINT pt, std::vector<MenuItem> const& items);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    static HMENU BuildMenu(std::vector<MenuItem> const& items, std::vector<std::function<void()> const*>& actions);
    bool AddIcon();
    void UpdateIcon();
    RECT GetIconRect() const;
    NOTIFYICONDATAW BaseData() const;

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    HICON m_icon = nullptr;
    Callbacks m_callbacks;
    std::wstring m_tooltip;
    bool m_dimmed = false;
    bool m_added = false;
};
