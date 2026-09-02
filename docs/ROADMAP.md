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
