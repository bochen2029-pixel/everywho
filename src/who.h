// everywho · who.h — identity: which process is this, and which agent session is it.
// Built from the ETW process/thread rundown (or, in the counters tier, from process
// enumeration), enriched from user mode (image path, working directory, start time), and
// attributed to a harness/project/session by named rules (ARCHITECTURE.md §3).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace everywho {

enum class Kind : uint8_t { Unknown, App, Agent, System, Wsl, Service };

struct Agent {
    std::string harness;          // "claude-code" | "dsh" | "codex" | "cursor" | ""
    std::wstring project;         // the project slug or directory
    std::wstring session;         // the session uuid, when known
    std::wstring subagent;        // "agent-…" when the tape is a subagent's
    const char* rule = "";        // which rule fired: "tape" | "cwd" | "image" | "inherit" | ""
};

struct Identity {
    uint32_t pid = 0, ppid = 0, session_id = 0;
    std::wstring name;            // "python.exe"
    std::wstring image;           // full path when openable
    std::wstring cmdline;
    std::wstring cwd;             // current directory when readable (own user, or elevated)
    std::wstring user;            // account name from the SID (cached)
    uint64_t start_qpc = 0;
    Kind kind = Kind::Unknown;
    Agent agent;
    bool exited = false;
    uint64_t exit_qpc = 0;
};

// Every source of identity in one table. Thread-safe for one writer (the collector) and readers
// under the snapshot lock: writes replace whole entries; readers copy what they need.
class IdentityTable {
public:
    IdentityTable();

    // Counters tier / startup: enumerate processes now (NtQuerySystemInformation) and enrich.
    void enumerate_now();

    // ETW: rundown and live process/thread events (from RawEvent Proc*/Thread*).
    void on_process_start(uint32_t pid, uint32_t ppid, uint32_t session, std::string_view image, std::wstring_view cmdline,
                          const uint8_t* sid, size_t sid_len, uint64_t qpc, bool rundown);
    void on_process_end(uint32_t pid, uint64_t qpc);
    void on_thread(uint32_t tid, uint32_t pid, bool ended);
    uint32_t pid_of_thread(uint32_t tid) const;          // 0xFFFFFFFF when unknown

    // Enrichment from user mode (image path, cwd, start time); best effort, never blocks the collector:
    // queued for the tick thread, which calls enrich_pending().
    void enrich_pending();

    // Attribution by tape append: called by the fold when a write's path looks like a session tape.
    void on_tape_write(uint32_t pid, std::wstring_view path);

    // The inherit rule (ADR-015): every process without an attribution of its own takes its nearest
    // attributed ancestor's (at most 8 parents up, never across session ids), marked rule "inherit".
    // Runs on the tick thread after enrich_pending(); cheap (pids are few, parents cached).
    void attribute_by_ancestry();

    const Identity* find(uint32_t pid) const;
    std::vector<Identity> snapshot() const;              // copies, for views
    size_t size() const;

    // The rules, exposed for --selftest
    static bool classify_tape(std::wstring_view path, Agent& out);      // .claude\projects\<slug>\<uuid>.jsonl, subagents, dsh
    static Kind classify_kind(uint32_t pid, std::wstring_view name, uint32_t session_id);
    static bool classify_image(std::wstring_view name, std::wstring_view cmdline, Agent& out);

    struct Impl;
private:
    Impl* p_;
};

// A glob on a process name: "node*", "python.exe", "*claude*" — case-insensitive, no paths.
bool name_glob(std::wstring_view glob, std::wstring_view name);

}  // namespace everywho
