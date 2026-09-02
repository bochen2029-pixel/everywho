// everywho · sys.h — the thin Windows layer every module shares: UTF-8 ↔ UTF-16, clocks,
// console setup, elevation and privilege queries, the exe's own location. Header-only.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "app_util.h"

namespace everywho {

inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline double now_ms() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)freq.QuadPart;
}
inline uint64_t now_filetime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
inline std::string iso_local(uint64_t ft, bool with_t = true) {
    if (!ft) return "";
    FILETIME f{ (DWORD)(ft & 0xFFFFFFFFu), (DWORD)(ft >> 32) };
    SYSTEMTIME ut, lt;
    if (!FileTimeToSystemTime(&f, &ut) || !SystemTimeToTzSpecificLocalTime(nullptr, &ut, &lt)) return "";
    return ssprintf(with_t ? "%04d-%02d-%02dT%02d:%02d:%02d" : "%04d-%02d-%02d %02d:%02d:%02d", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
}
inline std::string iso_now(bool with_t = true) { return iso_local(now_filetime(), with_t); }

inline bool is_elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}
// present in the token at all (enabled or not); SeSystemProfilePrivilege decides the ETW tier
inline bool has_privilege(const wchar_t* name) {
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) return false;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD n = 0;
    GetTokenInformation(tok, TokenPrivileges, nullptr, 0, &n);
    std::string buf(n, '\0');
    bool found = false;
    if (n && GetTokenInformation(tok, TokenPrivileges, buf.data(), n, &n)) {
        const auto* tp = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
        for (DWORD i = 0; i < tp->PrivilegeCount; ++i)
            if (tp->Privileges[i].Luid.LowPart == luid.LowPart && tp->Privileges[i].Luid.HighPart == luid.HighPart) found = true;
    }
    CloseHandle(tok);
    return found;
}

inline std::wstring exe_path() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return std::wstring(buf, n);
}
inline std::wstring exe_dir() {
    std::wstring p = exe_path();
    const size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? L".\\" : p.substr(0, s + 1);
}

inline bool stdout_is_console() {
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}
inline bool stderr_is_console() {
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
}
// UTF-8 code page for the console; ANSI escapes when wanted and possible
inline bool console_setup(bool want_ansi) {
    SetConsoleOutputCP(CP_UTF8);
    if (!want_ansi) return false;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
inline int console_width() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int w = info.srWindow.Right - info.srWindow.Left + 1;
        if (w >= 60 && w <= 400) return w;
    }
    return 120;
}
inline int console_height() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int h = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (h >= 10 && h <= 300) return h;
    }
    return 40;
}
inline void write_out(std::string_view s) {
    fwrite(s.data(), 1, s.size(), stdout);
}

}  // namespace everywho
