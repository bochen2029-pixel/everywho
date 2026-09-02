# everywho — who is touching what, right now

**Task Manager shows the disk at 100 %. `facet` shows what landed on it last night. everywho
shows who is writing which directory *this second* — per process, per agent session, per
directory tree — headless for agents, live in a window, one C++ exe, no driver.**

> **Status: Stage 0 built (0.1.0) — the counters tier.** `everywho.exe` answers *who* and *how much* for every process (identity, agent attribution, per-disk rates) as the report, the watch TUI, JSON / NDJSON, the stamp, the spool, `--where`, `--selftest` and a two-tool MCP server. The ETW tier (*what*: directories, files, bursts) is Stage 1. The design below is what the build implements:
> the contracts live in `src/*.h`, the reasoning in `docs/`, the rules in
> `CLAUDE.md`. Read `CLAUDE.md` first, then `docs/ARCHITECTURE.md`.

## The gap it fills

Windows accounts for every file operation in the kernel and offers three views of it:
Task Manager (bytes per second per process, no files), Resource Monitor (a flat per-file list,
no pivot, no headless surface) and Process Monitor (a firehose behind a filter driver, no
directory pivot, no JSON, no MCP). None of them answers the questions a multi-tenant desktop
asks every day:

- *which agent is filling the disk, and where?*
- *what is that 03:00 job touching?*
- *why is the SSD pegged — who is hitting the media, not the cache?*
- *is anyone touching these files right now?* (a tape from facet or everywhere)
- *which session wrote that burst facet just showed me?*

A box running resident LLMs, speech models, browsers and an agent swarm is a server; this is its
I/O instrument, in the family's shape: the OS already holds the data, the stock UI refuses to
pivot it, so we write the view and give it a headless twin.

## Two tiers, one tool

| tier | needs | who | how much | what |
|---|---|---|---|---|
| **counters** | nothing | every process | read/write bytes and ops per second, per-volume throughput and queue | — |
| **ETW** | elevation (or membership in *Performance Log Users*) | every process, thread-accurate, with rundown identity | the same, plus physical vs cached | **every file and directory**: create, read, write, delete, rename, with the path |

everywho starts in the ETW tier when it can, falls back to counters when it cannot, and always
says which one it is in (`tier` in JSON, the status line in the window). `--elevate` relaunches
through UAC; nothing ever elevates silently.

## Use  (Stage 0 builds the first two groups; lines marked · Stage 1+ wait for the ETW tier — see `docs/CLI.md`)

```
everywho                    a 3-second sample, then the report: who wrote / read what, by process and directory
everywho -w                 live TUI   q quit · s sort · g group (process ↔ directory) · p pause · f freeze
everywho --gui              the window   (everywho-gui.exe opens it with no console at all — pin it)   · Stage 3
everywho -j                 one JSON snapshot            ·   everywho -j -w -n 2000 = NDJSON stream
everywho --stamp            one-line receipt: MB/s per volume, queue, top writers — for timing rows
everywho --spool            change-gated "lane<TAB>text" (fusor / TOWER tailer food, lane "io")
everywho --paths            the files touched during the sample, as a tape → facet --files-from - / everywhere   · Stage 1+
everywho --files-from T     watch only the paths in a tape: "is anyone touching these?"   · Stage 1+
everywho --open PATH|DIR    who has it open — every process holding a handle to the path or below it   · Stage 2
everywho --mcp              MCP stdio server — tools: io_snapshot, io_stamp now; io_watch, io_paths with the ETW tier
everywho --where            tier, elevation, session state, other kernel loggers, the volume map
everywho --selftest         decoder, fold, rates, identity, and a planted live workload when elevated
```

## The numbers, honestly

- **File I/O vs disk I/O.** A write to a cached file is a *file* write now and a *disk* write
  later, by the System process. everywho reports both, separately: `file_*` (what processes
  asked for) and `disk_*` (what reached the media), and attributes System writeback to the last
  user-mode writer of that file as `attributed` — with the raw System number still visible.
- **A sample is a window.** One-shot mode watches for `--sample-ms` (3 s) and reports rates
  over exactly that window; the watch/stream/spool loops report per interval. Nothing is
  extrapolated.
