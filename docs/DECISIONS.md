# Decisions (ADRs)

Short records of the choices that shape the tool. Change one by adding a new record that
supersedes it, not by editing history.

## ADR-001 · ETW is the boundary; no driver, no injection
Process Monitor sees more (every IRP, in order, with stacks) through a filter driver. That path
means signing, kernel risk and a driver to keep alive. The kernel's ETW providers already carry
who, what, how much, and status for every file and disk operation. Everything everywho wants is
in that stream; the stream is the boundary.

## ADR-002 · A system-logger-mode session with our own name
The `NT Kernel Logger` is one per machine and is what WPR, xperf and PerfView take. Windows 8+
allows up to eight system loggers with arbitrary names (`EVENT_TRACE_SYSTEM_LOGGER_MODE`). We
name ours `everywho`, coexist, and fall back to the singleton name only when the mode is refused
(with `--where` saying so).

## ADR-003 · Two tiers, one tool, always labelled
Kernel file events need `SeSystemProfilePrivilege`. Rather than be an admin-only tool, everywho
runs the counters tier for anyone and adds the ETW tier when it can. The tier is in every
output; `--tier etw` demands it and exits 4 otherwise; `--elevate` is the only elevation.

## ADR-004 · Decode from tables, verify with TDH
`TdhGetEventInformation` per event is too slow for the hot path and needs a live provider to
test against. The classic kernel layouts are documented and stable; we decode from
`etw_layouts.h` (field order + sizes → offsets at runtime by pointer size) and use TDH once,
in `--verify-layouts`, to prove the tables against the running OS. Divergence is a build-break.

## ADR-005 · The fold is facet's fold
The directory trie, the fold rules (1 % threshold, chain collapse, 5 % expansion), bursts, and
the rail come from `C:\facet`; everywho adds per-process and per-op dimensions rather than
inventing a new model. Same look, same words, same JSON style. When a shared library is lifted
out of the family, this fold is its first member.

## ADR-006 · Snapshots, never shared mutable state
The collector thread folds into live tables; the tick thread swaps an immutable `Snapshot`
(top-K everywhere); views and JSON read snapshots only. No view ever touches a live table; the
collector never takes a UI lock. Same discipline as vramtop's collector thread.

## ADR-007 · Attribution is a rule with a name
System writeback, mapped writes, thread-completed disk I/O, and agent sessions inferred from
tape appends are all *rules*. Each result carries which rule fired; raw numbers stay visible;
`--raw` turns the rules off. Numbers the kernel did not say are never presented as if it had.

## ADR-008 · Bounded memory by construction
Per-file table: LRU, 100k entries, evictions fold into directory totals. FileObject map: dropped
on Close, capped. Trie: grows with distinct directories seen (the disk's shape, ≤ 1 M nodes),
never with events. A day-long `--spool` must hold steady; `--selftest` measures it.

## ADR-009 · The session never outlives the process
Every exit path stops the session; start stops a stale one first. The one leak that harms the
operator after we are gone is a kernel logger left running; it is treated as a correctness bug,
tested by 100 start/stop cycles.

## ADR-010 · Tapes in, tapes out
`--paths` and `--files-from` speak facet's tape exactly, so everywho slots into the existing
pipes (facet, everywhere, everywhen) with no new format. The MCP tools return the same JSON the
CLI prints.

## ADR-011 · Not USN, not polling directories
The USN journal tells what changed without who; polling `GetFileAttributes` over trees is what
facet's fold already does after the fact. everywho's value is *who*, live, so its source is the
kernel stream. Everything's index remains the source for names at rest.

## ADR-012 · Names: `everywho.exe` and `everywho-gui.exe`
The `every*` organs use `-gui` (everywhen); the console exe carries every mode and can open the
window itself (`--gui`), as facet and vramtop do.
