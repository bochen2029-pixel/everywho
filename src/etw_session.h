// everywho · etw_session.h — the collector: one system-logger-mode session named "everywho",
// one consumer thread blocked in ProcessTrace, one decoder from EVENT_RECORD to RawEvent, and
// the accounting of what the kernel dropped. Nothing here folds; the sink does.
//
// Contract (ETW.md): start stops a stale session first; every exit path stops the session; the
// callback never blocks and never allocates on the hot path; lost events are counted and
// exposed, never hidden.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace everywho {

enum class EvKind : uint8_t {
    None,
    FileCreate, FileRead, FileWrite, FileDelete, FileRename, FileSetInfo, FileCleanup, FileClose, FileFlush,
    FileDirEnum, FileOpEnd, FileName,
    DiskRead, DiskWrite, DiskFlush, DiskInit,
    ProcStart, ProcEnd, ProcRundown, ProcDefunct,
    ThreadStart, ThreadEnd, ThreadRundown,
};

// One decoded kernel event. Strings are views into the ETW buffer — valid only inside the sink.
struct RawEvent {
    EvKind kind = EvKind::None;
    uint8_t opcode = 0, version = 0;
    uint64_t qpc = 0;                 // raw timestamp (session clock = QPC)
    uint32_t pid = 0xFFFFFFFFu;       // header pid, or resolved via the thread table by the collector
    uint32_t tid = 0xFFFFFFFFu;       // header tid, else payload TTID / IssuingThreadId
    uint64_t file_object = 0, file_key = 0, irp = 0;
    uint64_t offset = 0;              // FileRead/Write byte offset; DiskRead/Write ByteOffset
    uint32_t size = 0;                // IoSize / TransferSize
    uint32_t flags = 0;               // IoFlags / IrpFlags / CreateOptions / InfoClass, by kind
    uint32_t status = 0;              // NTSTATUS on FileOpEnd
    uint32_t disk = 0;                // DiskNumber
    uint64_t response_qpc = 0;        // HighResResponseTime (DiskIo)
    const wchar_t* name = nullptr;    // OpenPath / FileName / rename target / CommandLine (Proc*)
    size_t name_len = 0;
    const char* image = nullptr;      // ImageFileName (Proc*)
    size_t image_len = 0;
    uint32_t proc_pid = 0, proc_ppid = 0, proc_session = 0;   // Proc* payload identities (the header pid is the creator)
    int32_t exit_status = 0;
    const uint8_t* sid = nullptr;     // Proc*: the user SID bytes, if present
    size_t sid_len = 0;
    uint32_t thr_pid = 0, thr_tid = 0;   // Thread* payload identities
};

struct EtwConfig {
    std::wstring session_name = L"everywho";
    uint32_t buffer_kb = 64;
    uint32_t min_buffers = 64;
    uint32_t max_buffers = 256;
    uint32_t flush_timer_s = 1;
    bool system_logger_mode = true;   // our own name; fall back to the NT Kernel Logger name if refused
    bool file_io = true, disk_io = true, process = true, thread = true;
};

struct EtwStats {
    bool running = false;
    bool fallback_kernel_logger = false;   // the singleton name was used
    uint64_t events = 0;                   // delivered to the sink
    uint64_t lost = 0;                     // EventsLost from the last session query
    uint64_t unknown = 0;                  // events with no layout (counted, not decoded)
    uint32_t buffers = 0;                  // current buffer count
    double decode_us_per_event = 0.0;      // measured over the last interval
    uint64_t qpc_freq = 0;
    uint64_t rundown_done_qpc = 0;         // when DCStart/DCEnd finished: the one-shot window starts after this
};

using EtwSink = std::function<void(const RawEvent&)>;

class EtwSession {
public:
    EtwSession();
    ~EtwSession();                    // stops the session if running (also on unwind)
    EtwSession(const EtwSession&) = delete;
    EtwSession& operator=(const EtwSession&) = delete;

    // Privilege and environment
    static bool is_elevated();                            // token elevation
    static bool enable_profile_privilege(std::string* err);   // SeSystemProfilePrivilege in this token
    static bool stop_stale(const std::wstring& name);     // ControlTrace(STOP) on a leftover session
    static std::vector<std::wstring> list_sessions();     // running trace sessions (for --where)

    // Lifecycle. start() stops a stale session, creates ours, enables the kernel flags, opens the
    // consumer and launches the thread; returns false with err on any failure (ACCESS_DENIED →
    // the caller falls back to the counters tier).
    bool start(const EtwConfig& cfg, EtwSink sink, std::string* err);
    void stop();                                          // ControlTrace(STOP) → ProcessTrace returns → join
    bool running() const;
    EtwStats stats();                                     // queries EventsLost; cheap enough per tick
    void tick();                                          // close the decode-cost interval; called by the tick thread

    // Stage 1 verification: TDH schema vs etw_layouts for every (provider, opcode, version) seen.
    // Fills report with one line per layout ("verified" / "DIVERGENT: field X at +N expected +M");
    // returns the divergence count.
    int verify_layouts(std::vector<std::string>& report);

    struct Impl;
private:
    Impl* p_;
};

// The decoder, exposed for --selftest: an EVENT_RECORD-shaped input (provider, opcode, version,
// header flags, pid, tid, timestamp, payload) → RawEvent. Returns false for unknown layouts.
struct RecordView {
    const GUID* provider;
    uint8_t opcode, version;
    bool header64;
    uint32_t pid, tid;
    uint64_t qpc;
    const uint8_t* data;
    size_t len;
};
bool decode_record(const RecordView& in, RawEvent& out);

}  // namespace everywho
