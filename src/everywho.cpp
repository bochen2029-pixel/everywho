// everywho · everywho.cpp — the console: every mode. Stage 0 is the counters tier: the kernel's
// process list (identity + cumulative I/O in one call) and PDH's disk counters, folded per
// process and per volume, printed as the report, the watch TUI, JSON / NDJSON, the stamp, the
// spool, --where, --selftest, and a minimal MCP server. The ETW tier (what) is Stage 1.
#include "sys.h"

#include "app_util.h"
#include "counters.h"
#include "rates.h"
#include "tape.h"
#include "where.h"
#include "who.h"

#include <conio.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace everywho {

int write_icon_file(const std::string& path);   // shell.cpp
int make_shortcut(const std::string& where);    // shell.cpp

namespace {

// ---------------------------------------------------------------- ANSI
bool g_ansi = false;
const char* R() { return g_ansi ? "\x1b[0m" : ""; }
const char* B() { return g_ansi ? "\x1b[1m" : ""; }
const char* D() { return g_ansi ? "\x1b[2m" : ""; }
const char* AMB() { return g_ansi ? "\x1b[33m" : ""; }
const char* CYN() { return g_ansi ? "\x1b[36m" : ""; }
const char* GRN() { return g_ansi ? "\x1b[32m" : ""; }
const char* RED() { return g_ansi ? "\x1b[31m" : ""; }

std::string bar(double frac, int w) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    const int n = (int)std::lround(frac * w);
    std::string s;
    for (int i = 0; i < w; ++i) s += g_ansi ? (i < n ? "\xE2\x96\x88" : "\xE2\x96\x91") : (i < n ? "#" : ".");
    return s;
}

// ---------------------------------------------------------------- JSON out
std::string jstr(std::string_view s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) o += ssprintf("\\u%04x", c);
                else o += (char)c;
        }
    }
    return o + "\"";
}
std::string jw(std::wstring_view w) { return jstr(narrow(w)); }
std::string jn(uint64_t v) { return std::to_string(v); }
std::string jf(double v) { return std::isfinite(v) ? ssprintf("%.3f", v) : "null"; }
std::string jb(bool v) { return v ? "true" : "false"; }

// ---------------------------------------------------------------- mini JSON in (MCP)
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JV> arr;
    std::vector<std::pair<std::string, JV>> obj;
    const JV* get(const char* k) const {
        for (const auto& [kk, v] : obj)
            if (kk == k) return &v;
        return nullptr;
    }
    std::string str(const char* k, const std::string& d = "") const { const JV* v = get(k); return v && v->t == Str ? v->s : d; }
    double num(const char* k, double d = 0) const { const JV* v = get(k); return v && v->t == Num ? v->n : d; }
    bool flag(const char* k, bool d = false) const { const JV* v = get(k); return v && v->t == Bool ? v->b : d; }
};
struct JParser {
    std::string_view s;
    size_t i = 0;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool lit(const char* w) { const size_t n = strlen(w); if (s.substr(i, n) == w) { i += n; return true; } return false; }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i++];
                        const int d = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
                        if (d < 0) return false;
                        cp = cp * 16 + (unsigned)d;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && s.substr(i, 2) == "\\u") {
                        unsigned lo = 0;
                        i += 2;
                        for (int k = 0; k < 4; ++k) { const char h = s[i++]; lo = lo * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10); }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: return false;
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool value(JV& v, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{') {
            ++i;
            v.t = JV::Obj;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                JV val;
                if (!value(val, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(val));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            v.t = JV::Arr;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                JV val;
                if (!value(val, depth + 1)) return false;
                v.arr.push_back(std::move(val));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JV::Str; return str(v.s); }
        if (lit("true")) { v.t = JV::Bool; v.b = true; return true; }
        if (lit("false")) { v.t = JV::Bool; v.b = false; return true; }
        if (lit("null")) { v.t = JV::Null; return true; }
        const size_t st = i;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) ++i;
        if (i == st) return false;
        v.t = JV::Num;
        v.n = strtod(std::string(s.substr(st, i - st)).c_str(), nullptr);
        return true;
    }
};
bool jparse(std::string_view s, JV& v) {
    JParser p{ s };
    if (!p.value(v)) return false;
    p.ws();
    return p.i == s.size();
}

// ---------------------------------------------------------------- the engine (counters tier)
FoldConfig fold_config(const Opts& o) {
    FoldConfig c;
    c.pids = o.pids;
    c.name_globs = o.names;
    c.agents_only = o.agents_only;
    c.ops = o.ops;
    c.volumes = o.volumes;
    c.attribute = !o.raw;
    c.top_procs = (uint32_t)(std::max)(o.top * 4, 200);
    return c;
}

struct Engine {
    const Opts& o;
    IdentityTable who;
    Fold fold;
    DiskCounters disks;
    bool disks_ok = false;
    std::string disk_err;
    std::unordered_map<uint32_t, ProcSample> last;
    bool primed = false;
    double t_prev = 0;                // when the previous tick was taken
    double t_start = 0;               // when the window began (the first tick after prime)
    uint32_t exited = 0;              // processes that vanished since the window began

    explicit Engine(const Opts& o_) : o(o_), fold(fold_config(o_), who) { disks_ok = disks.open(&disk_err); }

    bool tick(std::string* err) {
        std::vector<ProcSample> cur;
        if (!sample_processes(cur, err)) return false;
        const double now = now_ms();
        who.ingest(cur);
        who.enrich_pending();
        who.attribute_by_ancestry();
        std::unordered_map<uint32_t, ProcSample> now_map;
        now_map.reserve(cur.size() * 2);
        auto delta = [](uint64_t a, uint64_t b) { return a >= b ? a - b : 0; };
        for (auto& s : cur) {
            IoCounters d;
            bool have = false;
            auto it = last.find(s.pid);
            if (it != last.end() && it->second.create_ft == s.create_ft) {
                const ProcSample& b = it->second;
                d.file_read = delta(s.read_bytes, b.read_bytes);
                d.file_write = delta(s.write_bytes, b.write_bytes);
                d.other_bytes = delta(s.other_bytes, b.other_bytes);
                d.ops[1] = (uint32_t)delta(s.read_ops, b.read_ops);
                d.ops[2] = (uint32_t)delta(s.write_ops, b.write_ops);
                d.ops_other = (uint32_t)delta(s.other_ops, b.other_ops);
                have = true;
            } else if (primed) {   // born inside the window: everything it did is inside it
                d.file_read = s.read_bytes;
                d.file_write = s.write_bytes;
                d.other_bytes = s.other_bytes;
                d.ops[1] = (uint32_t)s.read_ops;
                d.ops[2] = (uint32_t)s.write_ops;
                d.ops_other = (uint32_t)s.other_ops;
                have = true;
            }
            if (have && !d.empty()) fold.add_process_counters(s.pid, d);
            now_map[s.pid] = std::move(s);
        }
        if (primed)
            for (const auto& [pid, b] : last)
                if (!now_map.count(pid)) exited++;
        last.swap(now_map);
        if (!primed) t_start = now;
        primed = true;
        t_prev = now;
        fold.set_processes_seen((uint32_t)cur.size());
        if (disks_ok) {
            std::vector<VolumeStat> v;
            if (disks.collect(v, nullptr))
                for (auto& x : v) fold.add_volume(x);
        }
        return true;
    }

    Snapshot snap(bool cumulative, double window_ms) {
        Snapshot s = fold.snapshot(o.sort, cumulative, (uint64_t)window_ms);
        s.tier = Tier::Counters;
        s.elevated = is_elevated();
        if (!disks_ok && s.error.empty()) s.error = "disk counters unavailable: " + disk_err;
        return s;
    }
};

// ---------------------------------------------------------------- rows (grouped by name)
struct Row {
    std::wstring name;
    uint32_t count = 0;
    IoCounters io;
    const ProcStat* rep = nullptr;    // the member with the most bytes: its cwd / agent / user stand for the group
    std::vector<uint32_t> pids;
    uint32_t projects = 0;            // distinct projects among agent members
};

