// everywho · who.cpp — identity: the kernel's process list, user-mode enrichment, the rules.
// Enrichment opens each new pid once (QUERY_LIMITED_INFORMATION, plus VM_READ when allowed) for
// the image path, the command line (ProcessCommandLineInformation, Win8.1+, no VM_READ needed),
// the working directory (PEB → RTL_USER_PROCESS_PARAMETERS.CurrentDirectory, VM_READ) and the
// user. Everything is best effort: a process that cannot be opened keeps its kernel-listed name.
#include "who.h"

#include "counters.h"
#include "sys.h"

#include <winternl.h>
#include <sddl.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace everywho {

namespace {

typedef NTSTATUS(NTAPI* PFN_NtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
PFN_NtQueryInformationProcess nt_qip() {
    static PFN_NtQueryInformationProcess f =
        reinterpret_cast<PFN_NtQueryInformationProcess>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    return f;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
bool istarts(std::wstring_view s, std::wstring_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (towlower(s[i]) != towlower(p[i])) return false;
    return true;
}
bool iequals(std::wstring_view a, std::wstring_view b) { return a.size() == b.size() && istarts(a, b); }
bool icontains(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (istarts(hay.substr(i), needle)) return true;
    return false;
}
bool iends(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::wstring read_image_path(HANDLE h) {
    wchar_t buf[32768];
    DWORD n = 32767;
    if (!QueryFullProcessImageNameW(h, 0, buf, &n)) return {};
    return std::wstring(buf, n);
}

// ProcessCommandLineInformation (60): a UNICODE_STRING followed by its characters; two calls, the
// first sizes. Works with PROCESS_QUERY_LIMITED_INFORMATION since Windows 8.1.
std::wstring read_cmdline(HANDLE h) {
    auto f = nt_qip();
    if (!f) return {};
    ULONG len = 0;
    f(h, (PROCESSINFOCLASS)60, nullptr, 0, &len);
    if (!len || len > (1u << 20)) return {};
    std::vector<uint8_t> buf(len);
    if (f(h, (PROCESSINFOCLASS)60, buf.data(), len, &len) != 0) return {};
    const auto* us = reinterpret_cast<const UNICODE_STRING*>(buf.data());
    if (!us->Buffer || !us->Length) return {};
    return std::wstring(us->Buffer, us->Length / sizeof(wchar_t));
}

// PEB → ProcessParameters (+0x20 on x64) → CurrentDirectory.DosPath (+0x38): needs PROCESS_VM_READ,
// so own-user processes unelevated, most processes elevated. A WOW64 process's 64-bit PEB carries
// the directory it started with; the live one lives in its 32-bit PEB (accepted, noted).
std::wstring read_cwd(HANDLE h, bool& ok) {
    ok = false;
    auto f = nt_qip();
    if (!f) return {};
    PROCESS_BASIC_INFORMATION pbi{};
    ULONG rl = 0;
    if (f(h, ProcessBasicInformation, &pbi, sizeof pbi, &rl) != 0 || !pbi.PebBaseAddress) return {};
    uint64_t params = 0;
    SIZE_T rd = 0;
    if (!ReadProcessMemory(h, reinterpret_cast<const BYTE*>(pbi.PebBaseAddress) + 0x20, &params, sizeof params, &rd) || !params) return {};
    struct { USHORT Length, MaximumLength; ULONG pad; uint64_t Buffer; } us{};
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(params + 0x38), &us, sizeof us, &rd) || !us.Buffer || !us.Length || us.Length > 65534) return {};
    std::wstring w(us.Length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(us.Buffer), w.data(), us.Length, &rd)) return {};
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    ok = true;
    return w;
}

std::wstring read_user(HANDLE h, std::unordered_map<std::wstring, std::wstring>& cache) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(h, TOKEN_QUERY, &tok)) return {};
    alignas(8) BYTE buf[1024];
    DWORD n = 0;
    std::wstring out;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n)) {
        PSID sid = reinterpret_cast<TOKEN_USER*>(buf)->User.Sid;
        LPWSTR str = nullptr;
        if (ConvertSidToStringSidW(sid, &str)) {
            const std::wstring key(str);
            LocalFree(str);
            auto it = cache.find(key);
            if (it != cache.end()) {
                out = it->second;
            } else {
                wchar_t name[256], dom[256];
                DWORD nn = 256, nd = 256;
                SID_NAME_USE use;
                out = LookupAccountSidW(nullptr, sid, name, &nn, dom, &nd, &use) ? std::wstring(name) : key;
                cache[key] = out;
            }
        }
    }
    CloseHandle(tok);
    return out;
}

