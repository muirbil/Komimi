#pragma once
#include <windows.h>
#include <format>
#include <string>

// The app has no console, so the log goes to OutputDebugString:
// see the Output window of the VS debugger or DebugView (Sysinternals).
template <typename... Args>
void Log(std::wformat_string<Args...> fmt, Args&&... args) {
    std::wstring line = std::format(fmt, std::forward<Args>(args)...);
    line += L'\n';
    OutputDebugStringW(line.c_str());
}