std::vector<Row> group_rows(const Snapshot& s, bool group) {
    std::vector<Row> rows;
    std::map<std::wstring, size_t> index;
    for (const ProcStat& p : s.procs) {
        std::wstring key = p.id.name;
        for (auto& c : key) c = (wchar_t)towlower(c);
        key += L"|" + widen(p.id.agent.harness);   // the Claude desktop app and the Claude Code CLI share a name, not a row
        if (!group) key += L"#" + std::to_wstring(p.id.pid);
        auto it = index.find(key);
        if (it == index.end()) {
            Row r;
            r.name = p.id.name;
            index[key] = rows.size();
            rows.push_back(std::move(r));
            it = index.find(key);
        }
        Row& r = rows[it->second];
        r.count++;
        r.io.add(p.io);
        r.pids.push_back(p.id.pid);
        const uint64_t bytes = p.io.file_write + p.io.file_read;
        if (!r.rep || bytes > r.rep->io.file_write + r.rep->io.file_read) r.rep = &p;
    }
    for (Row& r : rows) {
        std::vector<std::wstring> variants;   // distinct project + session pairs among the group's agents
        for (uint32_t pid : r.pids)
            for (const ProcStat& p : s.procs)
                if (p.id.pid == pid && p.id.agent.any()) {
                    const std::wstring v = p.id.agent.project + L"|" + p.id.agent.session;
                    if (std::find(variants.begin(), variants.end(), v) == variants.end()) variants.push_back(v);
                }
        r.projects = (uint32_t)variants.size();
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        const uint64_t ka = a.io.file_write, kb = b.io.file_write;
        if (ka != kb) return ka > kb;
        return a.io.file_read > b.io.file_read;
    });
    return rows;
}

std::string agent_text(const Identity& id) {
    if (id.agent.any()) {
        std::string t = id.agent.harness;
        if (!id.agent.project.empty()) t += " \xC2\xB7 " + narrow(id.agent.project);
        if (!id.agent.session.empty()) t += " \xC2\xB7 " + narrow(id.agent.session.substr(0, 8)) + "\xE2\x80\xA6";
        if (id.agent.rule && id.agent.rule[0] && std::string(id.agent.rule) != "tape") t += std::string(" (") + id.agent.rule + ")";
        return t;
    }
    switch (id.kind) {
        case Kind::System: return "kernel";
        case Kind::Wsl: return "WSL";
        case Kind::Service: return id.user.empty() ? "service" : "service \xC2\xB7 " + narrow(id.user);
        default: return narrow(id.user);
    }
}

std::string fmt_window(uint64_t ms) { return ssprintf("%.1f s", (double)ms / 1000.0); }

// ---------------------------------------------------------------- the report / the TUI frame
std::string render(const Snapshot& s, const Opts& o, int width, bool tui, uint32_t exited, bool paused, bool frozen, int window_s) {
    std::string out;
    std::vector<Row> rows = group_rows(s, o.group);
    // header
    out += B();
    out += ssprintf("everywho %s", kVersion);
    out += R();
    out += D();
    out += ssprintf("  \xC2\xB7  %s  \xC2\xB7  %s window  \xC2\xB7  counters tier (%s)  \xC2\xB7  %s processes, %s with I/O%s", iso_now(false).c_str(), fmt_window(s.window_ms).c_str(),
                    s.elevated ? "elevated" : "not elevated", fmt_count(s.processes_seen).c_str(), fmt_count(s.procs_total).c_str(),
                    exited ? ssprintf(", %s exited", fmt_count(exited).c_str()).c_str() : "");
    if (tui) {
        if (paused) out += "  \xC2\xB7  PAUSED";
        if (frozen) out += "  \xC2\xB7  FROZEN";
        if (window_s > 1) out += ssprintf("  \xC2\xB7  %d s moving", window_s);
    }
    out += R();
    out += "\n\n";
    // volumes
    if (!s.volumes.empty()) {
        out += B();
        out += pad_display("VOLUMES", 16) + rpad_display("disk read", 12) + rpad_display("disk write", 12) + rpad_display("reads/s", 10) + rpad_display("writes/s", 10) +
               rpad_display("queue", 8) + rpad_display("busy", 7);
        out += R();
        out += "\n";
        for (const VolumeStat& v : s.volumes) {
            std::string letters;
            for (wchar_t L : v.letters) letters += ssprintf("%c: ", (char)L);
            if (letters.empty()) letters = narrow(v.instance);
            const bool hot = v.busy_pct >= 50.0;
            out += " " + pad_display(letters, 8) + D() + pad_display(ssprintf("disk %u", v.disk), 7) + R();
            out += rpad_display(human_rate(v.read_bps), 12) + rpad_display(human_rate(v.write_bps), 12) + rpad_display(ssprintf("%.0f", v.read_iops), 10) +
                   rpad_display(ssprintf("%.0f", v.write_iops), 10) + rpad_display(ssprintf("%.1f", v.queue), 8);
            out += hot ? AMB() : "";
            out += rpad_display(ssprintf("%.0f %%", v.busy_pct), 7);
            out += R();
            out += "  " + bar(v.busy_pct / 100.0, 12) + "\n";
        }
        out += "\n";
    } else if (!s.error.empty()) {
        out += D();
        out += "VOLUMES  " + s.error + "\n\n";
        out += R();
    }
    // who
    const int name_w = 22, bar_w = 10, num_w = 10, ops_w = 8, who_w = 30;
    const int fixed = 1 + name_w + 1 + bar_w + 1 + num_w + 1 + num_w + 1 + ops_w + 1 + who_w + 1;
    const int cwd_w = (std::max)(18, width - fixed - 1);
    out += B();
    out += pad_display(ssprintf("WHO  (by %s)", sort_name(o.sort)), name_w + 1 + bar_w + 1) + rpad_display("write", num_w) + " " + rpad_display("read", num_w) + " " + rpad_display("ops", ops_w) +
           " " + pad_display("agent / user", who_w) + " " + "cwd";
    out += R();
    out += "\n";
    const uint64_t denom = (std::max<uint64_t>)(1, s.total.file_write);
    int shown = 0;
    const uint64_t floor_bytes = (uint64_t)(o.min_mb * 1048576.0);
    const int max_rows = tui ? (std::max)(5, console_height() - 12 - (int)s.volumes.size()) : o.top;
    for (const Row& r : rows) {
        if (shown >= max_rows) break;
        const uint64_t bytes = r.io.file_write + r.io.file_read;
        if (bytes < floor_bytes && shown >= 3) continue;
        const Identity& id = r.rep ? r.rep->id : Identity{};
        std::string name = narrow(r.name);
        if (r.count > 1) name += ssprintf(" \xC3\x97%u", r.count);
        const double share = (double)r.io.file_write / (double)denom;
        std::string who = agent_text(id);
        if (r.projects > 1) who += ssprintf(" +%u", r.projects - 1);
        out += " " + pad_display(trunc_end(name, name_w), name_w) + " ";
        out += (share >= 0.5 ? AMB() : D());
        out += bar(share, bar_w);
        out += R();
        out += " " + rpad_display(human_bytes(r.io.file_write), num_w) + " " + rpad_display(human_bytes(r.io.file_read), num_w) + " " + rpad_display(fmt_count(r.io.total_ops()), ops_w) + " ";
        out += (id.agent.any() ? CYN() : D());
        out += pad_display(trunc_end(who, who_w), who_w);
        out += R();
        out += " ";
        out += D();
        out += trunc_middle(narrow(id.cwd), cwd_w);
        out += R();
        out += "\n";
        shown++;
    }
    if (!shown) {
        out += D();
        out += " (no I/O in the window)\n";
        out += R();
    }
    // footer
    out += "\n";
    out += D();
    std::string filter;
    for (uint32_t pid : o.pids) filter += ssprintf(" pid:%u", pid);
    for (const auto& n : o.names) filter += " name:" + narrow(n);
    if (o.agents_only) filter += " agents";
    out += "FILTER " + (filter.empty() ? std::string(" (none)") : filter);
    out += "   \xC2\xB7   what needs the ETW tier: directories, files, bursts (Stage 1, elevated)";
    if (tui) out += "\n q quit \xC2\xB7 s sort \xC2\xB7 g group \xC2\xB7 p pause \xC2\xB7 f freeze \xC2\xB7 1/2/3 = 1 s / 10 s / 60 s window";
    out += R();
    out += "\n";
    return out;
}