bool is_uuid(std::wstring_view s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != L'-' : !iswxdigit(s[i])) return false;
    }
    return true;
}

}  // namespace

struct IdentityTable::Impl {
    mutable SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<uint32_t, Identity> procs;
    std::unordered_map<uint32_t, uint32_t> threads;
    std::vector<uint32_t> pending;
    std::unordered_map<std::wstring, std::wstring> sid_names;   // enrich thread only
    std::wstring projects_root;
};

IdentityTable::IdentityTable() : p_(new Impl) { p_->projects_root = claude_projects_root(); }
IdentityTable::~IdentityTable() { delete p_; }

void IdentityTable::ingest(const std::vector<ProcSample>& s) {
    AcquireSRWLockExclusive(&p_->lock);
    std::unordered_set<uint32_t> seen;
    seen.reserve(s.size() * 2);
    for (const auto& ps : s) {
        seen.insert(ps.pid);
        auto it = p_->procs.find(ps.pid);
        if (it != p_->procs.end() && it->second.create_ft == ps.create_ft) {
            Identity& id = it->second;
            id.threads = ps.threads;
            id.working_set = ps.working_set;
            id.ppid = ps.ppid;
            id.exited = false;
            continue;
        }
        Identity id;   // new, or a reused pid
        id.pid = ps.pid;
        id.ppid = ps.ppid;
        id.session_id = ps.session_id;
        id.threads = ps.threads;
        id.create_ft = ps.create_ft;
        id.working_set = ps.working_set;
        id.name = ps.name;
        if (ps.pid == 0) id.name = L"Idle";
        else if (ps.pid == 4 && id.name.empty()) id.name = L"System";
        p_->procs[ps.pid] = std::move(id);
        p_->pending.push_back(ps.pid);
    }
    for (auto it = p_->procs.begin(); it != p_->procs.end();) it = seen.count(it->first) ? std::next(it) : p_->procs.erase(it);
    ReleaseSRWLockExclusive(&p_->lock);
}

bool IdentityTable::enumerate_now(std::string* err) {
    std::vector<ProcSample> s;
    if (!sample_processes(s, err)) return false;
    ingest(s);
    enrich_pending();
    attribute_by_ancestry();
    return true;
}

void IdentityTable::on_process_start(uint32_t pid, uint32_t ppid, uint32_t session, std::string_view image, std::wstring_view cmdline, bool rundown) {
    (void)rundown;
    AcquireSRWLockExclusive(&p_->lock);
    Identity& id = p_->procs[pid];
    id.pid = pid;
    id.ppid = ppid;
    id.session_id = session;
    id.exited = false;
    if (!image.empty()) id.name = widen(image);
    if (!cmdline.empty()) id.cmdline = std::wstring(cmdline);
    if (!id.create_ft) id.create_ft = now_filetime();
    p_->pending.push_back(pid);
    ReleaseSRWLockExclusive(&p_->lock);
}

void IdentityTable::on_process_end(uint32_t pid) {
    AcquireSRWLockExclusive(&p_->lock);
    auto it = p_->procs.find(pid);
    if (it != p_->procs.end()) it->second.exited = true;
    ReleaseSRWLockExclusive(&p_->lock);
}

void IdentityTable::on_thread(uint32_t tid, uint32_t pid, bool ended) {
    AcquireSRWLockExclusive(&p_->lock);
    if (ended) p_->threads.erase(tid);
    else p_->threads[tid] = pid;
    ReleaseSRWLockExclusive(&p_->lock);
}

uint32_t IdentityTable::pid_of_thread(uint32_t tid) const {
    AcquireSRWLockShared(&p_->lock);
    auto it = p_->threads.find(tid);
    const uint32_t pid = it == p_->threads.end() ? 0xFFFFFFFFu : it->second;
    ReleaseSRWLockShared(&p_->lock);
    return pid;
}

