# everywho — architecture

*Version 0.1 of the design · 2026-09-02 · author: Bo Chen (operator) + Claude Fable 5.1
(synthesis) · license MIT · platform Windows 10 1809+ / 11, x64, C++20, single static exe.*

## §0 · The thesis, and the honest physics

**The kernel already accounts for every I/O; nothing on Windows pivots that stream by tenant,
live, in a form an agent can ask.** everywho is that pivot. It keeps no state, installs
nothing, and is true for exactly the window it watched.

| source | what it carries | cost | needs | tier |
|---|---|---|---|---|
| process I/O counters (`GetProcessIoCounters`) | per-process read/write/other bytes and operation counts, cumulative | one call per process per tick, ~2 µs | `PROCESS_QUERY_LIMITED_INFORMATION` (works for other users' processes too) | counters |
| disk performance (`IOCTL_DISK_PERFORMANCE`, or PDH `\PhysicalDisk`) | per-physical-disk bytes, IOPS, queue depth, idle time | one call per disk per tick | none | counters |
| ETW kernel *FileIo* events | every create / read / write / delete / rename / flush / dir-enum with **the path**, thread, sizes, offsets, flags, completion status | ~150 bytes/event; 50k–500k events/s under load | `SeSystemProfilePrivilege` (admin, or *Performance Log Users*) | ETW |
| ETW kernel *DiskIo* events | physical reads/writes: disk, transfer size, response time, issuing thread | tens per second idle, thousands under load | same | ETW |
| ETW kernel *Process* / *Thread* rundown + live | pid, parent, image, command line, SID; tid → pid | a burst at session start, then rare | same | ETW |
| process enrichment (`QueryFullProcessImageName`, PEB read via `NtQueryInformationProcess`) | image path, current directory, start time | per new pid | a handle; cwd needs `PROCESS_VM_READ` (own user, or elevated) | both |

The consequence is the two-tier design: **counters** answers *who* and *how much* with no
privilege; **ETW** adds *what*. Both feed the same fold and the same outputs; the snapshot says
which tier it came from.

The cost that matters is not the disk, it is the *event rate*: a clone landing 3,000 files a
second emits ~10 events per file. The consumer must decode and fold an event in well under a
microsecond and never block the ETW callback, or the kernel drops events. That is the whole
performance design: a hot path with no allocation, no locking beyond a snapshot swap, and
bounded tables.

## §1 · The questions, and the exact commands that answer them

| question | command | tier |
|---|---|---|
| who is writing right now, and where | `everywho` (3 s) · `everywho -j` | either (where: ETW) |
| which agent session is filling the disk | `everywho -w -s write -g process` — rows carry `harness · project · session` | ETW |
| what is that 03:00 job touching | `everywho -w --name backup*` · `--pid N` | ETW |
| why is the SSD pegged | `everywho --stamp` (queue, busy %, physical MB/s vs file MB/s) · `-w -s disk` | counters (rates) · ETW (who) |
| is anyone touching these files | `facet --paths Q \| everywho --files-from - -w` | ETW |
| who has this file or folder open (why can't I delete it) | `everywho --open C:\that\dir` | handles (elevated for other users' processes) |
| is this a clone landing or a hand at work | `everywho -w -s newfiles` — files per minute per process and directory, facet's provenance signal live | ETW |
| where did the last ten seconds land | `everywho --paths --sample-ms 10000 \| facet --files-from -` | ETW |
| which session wrote the burst facet showed | `everywho -w --dir C:\that\dir` while it recurs; or the `--paths` tape into `everywhen locate` for `.jsonl` tapes | ETW |
| a receipt beside a timing row | `everywho --stamp` (mirrors `vramtop --stamp`) | either |

## §2 · Anatomy

```
                  ┌────────────── everywho.exe (console: every mode) / everywho-gui.exe ──────────────┐
                  │                                                                                     │
  kernel ──ETW──▶ │ etw_session   decoder (etw_layouts)  ──RawEvent──▶  where (fold)  ──Snapshot──▶  views │
  counters ─────▶ │ counters_tier ──────────────────────────────────▶   rates         (swap under lock)  │
  processes ────▶ │ who (identity: rundown + enrichment + attribution rules)     ▲                     │
                  │                                                              │                     │
                  │   views: report · watch TUI · JSON/NDJSON · stamp · spool · paths · MCP · window   │
                  └─────────────────────────────────────────────────────────────────────────────────────┘
```

| module | file | owns | thread |
|---|---|---|---|
| **collector (ETW)** | `etw_session.h/.cpp` | session lifecycle, privilege, stale-session cleanup, the consumer (`ProcessTrace`), the decoder from `EVENT_RECORD` to `RawEvent` using `etw_layouts.h`, lost-event accounting | its own thread, blocked in `ProcessTrace` |
| **collector (counters)** | `counters.h/.cpp` | per-process I/O counters and per-disk performance deltas per tick | the tick thread |
| **identity** | `who.h/.cpp` | pid → `Identity` (name, image, cmdline, cwd, parent, session id, user, kind), tid → pid, agent attribution (harness / project / session), the System-writeback and vmmem rules | written by the collector thread, read by views under the snapshot lock |
| **fold** | `where.h/.cpp` | the directory trie with per-node, per-process, per-op accounting; the per-file table (bounded LRU); bursts (new-file clusters); the `Snapshot` (a frozen, renderable copy) | collector thread writes; tick thread snapshots |
| **rates** | `rates.h` | interval accounting: 1 s ticks, 10 s and 60 s windows, per-volume and per-process history rings for sparklines | tick thread |
| **tape** | `tape.h` | read/write/normalise path tapes (identical to facet's) | any |
| **handles** | `handles.h/.cpp` | the system handle table: FILE_OBJECT → (pid, name) for files already open — the rundown complement at session start and the `--open PATH` answer; name queries on a worker with a timeout (ADR-014) | a worker thread, never the collector |
| **console** | `everywho.cpp` | modes, ANSI rendering, JSON, NDJSON, stamp, spool, MCP, `--where`, `--selftest`, elevation relaunch | main thread |
| **window** | `everywho_gui.cpp` | the rail, chips, the live table, the volume band, the driver seam, `--shot` | UI thread; collectors on theirs |

Data flows one way. The collector thread never allocates on the hot path except when a new
path or process appears (arena-backed), never takes a lock the UI holds, and never calls into
a view. The tick thread (1 s) reads counters, closes the interval, and swaps a `Snapshot`; views
read snapshots only.

## §3 · The data model

### RawEvent (decoder output, one per kernel event we keep)

```
kind        FileCreate | FileRead | FileWrite | FileDelete | FileRename | FileSetInfo | FileCleanup | FileClose |
            FileFlush | FileDirEnum | FileOpEnd | FileName | DiskRead | DiskWrite | DiskFlush | ProcStart | ProcEnd |
            ProcRundown | ThreadStart | ThreadEnd | ThreadRundown
qpc         event timestamp (QPC ticks; the session uses QPC as its clock)
pid, tid    from the header when valid, else via the thread table (tid → pid)
file_object / file_key    the kernel's object identity (pointer-sized) for name lookup
irp         request identity, to pair a FileWrite with its FileOpEnd (status)
size, offset, flags       IoSize / Offset / IoFlags (paging, non-cached, …)
name        present only on FileCreate (OpenPath), FileName (rundown), FileRename (new name), ProcStart (image, cmdline)
status      NTSTATUS on FileOpEnd
disk, response_ns, issuing_tid  on Disk* events
```

### Identity (per process, from rundown + enrichment)

```
pid, ppid, session_id, user (SID → name, cached)
name ("python.exe"), image (full path), cmdline, cwd, start_qpc
kind   App | Agent | System | Wsl | Service | Unknown
agent  { harness: "claude-code" | "dsh" | "codex" | …, project: "C--facet", session: "<uuid>", subagent: "agent-…" | "" }
```

The **agent attribution rules**, in order, each marked in the record as the rule that fired:
1. *tape append*: the process writes to `<home>\.claude\projects\<slug>\<uuid>.jsonl` (or
   `…\subagents\agent-<id>.jsonl`) → harness `claude-code`, project `<slug>`, session `<uuid>`.
   The same for `${DSH_HOME}\sessions\<x>\<y>\session.jsonl.zstd` → `dsh`.
2. *cwd under a project*: cwd is a Claude Code project directory (a `<slug>` exists for it under
   `.claude\projects`) → harness by ancestor chain (a `claude.exe`/`node.exe claude` ancestor) with
   project from the cwd, session unknown.
3. *image name*: `claude.exe`, `node.exe` with `claude` in the command line, `codex`, `cursor`,
   `dsh` markers → harness only.
4. *inherit* (ADR-015): a process with no attribution of its own takes its nearest attributed
   ancestor's — a compiler, shell or formatter spawned by an agent session is that session's
   I/O — at most eight parents up, never across session ids.
5. `vmmem*` → `Wsl`; pid 4 → `System`; session 0 services → `Service`.

### The fold (where)

- **Directory trie** exactly as facet's (`C:\facet\facets.cpp`): nodes keyed by full parent
  path via a transparent-hash map with a last-path cache; children sorted at snapshot time.
  Per node: `file_read`, `file_write`, `disk_read`, `disk_write` bytes; op counts by kind;
  distinct files (a small counting Bloom or exact set below a bound); the top-4 contributing
  pids with their bytes (a tiny fixed array, replace-min).
- **Per process**: the same counters, plus the top-8 directories by bytes and distinct files.
- **Per file** (bounded LRU, 100k entries by default): pid of last writer/reader, bytes,
  ops, first/last qpc, deleted/renamed flags. Evicted files fold into their directory node and
  are not lost from totals.
- **Bursts**: new-file creates clustered by time gap (facet's rule, 60 s) with their dominant
  directory and pid — the live version of facet's write bursts. Every counter set also carries
  `new_files`, so `files_per_min` is a column and a sort (`-s newfiles`) everywhere, not just a
  burst summary.
- **Snapshot**: an immutable copy of the above (top-K everywhere, never the full tables) taken
  each tick; views render only snapshots; JSON is a snapshot serialised.

### Attribution rules for I/O (each yields `raw` and `attributed` numbers)

- A `FileWrite` in pid 4 (System) with the *paging I/O* flag, on a file whose last user-mode
  writer is known → attributed to that writer as `writeback`; raw stays on System.
- `DiskWrite` events carry the issuing thread (V3+): map to its process; if the thread is a
  System worker, use the FileObject's last writer as above.
- Reads served from cache are file reads with no disk read; the difference between
  `file_read` and `disk_read` per process is the cache hit volume, reported, not guessed.
- `vmmem` I/O is WSL's, shown as `WSL (<distro if known>)`.

## §4 · Contracts

### Snapshot JSON (single line; every field either kernel-sourced or marked derived)

```json
{"tool":"everywho","version":"0.1.0","wall_ms":0,"window_ms":3000,"tier":"etw","elevated":true,
 "session":{"name":"everywho","events":123456,"lost":0,"buffers":64,"decode_us_per_event":0.31},
 "volumes":[{"letter":"C:","device":"\\Device\\HarddiskVolume3","disk":0,
   "file_read_bps":0,"file_write_bps":0,"disk_read_bps":0,"disk_write_bps":0,
   "read_iops":0,"write_iops":0,"queue":0.0,"busy_pct":0.0,"response_ms_p50":0.0,"response_ms_p95":0.0}],
 "processes":[{"pid":0,"name":"","image":"","cmdline":"","cwd":"","parent":0,"session_id":1,"user":"",
   "kind":"Agent","agent":{"harness":"claude-code","project":"","session":"","subagent":"","rule":"tape"},
   "file_read":0,"file_write":0,"disk_read":0,"disk_write":0,"attributed_write":0,
   "ops":{"create":0,"read":0,"write":0,"delete":0,"rename":0,"flush":0,"direnum":0},
   "files":0,"top_dirs":[{"path":"","write":0,"read":0,"files":0}]}],
 "directories":[{"path":"","file_write":0,"file_read":0,"disk_write":0,"disk_read":0,"ops":0,"files":0,
   "pids":[{"pid":0,"write":0}],"children":[]}],
 "files":[{"path":"","pid":0,"write":0,"read":0,"ops":0,"first_ms":0,"last_ms":0,"deleted":false,"renamed_to":null}],
 "bursts":[{"start":"","end":"","files":0,"bytes":0,"pid":0,"dir":"","dir_share":1.0}],
 "error":null}
```

NDJSON (`-j -w`) emits one such line per interval. `--paths` emits the `files[].path` set
(unique, every file touched in the window, not just the top-K) as a tape.

### Stamp (one line, embeddable verbatim; the io twin of `gpu_stamp`)

```
io_stamp t=2026-09-02T10:41:03 tier=etw C: fr=12.1 fw=48.3 dr=0.4 dw=41.0 q=1.2 busy=61% D: idle top=node:41.0M(claude-code/C--facet),Everything:3.1M,chrome:2.2Mx14 lost=0
```

### Spool (`lane<TAB>text`, lane `io`, change-gated on `--gate-mb` / `--gate-pct` / new top writer)

```
io	C: w 48.3M/s r 12.1M/s q1.2 | node 41.0M C:\Users\user\.claude\projects\C--facet\ | Everything 3.1M | chrome×14 2.2M
```

### MCP tools (stdio JSON-RPC, framing identical to facet/vramtop)

- `io_snapshot {sample_ms, top, pid?, name?, dir?, exclude?, ops?, tier?}` → the snapshot JSON.
- `io_watch {seconds, interval_ms, …}` → one aggregated snapshot over the whole period plus the
  per-interval series for the volumes (so an agent can watch a build without polling).
- `io_stamp {sample_ms}` → the stamp line (or `json:true` for the object).
- `io_paths {seconds, dir?, pid?, ops?}` → `{paths:[…]}` — the tape of touched files.

### Exit codes

`0` ok · `1` bad arguments · `2` collector error (JSON still emitted, with `error`) · `3` selftest
failed · `4` the ETW tier was required (`--tier etw`, `--paths`, `--files-from`, `--dir`) and
elevation is missing — the message says how to get it.

### Settings (`everywho.ini` next to the exe, or `--ini`)

Standing excludes (chips pinned in the window), window placement, the last tier choice.

## §5 · Time

The session clock is QPC (`Wnode.ClientContext = 1`); event timestamps are converted with the
QPC frequency from the trace header. A tick is 1 s of wall time; the one-shot window is
`--sample-ms` exactly, measured from the first event after rundown completes to the stop. Rates
are bytes per second over the window they name, never smoothed unless the window is named
(`--window 10` = a 10 s moving window in the TUI).

## §6 · Performance targets (falsifiers in `ROADMAP.md`)

| target | number | why |
|---|---|---|
| decode + fold per event, hot path | ≤ 0.5 µs, no allocation for known paths | 500k events/s on one core with headroom |
| lost events | 0 at 100 MB/s of 4 KB writes with 64 × 64 KB buffers | the clone-landing case |
| memory | ≤ 100 MB at 1 M distinct paths; file table bounded at 100k entries | a whole-day `--spool` must not grow |
| first snapshot | ≤ 1.2 s after start (rundown + one tick) | the agent's `io_snapshot` budget |
| window | 60 fps not required; ≤ 16 ms per paint at 2762×1721 | the 225 % display |

## §7 · Elevation

`SeSystemProfilePrivilege` is required to start a system logger. The process enables it in its
token if present (admin, or *Performance Log Users*). Without it: `--tier auto` (default) falls
to counters and says so; `--tier etw` exits 4 with the how-to; `--elevate` relaunches the same
command line through `ShellExecuteEx` `runas`, with `--out FILE` so the elevated child's report
is readable by the unelevated caller (consoles do not cross the boundary). The window offers
the same relaunch through its own prompt. Nothing else asks.

## §8 · Stages

See `ROADMAP.md`: 0 counters + identity · 1 ETW + oracle · 2 fold/rates/views/tapes/MCP ·
3 window · 4 seams. The oracle (`tests/`) exists before the engine, like ripgrep before
everywhere.

## §9 · Refusals

No driver. No injection. No contents. No daemon by default. No verdicts. No silent elevation.
No third-party code. No language but C/C++.

## §10 · Traps to pre-empt (the family's scar tissue, plus ETW's own)

- **The NT Kernel Logger is a singleton.** Take the system-logger mode with our own name, or
  everywho fights WPR/xperf and any other consumer (`ETW.md` §1).
- **`FileIo_ReadWrite` carries no name.** Keep `FileObject → name` from `FileCreate` and
  `FileKey → name` from the rundown; a read on a file opened before the session started is
  nameless until the rundown (`EVENT_TRACE_FLAG_DISK_FILE_IO`) or the next open names it.
- **Names arrive as NT paths** (`\Device\HarddiskVolume3\…`, `\??\C:\…`, `\SystemRoot\…`,
  `\Device\Mup\…`, `\Device\NamedPipe\…`). Build the device table with `QueryDosDevice` at start,
  refresh on volume arrival; classify non-file devices out of the tree, not into it.
- **System (pid 4) writes are somebody's.** Writeback, mapped files, the lazy writer. Attribute
  with the rule, label the rule.
- **`ProcessTrace` blocks the thread it runs on**; stop the session with `ControlTrace(STOP)`
  and let it return. `CloseTrace` after.
- **Buffers**: default `BufferSize` is too small for a storm; 64 KB × 64 minimum, `MaximumBuffers`
  256, `FlushTimer` 1 s. Read `EventsLost` every tick.
- **Pointer size**: the kernel is 64-bit and so are we; still honour `EVENT_HEADER_FLAG_64_BIT_HEADER`
  in the decoder and refuse 32-bit traces cleanly.
- **The same physical-pixel and child-window traps as facet**: size the window by system DPI,
  `WS_CLIPCHILDREN`, caret at the end of the query box, `gdiplus` needs `objidl.h` and
  `using std::min/max` under `NOMINMAX`, `small` is a macro.
- **Argument evaluation order** in a `check(cond, message)` — build the message after the call
  (twice bitten in facet).
- **A DPI-unaware test driver sees a virtualised client rect**; make the driver DPI-aware.
