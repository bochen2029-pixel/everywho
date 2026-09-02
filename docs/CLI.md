# CLI — the console surface, the JSON, the seams

Modelled on `vramtop` (watch / stamp / spool / NDJSON / MCP) and `facet` (report / rail / tapes /
where / selftest). Every mode prints for agents first.

## Modes

```
everywho                    Snap:   watch --sample-ms (3000), then the report
everywho -w | --watch       Watch:  live TUI, keys below
everywho --gui              Gui:    the window (everywho-gui.exe = no console)
everywho -j | --json        JSON:   one snapshot line; with -w: NDJSON, one line per --interval
everywho --stamp            Stamp:  one io_stamp line (--json for the object)
everywho --spool            Spool:  change-gated "lane<TAB>text" stream (lane "io")
everywho --paths            Paths:  the files touched in the window, one full path per line (-0 NUL)
everywho --open PATH|DIR    Open:   who has it open — every process holding a handle to the path or below it (JSON with -j)
everywho --mcp              Mcp:    MCP stdio server (tools below)
everywho --where            Where:  tier, elevation, privilege, session state, other loggers, volumes
everywho --selftest         Selftest
everywho --make-icon F.ico · --shortcut [startmenu|desktop] · -h · -v
```

## Options

```
WINDOW
  --sample-ms N        one-shot window, ms (default 3000; the window starts after rundown completes)
  -n, --interval N     watch / stream / spool cadence, ms (default 1000)
  --frames N           stop watch / stream / spool after N intervals (0 = until quit)
  --window S           TUI moving window in seconds: 1 | 10 | 60 (default: the interval)

TIER
  --tier auto|etw|counters   default auto: ETW when the privilege is there, else counters
  --elevate            relaunch this exact command through UAC; use with --out FILE
  --out FILE           write the report / JSON there instead of stdout (the elevated child's channel)

FILTERS (each narrows the fold; all are visible in the report's FILTER line)
  --pid N              only this process (repeatable)
  --name GLOB          only processes whose name matches (node.exe, python*, repeatable)
  --dir DIR            only I/O under this directory (repeatable; ETW tier)
  -x, --exclude DIR    drop I/O under this directory (repeatable; pinned excludes come from everywho.ini)
  --files-from F|-     only I/O to paths in this tape (ETW tier): "is anyone touching these?"
  --ops K[,K…]         read | write | create | delete | rename | flush | direnum (default all)
  --volume L           only this volume letter (repeatable)
  --agents             only processes attributed to a harness (claude-code, dsh, codex, …)

SHAPE
  -s, --sort K         write | read | disk | ops | files | newfiles | name | pid   (default write; newfiles = files per minute)
  -g, --group          rows grouped by process name (chrome.exe ×14) — the default in the TUI
  --top N              rows per section (default 12)
  --depth N            directory tree levels below the top entries (default 2)
  --min-mb X           hide rows lighter than X MB in the window (default 0.05)
  --raw                show raw (unattributed) numbers only: System stays System
  --plain              no colours / ASCII bars      -q  no progress on stderr

SPOOL
  --spool-file P · --lane S (default io) · --gate-mb X (default 8) · --gate-pct X (default 25)

WINDOW (gui)
  --shot FILE.png · --ini PATH · --no-activate
```

## The report (one-shot)

