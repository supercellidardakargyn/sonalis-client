#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace ss {

inline std::filesystem::path LocalAppDataPath() {
    std::wstring value(32'768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return {};
    value.resize(length);
    return std::filesystem::path(value);
}

inline bool DynamicShellExecute(const std::wstring& target,
                                const std::wstring_view parameters = {}) noexcept {
    const HMODULE module = LoadLibraryW(L"shell32.dll");
    if (module == nullptr) return false;
    using Function = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    const auto function = reinterpret_cast<Function>(GetProcAddress(module, "ShellExecuteW"));
    const std::wstring parameterStorage(parameters);
    const bool succeeded = function != nullptr
        && reinterpret_cast<std::intptr_t>(function(nullptr, L"open", target.c_str(),
            parameterStorage.empty() ? nullptr : parameterStorage.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
    FreeLibrary(module);
    return succeeded;
}

inline bool DynamicShellNotify(const DWORD operation, NOTIFYICONDATAW* notification) noexcept {
    const HMODULE module = LoadLibraryW(L"shell32.dll");
    if (module == nullptr) return false;
    using Function = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
    const auto function = reinterpret_cast<Function>(GetProcAddress(module, "Shell_NotifyIconW"));
    const bool succeeded = function != nullptr && function(operation, notification) != FALSE;
    FreeLibrary(module);
    return succeeded;
}

}  // namespace ss
