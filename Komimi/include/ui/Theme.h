#pragma once
#include <windows.h>

// Theme of the taskbar and system flyouts (not to be confused with AppsUseLightTheme, the app theme).
inline bool SystemUsesLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value != 0;
}

// WM_SETTINGCHANGE arrives with this parameter when the theme or accent colour changes
inline bool IsThemeChangeMessage(LPARAM lParam) {
    auto area = reinterpret_cast<wchar_t const*>(lParam);
    return area && lstrcmpiW(area, L"ImmersiveColorSet") == 0;
}