```
everywho 0.1.0 · 2026-09-02 10:41:03 · 3.0 s window · etw (elevated) · 12,406 events · 0 lost · 0.31 µs/event

VOLUMES         file read     file write    disk read     disk write   queue   busy
 C:  NVMe        12.1 MB/s     48.3 MB/s      0.4 MB/s     41.0 MB/s    1.2    61 %  ████████░░░░
 D:  HDD              idle          idle          idle          idle    0.0     0 %

WHO  (by file write)                                write      read     ops   files   where
 node.exe ×3   claude-code · C--facet · 6ebf…       41.0 MB    0.3 MB   1,204     3   C:\Users\user\.claude\projects\C--facet\  100 %
 Everything.exe                                      3.1 MB      0 B      88     1   C:\Users\user\AppData\Local\Everything\
 chrome.exe ×14                                      2.2 MB    9.8 MB   2,310   211   C:\Users\user\AppData\Local\Google\Chrome\…  93 %
 System  (writeback → node.exe 1.7 MB, chrome 0.2 MB)   1.9 MB    0 B      41    12

WHERE  (by bytes, file)                              write      read     ops   procs
 C:\Users\user\.claude\projects\C--facet\           41.0 MB    0.3 MB   1,204   1
   6ebfe793-…-49923015efbc.jsonl                     41.0 MB    …
 C:\Users\user\AppData\Local\                         5.3 MB    9.8 MB   2,398   15
   Google\Chrome\User Data\Default\                   2.2 MB    9.8 MB
   Everything\                                        3.1 MB      0 B

OPERATIONS   read 9,812 · write 1,301 · create 412 · delete 17 · rename 3 · flush 40 · dir-enum 2,104
FILES (top)  41.0 MB  node.exe   C:\Users\user\.claude\projects\C--facet\6ebfe793-….jsonl
             …
BURSTS       412 new files in 2.1 s  chrome.exe  C:\Users\user\AppData\Local\Google\Chrome\User Data\Default\Cache\  98 %

FILTER   (none)      --pid N · --name GLOB · --dir DIR · -x DIR · --files-from TAPE · --ops write,create
```

Colours and bars as in facet/vramtop: amber for ≥ 50 % share and for a non-zero `lost`.

## Watch (TUI) keys

`q` quit · `s` cycle sort (write → read → disk → ops → files → newfiles) · `g` group by process ↔ by
directory · `p` pause · `f` freeze (keep the current snapshot on screen while collection
continues; `f` again to release) · `t` cycle tier view (attributed ↔ raw) · `1/10/60` moving window.

## `--open PATH|DIR`

```
everywho --open C:\Users\user\AppData\Local\Everything\
 pid    process         access      path
 17292  Everything.exe  rw          C:\Users\user\AppData\Local\Everything\Everything.db
 4      System          r           C:\Users\user\AppData\Local\Everything\Everything.db
# 2 handles in 2 processes · scan 41 ms · 8,912 file objects · 3 name queries skipped (pipes)
```

The handle scan (`ARCHITECTURE.md` §2, ADR-014). `-j` gives `{"open":[{pid,name,access,path,directory}],"scan":{…}}`.
Other users' processes need elevation; without it the line says how many were not openable.

## JSON

See `ARCHITECTURE.md` §4 for the full snapshot shape. Stability rules: field names never
change meaning; new fields are added, never renamed; numbers are integers in bytes and counts,
floats only for rates, percentages and seconds; `null` means not available in this tier, `0`
means measured zero. NDJSON is the same object per interval with `interval_ms` set.

## Stamp

```
io_stamp t=<iso> tier=<etw|counters> <volume> fr=<MB/s> fw=<MB/s> dr=<MB/s> dw=<MB/s> q=<depth> busy=<pct>% … top=<name>:<MB>(<harness>/<project>)[x<count>],… lost=<n>
```

`--stamp --json` → `{"stamp":"io","wall_ms":…,"tier":…,"volumes":[…],"top":[…],"lost":0}`.

## Spool

`lane<TAB>text` on stdout or `--spool-file`; a line only when any volume's write or read rate
moved ≥ `--gate-mb`, the busy % moved ≥ `--gate-pct`, or the top writer changed. Silence stays
silent; the first line always prints.

## Tapes

`--paths` prints every path touched in the window (unique, order of first touch), so:

```
everywho --paths --sample-ms 10000 | facet --files-from -            where the last ten seconds landed
everywho --paths --ops write,create --sample-ms 60000 | everywhere --files-from - -e "secret" -l
facet --paths ext:jsonl path:.claude | everywho --files-from - -w     which session tapes are being appended, live
```

