// everywho · where.cpp — the fold. Stage 0: per-process and per-volume accounting behind the
// full API; the directory trie, the file table and the bursts land with the ETW tier (Stage 2).
#include "where.h"

#include "sys.h"
#include "tape.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace everywho {

void IoCounters::add(const IoCounters& o) {
    file_read += o.file_read;
    file_write += o.file_write;
    other_bytes += o.other_bytes;
    disk_read += o.disk_read;
    disk_write += o.disk_write;
    attributed_write += o.attributed_write;
    unnamed += o.unnamed;
    for (int i = 0; i < kOpKinds; ++i) ops[i] += o.ops[i];
    ops_other += o.ops_other;
    files += o.files;
    new_files += o.new_files;
}

uint64_t IoCounters::total_ops() const {
    uint64_t n = ops_other;
    for (int i = 0; i < kOpKinds; ++i) n += ops[i];
    return n;
}

struct Fold::Impl {
    FoldConfig cfg;
    IdentityTable* who = nullptr;
    std::unordered_map<uint32_t, IoCounters> interval, total;
    std::vector<VolumeStat> volumes;
    std::vector<std::wstring> paths;   // Stage 2: first-touch order
    uint32_t seen = 0;

    bool pass(uint32_t pid, const Identity* id) const {
        if (!cfg.pids.empty() && std::find(cfg.pids.begin(), cfg.pids.end(), pid) == cfg.pids.end()) return false;
        if (!cfg.name_globs.empty()) {
            bool any = false;
            for (const auto& g : cfg.name_globs)
                if (id && name_glob(g, id->name)) { any = true; break; }
            if (!any) return false;
        }
        if (cfg.agents_only && !(id && id->agent.any())) return false;
        return true;
    }
};

Fold::Fold(const FoldConfig& cfg, IdentityTable& who) : p_(new Impl) {
    p_->cfg = cfg;
    p_->who = &who;
}
Fold::~Fold() { delete p_; }

void Fold::add(const RawEvent& ev, std::wstring_view dos_path) {
    (void)dos_path;   // Stage 2: the trie and the file table
    if (ev.pid == 0xFFFFFFFFu) return;
    IoCounters& c = p_->interval[ev.pid];
    IoCounters& t = p_->total[ev.pid];
    auto bump = [&](int op) { c.ops[op]++; t.ops[op]++; };
    switch (ev.kind) {
        case EvKind::FileRead: c.file_read += ev.size; t.file_read += ev.size; bump(1); break;
        case EvKind::FileWrite: c.file_write += ev.size; t.file_write += ev.size; bump(2); break;
        case EvKind::FileCreate: bump(0); break;
        case EvKind::FileDelete: bump(3); break;
        case EvKind::FileRename: bump(4); break;
        case EvKind::FileFlush: bump(5); break;
        case EvKind::FileDirEnum: bump(6); break;
        case EvKind::FileSetInfo: bump(7); break;
        case EvKind::DiskRead: c.disk_read += ev.size; t.disk_read += ev.size; break;
        case EvKind::DiskWrite: c.disk_write += ev.size; t.disk_write += ev.size; break;
        default: break;
    }
}

void Fold::add_process_counters(uint32_t pid, const IoCounters& d) {
    p_->interval[pid].add(d);
    p_->total[pid].add(d);
}

void Fold::add_volume(const VolumeStat& v) {
    for (auto& x : p_->volumes)
        if (x.instance == v.instance) { x = v; return; }
    p_->volumes.push_back(v);
    std::sort(p_->volumes.begin(), p_->volumes.end(), [](const VolumeStat& a, const VolumeStat& b) { return a.disk < b.disk; });
}

void Fold::set_processes_seen(uint32_t n) { p_->seen = n; }