void IdentityTable::enrich_pending() {
    std::vector<uint32_t> todo;
    AcquireSRWLockExclusive(&p_->lock);
    todo.swap(p_->pending);
    ReleaseSRWLockExclusive(&p_->lock);
    for (uint32_t pid : todo) {
        Identity work;
        {
            AcquireSRWLockShared(&p_->lock);
            auto it = p_->procs.find(pid);
            const bool have = it != p_->procs.end();
            if (have) work = it->second;
            ReleaseSRWLockShared(&p_->lock);
            if (!have || work.enriched) continue;
        }
        work.enriched = true;
        if (pid > 4) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            const bool vm = h != nullptr;
            if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h) {
                work.image = read_image_path(h);
                if (work.name.empty() && !work.image.empty()) work.name = work.image.substr(work.image.find_last_of(L'\\') + 1);
                work.cmdline = read_cmdline(h);
                if (vm) work.cwd = read_cwd(h, work.cwd_ok);
                work.user = read_user(h, p_->sid_names);
                CloseHandle(h);
            }
        }
        Agent a;
        if (classify_image(work.name, work.image, work.cmdline, a)) work.agent = a;
        if (work.agent.any() && work.agent.project.empty() && work.cwd_ok) {
            Agent c = work.agent;
            if (classify_cwd(work.cwd, p_->projects_root, c)) work.agent = c;
        }
        work.kind = classify_kind(pid, work.name, work.session_id, work.agent.any());
        AcquireSRWLockExclusive(&p_->lock);
        auto it = p_->procs.find(pid);
        if (it != p_->procs.end() && it->second.create_ft == work.create_ft) {
            work.exited = it->second.exited;
            it->second = work;
        }
        ReleaseSRWLockExclusive(&p_->lock);
    }
}

void IdentityTable::on_tape_write(uint32_t pid, std::wstring_view path) {
    Agent a;
    if (!classify_tape(path, a)) return;
    AcquireSRWLockExclusive(&p_->lock);
    auto it = p_->procs.find(pid);
    if (it != p_->procs.end()) {
        it->second.agent = a;
        it->second.kind = Kind::Agent;
    }
    ReleaseSRWLockExclusive(&p_->lock);
}

void IdentityTable::attribute_by_ancestry() {
    AcquireSRWLockExclusive(&p_->lock);
    for (auto& [pid, id] : p_->procs) {
        if (id.agent.any() || id.kind == Kind::System || id.kind == Kind::Wsl) continue;
        uint32_t cur = id.ppid;
        uint64_t child_ft = id.create_ft;
        for (int hops = 0; hops < 8 && cur; ++hops) {
            auto it = p_->procs.find(cur);
            if (it == p_->procs.end()) break;
            const Identity& anc = it->second;
            if (anc.session_id != id.session_id) break;   // never across sessions
            if (anc.create_ft > child_ft) break;          // a reused pid: the "parent" is younger than the child
            if (anc.agent.any()) {
                id.agent = anc.agent;
                id.agent.rule = "inherit";
                if (id.kind == Kind::App || id.kind == Kind::Unknown) id.kind = Kind::Agent;
                break;
            }
            child_ft = anc.create_ft;
            cur = anc.ppid;
        }
    }
    ReleaseSRWLockExclusive(&p_->lock);
}

const Identity* IdentityTable::find(uint32_t pid) const {
    auto it = p_->procs.find(pid);
    return it == p_->procs.end() ? nullptr : &it->second;
}

std::vector<Identity> IdentityTable::snapshot() const {
    AcquireSRWLockShared(&p_->lock);
    std::vector<Identity> out;
    out.reserve(p_->procs.size());
    for (const auto& [pid, id] : p_->procs) out.push_back(id);
    ReleaseSRWLockShared(&p_->lock);
    return out;
}

size_t IdentityTable::size() const { return p_->procs.size(); }

// ---- the rules ----

bool IdentityTable::classify_tape(std::wstring_view path, Agent& out) {
    const std::wstring p = lower(std::wstring(path));
    const size_t k = p.find(L"\\.claude\\projects\\");
    if (k != std::wstring::npos) {
        const size_t i = k + 18;   // after "\.claude\projects\"
        const size_t j = p.find(L'\\', i);
        if (j == std::wstring::npos) return false;
        out = Agent{};
        out.harness = "claude-code";
        out.project = std::wstring(path.substr(i, j - i));
        out.rule = "tape";
        const std::wstring_view rest = path.substr(j + 1);
        const std::wstring rest_l = lower(std::wstring(rest));
        const size_t sub = rest_l.find(L"\\subagents\\agent-");
        if (sub != std::wstring::npos) {
            const std::wstring_view sess = rest.substr(0, sub);
            if (is_uuid(sess)) out.session = std::wstring(sess);
            std::wstring_view f = rest.substr(sub + 11);   // "agent-<id>.jsonl"
            const size_t dot = f.rfind(L'.');
            out.subagent = std::wstring(f.substr(0, dot));
            return true;
        }
        if (rest.size() >= 42 && is_uuid(rest.substr(0, 36)) && iends(rest, L".jsonl")) out.session = std::wstring(rest.substr(0, 36));
        return true;
    }
    if (p.find(L"\\sessions\\") != std::wstring::npos && (iends(p, L"session.jsonl.zstd") || iends(p, L"session.jsonl"))) {
        out = Agent{};
        out.harness = "dsh";
        out.rule = "tape";
        const size_t e = path.find_last_of(L'\\');
        const size_t b = e == std::wstring::npos ? std::wstring::npos : path.substr(0, e).find_last_of(L'\\');
        if (e != std::wstring::npos && b != std::wstring::npos) out.session = std::wstring(path.substr(b + 1, e - b - 1));
        return true;
    }
    return false;
}

