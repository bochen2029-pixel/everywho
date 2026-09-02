# Testing — the oracle before the engine

Three layers, all C++: `--selftest` inside the exe, the planted-workload oracle in `tests/`, and
the window driver. A stage is not done until all three that apply are green and the devlog has
the numbers.

## 1 · `everywho --selftest`

Runs unelevated and elevated; prints `PASS`/`FAIL` per check, `SELFTEST: ALL PASS` or the
count, exit 3 on any failure. Offline checks never touch a session.

**Offline**
- `etw_layouts`: every layout table has monotone field sizes, every `(provider, opcode)` we
  handle has a table for each version we claim; synthetic `EVENT_RECORD`s built from the tables
  decode back to the values put in (round trip for every event kind, both pointer sizes).
- Truncated payloads decode to *absent* fields, never read past `UserDataLength`.
- NT → DOS name mapping: a fixture device table maps `\Device\HarddiskVolume3\Users\x` →
  `C:\Users\x`, `\??\D:\a` → `D:\a`, `\SystemRoot\notepad.exe` → the real one, `\Device\Mup\s\p\f`
  → `\\s\p\f`, `\Device\NamedPipe\x` → *device* class.
- The fold: a scripted sequence of RawEvents (create, writes, reads, delete, rename, close,
  System paging writes) produces the expected per-node, per-process, per-file, per-op numbers;
  attribution rules fire as documented and are labelled; eviction from the file table keeps
  directory totals exact.
- Rates: rings, moving windows, interval-length normalisation, sparkline downsampling.
- Identity: the current process resolves its own cwd, command line, parent and start time;
  the agent rules classify fixture identities (a `node.exe` writing a `.claude\projects\…jsonl`
  path → `claude-code`, project, session).
- Tape: facet's tape tests verbatim (CRLF, JSONL `path`/`file`, comments, duplicates, NUL).
- JSON: the snapshot serialises and parses back; the stamp line carries every volume; the spool
  gate is quiet when nothing moved and fires on each documented trigger.
- Formatting: counts, bytes, rates, display width, truncation.

**Live, counters tier (any user)**
- A counters snapshot lists this process; writing 8 MB to a temp file from a worker thread
  raises this process's `file_write` by ≥ 8 MB within two ticks.

**Live, ETW tier (elevated only; skipped with a note otherwise)**
- A stale `everywho` session, if any, is stopped; a new one starts; rundown delivers ≥ 1
  process `DCStart` and ≥ 1 `FileIo_Name`.
- `--verify-layouts` reports 0 divergences for every event kind seen during a 2 s window.
- A planted write (8 MB in 4 KB writes to a temp file from a child thread, then a rename, then
  a delete) is attributed byte-exact to this pid, the file appears with the right path, the
  rename and delete flags are set, and `lost == 0`.
- Stopping the session returns `ProcessTrace` within 2 s and leaves no session behind
  (`ControlTrace(QUERY)` fails with not-found).

## 2 · The oracle: `tests/plant` and `tests/harness`

`everywho-plant.exe` is a deterministic workload generator, seeded; it spawns *K* child copies
of itself (so the kernel sees distinct pids with distinct command lines) and each child performs
a scripted plan: create *N* files of listed sizes in a temp tree with a chosen directory shape,
write them with a chosen block size and cadence (to form bursts), read some back (cached and
non-cached via `FILE_FLAG_NO_BUFFERING`), rename a few, delete a few, flush some, enumerate a
directory. It prints a **manifest** (JSON): every child's pid, and every operation with path,
bytes and timestamps.

`everywho-harness.exe` starts `everywho -j -w --interval 500` (elevated), runs the plant, stops
everywho, and diffs: for each child pid, `file_write` equals the manifest's bytes exactly;
`file_read` equals the cached + non-cached reads; each file's path, first/last time, deleted
and renamed flags match; `disk_write` for non-buffered writes is ≥ the bytes written; no
unnamed bytes above a small tolerance for files created before the session; `lost == 0`.
It also runs the **performance smoke**: a storm of 100 MB in 4 KB writes across 1,000 files from
4 children; the falsifier is `lost > 0` or `decode_us_per_event > 0.5`.

The harness is red on any divergence and prints a minimal repro: the event sequence for the
first mismatching file.

## 3 · The window driver: `tests/drive`

A C++ program (DPI-aware) that starts `everywho-gui.exe --no-activate --ini <scratch>`, runs the
plant in the background, waits for the planted pid to appear in the log, and then: clicks the
planted process in the rail (only) and checks the table narrows to it; Esc; right-clicks it
(not) and checks it vanishes; posts a cell pick; toggles pause and freeze; captures PNGs with
`PrintWindow`; reads `EVERYWHO_LOG`; and exits non-zero on any broken invariant. Screenshots go
to the scratch folder, one to `docs/screenshot.png` when the operator says so.

## 4 · Acceptance per stage

| stage | green means |
|---|---|
| 0 | selftest offline + counters live; `everywho -j` lists every process the operator can see in Task Manager with a working directory for ≥ 95 % of the top 20 by I/O |
| 1 | `--verify-layouts` 0 divergences; the oracle byte-exact; performance smoke passes; no session leaks after 100 start/stop cycles |
| 2 | report / TUI / stamp / spool / paths / MCP all read from the same snapshot; `--paths` of a plant run equals the manifest's file set; `--files-from` a tape sees only those files |
| 3 | the driver's invariants; a `--shot` that shows the plant's process, directory and burst |
| 4 | a fusor lane consumes the spool; `facet --files-from` folds everywho's tape; `everywhen locate` resolves a session tape hit everywho reported; muster reads the JSON |

## 5 · Added after the fork review

- **Handle scan (offline shape, live behaviour).** Live, any user: open a temp file in this
  process, run `scan_open_files`; the file appears with this pid, the right DOS path and
  `directory == false`; `who_has_open(<temp dir>)` returns this pid; the stats report zero
  timeouts on a quiet box. Elevated: the scan also lists another user's processes and the
  `denied` count is 0. Negative: a named pipe handle is classified `skipped_unsafe`, never
  queried, and the scan still finishes under 500 ms with 10k file objects.
- **`--open`** against a plant child that holds files open: every held file is listed with the
  child's pid; after the child exits the list is empty.
- **The inherit rule.** A plant child spawned by a plant parent that carries a tape attribution
  reports `agent.rule == "inherit"` with the parent's session; a process in another session id
  never inherits.
- **files per minute.** Plant child 0 (200 files in one burst) shows `files_per_min` consistent
  with its manifest window; `-s newfiles` puts it first.
- **Stage 1b (manifest backend).** The same oracle, `--backend manifest`: attribution
  byte-exact for writes and reads; deletes and renames carry paths without a FileObject
  lookup; the nameless share is reported and compared with the classic run.