// ---------------------------------------------------------------- JSON
std::string snapshot_json(const Snapshot& s, const Opts& o, uint32_t exited, bool ndjson) {
    std::string j = "{\"tool\":\"everywho\",\"version\":" + jstr(kVersion) + ",\"wall_ms\":" + jn(s.wall_ms) + ",\"window_ms\":" + jn(s.window_ms);
    if (ndjson) j += ",\"interval_ms\":" + jn(o.interval_ms);
    j += ",\"tier\":\"counters\",\"elevated\":" + jb(s.elevated) + ",\"session\":null";
    j += ",\"processes_seen\":" + jn(s.processes_seen) + ",\"processes_with_io\":" + jn(s.procs_total) + ",\"exited\":" + jn(exited);
    j += ",\"total\":{\"file_read\":" + jn(s.total.file_read) + ",\"file_write\":" + jn(s.total.file_write) + ",\"other_bytes\":" + jn(s.total.other_bytes) + ",\"ops\":" + jn(s.total.total_ops()) + "}";
    j += ",\"volumes\":[";
    for (size_t i = 0; i < s.volumes.size(); ++i) {
        const VolumeStat& v = s.volumes[i];
        if (i) j += ",";
        j += "{\"letters\":" + jw(v.letters) + ",\"instance\":" + jw(v.instance) + ",\"disk\":" + jn(v.disk) + ",\"file_read_bps\":null,\"file_write_bps\":null,\"disk_read_bps\":" + jf(v.read_bps) +
             ",\"disk_write_bps\":" + jf(v.write_bps) + ",\"read_iops\":" + jf(v.read_iops) + ",\"write_iops\":" + jf(v.write_iops) + ",\"queue\":" + jf(v.queue) + ",\"busy_pct\":" + jf(v.busy_pct) +
             ",\"response_ms_p50\":null,\"response_ms_p95\":null}";
    }
    j += "],\"processes\":[";
    size_t n = 0;
    for (const ProcStat& p : s.procs) {
        if ((int)n >= (std::max)(o.top, 1) * 4) break;
        if (n) j += ",";
        n++;
        const Identity& id = p.id;
        j += "{\"pid\":" + jn(id.pid) + ",\"name\":" + jw(id.name) + ",\"image\":" + jw(id.image) + ",\"cmdline\":" + jw(id.cmdline) + ",\"cwd\":" + (id.cwd_ok ? jw(id.cwd) : "null") +
             ",\"parent\":" + jn(id.ppid) + ",\"session_id\":" + jn(id.session_id) + ",\"user\":" + jw(id.user) + ",\"kind\":" + jstr(kind_name(id.kind));
        j += ",\"agent\":";
        if (id.agent.any())
            j += "{\"harness\":" + jstr(id.agent.harness) + ",\"project\":" + jw(id.agent.project) + ",\"session\":" + (id.agent.session.empty() ? "null" : jw(id.agent.session)) +
                 ",\"subagent\":" + (id.agent.subagent.empty() ? "null" : jw(id.agent.subagent)) + ",\"rule\":" + jstr(id.agent.rule ? id.agent.rule : "") + "}";
        else
            j += "null";
        j += ",\"file_read\":" + jn(p.io.file_read) + ",\"file_write\":" + jn(p.io.file_write) + ",\"other_bytes\":" + jn(p.io.other_bytes) +
             ",\"disk_read\":null,\"disk_write\":null,\"attributed_write\":null";
        j += ",\"ops\":{\"read\":" + jn(p.io.ops[1]) + ",\"write\":" + jn(p.io.ops[2]) + ",\"other\":" + jn(p.io.ops_other) + "}";
        j += ",\"files\":null,\"top_dirs\":[],\"exited\":" + jb(id.exited) + "}";
    }
    j += "],\"directories\":[],\"files\":[],\"bursts\":[],\"error\":" + (s.error.empty() ? std::string("null") : jstr(s.error)) + "}";
    return j;
}

// ---------------------------------------------------------------- stamp / spool
std::string mb(double bytes) { return ssprintf("%.1f", bytes / 1048576.0); }

std::string stamp_line(const Snapshot& s) {
    std::string t = "io_stamp t=" + iso_now() + " tier=counters";
    for (const VolumeStat& v : s.volumes) {
        const std::string l = v.letters.empty() ? narrow(v.instance) : narrow(v.letters.substr(0, 1)) + ":";
        if (v.read_bps < 1024 && v.write_bps < 1024 && v.queue < 0.05) t += " " + l + " idle";
        else t += " " + l + " dr=" + mb(v.read_bps) + " dw=" + mb(v.write_bps) + ssprintf(" q=%.1f busy=%.0f%%", v.queue, v.busy_pct);
    }
    std::vector<Row> rows = group_rows(s, true);
    t += " top=";
    int n = 0;
    for (const Row& r : rows) {
        if (n >= 3 || r.io.file_write < 65536) break;
        if (n) t += ",";
        t += narrow(r.name) + ":" + mb((double)r.io.file_write) + "M";
        if (r.rep && r.rep->id.agent.any()) t += "(" + r.rep->id.agent.harness + (r.rep->id.agent.project.empty() ? "" : "/" + narrow(r.rep->id.agent.project)) + ")";
        if (r.count > 1) t += ssprintf("x%u", r.count);
        n++;
    }
    if (!n) t += "none";
    t += ssprintf(" procs=%u window=%.1fs", s.procs_total, (double)s.window_ms / 1000.0);
    return t;
}

std::string stamp_json(const Snapshot& s) {
    std::string j = "{\"stamp\":\"io\",\"t\":" + jstr(iso_now()) + ",\"tier\":\"counters\",\"window_ms\":" + jn(s.window_ms) + ",\"volumes\":[";
    for (size_t i = 0; i < s.volumes.size(); ++i) {
        const VolumeStat& v = s.volumes[i];
        if (i) j += ",";
        j += "{\"letters\":" + jw(v.letters) + ",\"disk\":" + jn(v.disk) + ",\"disk_read_bps\":" + jf(v.read_bps) + ",\"disk_write_bps\":" + jf(v.write_bps) + ",\"queue\":" + jf(v.queue) + ",\"busy_pct\":" + jf(v.busy_pct) + "}";
    }
    j += "],\"top\":[";
    std::vector<Row> rows = group_rows(s, true);
    int n = 0;
    for (const Row& r : rows) {
        if (n >= 5) break;
        if (n) j += ",";
        j += "{\"name\":" + jw(r.name) + ",\"count\":" + jn(r.count) + ",\"file_write\":" + jn(r.io.file_write) + ",\"file_read\":" + jn(r.io.file_read);
        if (r.rep && r.rep->id.agent.any()) j += ",\"harness\":" + jstr(r.rep->id.agent.harness) + ",\"project\":" + jw(r.rep->id.agent.project);
        j += "}";
        n++;
    }
    j += "],\"procs\":" + jn(s.procs_total) + "}";
    return j;
}

struct SpoolGate {
    bool first = true;
    std::map<std::wstring, std::pair<double, double>> vol;   // instance → (read, write) bps
    std::map<std::wstring, double> busy;
    std::wstring top;
    bool fire(const Snapshot& s, const std::vector<Row>& rows, const Opts& o) {
        bool moved = first;
        first = false;
        for (const VolumeStat& v : s.volumes) {
            auto it = vol.find(v.instance);
            if (it == vol.end() || std::fabs(it->second.first - v.read_bps) >= o.gate_mb * 1048576.0 || std::fabs(it->second.second - v.write_bps) >= o.gate_mb * 1048576.0) moved = true;
            auto b = busy.find(v.instance);
            if (b == busy.end() || std::fabs(b->second - v.busy_pct) >= o.gate_pct) moved = true;
            vol[v.instance] = { v.read_bps, v.write_bps };
            busy[v.instance] = v.busy_pct;
        }
        const std::wstring now_top = rows.empty() || rows[0].io.file_write < 65536 ? L"" : rows[0].name;
        if (now_top != top) moved = true;
        top = now_top;
        return moved;
    }
};

