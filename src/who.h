// everywho · who.h — identity: which process is this, and which agent session is it.
// Built from the kernel's process list (counters tier: SystemProcessInformation, one call, no
// handles) or the ETW process/thread rundown, enriched from user mode (image path, command
// line, working directory, user), and attributed to a harness / project / session by named
// rules (ARCHITECTURE.md §3, ADR-015).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace everywho {

enum class Kind : uint8_t { Unknown, App, Agent, System, Wsl, Service };
inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::App: return "App";
        case Kind::Agent: return "Agent";
        case Kind::System: return "System";
        case Kind::Wsl: return "Wsl";
        case Kind::Service: return "Service";
        default: return "Unknown";
    }
}

struct Agent {
    std::string harness;          // "claude-code" | "dsh" | "codex" | "cursor" | ""
    std::wstring project;         // Claude Code's project slug (C--facet) or the directory
    std::wstring session;         // the session uuid, when known
    std::wstring subagent;        // "agent-…" when the tape is a subagent's
    const char* rule = "";        // which rule fired: "tape" | "cwd" | "image" | "inherit" | ""
    bool any() const { return !harness.empty(); }
};

// One process as the kernel lists it: identity and cumulative I/O in the same record.
struct ProcSample {
    uint32_t pid = 0, ppid = 0, session_id = 0, threads = 0;
    uint64_t create_ft = 0;                 // FILETIME
    uint64_t read_bytes = 0, write_bytes = 0, other_bytes = 0;
    uint64_t read_ops = 0, write_ops = 0, other_ops = 0;
    uint64_t working_set = 0;
    std::wstring name;                      // the short image name
};

struct Identity {
    uint32_t pid = 0, ppid = 0, session_id = 0, threads = 0;
    std::wstring name;            // "python.exe"
    std::wstring image;           // full path when openable
    std::wstring cmdline;
    std::wstring cwd;             // current directory when readable (own user, or elevated)
    std::wstring user;            // account name from the SID (cached)
    uint64_t create_ft = 0;
    uint64_t working_set = 0;
    Kind kind = Kind::Unknown;
    Agent agent;
    bool enriched = false;        // image / cmdline / cwd attempted
    bool cwd_ok = false;          // the working directory was read
    bool exited = false;
};

// Every source of identity in one table. One writer (the tick or collector thread); readers take
// copies through snapshot() / find() under the table's lock.
class IdentityTable {
public:
    IdentityTable();
    ~IdentityTable();
    IdentityTable(const IdentityTable&) = delete;
    IdentityTable& operator=(const IdentityTable&) = delete;

    // Counters tier: upsert from a process list; processes no longer listed are dropped.
    void ingest(const std::vector<ProcSample>& s);
    // sample + ingest + enrich_pending + attribute_by_ancestry, in one call
    bool enumerate_now(std::string* err = nullptr);

    // ETW: rundown and live process / thread events.
    void on_process_start(uint32_t pid, uint32_t ppid, uint32_t session, std::string_view image, std::wstring_view cmdline, bool rundown);
    void on_process_end(uint32_t pid);
    void on_thread(uint32_t tid, uint32_t pid, bool ended);
    uint32_t pid_of_thread(uint32_t tid) const;          // 0xFFFFFFFF when unknown

    // User-mode enrichment for pids not yet enriched: image path, command line, cwd, user, rules.
    void enrich_pending();

    // Attribution by tape append (the fold calls this when a write's path is a session tape).
    void on_tape_write(uint32_t pid, std::wstring_view path);

    // ADR-015: a process without an attribution takes its nearest attributed ancestor's, marked "inherit".
    void attribute_by_ancestry();

    const Identity* find(uint32_t pid) const;
    std::vector<Identity> snapshot() const;
    size_t size() const;

    // The rules, exposed for --selftest
    static bool classify_tape(std::wstring_view path, Agent& out);
    // The harness from the image and its command line; the session from --resume= / --session-id when
    // present (rule "cmdline"). The Claude desktop app (WindowsApps\Claude_…, Electron --type= children)
    // is an App, not a harness; the Claude Code CLI (…\claude-code\<ver>\claude.exe, stream-json args) is.
    static bool classify_image(std::wstring_view name, std::wstring_view image, std::wstring_view cmdline, Agent& out);
    static Kind classify_kind(uint32_t pid, std::wstring_view name, uint32_t session_id, bool is_agent);
    static std::wstring project_slug(std::wstring_view cwd);            // Claude Code's projects\<slug> spelling
    static bool classify_cwd(std::wstring_view cwd, std::wstring_view projects_root, Agent& out);   // the slug exists under the root
    static std::wstring claude_projects_root();                          // %USERPROFILE%\.claude\projects

    struct Impl;
private:
    Impl* p_;
};

// A glob on a process name: "node*", "python.exe", "*claude*" — case-insensitive, no paths.
bool name_glob(std::wstring_view glob, std::wstring_view name);

}  // namespace everywho
