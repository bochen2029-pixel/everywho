# Roadmap — stages, deliverables, falsifiers

Each stage ends green (`--selftest`, the oracle where it applies, the driver where it applies),
with a devlog entry carrying numbers, and one commit `everywho 0.X.0 — …`.

## Stage 0 · counters tier, identity, the report — `0.1.0`
**Build:** `app_util.h`, `who.cpp` (rundown-free identity from `NtQuerySystemInformation`
process enumeration + enrichment), `counters.cpp` (I/O counters + disk performance deltas),
`rates.h`, `where.cpp` with per-process accounting only, `everywho.cpp` with Snap / Watch / JSON /
NDJSON / stamp / where / selftest / help, `build.bat`, rc, manifest, icon, shortcut.
**Green:** selftest offline + counters live; the report on the reference box names the working
directory and harness of the Claude Code processes and the WSL `vmmem`.
**Falsifier:** if identity cannot name the cwd of ≥ 95 % of the top-20 I/O processes (own user,
unelevated), the enrichment approach is wrong — fix before Stage 1 (elevation is not the cure).

## Stage 1 · the ETW session, the decoder, the oracle — `0.2.0`
**Build first:** `tests/plant` and `tests/harness`. **Then:** `etw_layouts.h` complete for every
event in `ETW.md` §2, `etw_session.cpp` (privilege, stale stop, start, consumer thread, decoder,
lost-event accounting, stop on every exit path), `--verify-layouts`, name resolution (device
table, FileObject/FileKey maps), the thread table, `--elevate --out`.
**Green:** `--verify-layouts` 0 divergences; the oracle byte-exact for writes, reads, renames,
deletes across 4 child processes; performance smoke: 100 MB in 4 KB writes, `lost == 0`,
`decode_us_per_event ≤ 0.5`; 100 start/stop cycles leave nothing behind.
**Falsifier:** attribution off by > 1 % of bytes on the plant, or events lost below 100 MB/s with
4 MB of buffers → the consumer/decoder design is wrong; stop and fix, do not tune around it.

## Stage 2 · the fold, rates, views, tapes, MCP — `0.3.0`
**Build:** `where.cpp` complete (trie, per-file LRU, bursts, attribution rules, snapshot),
volume rings, the full report and TUI, `--paths`, `--files-from`, `--dir`/`-x`/`--ops`
filters, spool, MCP (`io_snapshot`, `io_watch`, `io_stamp`, `io_paths`).
**Green:** `--paths` of a plant run equals the manifest's file set; `--files-from` sees only the
tape; the spool gate stays silent on an idle box and fires on the plant; MCP tools answer with
the same JSON as the CLI.
**Falsifier:** memory grows with events rather than with distinct paths during a 30-minute
`--spool` under the plant loop → the tables are not bounded; fix before the window.

## Stage 3 · the window — `0.4.0`
**Build:** `everywho_gui.cpp` per `GUI.md`; `tests/drive.cpp`.
**Green:** the driver's invariants; a `--shot` showing the plant.
**Falsifier:** a paint over 16 ms at the reference resolution, or a UI stall while the collector
runs a storm → the snapshot boundary is being crossed; find the lock.

## Stage 4 · the seams — `0.5.0`
- fusor / TOWER: the `io` lane beside vramtop's `gpu` lane.
- facet: `everywho --paths | facet --files-from -` in facet's README; a facet burst row that
  offers "watch this directory live" (opens everywho with `--dir`).
- everywhen: session tapes everywho attributes carry the uuid everywhen speaks; `everywhen
  locate` resolves an everywho `.jsonl` hit to the message being appended.
- muster (the fleet console, next tool): the JSON here is the I/O column there.
**Green:** each seam demonstrated in the devlog with a command and its output.

## After
- Per-thread breakdown inside a process (the thread table already exists).
- Network I/O (`TcpIp` / `UdpIp` kernel providers) as a sibling band, then muster.
- Registry I/O (`EVENT_TRACE_FLAG_REGISTRY`) as a facet: which process hammers the registry.

## Additions after the fork review (2026-09-02, before implementation)

- **Stage 1b - the manifest backend spike.** After the classic decoder is green, enable
  `Microsoft-Windows-Kernel-File` / `-Disk` / `-Process` in a *private* session (`ETW.md` §9)
  and measure against the same oracle: attribution exactness, decode cost, loss, and the
  nameless share without the classic rundown. If the classic system-logger session is refused
  on a box, or the manifest path is equal on the oracle and cheaper to keep alive, the manifest
  backend becomes the default; `EtwConfig::backend` already carries the switch.
- **Stage 1 - handles as the rundown complement** (`src/handles.h`): after the session starts,
  one scan of the system handle table names every FILE_OBJECT already open, so long-held
  files (databases, caches, log files, session tapes held open) are named from their first
  event. Falsifier: a name query that hangs the collector; the scan runs on a worker with a
  per-handle timeout, never on the ETW thread.
- **Stage 2 - `--open PATH|DIR`**: who has this open, Process Explorer's find-handle as one
  line with JSON; the same handle scan, filtered.
- **Stage 2 - files per minute** as a first-class rate (`new_files` in every counter set,
  `files_per_min` in the snapshot, `-s newfiles`): facet's provenance signal, live.
- **Stage 3 - the treemap** (vramtop's, reused): `T` toggles the table to a treemap of process
  × directory × bytes; the burst facet's live twin as a picture.
- **Identity - the inherit rule**: a process with no agent attribution of its own takes its
  nearest ancestor's (a build spawned by an agent session is that session's I/O), marked
  `rule: "inherit"`.

## Effort, honestly (the vramtop rhythm)

| stage | days | what dominates |
|---|---|---|
| 0 | 1-2 | identity enrichment and the report's shape |
| 1 | 3-4 | the oracle first, then the decoder, then `--verify-layouts` and the storm |
| 1b | 1 | the manifest spike, measured, decided |
| 2 | 3 | the fold's bounds, `--paths` / `--files-from`, MCP, spool, `--open` |
| 3 | 3-4 | the window, the driver, the treemap |
| 4 | 1-2 | seams and screenshots |

About two and a half weeks of focused sessions; the fork's estimate of two was close.