std::string spool_text(const Snapshot& s, const std::vector<Row>& rows) {
    std::string t;
    for (const VolumeStat& v : s.volumes) {
        const std::string l = v.letters.empty() ? narrow(v.instance) : narrow(v.letters.substr(0, 1)) + ":";
        if (!t.empty()) t += "  ";
        if (v.read_bps < 1024 && v.write_bps < 1024) t += l + " idle";
        else t += l + " w " + mb(v.write_bps) + "M/s r " + mb(v.read_bps) + ssprintf("M/s q%.1f", v.queue);
    }
    int n = 0;
    for (const Row& r : rows) {
        if (n >= 3 || r.io.file_write < 65536) break;
        t += " | " + narrow(r.name) + (r.count > 1 ? ssprintf("\xC3\x97%u", r.count) : "") + " " + mb((double)r.io.file_write) + "M";
        if (r.rep && r.rep->id.cwd_ok) t += " " + narrow(r.rep->id.cwd);
        n++;
    }
    return t;
}

// ---------------------------------------------------------------- moving window (TUI)
Snapshot merge(const std::deque<Snapshot>& hist, size_t n, SortKey sort) {
    Snapshot m;
    if (hist.empty()) return m;
    const size_t from = hist.size() > n ? hist.size() - n : 0;
    std::map<uint32_t, ProcStat> procs;
    std::map<std::wstring, std::pair<VolumeStat, int>> vols;
    for (size_t i = from; i < hist.size(); ++i) {
        const Snapshot& s = hist[i];
        m.window_ms += s.window_ms;
        m.wall_ms = s.wall_ms;
        m.tier = s.tier;
        m.elevated = s.elevated;
        m.processes_seen = s.processes_seen;
        for (const ProcStat& p : s.procs) {
            auto it = procs.find(p.id.pid);
            if (it == procs.end()) procs.emplace(p.id.pid, p);
            else { it->second.io.add(p.io); it->second.id = p.id; }
        }
        for (const VolumeStat& v : s.volumes) {
            auto it = vols.find(v.instance);
            if (it == vols.end()) { vols.emplace(v.instance, std::make_pair(v, 1)); continue; }
            VolumeStat& a = it->second.first;
            a.read_bps += v.read_bps; a.write_bps += v.write_bps; a.read_iops += v.read_iops; a.write_iops += v.write_iops; a.queue += v.queue; a.busy_pct += v.busy_pct;
            it->second.second++;
        }
    }
    for (auto& [pid, p] : procs) { m.total.add(p.io); m.procs.push_back(p); }
    m.procs_total = (uint32_t)m.procs.size();
    for (auto& [k, pv] : vols) {
        VolumeStat v = pv.first;
        const double d = (double)pv.second;
        v.read_bps /= d; v.write_bps /= d; v.read_iops /= d; v.write_iops /= d; v.queue /= d; v.busy_pct /= d;
        m.volumes.push_back(v);
    }
    std::sort(m.volumes.begin(), m.volumes.end(), [](const VolumeStat& a, const VolumeStat& b) { return a.disk < b.disk; });
    auto key = [sort](const ProcStat& p) -> uint64_t {
        switch (sort) {
            case SortKey::Read: return p.io.file_read;
            case SortKey::Ops: return p.io.total_ops();
            default: return p.io.file_write;
        }
    };
    std::stable_sort(m.procs.begin(), m.procs.end(), [&](const ProcStat& a, const ProcStat& b) { return key(a) > key(b); });
    return m;
}

// ---------------------------------------------------------------- modes
std::atomic<bool> g_stop{ false };
BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

int fail(const std::string& err, bool json) {
    if (json) write_out("{\"tool\":\"everywho\",\"version\":" + jstr(kVersion) + ",\"error\":" + jstr(err) + "}\n");
    else fprintf(stderr, "everywho: %s\n", err.c_str());
    return 2;
}

void sleep_ms(uint32_t ms) { Sleep(ms); }

int run_snap(const Opts& o) {
    Engine e(o);
    std::string err;
    if (!e.tick(&err)) return fail(err, o.json);
    const double t0 = now_ms();
    sleep_ms(o.sample_ms);
    if (!e.tick(&err)) return fail(err, o.json);
    Snapshot s = e.snap(true, now_ms() - t0);
    if (o.json) write_out(snapshot_json(s, o, e.exited, false) + "\n");
    else write_out(render(s, o, console_width(), false, e.exited, false, false, 0));
    return 0;
}

int run_stamp(const Opts& o) {
    Engine e(o);
    std::string err;
    if (!e.tick(&err)) return fail(err, o.json);
    const double t0 = now_ms();
    sleep_ms(o.sample_ms);
    if (!e.tick(&err)) return fail(err, o.json);
    Snapshot s = e.snap(true, now_ms() - t0);
    write_out((o.json ? stamp_json(s) : stamp_line(s)) + "\n");
    return 0;
}

