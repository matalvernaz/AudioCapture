#pragma once

#include <windows.h>
#include <string>

// Loads a Windows DLL from System32 only, never from the program's folder or the current
// directory, so a DLL planted next to the exe cannot be picked up by name.
inline HMODULE LoadSystemDll(const wchar_t* name) {
    HMODULE module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module) {
        return module;
    }
    if (GetLastError() != ERROR_INVALID_PARAMETER) {
        return nullptr;  // the DLL really is missing
    }
    // Windows 7 without KB2533623 rejects the flag: use the full System32 path instead
    wchar_t system32[MAX_PATH];
    UINT length = GetSystemDirectoryW(system32, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return nullptr;
    }
    std::wstring path = std::wstring(system32) + L"\\" + name;
    return LoadLibraryW(path.c_str());
}
