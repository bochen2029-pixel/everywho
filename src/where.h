// everywho · where.h — the fold: every event (or counter delta) lands in one process, one
// directory node, one file entry and one op counter; a Snapshot is an immutable top-K copy for
// views. Stage 0 folds per process and per volume (the counters tier); the directory trie, the
// per-file table and the bursts arrive with the ETW tier (Stage 2) on the same API.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app_util.h"
#include "etw_session.h"
#include "who.h"

namespace everywho {

struct IoCounters {
    uint64_t file_read = 0, file_write = 0, other_bytes = 0;   // bytes the process asked for (other = ioctl / misc transfers)
    uint64_t disk_read = 0, disk_write = 0;                    // bytes that reached the media (ETW tier)
    uint64_t attributed_write = 0;                             // writeback re-attributed to this owner (derived, labelled)
    uint64_t unnamed = 0;                                      // bytes whose file could not be named
    uint32_t ops[kOpKinds] = {};
    uint32_t ops_other = 0;                                    // counters tier: "other" operations
    uint32_t files = 0;                                        // distinct files touched
    uint32_t new_files = 0;                                    // files created in the window
    void add(const IoCounters& o);
    uint64_t total_ops() const;
    bool empty() const { return !file_read && !file_write && !other_bytes && !disk_read && !disk_write && !total_ops(); }
};

// facet's provenance signal, live: thousands a minute is a clone or extract, a dozen is an agent, one or two is a hand
inline double files_per_min(uint32_t new_files, uint64_t window_ms) {
    return window_ms ? (double)new_files * 60000.0 / (double)window_ms : 0.0;
}

struct PidShare { uint32_t pid = 0; uint64_t bytes = 0; };

struct DirNode {
    uint32_t parent = 0;
    uint32_t depth = 0;
    std::wstring name;                         // one component; root = ""
    uint64_t self_items = 0;                   // files directly here
    IoCounters io;                             // subtree totals after finish()
    PidShare top[4];                           // the biggest contributors (replace-min)
    std::vector<uint32_t> children;            // by bytes desc after finish()
};

struct FileEntry {
    std::wstring path;
    uint32_t dir = 0;
    uint32_t last_writer = 0, last_reader = 0;
    IoCounters io;
    uint64_t first_qpc = 0, last_qpc = 0;
    bool deleted = false, created_here = false;
    std::wstring renamed_to;
};

struct Burst {
    uint64_t start_qpc = 0, end_qpc = 0;
    uint32_t files = 0;
    uint64_t bytes = 0;
    uint32_t pid = 0;                          // dominant creator
    uint32_t dir = 0;                          // dominant directory (facet's >= 90 % descent)
    double dir_share = 0.0;
};

struct VolumeStat {
    wchar_t letter = 0;                        // the first letter on the disk ("0 C:" → C)
    std::wstring letters;                      // every letter PDH lists for the disk
    std::wstring device;                       // \Device\HarddiskVolumeN when known
    std::wstring instance;                     // the PDH instance name, verbatim
    uint32_t disk = 0;
    IoCounters io;                             // bytes over the interval (ETW tier fills file_*)
    double read_bps = 0, write_bps = 0;        // media rates (PDH: Disk Read/Write Bytes/sec)
    double read_iops = 0, write_iops = 0;
    double queue = 0.0, busy_pct = 0.0;
    double response_ms_p50 = 0.0, response_ms_p95 = 0.0;
};

struct ProcStat {
    Identity id;
    IoCounters io;
    std::vector<std::pair<uint32_t, IoCounters>> top_dirs;   // (node, io), by bytes (ETW tier)
};

// The frozen, renderable state of one interval (or one-shot window). Top-K everywhere.
struct Snapshot {
    uint64_t wall_ms = 0;                      // wall clock at the snapshot (FILETIME ms since epoch)
    uint64_t window_ms = 0;                    // measured length
    Tier tier = Tier::Counters;
    bool elevated = false;
    EtwStats etw;
    std::vector<VolumeStat> volumes;
    std::vector<ProcStat> procs;               // by the requested sort, top_procs at most
    uint32_t procs_total = 0;                  // before truncation (with any I/O in the window)
    uint32_t processes_seen = 0;               // every process the kernel listed
    std::vector<DirNode> nodes;                // a pruned copy of the trie (top branches only)
    std::vector<FileEntry> files;              // top-K by bytes
    std::vector<Burst> bursts;
    uint64_t total_files = 0;
    IoCounters total;                          // the sum over every process in the window
    std::string error;
};

struct FoldConfig {
    uint32_t file_table_cap = 100000;          // FileEntry LRU bound
    uint32_t burst_gap_s = 60;
    uint32_t top_files = 64, top_dirs = 64, top_procs = 200;
    bool attribute = true;                     // --raw = false
    // filters (compiled from Opts by the caller)
    std::vector<uint32_t> pids;
    std::vector<std::wstring> name_globs;
    std::vector<std::wstring> dirs, exclude;   // normalised prefixes with trailing separator
    std::vector<std::wstring> only_paths;      // --files-from tape (normalised keys)
    uint32_t ops = kOpAll;
    std::vector<wchar_t> volumes;
    bool agents_only = false;
};

class Fold {
public:
    explicit Fold(const FoldConfig& cfg, IdentityTable& who);
    ~Fold();
    Fold(const Fold&) = delete;
    Fold& operator=(const Fold&) = delete;

    // The ETW hot path: called on the collector thread for every RawEvent after pid/tid and
    // (for names) the DOS path are resolved. Never allocates for known paths.
    void add(const RawEvent& ev, std::wstring_view dos_path);

    // Counters tier: per-process deltas per tick (no paths).
    void add_process_counters(uint32_t pid, const IoCounters& delta);
    void add_volume(const VolumeStat& v);
    void set_processes_seen(uint32_t n);

    // Close the interval and build a Snapshot: cumulative = since start (the one-shot window),
    // else the interval since the last snapshot. Interval counters reset either way.
    Snapshot snapshot(SortKey sort, bool cumulative, uint64_t window_ms);

    // --paths: every distinct path touched since start (or since the last drain), first-touch order.
    std::vector<std::wstring> drain_paths();

    size_t nodes() const;
    size_t files() const;
    size_t bytes_in_use() const;

    struct Impl;
private:
    Impl* p_;
};

// NT path → DOS path with a device table (ETW.md §3). Returns false for non-file devices.
class DevicePaths {
public:
    DevicePaths();
    void refresh();                                            // QueryDosDevice for every letter + SystemRoot
    void set_table(std::vector<std::pair<std::wstring, std::wstring>> nt_to_dos, std::wstring system_root);   // tests
    bool to_dos(std::wstring_view nt, std::wstring& dos) const;
    bool is_device(std::wstring_view nt) const;                // NamedPipe, Mailslot, Afd, Null, ShadowCopy…
    const std::vector<std::pair<std::wstring, std::wstring>>& table() const { return map_; }
private:
    std::vector<std::pair<std::wstring, std::wstring>> map_;   // (NT prefix, DOS prefix), longest first
    std::wstring system_root_;
};

// facet's directory view over the fold's nodes (ETW tier).
struct DirLine {
    int level = 0;
    std::string label;
    IoCounters io;
    bool note = false;
    uint32_t node = 0;
};
std::vector<DirLine> dir_lines(const Snapshot& s, int top, int depth, uint64_t fold_threshold_bytes);

}  // namespace everywho