// watch: the TUI, or NDJSON / stamp / spool streams, one interval at a time
int run_watch(const Opts& o, Opts::Mode stream) {
    Engine e(o);
    std::string err;
    if (!e.tick(&err)) return fail(err, o.json);
    const bool tui = stream == Opts::Mode::Watch && !o.json;
    FILE* spool = nullptr;
    if (stream == Opts::Mode::Spool && !o.spool_file.empty()) {
        spool = _wfopen(widen(o.spool_file).c_str(), L"ab");
        if (!spool) return fail("cannot open spool file " + o.spool_file, false);
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (tui) write_out("\x1b[?1049h\x1b[?25l");
    Opts live = o;
    bool paused = false, frozen = false;
    int window_s = (std::max)(1, o.window_s);
    std::deque<Snapshot> hist;
    Snapshot shown;
    SpoolGate gate;
    int frames = 0;
    double t_prev = now_ms();
    int rc = 0;
    while (!g_stop) {
        sleep_ms(o.interval_ms);
        if (tui) {
            while (_kbhit()) {
                const int k = _getch();
                if (k == 'q' || k == 'Q' || k == 27) { g_stop = true; break; }
                if (k == 's') live.sort = (SortKey)(((int)live.sort + 1) % 5);
                if (k == 'g') live.group = !live.group;
                if (k == 'p') paused = !paused;
                if (k == 'f') frozen = !frozen;
                if (k == '1') window_s = 1;
                if (k == '2') window_s = 10;
                if (k == '3') window_s = 60;
            }
            if (g_stop) break;
        }
        if (paused) continue;
        if (!e.tick(&err)) { rc = fail(err, o.json); break; }
        const double now = now_ms();
        Snapshot s = e.snap(false, now - t_prev);
        t_prev = now;
        if (tui) {
            hist.push_back(s);
            while (hist.size() > 60) hist.pop_front();
            if (!frozen) shown = window_s > 1 ? merge(hist, (size_t)window_s, live.sort) : s;
            std::string frame = "\x1b[H" + render(shown, live, console_width(), true, e.exited, paused, frozen, window_s) + "\x1b[J";
            write_out(frame);
            fflush(stdout);
        } else if (stream == Opts::Mode::Stamp) {
            write_out((o.json ? stamp_json(s) : stamp_line(s)) + "\n");
            fflush(stdout);
        } else if (stream == Opts::Mode::Spool) {
            std::vector<Row> rows = group_rows(s, true);
            if (gate.fire(s, rows, o)) {
                const std::string line = o.lane + "\t" + spool_text(s, rows) + "\n";
                if (spool) { fwrite(line.data(), 1, line.size(), spool); fflush(spool); }
                else { write_out(line); fflush(stdout); }
            }
        } else {
            write_out(snapshot_json(s, o, e.exited, true) + "\n");
            fflush(stdout);
        }
        if (o.frames && ++frames >= o.frames) break;
    }
    if (tui) write_out("\x1b[?25h\x1b[?1049l");
    if (spool) fclose(spool);
    SetConsoleCtrlHandler(on_ctrl, FALSE);
    return rc;
}

int run_where(const Opts& o) {
    std::vector<ProcSample> procs;
    std::string err;
    const bool have = sample_processes(procs, &err);
    IdentityTable who;
    who.enumerate_now();
    std::vector<Identity> ids = who.snapshot();
    uint32_t own = 0, cwd_ok = 0, agents = 0;
    std::wstring me;
    for (const Identity& id : ids)
        if (id.pid == GetCurrentProcessId()) me = id.user;
    for (const Identity& id : ids) {
        if (id.pid <= 4) continue;
        if (!me.empty() && id.user == me) { own++; if (id.cwd_ok) cwd_ok++; }
        if (id.agent.any()) agents++;
    }
    DiskCounters d;
    std::string derr;
    const bool dok = d.open(&derr);
    std::vector<VolumeStat> vols;
    if (dok) { Sleep(600); d.collect(vols, nullptr); }
    const auto disks = volume_disks();
    const std::wstring root = IdentityTable::claude_projects_root();
    const DWORD ra = GetFileAttributesW(root.c_str());
    if (o.json) {
        std::string j = "{\"tool\":\"everywho\",\"version\":" + jstr(kVersion) + ",\"tier\":\"counters\",\"elevated\":" + jb(is_elevated()) +
                        ",\"profile_privilege\":" + jb(has_privilege(L"SeSystemProfilePrivilege")) + ",\"processes\":" + jn(procs.size()) + ",\"own_user\":" + jn(own) + ",\"own_cwd_readable\":" + jn(cwd_ok) +
                        ",\"agents\":" + jn(agents) + ",\"pdh\":" + jb(dok) + ",\"volumes\":[";
        for (size_t i = 0; i < disks.size(); ++i) j += (i ? "," : "") + std::string("{\"letter\":\"") + (char)disks[i].first + "\",\"disk\":" + jn(disks[i].second) + "}";
        j += "],\"projects_root\":" + jw(root) + ",\"projects_root_exists\":" + jb(ra != INVALID_FILE_ATTRIBUTES) + ",\"error\":" + (have ? "null" : jstr(err)) + "}\n";
        write_out(j);
        return have ? 0 : 2;
    }
    printf("tier             counters   (the ETW tier - directories, files, bursts - is Stage 1)\n");
    printf("elevated         %s\n", is_elevated() ? "yes" : "no");
    printf("privilege        SeSystemProfilePrivilege: %s\n", has_privilege(L"SeSystemProfilePrivilege") ? "present" : "absent (not admin; the ETW tier will need --elevate)");
    if (have) printf("process list     %zu processes \xC2\xB7 own user (%s): %u, cwd readable: %u (%.0f %%) \xC2\xB7 agents attributed: %u\n", procs.size(), narrow(me).c_str(), own, cwd_ok, own ? 100.0 * cwd_ok / own : 0.0, agents);
    else printf("process list     FAILED: %s\n", err.c_str());
    printf("disks (PDH)      %s\n", dok ? ssprintf("ok \xC2\xB7 %zu instances", vols.size()).c_str() : ("FAILED: " + derr).c_str());
    std::string vl;
    for (const auto& [L, n] : disks) vl += ssprintf("%c: \xE2\x86\x92 disk %u   ", (char)L, n);
    printf("volumes          %s\n", vl.empty() ? "(none)" : vl.c_str());
    printf("projects root    %s (%s)\n", narrow(root).c_str(), ra != INVALID_FILE_ATTRIBUTES ? "exists" : "missing: the cwd rule has nothing to match");
    return have ? 0 : 2;
}

// ---------------------------------------------------------------- --elevate: relaunch through UAC, read back --out
int run_elevate(int argc, wchar_t** argv, const Opts& o) {
    if (o.out_file.empty()) {
        fprintf(stderr, "everywho: --elevate needs --out FILE (an elevated process cannot write to this console)\n");
        return 1;
    }
    std::wstring args;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--elevate") continue;
        if (!args.empty()) args += L' ';
        if (a.find(L' ') != std::wstring::npos || a.empty()) args += L'"' + a + L'"';
        else args += a;
    }
    const std::wstring exe = exe_path();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        fprintf(stderr, "everywho: elevation declined or failed (%lu)\n", (unsigned long)GetLastError());
        return 4;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    FILE* f = _wfopen(widen(o.out_file).c_str(), L"rb");
    if (f) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
        fclose(f);
    }
    return (int)code;
}

// ---------------------------------------------------------------- MCP (io_snapshot, io_stamp)
void mcp_result(const std::string& id, const std::string& result) { write_out("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}\n"); fflush(stdout); }
std::string mcp_text(const std::string& text, bool is_error) { return "{\"content\":[{\"type\":\"text\",\"text\":" + jstr(text) + "}],\"isError\":" + jb(is_error) + "}"; }

const char* kTools =
    "[{\"name\":\"io_snapshot\",\"description\":\"Who is doing I/O right now: every process's read/write bytes and ops over a sample window, "
    "with identity (image, command line, cwd, user, agent harness/project), plus per-disk rates, queue and busy %. Counters tier (no elevation): "
    "who and how much; directories and files need the ETW tier (Stage 1).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"sample_ms\":{\"type\":\"integer\",\"default\":2000},\"top\":{\"type\":\"integer\",\"default\":12},"
    "\"pid\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\",\"description\":\"process name glob, e.g. node*\"},"
    "\"agents\":{\"type\":\"boolean\",\"description\":\"only processes attributed to a coding harness\"},"
    "\"sort\":{\"type\":\"string\",\"enum\":[\"write\",\"read\",\"ops\",\"name\",\"pid\"]}}}},"
    "{\"name\":\"io_stamp\",\"description\":\"One-line I/O receipt: per-disk MB/s, queue, busy %, and the top writers with their harness/project.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sample_ms\":{\"type\":\"integer\",\"default\":2000},\"json\":{\"type\":\"boolean\",\"default\":false}}}}]";

int run_mcp(const Opts& base) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        JV req;
        if (!jparse(line, req) || req.t != JV::Obj) continue;
        const JV* idv = req.get("id");
        std::string id = "null";
        if (idv) {
            if (idv->t == JV::Num) id = ssprintf("%.0f", idv->n);
            else if (idv->t == JV::Str) id = jstr(idv->s);
        }
        const std::string method = req.str("method");
        if (method == "initialize") {
            mcp_result(id, "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"everywho\",\"version\":" + jstr(kVersion) + "}}");
        } else if (method == "ping") {
            mcp_result(id, "{}");
        } else if (method == "tools/list") {
            mcp_result(id, std::string("{\"tools\":") + kTools + "}");
        } else if (method == "tools/call") {
            const JV* params = req.get("params");
            const std::string name = params ? params->str("name") : "";
            const JV* a = params ? params->get("arguments") : nullptr;
            Opts o = base;
            o.json = true;
            o.sample_ms = (uint32_t)(a ? a->num("sample_ms", 2000) : 2000);
            if (o.sample_ms < 200) o.sample_ms = 200;
            if (o.sample_ms > 60000) o.sample_ms = 60000;
            if (a) {
                o.top = (int)a->num("top", o.top);
                if (a->get("pid") && a->get("pid")->t == JV::Num) o.pids.push_back((uint32_t)a->num("pid"));
                if (!a->str("name").empty()) o.names.push_back(widen(a->str("name")));
                o.agents_only = a->flag("agents", false);
                SortKey k = SortKey::Write;
                if (!a->str("sort").empty() && parse_sort(a->str("sort"), k)) o.sort = k;
            }
            if (name == "io_snapshot" || name == "io_stamp") {
                Engine e(o);
                std::string err;
                if (!e.tick(&err)) { mcp_result(id, mcp_text(err, true)); continue; }
                const double t0 = now_ms();
                sleep_ms(o.sample_ms);
                if (!e.tick(&err)) { mcp_result(id, mcp_text(err, true)); continue; }
                Snapshot s = e.snap(true, now_ms() - t0);
                if (name == "io_snapshot") mcp_result(id, mcp_text(snapshot_json(s, o, e.exited, false), false));
                else mcp_result(id, mcp_text(a && a->flag("json", false) ? stamp_json(s) : stamp_line(s), false));
            } else {
                mcp_result(id, mcp_text("unknown tool: " + name, true));
            }
        } else if (idv) {
            write_out("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32601,\"message\":\"method not found\"}}\n");
            fflush(stdout);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- selftest
int g_pass = 0, g_fail = 0;
void check(bool ok, const std::string& what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? g_pass : g_fail)++;
}
std::wstring w_(std::string_view s) { return widen(s); }

