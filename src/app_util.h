// everywho · app_util.h — shared app-side helpers: options, formatting, Unicode-width columns.
// Pure std; no windows.h here. Lifted from C:\facet\app_util.h so the family reads alike.
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace everywho {

constexpr const char* kVersion = "0.1.0";          // Stage 0: the counters tier, identity, the report
constexpr uint64_t kUnknown64 = ~0ull;             // "not reported" — never confuse with 0
constexpr uint64_t kTicksPerSec = 10000000ull;     // FILETIME resolution
constexpr uint64_t kTicksPerDay = 864000000000ull;

enum class SortKey { Write, Read, Disk, Ops, Files, NewFiles, Name, Pid };
enum class Tier { Auto, Etw, Counters };

// operation kinds, as a bit set (filters) and as an index (counters)
enum Op : uint32_t {
    kOpCreate = 1u << 0, kOpRead = 1u << 1, kOpWrite = 1u << 2, kOpDelete = 1u << 3, kOpRename = 1u << 4,
    kOpFlush = 1u << 5, kOpDirEnum = 1u << 6, kOpSetInfo = 1u << 7, kOpAll = 0xFFu,
};
constexpr int kOpKinds = 8;
inline const char* op_name(int index) {
    static const char* n[kOpKinds] = { "create", "read", "write", "delete", "rename", "flush", "direnum", "setinfo" };
    return index >= 0 && index < kOpKinds ? n[index] : "?";
}

struct Opts {
    enum class Mode { Auto, Snap, Watch, Gui, Json, Stamp, Spool, Paths, Open, Mcp, Where, Selftest, MakeIcon, Shortcut, Help, Version };
    Mode mode = Mode::Auto;
    bool json = false;                     // modifier: Snap/Watch emit JSON (NDJSON when repeating)
    // window
    uint32_t sample_ms = 3000;             // one-shot window; starts after rundown completes
    uint32_t interval_ms = 1000;           // watch / stream / spool cadence
    int frames = 0;                        // stop after N intervals (0 = until quit)
    int window_s = 0;                      // TUI moving window (0 = the interval)
    // tier
    Tier tier = Tier::Auto;
    bool elevate = false;                  // --elevate: relaunch through UAC
    std::string out_file;                  // --out FILE (the elevated child's channel) · --make-icon / --shortcut target
    bool verify_layouts = false;           // --where --verify-layouts
    // filters
    std::vector<uint32_t> pids;
    std::vector<std::wstring> names;       // globs on the process name
    std::vector<std::wstring> dirs, exclude;
    std::string files_from;                // a tape: only I/O to these paths
    uint32_t ops = kOpAll;
    std::vector<wchar_t> volumes;
    bool agents_only = false;
    // shape
    SortKey sort = SortKey::Write;
    bool group = true;                     // rows by process name (chrome.exe x14)
    int top = 12;
    int depth = 2;
    double min_mb = 0.05;
    bool raw = false;                      // attribution rules off
    bool plain = false;
    bool quiet = false;
    // spool
    std::string spool_file;
    std::string lane = "io";
    double gate_mb = 8.0, gate_pct = 25.0;
    // window
    std::string shot;
    std::string ini;
    bool no_activate = false;
    // tapes
    bool nul = false;                      // -0: NUL-separated paths out
    // --open PATH|DIR: who has it open (the handle scan, ADR-014)
    std::wstring open_path;
};

inline const char* sort_name(SortKey s) {
    switch (s) {
        case SortKey::Read: return "read";
        case SortKey::Disk: return "disk";
        case SortKey::Ops: return "ops";
        case SortKey::Files: return "files";
        case SortKey::NewFiles: return "newfiles";
        case SortKey::Name: return "name";
        case SortKey::Pid: return "pid";
        default: return "write";
    }
}
inline bool parse_sort(const std::string& s, SortKey& out) {
    if (s == "write") out = SortKey::Write;
    else if (s == "read") out = SortKey::Read;
    else if (s == "disk") out = SortKey::Disk;
    else if (s == "ops") out = SortKey::Ops;
    else if (s == "files") out = SortKey::Files;
    else if (s == "newfiles" || s == "rate") out = SortKey::NewFiles;
    else if (s == "name") out = SortKey::Name;
    else if (s == "pid") out = SortKey::Pid;
    else return false;
    return true;
}

inline std::string ssprintf(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    char buf[2048];
    const int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf, n > 0 ? (size_t)std::min<int>(n, sizeof(buf) - 1) : 0);
}