Snapshot Fold::snapshot(SortKey sort, bool cumulative, uint64_t window_ms) {
    Snapshot s;
    s.wall_ms = (now_filetime() - 116444736000000000ull) / 10000ull;   // Unix ms
    s.window_ms = window_ms;
    s.volumes = p_->volumes;
    s.processes_seen = p_->seen;
    const auto& src = cumulative ? p_->total : p_->interval;
    for (const auto& [pid, io] : src) {
        if (io.empty()) continue;
        const Identity* id = p_->who->find(pid);
        if (!p_->pass(pid, id)) continue;
        ProcStat ps;
        if (id) {
            ps.id = *id;
        } else {
            ps.id.pid = pid;
            ps.id.name = L"(exited)";
            ps.id.exited = true;
        }
        ps.io = io;
        s.total.add(io);
        s.procs.push_back(std::move(ps));
    }
    s.procs_total = (uint32_t)s.procs.size();
    auto key = [sort](const ProcStat& p) -> uint64_t {
        switch (sort) {
            case SortKey::Read: return p.io.file_read;
            case SortKey::Disk: return p.io.disk_read + p.io.disk_write;
            case SortKey::Ops: return p.io.total_ops();
            case SortKey::Files: return p.io.files;
            case SortKey::NewFiles: return p.io.new_files;
            default: return p.io.file_write;
        }
    };
    std::stable_sort(s.procs.begin(), s.procs.end(), [&](const ProcStat& a, const ProcStat& b) {
        if (sort == SortKey::Name) {
            std::wstring an = a.id.name, bn = b.id.name;
            for (auto& c : an) c = (wchar_t)towlower(c);
            for (auto& c : bn) c = (wchar_t)towlower(c);
            if (an != bn) return an < bn;
            return a.id.pid < b.id.pid;
        }
        if (sort == SortKey::Pid) return a.id.pid < b.id.pid;
        const uint64_t ka = key(a), kb = key(b);
        if (ka != kb) return ka > kb;
        const uint64_t ta = a.io.file_write + a.io.file_read, tb = b.io.file_write + b.io.file_read;
        if (ta != tb) return ta > tb;
        return a.id.pid < b.id.pid;
    });
    if (s.procs.size() > p_->cfg.top_procs) s.procs.resize(p_->cfg.top_procs);
    p_->interval.clear();
    return s;
}

std::vector<std::wstring> Fold::drain_paths() {
    std::vector<std::wstring> out;
    out.swap(p_->paths);
    return out;
}

size_t Fold::nodes() const { return 0; }
size_t Fold::files() const { return 0; }
size_t Fold::bytes_in_use() const { return (p_->interval.size() + p_->total.size()) * (sizeof(IoCounters) + 32); }

// ---- NT → DOS ----

DevicePaths::DevicePaths() = default;

void DevicePaths::refresh() {
    std::vector<std::pair<std::wstring, std::wstring>> m;
    wchar_t target[1024];
    for (wchar_t L = L'A'; L <= L'Z'; ++L) {
        const wchar_t drive[] = { L, L':', 0 };
        if (!QueryDosDeviceW(drive, target, 1024)) continue;
        std::wstring t(target);
        if (t.empty()) continue;
        m.emplace_back(t + L"\\", std::wstring(drive) + L"\\");
    }
    wchar_t win[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring root = n ? std::wstring(win, n) : L"C:\\Windows";
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    set_table(std::move(m), root);
}

void DevicePaths::set_table(std::vector<std::pair<std::wstring, std::wstring>> nt_to_dos, std::wstring system_root) {
    map_ = std::move(nt_to_dos);
    system_root_ = std::move(system_root);
    std::sort(map_.begin(), map_.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}

namespace {
bool iprefix(std::wstring_view s, std::wstring_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (towlower(s[i]) != towlower(p[i])) return false;
    return true;
}
}  // namespace

bool DevicePaths::to_dos(std::wstring_view nt, std::wstring& dos) const {
    if (iprefix(nt, L"\\??\\")) {
        dos = std::wstring(nt.substr(4));
        return true;
    }
    if (iprefix(nt, L"\\SystemRoot\\")) {
        dos = system_root_ + std::wstring(nt.substr(12));
        return true;
    }
    if (iprefix(nt, L"\\Device\\Mup\\")) {
        dos = L"\\\\" + std::wstring(nt.substr(12));
        return true;
    }
    if (is_device(nt)) return false;
    for (const auto& [ntp, dosp] : map_) {
        if (iprefix(nt, ntp)) {
            dos = dosp + std::wstring(nt.substr(ntp.size()));
            return true;
        }
        // the bare device (a volume open), no trailing separator
        if (nt.size() + 1 == ntp.size() && iprefix(ntp, nt)) {
            dos = dosp;
            return true;
        }
    }
    return false;
}

bool DevicePaths::is_device(std::wstring_view nt) const {
    static const wchar_t* kDevices[] = {
        L"\\Device\\NamedPipe", L"\\Device\\Mailslot", L"\\Device\\Afd", L"\\Device\\Null", L"\\Device\\Tcp", L"\\Device\\Udp",
        L"\\Device\\HarddiskVolumeShadowCopy", L"\\Device\\ConDrv", L"\\Device\\DeviceApi", L"\\Device\\Beep", L"\\Device\\KsecDD",
        L"\\Device\\CNG", L"\\Device\\Nsi", L"\\Device\\WMIDataDevice", L"\\Device\\PhysicalMemory", L"\\Device\\vmci",
    };
    for (const wchar_t* d : kDevices)
        if (iprefix(nt, d)) return true;
    return false;
}

std::vector<DirLine> dir_lines(const Snapshot& s, int top, int depth, uint64_t fold_threshold_bytes) {
    (void)s; (void)top; (void)depth; (void)fold_threshold_bytes;
    return {};   // Stage 2
}

}  // namespace everywho