int run_selftest(const Opts& opts) {
    printf("everywho %s --selftest\n", kVersion);
    // ---- formatting
    check(fmt_count(1234567) == "1,234,567" && fmt_count(0) == "0", "fmt_count");
    check(human_bytes(1536) == "1.5 KB" && human_bytes(0) == "0 B" && human_bytes(kUnknown64) == "-", "human_bytes");
    check(human_rate(48.3 * 1048576) == "48 MB/s" && human_rate(100) == "idle" && human_rate(2.5 * 1024) == "2.5 KB/s", "human_rate");
    check(display_width("ab\xE4\xB8\xAD") == 4 && trunc_middle("abcdefghijklmnop", 9).size() > 0 && display_width(trunc_middle("abcdefghijklmnop", 9)) <= 9, "display width / truncation");
    // ---- rates
    {
        Ring<int, 4> r;
        for (int i = 0; i < 6; ++i) r.push(i);
        check(r.size() == 4 && r.at(0) == 2 && r.at(3) == 5, "ring keeps the newest");
        History h;
        for (int i = 0; i < 20; ++i) { RatePoint p; p.file_write_bps = 10.0f; h.push(p); }
        check(std::fabs(h.window(10).file_write_bps - 10.0f) < 0.01f, "history window mean");
        Delta64 d;
        check(d.step(100) == 0 && d.step(150) == 50 && d.step(20) == 0 && d.step(25) == 5, "delta64 primes and survives a reset");
        Percentiles p;
        for (uint32_t i = 1; i <= 100; ++i) p.push(i * 1000);
        check(std::fabs(p.p(0.5) - 50.0) <= 1.5 && std::fabs(p.p(0.95) - 95.0) <= 1.5, "percentiles");
    }
    // ---- tape
    {
        Tape t;
        parse_tape("C:/a/b.md\r\nc:\\A\\B.MD\n# comment\n\n{\"path\":\"D:\\\\x\\\\y.txt\"}\n{\"type\":\"path\",\"file\":\"E:\\\\z\"}\n{\"nope\":1}\n{bad\n", t, &w_);
        check(t.paths.size() == 3 && t.paths[0] == L"C:\\a\\b.md" && t.paths[1] == L"D:\\x\\y.txt" && t.paths[2] == L"E:\\z" && t.records == 6 && t.dups == 1 && t.bad == 2,
              "tape: lines, CRLF, JSONL path/file, comments, duplicates, bad records");
        check(norm_key(L"\\\\?\\C:/A/B\\") == L"c:\\a\\b" && norm_key(L"C:\\") == L"c:\\", "norm_key");
    }
    // ---- device paths (fixture table)
    {
        DevicePaths dp;
        dp.set_table({ { L"\\Device\\HarddiskVolume3\\", L"C:\\" }, { L"\\Device\\HarddiskVolume5\\", L"D:\\" } }, L"C:\\Windows\\");
        std::wstring d;
        check(dp.to_dos(L"\\Device\\HarddiskVolume3\\Users\\x", d) && d == L"C:\\Users\\x", "nt → dos: volume");
        check(dp.to_dos(L"\\??\\D:\\a", d) && d == L"D:\\a", "nt → dos: \\??\\");
        check(dp.to_dos(L"\\SystemRoot\\notepad.exe", d) && d == L"C:\\Windows\\notepad.exe", "nt → dos: SystemRoot");
        check(dp.to_dos(L"\\Device\\Mup\\s\\p\\f", d) && d == L"\\\\s\\p\\f", "nt → dos: Mup");
        check(!dp.to_dos(L"\\Device\\NamedPipe\\x", d) && dp.is_device(L"\\Device\\NamedPipe\\x"), "nt → dos: a pipe is a device");
        check(!dp.to_dos(L"\\Device\\HarddiskVolume9\\x", d), "nt → dos: an unknown volume is not guessed");
    }
    // ---- identity rules
    {
        Agent a;
        check(IdentityTable::classify_image(L"claude.exe", L"C:\\Users\\user\\AppData\\Roaming\\Claude\\claude-code\\2.1.255\\claude.exe",
                                            L"C:\\Users\\user\\AppData\\Roaming\\Claude\\claude-code\\2.1.255\\claude.exe --output-format stream-json --verbose --resume=6ebfe793-4649-4f7d-862d-a3674fb1328c --allowedTools x", a) &&
                  a.harness == "claude-code" && a.session == L"6ebfe793-4649-4f7d-862d-a3674fb1328c" && std::string(a.rule) == "cmdline",
              "rule image: the Claude Code CLI, session from --resume=");
        check(IdentityTable::classify_image(L"claude.exe", L"", L"claude --session-id 11111111-2222-3333-4444-555555555555 -p hi", a) && a.session == L"11111111-2222-3333-4444-555555555555",
              "rule image: session from --session-id");
        check(!IdentityTable::classify_image(L"Claude.exe", L"C:\\Program Files\\WindowsApps\\Claude_1.40609.1.0_x64__pzs8sxrjxfjjc\\app\\Claude.exe",
                                             L"\"C:\\Program Files\\WindowsApps\\Claude_1.40609.1.0_x64__pzs8sxrjxfjjc\\app\\Claude.exe\" --type=renderer --user-data-dir=\"C:\\Users\\user\\AppData\\Roaming\\Claude\"", a),
              "rule image: the Claude desktop app is not a harness");
        check(!IdentityTable::classify_image(L"Claude.exe", L"C:\\Program Files\\WindowsApps\\Claude_1.40609.1.0_x64__pzs8sxrjxfjjc\\app\\Claude.exe", L"\"C:\\...\\Claude.exe\"", a),
              "rule image: nor its main process");
        check(IdentityTable::classify_image(L"claude.exe", L"", L"", a) && a.harness == "claude-code", "rule image: an unenrichable claude.exe is assumed to be the CLI");
        check(IdentityTable::classify_image(L"node.exe", L"", L"node C:\\Users\\u\\AppData\\Roaming\\npm\\node_modules\\@anthropic-ai\\claude-code\\cli.js", a) && a.harness == "claude-code", "rule image: node + claude");
        check(!IdentityTable::classify_image(L"node.exe", L"", L"node server.js", a), "rule image: plain node is not an agent");
        check(IdentityTable::classify_image(L"python.exe", L"", L"python C:\\deepseek-harness-master\\run.py", a) && a.harness == "dsh", "rule image: dsh");
        check(IdentityTable::classify_tape(L"C:\\Users\\user\\.claude\\projects\\C--facet\\6ebfe793-4649-4f7d-862d-a3674fb1328c.jsonl", a) && a.harness == "claude-code" &&
                  a.project == L"C--facet" && a.session == L"6ebfe793-4649-4f7d-862d-a3674fb1328c" && a.subagent.empty() && std::string(a.rule) == "tape",
              "rule tape: session");
        check(IdentityTable::classify_tape(L"C:\\Users\\user\\.claude\\projects\\C--\\6ebfe793-4649-4f7d-862d-a3674fb1328c\\subagents\\agent-a1b2.jsonl", a) &&
                  a.session == L"6ebfe793-4649-4f7d-862d-a3674fb1328c" && a.subagent == L"agent-a1b2" && a.project == L"C--",
              "rule tape: subagent");
        check(IdentityTable::classify_tape(L"C:\\Users\\user\\.dsh\\sessions\\proj\\abc123\\session.jsonl.zstd", a) && a.harness == "dsh" && a.session == L"abc123", "rule tape: dsh");
        check(!IdentityTable::classify_tape(L"C:\\Users\\user\\notes.md", a), "rule tape: an ordinary file is not a tape");
        check(IdentityTable::project_slug(L"C:\\facet") == L"C--facet" && IdentityTable::project_slug(L"C:\\Users\\user\\.claude") == L"C--Users-user--claude", "project slug");
        check(IdentityTable::classify_kind(4, L"System", 0, false) == Kind::System && IdentityTable::classify_kind(900, L"vmmemWSL", 0, false) == Kind::Wsl &&
                  IdentityTable::classify_kind(901, L"svchost.exe", 0, false) == Kind::Service && IdentityTable::classify_kind(902, L"node.exe", 1, true) == Kind::Agent &&
                  IdentityTable::classify_kind(903, L"chrome.exe", 1, false) == Kind::App,
              "kind classification");
        check(name_glob(L"node*", L"node.exe") && name_glob(L"*claude*", L"Claude.exe") && name_glob(L"python.exe", L"PYTHON.EXE") && !name_glob(L"node*", L"code.exe") && name_glob(L"?ode.exe", L"node.exe"),
              "name glob");
        // the inherit rule on a fixture tree: claude.exe (100) → cmd.exe (101) → cl.exe (102); 103 is in another session
        IdentityTable t;
        t.on_process_start(100, 1, 1, "claude.exe", L"claude --output-format stream-json --resume=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", true);
        t.on_process_start(101, 100, 1, "cmd.exe", L"cmd /c build", true);
        t.on_process_start(102, 101, 1, "cl.exe", L"cl a.cpp", true);
        t.on_process_start(103, 100, 0, "svchost.exe", L"", true);
        t.enrich_pending();
        t.attribute_by_ancestry();
        const Identity* c = t.find(102);
        const Identity* s = t.find(103);
        check(t.find(100) && t.find(100)->agent.harness == "claude-code" && t.find(100)->agent.session == L"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" && c && c->agent.harness == "claude-code" &&
                  c->agent.session == L"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" && std::string(c->agent.rule) == "inherit" && s && !s->agent.any(),
              "inherit rule: grandchild inherits harness and session, another session does not");
        t.on_tape_write(102, L"C:\\Users\\user\\.claude\\projects\\C--x\\11111111-2222-3333-4444-555555555555.jsonl");
        check(t.find(102)->agent.session == L"11111111-2222-3333-4444-555555555555" && std::string(t.find(102)->agent.rule) == "tape", "tape write overrides the inherited attribution");
    }
    // ---- JSON parser
    {
        JV v;
        check(jparse("{\"a\":[1,2,{\"b\":\"x\\u0041\\n\"}],\"c\":true,\"d\":null,\"e\":-1.5e2}", v) && v.t == JV::Obj && v.get("a")->arr.size() == 3 && v.get("a")->arr[2].str("b") == "xA\n" && v.flag("c") &&
                  v.num("e") == -150.0,
              "mini JSON parser");
    }
    // ---- live: the process list and this process
    {
        std::vector<ProcSample> ps;
        std::string err;
        const bool ok = sample_processes(ps, &err);
        const uint32_t me = GetCurrentProcessId();
        const ProcSample* self = nullptr;
        for (const auto& p : ps)
            if (p.pid == me) self = &p;
        check(ok && ps.size() > 20 && self && self->name == L"everywho.exe" && self->create_ft > 0, ssprintf("process list: %zu processes, this one listed with its counters", ps.size()));
        IdentityTable who;
        check(who.enumerate_now(&err), "identity: enumerate + enrich");
        const Identity* id = who.find(me);
        wchar_t cwd[MAX_PATH * 2];
        GetCurrentDirectoryW((DWORD)(sizeof cwd / sizeof cwd[0]), cwd);
        std::wstring cwd_s(cwd);
        while (cwd_s.size() > 3 && cwd_s.back() == L'\\') cwd_s.pop_back();
        check(id && id->cwd_ok && _wcsicmp(id->cwd.c_str(), cwd_s.c_str()) == 0, "identity: this process's cwd is read from its PEB");
        check(id && id->cmdline.find(L"selftest") != std::wstring::npos && !id->image.empty() && !id->user.empty(), "identity: command line, image path, user");
        // the Stage 0 falsifier: own-user processes whose cwd we can read (top 20 by cumulative I/O)
        std::vector<Identity> ids = who.snapshot();
        std::vector<std::pair<uint64_t, const Identity*>> own;
        for (const Identity& i : ids)
            if (i.pid > 4 && id && i.user == id->user)
                for (const auto& p : ps)
                    if (p.pid == i.pid) own.emplace_back(p.read_bytes + p.write_bytes, &i);
        std::sort(own.begin(), own.end(), [](auto& a, auto& b) { return a.first > b.first; });
        uint32_t n = 0, okc = 0;
        for (const auto& [bytes, i] : own) { if (n >= 20) break; n++; if (i->cwd_ok) okc++; }
        check(n > 0 && okc * 100 >= n * 95, ssprintf("falsifier: cwd readable for %u of the top %u own-user I/O processes (%.0f %%)", okc, n, n ? 100.0 * okc / n : 0.0));
        uint32_t agents = 0;
        for (const Identity& i : ids)
            if (i.agent.any()) agents++;
        printf("  info: %u processes carry an agent attribution\n", agents);
    }
    // ---- live: counters delta
    {
        Opts o = opts;
        o.quiet = true;
        Engine e(o);
        std::string err;
        check(e.tick(&err), "counters: first tick (prime)");
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring path = std::wstring(tmp) + L"everywho-selftest-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        std::vector<char> block(65536, 'x');
        DWORD wr = 0;
        for (int i = 0; i < 128 && h != INVALID_HANDLE_VALUE; ++i) WriteFile(h, block.data(), (DWORD)block.size(), &wr, nullptr);   // 8 MB
        FlushFileBuffers(h);
        Sleep(1100);
        check(e.tick(&err), "counters: second tick");
        Snapshot s = e.snap(true, 1100);
        uint64_t mine = 0;
        for (const ProcStat& p : s.procs)
            if (p.id.pid == GetCurrentProcessId()) mine = p.io.file_write;
        check(mine >= 8u * 1048576u, ssprintf("counters: this process wrote %s in the window (>= 8 MB)", human_bytes(mine).c_str()));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        check(e.disks_ok, "disks: PDH query opened (" + (e.disks_ok ? std::string("PhysicalDisk counters") : e.disk_err) + ")");
        bool lettered = false;
        for (const VolumeStat& v : s.volumes)
            if (v.letter) lettered = true;
        check(!e.disks_ok || (!s.volumes.empty() && lettered), ssprintf("disks: %zu instances with drive letters", s.volumes.size()));
        JV v;
        const std::string j = snapshot_json(s, o, e.exited, false);
        check(jparse(j, v) && v.get("processes") && v.get("processes")->t == JV::Arr && v.str("tier") == "counters", "snapshot JSON parses back");
        const std::string st = stamp_line(s);
        check(st.rfind("io_stamp t=", 0) == 0 && st.find(" top=") != std::string::npos, "stamp line shape");
    }
    printf("SELFTEST: %s\n", g_fail ? ssprintf("%d FAILED, %d passed", g_fail, g_pass).c_str() : "ALL PASS");
    return g_fail ? 3 : 0;
}

