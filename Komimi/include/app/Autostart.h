#pragma once
#include <windows.h>
#include <cstring>
#include <string>

// Start at Windows sign-in, using the same registry key that is shown in
// Task Manager → Startup apps. When disabling, Task Manager does not delete the entry
// but flags it in StartupApproved\Run; we do the same, so the tray checkbox
// and the Task Manager switch always agree.
namespace Autostart {
    namespace detail {
        inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        inline constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        inline constexpr wchar_t kValueName[] = L"Komimi";

        inline std::wstring CurrentExeCommand() {
            std::wstring path(MAX_PATH, L'\0');
            for (;;) {
                DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length < path.size()) {
                    path.resize(length);
                    break;
                }
                path.resize(path.size() * 2);
            }
            return L"\"" + path + L"\"";
        }

        // 12 bytes: the first is 02 enabled / 03 disabled, from offset 4 the FILETIME of disabling
        inline void WriteApproved(bool enabled) {
            BYTE data[12]{};
            data[0] = enabled ? 0x02 : 0x03;
            if (!enabled) {
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                std::memcpy(data + 4, &now, sizeof(now));
            }
            RegSetKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, REG_BINARY, data, sizeof(data));
        }
    }

    inline bool IsEnabled() {
        using namespace detail;
        if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            return false;
        }
        BYTE data[12]{};
        DWORD size = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS) {
            return true; // no flag = the entry is active
        }
        return (data[0] & 1) == 0; // 02/06 = enabled, 03/07 = disabled in Task Manager
    }

    // Enabling stores the path of the CURRENT exe: enable it from the build that should start with Windows
    inline void SetEnabled(bool enabled) {
        using namespace detail;
        if (enabled) {
            auto command = CurrentExeCommand();
            RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        WriteApproved(enabled);
    }
}