- **Lost events are printed**, never hidden: the session's `EventsLost` counter is in every
  snapshot; if it is non-zero the rates are lower bounds and the status line says so.
- **Names are names.** everywho never reads a file's contents. It sees paths, sizes, offsets
  and operation kinds — what the kernel logs.
- **Who means the process; the session is inferred.** A Claude Code process that appends to
  `~/.claude/projects/<slug>/<uuid>.jsonl` *is* that session; the tape it writes identifies it.
  That attribution is a rule, marked as such, and it is what lets everywho answer with the
  session id that `everywhen` and `facet` already speak.

## Agents

```bash
everywho -j --sample-ms 2000                 who is writing, right now, and where
everywho --stamp                             the io line to embed next to a vramtop gpu_stamp
everywho --paths --sample-ms 10000 | facet --files-from -      where the last ten seconds landed
facet --paths ext:md dm:today | everywho --files-from - -w     is anyone still touching today's markdown
claude mcp add everywho -- C:/Intellect_AI_tools/everywho/everywho.exe --mcp
```

The JSON shape is stable and documented in `docs/CLI.md`; every field either comes from the
kernel or is marked derived.

## The family

| organ | answers | surface |
|---|---|---|
| `C:\Everything` + `C:\facet` (0.4) | which files, by name / date / size — and *where they went* (+ `--grep`: which of them contain a phrase) | report · rows · JSON · MCP · window · tapes |
| `C:\everywhere` | which files *contain* this, at drive speed | rg-compatible · JSONL · `--files-from` |
| `C:\everywhen` | which *sessions* said it, message-grain, forks deduped; `locate` a tape line | search · `--paths` · `--json` |
| `C:\GPUz` vramtop | who holds the VRAM, who burns the engines | snapshot · TUI · JSON · MCP · stamp · spool · treemap |
| **everywho** | who is touching what, right now | the same surfaces, plus tapes |

The **tape** — one full path per line, or JSONL with `path`/`file` — is the pipe between all
of them; everywho both writes one (`--paths`) and reads one (`--files-from`).

## Build

```
build.bat     # VS2022: cl /std:c++20 /O2 /W4 /WX /permissive- /utf-8 /MT → everywho.exe (+ everywho-gui.exe once src/everywho_gui.cpp exists)
```

Files: `src/etw_layouts.h` (the kernel events we decode: GUIDs, opcodes, field orders) ·
`src/etw_session.h/.cpp` (the collector: session, consumer thread, decoder → `RawEvent`) ·
`src/who.h/.cpp` (process and thread identity, agent attribution, the inherit rule) ·
`src/where.h/.cpp` (the fold: directory trie, per-process and per-file accounting, bursts,
files per minute) · `src/handles.h/.cpp` (the system handle table: names for files already
open, and `--open`) · `src/rates.h` (windows, rings, sparklines) · `src/tape.h` (the tape
contract) · `src/app_util.h` (options, formatting) ·
`src/everywho.cpp` (console modes, JSON, MCP, stamp, spool, selftest) · `src/everywho_gui.cpp`
(the window) · `tests/` (the planted-workload oracle and its harness) · `docs/` (this design).

## Known limits (by design)

- The *what* tier needs elevation: the kernel's file events are privileged. Unelevated,
  everywho still names who and how much.
- ETW is a sampled truth: under extreme event rates buffers can drop events; everywho sizes
  buffers for 100 MB/s of small writes and reports any loss rather than pretending.
- Memory-mapped writes surface as System writeback; attribution to the mapper is a heuristic
  (the last process that opened the file for write), labelled as such.
- Windows 10 1809+ / 11, x64 only, by construction.

## Why the classics can't do this

Task Manager and Resource Monitor read the same counters everywho's first tier reads and stop
there. Process Monitor sees everything through its own filter driver but is built to *browse*
one event at a time; it has no directory pivot, no rates by tenant, no JSON, and it cannot be
asked a question by an agent. Windows Performance Recorder captures the same kernel events to a
file for later analysis in a GUI. everywho is the live pivot over that kernel stream, in the
shape the family already has: a rail you click, a JSON an agent reads, a line a log embeds.