// ---------------------------------------------------------------- help
void print_help() {
    printf(R"(everywho %s - who is touching what, right now   (Stage 0: the counters tier)

  everywho                  a 3-second sample, then the report: who read / wrote how much, by process
  everywho -w               live TUI   q quit . s sort . g group . p pause . f freeze . 1/2/3 window
  everywho -j               one JSON snapshot        everywho -j -w [-n MS] [--frames N] = NDJSON stream
  everywho --stamp          one-line receipt: MB/s per disk, queue, busy, top writers  (--json for the object)
  everywho --spool          change-gated "lane<TAB>text" stream (lane io) for a log tailer
  everywho --mcp            MCP stdio server: io_snapshot, io_stamp
  everywho --where          tier, elevation, privilege, the process list, PDH, volumes
  everywho --selftest       formatting, rates, tapes, name mapping, identity rules, live counters
  everywho --make-icon F    write the app icon as a .ico     everywho --shortcut [desktop]  Start Menu entry

WINDOW      --sample-ms N (3000)  -n, --interval MS (1000)  --frames N  --window S (TUI: 1 | 10 | 60)
FILTERS     --pid N  --name GLOB  --agents        (--dir, -x, --files-from, --ops: the ETW tier, Stage 1)
SHAPE       -s, --sort write|read|ops|name|pid   --top N (12)  --no-group  --min-mb X (0.05)  --plain  -q
SPOOL       --spool-file P  --lane S (io)  --gate-mb X (8)  --gate-pct X (25)
ELEVATION   --elevate --out FILE   (relaunch through UAC; the report comes back through FILE)

Not in this build (the ETW tier, Stage 1+): --paths, --files-from, --open, --gui, --tier etw, --verify-layouts.
Exit codes: 0 ok . 1 arguments . 2 collector error (JSON still emitted) . 3 selftest failed . 4 needs elevation
)", kVersion);
}

