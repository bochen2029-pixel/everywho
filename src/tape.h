// everywho · tape.h — the tape contract, identical to facet's (C:\facet\facet.cpp, "tapes"):
// one full path per line (LF/CRLF; NUL-separated detected), or JSONL whose objects carry
// "path" or "file"; comments (#), blanks and duplicates dropped; order kept. Header-only.
// everywho writes tapes with --paths and reads them with --files-from.
#pragma once
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace everywho {

struct Tape {
    std::vector<std::wstring> paths;   // unique, backslashed, no trailing separator
    size_t records = 0, dups = 0, bad = 0;
    bool nul = false;
};

// the identity of a path in a set: case-folded, backslashed, no \\?\ prefix, no trailing separator
inline std::wstring norm_key(std::wstring p) {
    for (auto& c : p) c = (c == L'/') ? L'\\' : (wchar_t)towlower(c);
    if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}
inline std::wstring tidy_path(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

// UTF-8 → UTF-16 is the caller's (es_client-style widen()); this parser takes a decoder so the
// header stays free of windows.h.
using Widen = std::wstring (*)(std::string_view);

// Minimal JSON string extraction for {"path":"…"} / {"file":"…"} records: finds the key and
// decodes the string value (escapes: \" \\ \/ \n \r \t \uXXXX). Returns false when absent.
inline bool json_path_field(std::string_view rec, std::string& out) {
    for (const char* key : { "\"path\"", "\"file\"" }) {
        const size_t k = rec.find(key);
        if (k == std::string_view::npos) continue;
        size_t i = rec.find(':', k);
        if (i == std::string_view::npos) continue;
        ++i;
        while (i < rec.size() && (rec[i] == ' ' || rec[i] == '\t')) ++i;
        if (i >= rec.size() || rec[i] != '"') continue;
        ++i;
        out.clear();
        while (i < rec.size() && rec[i] != '"') {
            char c = rec[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= rec.size()) return false;
            const char e = rec[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > rec.size()) return false;
                    unsigned cp = 0;
                    for (int k2 = 0; k2 < 4; ++k2) {
                        const char h = rec[i++];
                        cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : 0);
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: return false;
            }
        }
        return !out.empty();
    }
    return false;
}

inline void parse_tape(std::string_view bytes, Tape& t, Widen widen) {
    t.nul = bytes.find('\0') != std::string_view::npos;
    const char sep = t.nul ? '\0' : '\n';
    std::unordered_set<std::wstring> seen;
    size_t i = (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) ? 3 : 0;
    while (i < bytes.size()) {
        size_t j = bytes.find(sep, i);
        if (j == std::string_view::npos) j = bytes.size();
        std::string_view rec(bytes.data() + i, j - i);
        i = j + 1;
        while (!rec.empty() && (rec.back() == '\r' || rec.back() == ' ' || rec.back() == '\t')) rec.remove_suffix(1);
        while (!rec.empty() && (rec.front() == ' ' || rec.front() == '\t')) rec.remove_prefix(1);
        if (rec.empty() || rec.front() == '#') continue;
        t.records++;
        std::string path;
        if (rec.front() == '{') {
            if (!json_path_field(rec, path)) { t.bad++; continue; }
        } else {
            path = std::string(rec);
        }
        std::wstring w = tidy_path(widen(path));
        if (w.empty()) { t.bad++; continue; }
        if (!seen.insert(norm_key(w)).second) { t.dups++; continue; }
        t.paths.push_back(std::move(w));
    }
}

}  // namespace everywho
