# tests — the oracle, the harness, the driver

All C++ (`build.bat tests`). Built **before** the engine in Stage 1, as `docs/TESTING.md` §2 says.

## `plant.cpp` → `everywho-plant.exe`

A deterministic, seeded workload generator. It spawns *K* child copies of itself (distinct pids,
distinct command lines: `everywho-plant --child N --seed S --root DIR`) and each child runs a
plan chosen by its index:

| child | plan | what it exercises |
|---|---|---|
| 0 | 200 files × 4 KB writes in one burst, then read back cached | small-file storm, bursts, cached reads |
| 1 | 4 files × 32 MB written with 1 MB blocks, `FILE_FLAG_NO_BUFFERING` on two | large sequential, disk vs file bytes |
| 2 | create 50 files, rename 25, delete 25, flush 10, enumerate the tree twice | info events, rename/delete flags, dir-enum |
| 3 | a `.claude\projects\<slug>\<uuid>.jsonl`-shaped tape under the root, appended 500 times in 2 KB lines | the tape-append session rule |
| 4 (optional, `--storm`) | 100 MB in 4 KB writes across 1,000 files, as fast as possible | the loss / decode-cost falsifier |

Every child prints a **manifest** as JSONL to a file (`--manifest FILE`): its pid and, per
operation, `{op, path, bytes, t_qpc_start, t_qpc_end, flags}`; the parent merges them. Paths are
under a temp root that the harness deletes afterwards. The plant is deterministic for a seed;
the byte totals per child are printed at the end as the expected values.

## `harness.cpp` → `everywho-harness.exe`

Runs elevated. Starts `everywho -j -w --interval 500 --out <file>`, waits for the first snapshot
(`session.rundown_done`), runs the plant, waits for the plant to finish plus two intervals, stops
everywho (Ctrl+Break to the child console, then `WM_CLOSE`-equivalent, then a timeout kill and
a stale-session check), and compares:

- per child pid: `file_write == manifest bytes` exactly; `file_read == cached + non-cached`
- per file: path present, `first_ms`/`last_ms` within the manifest's window, `deleted` and
  `renamed_to` as planned, `created_here` true for created files
- child 1: `disk_write >= non-buffered bytes`; `file_write - disk_write` is the cached share
- child 3: the process carries `agent = {claude-code, <slug>, <uuid>, rule: "tape"}`
- `session.lost == 0`; `decode_us_per_event <= 0.5` on the storm
- after stop: `everywho --where` reports no `everywho` session

Red prints the first mismatching file's event sequence from everywho's `--paths` and JSON so
the repro is one screen.

## `drive.cpp` → `everywho-drive.exe`

DPI-aware (`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`). Starts `everywho-gui.exe
--no-activate --ini <scratch>` with `EVERYWHO_LOG` set, runs the plant in the background, waits
for child 0's pid in the log, then posts real messages (`WM_LBUTTONDOWN`/`UP`, `WM_RBUTTONDOWN`,
`WM_KEYDOWN`, the `WM_APP+7/8/9` seams), captures with `PrintWindow(PW_RENDERFULLCONTENT)` after
`RedrawWindow(RDW_ALLCHILDREN)`, reads the log, and checks: the plant's process row exists with
its bytes; *only* narrows the table to it; Esc restores; *not* removes it; a cell pick lands as
a chip; pause holds the tally; freeze holds the table while the band moves. Exit non-zero on
any failure; PNGs in the scratch folder.

## Running

```
build.bat && build.bat tests
everywho --selftest                                   (any user; the ETW checks need elevation)
everywho-harness --plant everywho-plant.exe --storm   (elevated pwsh; exit 0 = green)
everywho-drive --gui everywho-gui.exe --plant everywho-plant.exe --out <scratch>
```
