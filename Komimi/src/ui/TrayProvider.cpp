#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <vector>
#include "ui/TrayProvider.h"
#include "resource.h"

namespace {
    constexpr wchar_t kWindowClass[] = L"KomimiTrayWindow";
    constexpr UINT kIconId = 1;
    constexpr UINT WM_TRAYICON = WM_APP + 1;

    UINT TaskbarCreatedMessage() {
        // Explorer sends it after restarting: the icon must be added again
        static UINT message = RegisterWindowMessageW(L"TaskbarCreated");
        return message;
    }

    // Undocumented uxtheme API (ordinals 135/136): enables dark
    // context menus for the process. If it ever disappears, the menus simply stay light.
    void AllowDarkMenus() {
        HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!uxtheme) return;
        using SetPreferredAppModeFn = int(WINAPI*)(int);
        using FlushMenuThemesFn = void(WINAPI*)();
        auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        if (setPreferredAppMode) setPreferredAppMode(1 /* AllowDark */);
        if (flushMenuThemes) flushMenuThemes();
    }

    // A greyscale translucent copy of the icon, meaning "earbuds not connected".
    HICON CreateDimmedIcon(HICON source) {
        ICONINFO info{};
        if (!GetIconInfo(source, &info)) return nullptr;

        BITMAP bm{};
        GetObjectW(info.hbmColor, sizeof(bm), &bm);
        int width = bm.bmWidth, height = bm.bmHeight;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC dc = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HICON icon = nullptr;
        if (color && GetDIBits(dc, info.hbmColor, 0, height, bits, &bi, DIB_RGB_COLORS) == height) {
            auto pixels = static_cast<uint32_t*>(bits);
            for (int i = 0; i < width * height; ++i) {
                uint32_t p = pixels[i];
                uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF, a = p >> 24;
                uint32_t gray = (r * 77 + g * 151 + b * 28) >> 8;
                pixels[i] = ((a * 55 / 100) << 24) | (gray << 16) | (gray << 8) | gray;
            }
            ICONINFO dimmed{ TRUE, 0, 0, info.hbmMask, color };
            icon = CreateIconIndirect(&dimmed);
        }
        ReleaseDC(nullptr, dc);

        if (color) DeleteObject(color);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        return icon;
    }
}

HMENU TrayProvider::BuildMenu(std::vector<MenuItem> const& items, std::vector<std::function<void()> const*>& actions) {
    HMENU menu = CreatePopupMenu();
    for (auto const& item : items) {
        if (item.text.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING | (item.enabled ? 0 : MF_GRAYED) | (item.checked ? MF_CHECKED : 0);
        if (!item.children.empty()) {
            HMENU submenu = BuildMenu(item.children, actions); // freed together with the parent in DestroyMenu
            AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), item.text.c_str());
        }
        else {
            actions.push_back(&item.action);
            AppendMenuW(menu, flags, actions.size(), item.text.c_str());
        }
    }
    return menu;
}

TrayProvider::~TrayProvider() {
    if (m_added) {
        NOTIFYICONDATAW data = BaseData();
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_icon) DestroyIcon(m_icon);
}

bool TrayProvider::CloseOtherInstance(DWORD timeoutMs) {
    HWND other = FindWindowW(kWindowClass, nullptr);
    if (!other) return true;

    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    bool exited = !process || WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
    if (process) CloseHandle(process);
    return exited;
}

bool TrayProvider::Create(HINSTANCE instance, Callbacks callbacks) {
    m_instance = instance;
    m_callbacks = std::move(callbacks);
    AllowDarkMenus();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // A regular (not message-only) window: the "TaskbarCreated" broadcast does not reach message-only windows.
    // It is never shown.
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!m_hwnd) return false;

    UpdateIcon();
    return AddIcon();
}

NOTIFYICONDATAW TrayProvider::BaseData() const {
    NOTIFYICONDATAW data{ sizeof(data) };
    data.hWnd = m_hwnd;
    data.uID = kIconId;
    return data;
}

bool TrayProvider::AddIcon() {
    NOTIFYICONDATAW data = BaseData();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_TRAYICON;
    data.hIcon = m_icon;
    lstrcpynW(data.szTip, m_tooltip.c_str(), ARRAYSIZE(data.szTip));
    m_added = Shell_NotifyIconW(NIM_ADD, &data);
    if (!m_added) return false;

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void TrayProvider::UpdateIcon() {
    // Take the app.ico frame that matches the tray icon size at the current scale (16/20/24/32 px)
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(m_hwnd));
    auto icon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!icon) return;
    if (m_dimmed) {
        HICON dimmed = CreateDimmedIcon(icon);
        if (dimmed) {
            DestroyIcon(icon);
            icon = dimmed;
        }
    }

    HICON old = m_icon;
    m_icon = icon;
    if (m_added) {
        NOTIFYICONDATAW data = BaseData();
        data.uFlags = NIF_ICON;
        data.hIcon = m_icon;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (old) DestroyIcon(old);
}

void TrayProvider::SetTooltip(std::wstring_view text) {
    if (m_tooltip == text) return;
    m_tooltip = text;
    if (!m_added) return;
    NOTIFYICONDATAW data = BaseData();
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    lstrcpynW(data.szTip, m_tooltip.c_str(), ARRAYSIZE(data.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayProvider::SetDimmed(bool dimmed) {
    if (m_dimmed == dimmed) return;
    m_dimmed = dimmed;
    UpdateIcon();
}

RECT TrayProvider::GetIconRect() const {
    NOTIFYICONIDENTIFIER id{ sizeof(id) };
    id.hWnd = m_hwnd;
    id.uID = kIconId;
    RECT rect{};
    if (FAILED(Shell_NotifyIconGetRect(&id, &rect))) {
        POINT pt{};
        GetCursorPos(&pt);
        rect = { pt.x, pt.y, pt.x + 1, pt.y + 1 };
    }
    return rect;
}

void TrayProvider::ShowMenu(POINT pt, std::vector<MenuItem> const& items) {
    // Commands are numbered sequentially across all menu levels, 0 = "nothing selected"
    std::vector<std::function<void()> const*> actions;
    HMENU menu = BuildMenu(items, actions);

    // Without SetForegroundWindow the menu does not close when clicking outside it
    SetForegroundWindow(m_hwnd);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT command = TrackPopupMenuEx(menu, align | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command > 0 && command <= actions.size() && *actions[command - 1]) {
        (*actions[command - 1])();
    }
}

LRESULT CALLBACK TrayProvider::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<TrayProvider*>(create->lpCreateParams)->m_hwnd = hwnd;
    }
    auto self = reinterpret_cast<TrayProvider*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayProvider::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        // NOTIFYICON_VERSION_4: the event is in LOWORD(lParam), the coordinates in wParam
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (m_callbacks.onActivate) m_callbacks.onActivate(GetIconRect());
            break;
        case WM_CONTEXTMENU:
            if (m_callbacks.onContextMenu) m_callbacks.onContextMenu({ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
            break;
        }
        return 0;
    }

    // Comes from a new instance (CloseOtherInstance): leave the message loop normally
    if (msg == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }

    if (msg == TaskbarCreatedMessage()) {
        m_added = false;
        UpdateIcon();
        AddIcon();
        return 0;
    }

    // The display scale changed: a different icon frame is needed
    if (msg == WM_DISPLAYCHANGE || (msg == WM_SETTINGCHANGE && wParam == SPI_SETNONCLIENTMETRICS)) {
        UpdateIcon();
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}