inline std::string fmt_count(uint64_t v) {
    const std::string s = std::to_string(v);
    std::string o;
    const size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return o;
}

inline std::string human_bytes(uint64_t b) {
    if (b == kUnknown64) return "-";
    if (b < 1024) return ssprintf("%llu B", (unsigned long long)b);
    double v = (double)b / 1024.0;
    static const char* u[] = { "KB", "MB", "GB", "TB", "PB" };
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return ssprintf(v < 10.0 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

// bytes per second, the unit the report and the stamp use ("48.3 MB/s", "idle" below 1 KB/s)
inline std::string human_rate(double bps) {
    if (bps < 1024.0) return "idle";
    double v = bps / 1024.0;
    static const char* u[] = { "KB/s", "MB/s", "GB/s" };
    int i = 0;
    while (v >= 1024.0 && i < 2) { v /= 1024.0; ++i; }
    return ssprintf(v < 10.0 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

// ---- UTF-8 display width (terminal columns), so CJK names align ----
inline uint32_t utf8_next(std::string_view s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    const int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = (n == 3) ? (c & 0x07u) : (n == 2) ? (c & 0x0Fu) : (n == 1) ? (c & 0x1Fu) : c;
    for (int k = 0; k < n && i < s.size() && (((unsigned char)s[i]) & 0xC0) == 0x80; ++k, ++i)
        cp = (cp << 6) | (((unsigned char)s[i]) & 0x3Fu);
    return cp;
}
inline int cp_width(uint32_t cp) {
    if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF)
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}
inline int display_width(std::string_view s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) w += cp_width(utf8_next(s, i));
    return w;
}
inline std::string pad_display(const std::string& s, int cols) {
    const int w = display_width(s);
    return w >= cols ? s : s + std::string((size_t)(cols - w), ' ');
}
inline std::string rpad_display(const std::string& s, int cols) {
    const int w = display_width(s);
    return w >= cols ? s : std::string((size_t)(cols - w), ' ') + s;
}
inline std::string trunc_middle(const std::string& s, int cols) {
    if (cols < 4 || display_width(s) <= cols) return s;
    std::vector<std::pair<size_t, int>> cps;
    for (size_t i = 0; i < s.size();) {
        const size_t at = i;
        cps.emplace_back(at, cp_width(utf8_next(s, i)));
    }
    const int head_cols = std::max(1, (cols - 1) * 2 / 5);
    const int tail_cols = cols - 1 - head_cols;
    size_t h = 0;
    int w = 0;
    while (h < cps.size() && w + cps[h].second <= head_cols) w += cps[h++].second;
    size_t t = cps.size();
    w = 0;
    while (t > h && w + cps[t - 1].second <= tail_cols) w += cps[--t].second;
    const size_t hb = h < cps.size() ? cps[h].first : s.size();
    const size_t tb = t < cps.size() ? cps[t].first : s.size();
    return s.substr(0, hb) + "\xE2\x80\xA6" + s.substr(tb);
}
inline std::string trunc_end(const std::string& s, int cols) {
    if (display_width(s) <= cols) return s;
    std::string out;
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        const int cw = cp_width(utf8_next(s, j));
        if (w + cw > cols - 1) break;
        out.append(s, i, j - i);
        w += cw;
        i = j;
    }
    return out + "\xE2\x80\xA6";
}

}  // namespace everywho