bool IdentityTable::classify_image(std::wstring_view name, std::wstring_view image, std::wstring_view cmdline, Agent& out) {
    out = Agent{};
    const bool claude_name = iequals(name, L"claude.exe");
    // the CLI on this box: ...\AppData\Roaming\Claude\claude-code\<ver>\claude.exe, driven with stream-json args
    const bool cli_image = icontains(image, L"\\claude-code\\");
    const bool cli_args = icontains(cmdline, L"--output-format") || icontains(cmdline, L"--resume") || icontains(cmdline, L"--session-id") ||
                          icontains(cmdline, L"--print") || icontains(cmdline, L"--permission-prompt-tool");
    // the desktop app: the WindowsApps package and its Electron children (--type=renderer|gpu-process|utility)
    const bool desktop_app = icontains(image, L"\\WindowsApps\\Claude_") || icontains(cmdline, L"--user-data-dir=") || icontains(cmdline, L"--type=");
    if (claude_name && !desktop_app && (cli_image || cli_args || (image.empty() && cmdline.empty())))
        out.harness = "claude-code";
    else if (iequals(name, L"node.exe") && (icontains(cmdline, L"claude-code") || icontains(cmdline, L"@anthropic-ai") || icontains(cmdline, L"\\claude\\cli.js")))
        out.harness = "claude-code";
    else if (icontains(name, L"codex") || icontains(cmdline, L"\\codex"))
        out.harness = "codex";
    else if (iequals(name, L"cursor.exe"))
        out.harness = "cursor";
    else if (icontains(cmdline, L"deepseek-harness") || icontains(cmdline, L"\\dsh\\") || icontains(cmdline, L"dsh.py"))
        out.harness = "dsh";
    else
        return false;
    out.rule = "image";
    // the session from the command line: --resume=<uuid> | --resume <uuid> | --session-id=<uuid> | --session-id <uuid>
    for (const wchar_t* key : { L"--resume=", L"--resume ", L"--session-id=", L"--session-id " }) {
        const std::wstring_view k(key);
        for (size_t i = 0; i + k.size() + 36 <= cmdline.size(); ++i) {
            if (!istarts(cmdline.substr(i), k)) continue;
            const std::wstring_view cand = cmdline.substr(i + k.size(), 36);
            if (is_uuid(cand)) {
                out.session = std::wstring(cand);
                out.rule = "cmdline";
            }
            break;
        }
        if (!out.session.empty()) break;
    }
    return true;
}

Kind IdentityTable::classify_kind(uint32_t pid, std::wstring_view name, uint32_t session_id, bool is_agent) {
    if (pid == 0 || pid == 4) return Kind::System;
    if (istarts(name, L"vmmem")) return Kind::Wsl;
    if (is_agent) return Kind::Agent;
    if (session_id == 0) return Kind::Service;
    return Kind::App;
}

std::wstring IdentityTable::project_slug(std::wstring_view cwd) {
    std::wstring s;
    s.reserve(cwd.size());
    for (wchar_t c : cwd) s += iswalnum(c) ? c : L'-';
    return s;
}

bool IdentityTable::classify_cwd(std::wstring_view cwd, std::wstring_view projects_root, Agent& out) {
    if (cwd.empty() || projects_root.empty()) return false;
    const std::wstring dir = std::wstring(projects_root) + L"\\" + project_slug(cwd);
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out.project = project_slug(cwd);
    if (!out.rule || !out.rule[0]) out.rule = "cwd";
    return true;
}

std::wstring IdentityTable::claude_projects_root() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (!n || n >= sizeof buf / sizeof buf[0]) return {};
    return std::wstring(buf, n) + L"\\.claude\\projects";
}

bool name_glob(std::wstring_view glob, std::wstring_view name) {
    size_t g = 0, n = 0, star = std::wstring_view::npos, mark = 0;
    while (n < name.size()) {
        if (g < glob.size() && (glob[g] == L'?' || towlower(glob[g]) == towlower(name[n]))) { ++g; ++n; continue; }
        if (g < glob.size() && glob[g] == L'*') { star = g++; mark = n; continue; }
        if (star != std::wstring_view::npos) { g = star + 1; n = ++mark; continue; }
        return false;
    }
    while (g < glob.size() && glob[g] == L'*') ++g;
    return g == glob.size();
}

}  // namespace everywho