`--files-from` accepts everything facet accepts: LF/CRLF/NUL lines, JSONL with `path` or `file`,
comments, duplicates.

## MCP (`--mcp`)

Framing identical to facet's: newline-delimited JSON-RPC 2.0 on stdio; `initialize`, `ping`,
`tools/list`, `tools/call`; notifications ignored; errors as `{"content":[…],"isError":true}`.

| tool | arguments | returns |
|---|---|---|
| `io_snapshot` | `sample_ms` (default 2000), `top`, `pid`, `name`, `dir`, `exclude[]`, `ops[]`, `tier`, `agents` | the snapshot JSON |
| `io_watch` | `seconds` (≤ 300), `interval_ms`, the same filters | `{summary: <snapshot over the whole period>, series: [{t, volumes:[…]}…]}` |
| `io_stamp` | `sample_ms`, `json` | the stamp line or object |
| `io_paths` | `seconds`, `dir`, `pid`, `ops[]` | `{paths:[…], count}` |

Register: `claude mcp add everywho -- C:/Intellect_AI_tools/everywho/everywho.exe --mcp`. An
unelevated MCP server answers in the counters tier and says so in every result; a tool call that
needs the ETW tier returns an `isError` result whose text is the elevation how-to, verbatim.

## Exit codes

`0` ok · `1` bad arguments · `2` collector error (JSON still emitted with `error`) · `3` selftest
failed · `4` the ETW tier was required and elevation is missing.

## `--where`

```
tier             etw (elevated)            | counters (SeSystemProfilePrivilege absent)
privilege        SeSystemProfilePrivilege  present / absent · Performance Log Users: no
session          everywho: running, 64 buffers × 64 KB, 0 lost   |   none
other loggers    NT Kernel Logger: free · Circular Kernel Context Logger: running · 27 sessions
volumes          C: \Device\HarddiskVolume3 (disk 0, NVMe)   D: \Device\HarddiskVolume5 (disk 1)
layouts          --verify-layouts: 21 verified, 0 divergent
```

## Stage 0 build notes (0.1.0, the counters tier)

What the shipped build answers, and how the contract above degrades without the ETW tier:

- **Volumes** carry `disk_read_bps` / `disk_write_bps` / `read_iops` / `write_iops` / `queue` /
  `busy_pct` from PDH; `file_*_bps` and the response percentiles are `null`.
- **Processes** carry `file_read` / `file_write` / `other_bytes` (what the process asked for, from
  the kernel's per-process counters) and `ops: {read, write, other}`; `disk_*`, `attributed_write`,
  `files` and `top_dirs` are `null` / empty until Stage 1. `cwd` is `null` when the PEB could not
  be read (another user's process, unelevated). `agent.rule` is `image`, `cmdline` (the session
  came from `--resume=` / `--session-id`), `cwd` or `inherit`; `tape` arrives with the ETW tier.
- `directories`, `files`, `bursts` are empty arrays; `session` is `null`; `tier` is `"counters"`.
- Extra fields: `processes_seen` (every process the kernel listed), `processes_with_io`, `exited`
  (processes that vanished during the window — their final counters are not recoverable in this
  tier), and `total`.
- **Report rows** group by process name *and* harness, so the Claude desktop app and the Claude
  Code CLI (both `claude.exe`) never share a row; `--no-group` lists pids. `--min-mb` hides
  rows below the threshold once three rows are shown.
- **TUI keys**: `q` · `s` (write → read → disk → ops → files) · `g` · `p` · `f` · `1` / `2` / `3`
  for a 1 s / 10 s / 60 s moving window.
- `--sort` accepts `write | read | disk | ops | files | newfiles | name | pid`; `disk`, `files`
  and `newfiles` order by zero until the ETW tier and fall back to write.
- `--stamp` prints `dr` / `dw` (media) per disk; `fr` / `fw` (file-level per volume) appear with
  the ETW tier. `--stamp -n MS --frames N` streams stamps.
- `--mcp` serves `io_snapshot` and `io_stamp`; `io_watch` and `io_paths` are Stage 2.