std::wstring need_str(int argc, wchar_t** argv, int& i, const char* flag) {
    if (i + 1 >= argc) {
        fprintf(stderr, "everywho: %s needs a value\n", flag);
        exit(1);
    }
    return argv[++i];
}
int need_int(int argc, wchar_t** argv, int& i, const char* flag) { return _wtoi(need_str(argc, argv, i, flag).c_str()); }

int app_main(int argc, wchar_t** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring aw = argv[i];
        const std::string a = narrow(aw);
        if (a == "-h" || a == "--help" || a == "/?") o.mode = Opts::Mode::Help;
        else if (a == "-v" || a == "--version") o.mode = Opts::Mode::Version;
        else if (a == "-w" || a == "--watch") { if (o.mode == Opts::Mode::Auto) o.mode = Opts::Mode::Watch; }
        else if (a == "-j" || a == "--json") o.json = true;
        else if (a == "--stamp") o.mode = Opts::Mode::Stamp;
        else if (a == "--spool") o.mode = Opts::Mode::Spool;
        else if (a == "--mcp") o.mode = Opts::Mode::Mcp;
        else if (a == "--where") o.mode = Opts::Mode::Where;
        else if (a == "--selftest") o.mode = Opts::Mode::Selftest;
        else if (a == "--gui") o.mode = Opts::Mode::Gui;
        else if (a == "--paths") o.mode = Opts::Mode::Paths;
        else if (a == "--open") { o.mode = Opts::Mode::Open; o.open_path = need_str(argc, argv, i, "--open"); }
        else if (a == "--make-icon") { o.mode = Opts::Mode::MakeIcon; o.out_file = narrow(need_str(argc, argv, i, "--make-icon")); }
        else if (a == "--shortcut") {
            o.mode = Opts::Mode::Shortcut;
            if (i + 1 < argc) { const std::string v = narrow(argv[i + 1]); if (v == "desktop" || v == "startmenu") { o.out_file = v; ++i; } }
        }
        else if (a == "--sample-ms") o.sample_ms = (uint32_t)(std::max)(100, need_int(argc, argv, i, "--sample-ms"));
        else if (a == "-n" || a == "--interval") o.interval_ms = (uint32_t)(std::max)(100, need_int(argc, argv, i, "--interval"));
        else if (a == "--frames") o.frames = need_int(argc, argv, i, "--frames");
        else if (a == "--window") o.window_s = need_int(argc, argv, i, "--window");
        else if (a == "--tier") { const std::string v = narrow(need_str(argc, argv, i, "--tier")); o.tier = v == "etw" ? Tier::Etw : v == "counters" ? Tier::Counters : Tier::Auto; }
        else if (a == "--elevate") o.elevate = true;
        else if (a == "--out") o.out_file = narrow(need_str(argc, argv, i, "--out"));
        else if (a == "--verify-layouts") o.verify_layouts = true;
        else if (a == "--pid") o.pids.push_back((uint32_t)need_int(argc, argv, i, "--pid"));
        else if (a == "--name") o.names.push_back(need_str(argc, argv, i, "--name"));
        else if (a == "--dir") o.dirs.push_back(need_str(argc, argv, i, "--dir"));
        else if (a == "-x" || a == "--exclude") o.exclude.push_back(need_str(argc, argv, i, "--exclude"));
        else if (a == "--files-from") o.files_from = narrow(need_str(argc, argv, i, "--files-from"));
        else if (a == "--agents") o.agents_only = true;
        else if (a == "-s" || a == "--sort") { const std::string k = narrow(need_str(argc, argv, i, "--sort")); if (!parse_sort(k, o.sort)) { fprintf(stderr, "everywho: unknown sort '%s'\n", k.c_str()); return 1; } }
        else if (a == "-g" || a == "--group") o.group = true;
        else if (a == "--no-group") o.group = false;
        else if (a == "--top") o.top = (std::max)(1, need_int(argc, argv, i, "--top"));
        else if (a == "--depth") o.depth = need_int(argc, argv, i, "--depth");
        else if (a == "--min-mb") o.min_mb = _wtof(need_str(argc, argv, i, "--min-mb").c_str());
        else if (a == "--raw") o.raw = true;
        else if (a == "--plain") o.plain = true;
        else if (a == "-q" || a == "--quiet") o.quiet = true;
        else if (a == "--spool-file") o.spool_file = narrow(need_str(argc, argv, i, "--spool-file"));
        else if (a == "--lane") o.lane = narrow(need_str(argc, argv, i, "--lane"));
        else if (a == "--gate-mb") o.gate_mb = _wtof(need_str(argc, argv, i, "--gate-mb").c_str());
        else if (a == "--gate-pct") o.gate_pct = _wtof(need_str(argc, argv, i, "--gate-pct").c_str());
        else if (a == "--shot") o.shot = narrow(need_str(argc, argv, i, "--shot"));
        else if (a == "--ini") o.ini = narrow(need_str(argc, argv, i, "--ini"));
        else if (a == "--no-activate") o.no_activate = true;
        else if (a == "-0" || a == "--null") o.nul = true;
        else {
            fprintf(stderr, "everywho: unknown option '%s' (-h for help)\n", a.c_str());
            return 1;
        }
    }
    if (o.mode == Opts::Mode::Auto) o.mode = Opts::Mode::Snap;
    if (o.elevate) return run_elevate(argc, argv, o);
    if (!o.out_file.empty() && o.mode != Opts::Mode::MakeIcon && o.mode != Opts::Mode::Shortcut) {
        if (!_wfreopen(widen(o.out_file).c_str(), L"wb", stdout)) {
            fprintf(stderr, "everywho: cannot write %s\n", o.out_file.c_str());
            return 1;
        }
    }
    const bool machine = o.json || o.mode == Opts::Mode::Mcp || o.mode == Opts::Mode::Spool || o.mode == Opts::Mode::Stamp;
    g_ansi = !o.plain && !machine && stdout_is_console() && console_setup(true);
    if (!g_ansi) SetConsoleOutputCP(CP_UTF8);
    if (machine || !stdout_is_console()) _setmode(_fileno(stdout), _O_BINARY);   // LF-only for pipes and files

    switch (o.mode) {
        case Opts::Mode::Help: print_help(); return 0;
        case Opts::Mode::Version: printf("everywho %s\n", kVersion); return 0;
        case Opts::Mode::Snap: return run_snap(o);
        case Opts::Mode::Watch: return run_watch(o, Opts::Mode::Watch);
        case Opts::Mode::Stamp: return o.frames || o.interval_ms != 1000 ? run_watch(o, Opts::Mode::Stamp) : run_stamp(o);
        case Opts::Mode::Spool: return run_watch(o, Opts::Mode::Spool);
        case Opts::Mode::Mcp: return run_mcp(o);
        case Opts::Mode::Where: return run_where(o);
        case Opts::Mode::Selftest: return run_selftest(o);
        case Opts::Mode::MakeIcon: return write_icon_file(o.out_file);
        case Opts::Mode::Shortcut: return make_shortcut(o.out_file);
        case Opts::Mode::Gui:
        case Opts::Mode::Paths:
        case Opts::Mode::Open:
        default:
            fprintf(stderr, "everywho %s: that mode needs the ETW tier or the window, which arrive in Stage 1-3 (docs/ROADMAP.md); this build is the counters tier\n", kVersion);
            return 4;
    }
}

}  // namespace
}  // namespace everywho

int wmain(int argc, wchar_t** argv) { return everywho::app_main(argc, argv); }
