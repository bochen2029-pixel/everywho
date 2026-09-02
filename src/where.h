// everywho · where.h — the fold: every RawEvent lands in one process, one directory node, one
// file entry and one op counter; a Snapshot is an immutable top-K copy for views.
// The trie and its fold rules are facet's (C:\facet\facets.cpp) with per-process and per-op
// dimensions added; the attribution rules are named and switchable (--raw).
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
    uint64_t file_read = 0, file_write = 0;    // bytes requested by the process
    uint64_t disk_read = 0, disk_write = 0;    // bytes that reached the media (DiskIo)
    uint64_t attributed_write = 0;             // writeback re-attributed to this owner (derived, labelled)
    uint64_t unnamed = 0;                      // bytes whose file could not be named
    uint32_t ops[kOpKinds] = {};
    uint32_t files = 0;                        // distinct files touched
    void add(const IoCounters& o);
};

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
    wchar_t letter = 0;
    std::wstring device;                       // \Device\HarddiskVolumeN
    uint32_t disk = 0;
    IoCounters io;
    double queue = 0.0, busy_pct = 0.0;
    double response_ms_p50 = 0.0, response_ms_p95 = 0.0;
    uint32_t read_iops = 0, write_iops = 0;
};

struct ProcStat {
    Identity id;
    IoCounters io;
    std::vector<std::pair<uint32_t, IoCounters>> top_dirs;   // (node, io), by bytes
};

// The frozen, renderable state of one interval (or one-shot window). Top-K everywhere.
struct Snapshot {
    uint64_t wall_ms = 0;
    uint64_t window_ms = 0;                    // measured length
    Tier tier = Tier::Counters;
    bool elevated = false;
    EtwStats etw;
    std::vector<VolumeStat> volumes;
    std::vector<ProcStat> procs;               // by the requested sort
    std::vector<DirNode> nodes;                // a pruned copy of the trie (top branches only)
    std::vector<FileEntry> files;              // top-K by bytes
    std::vector<Burst> bursts;
    uint64_t total_files = 0;                  // distinct files touched in the window (for --paths, the full set is kept separately)
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

    // The hot path: called on the collector thread for every RawEvent after the collector has
    // resolved pid/tid and (for names) the DOS path. Never allocates for known paths.
    void add(const RawEvent& ev, std::wstring_view dos_path /* empty when unnamed / not applicable */);

    // Counters tier: per-process deltas per tick (no paths).
    void add_process_counters(uint32_t pid, uint64_t read_delta, uint64_t write_delta, uint32_t reads, uint32_t writes);
    void add_volume(const VolumeStat& v);

    // The tick: close the interval, build a Snapshot (sorted, pruned), reset interval counters.
    // `cumulative` = true keeps totals across ticks (one-shot window); false = per-interval.
    Snapshot snapshot(SortKey sort, bool group, bool cumulative);

    // --paths: every distinct path touched since start (or since the last drain), in first-touch order.
    std::vector<std::wstring> drain_paths();

    // memory accounting for --selftest / --where
    size_t nodes() const;
    size_t files() const;
    size_t bytes_in_use() const;

    struct Impl;
private:
    Impl* p_;
};

// Name resolution owned by the collector but specified here because the fold consumes its output:
// NT path → DOS path with a device table (ETW.md §3). Returns false for non-file devices.
class DevicePaths {
public:
    DevicePaths();
    void refresh();                                            // QueryDosDevice for every letter + volume GUID paths
    bool to_dos(std::wstring_view nt, std::wstring& dos) const;   // \Device\HarddiskVolume3\x → C:\x; \??\C:\x; \SystemRoot\x; \Device\Mup\s\p
    bool is_device(std::wstring_view nt) const;                // NamedPipe, Mailslot, Afd, Null, ShadowCopy…
    std::vector<std::pair<wchar_t, std::wstring>> table() const;
private:
    std::vector<std::pair<std::wstring, std::wstring>> map_;   // (NT prefix, DOS prefix), longest first
    std::wstring system_root_;
};

// facet's directory view over the fold's nodes, so the rail and the report share one rule set.
struct DirLine {
    int level = 0;
    std::string label;
    IoCounters io;
    bool note = false;
    uint32_t node = 0;
};
std::vector<DirLine> dir_lines(const Snapshot& s, int top, int depth, uint64_t fold_threshold_bytes);

}  // namespace everywho
