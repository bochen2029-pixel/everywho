# CLAUDE.md — session rules for everywho

You are implementing **everywho**, the fifth organ of Bo Chen's tool family (everything/facet ·
everywhere · everywhen · vramtop · everywho). This file is the contract for any session that
touches this folder. The machine-wide rules in `C:\Users\user\.claude\CLAUDE.md` apply on top.

## 1. What this is

everywho answers *who is touching what, right now*: per-process, per-directory file and disk I/O
from the Windows kernel's ETW events, folded into facet's rail, exposed headless (JSON, NDJSON,
MCP, stamp, spool, tapes) and in a window. Read, in this order, before writing code:
`README.md` → `docs/ARCHITECTURE.md` → `docs/ETW.md` → `docs/CLI.md` → `docs/TESTING.md` →
`docs/DECISIONS.md` → `docs/ROADMAP.md` → `docs/devlog.md`. The headers in `src/` are the
contracts; implement to them, and change them only with a decision record.

## 2. Hard rules (never negotiable)

1. **C or C++ only. No other language in the product.** No Python, no PowerShell, no scripts
   shipped. The test driver that clicks the window is C++ too (`tests/drive.cpp`). Scratch
   scripts outside the repo for a one-off measurement are tolerable; nothing of them enters
   the tree.
2. **OS APIs only, single static exe.** `advapi32` (ETW control), `tdh` (schema verification
   only), `ntdll` (runtime-resolved, for process identity), `user32`/`gdi32`/`gdiplus`
   (the window), `psapi`/`kernel32`. No third-party code, no vendored libraries.
3. **No driver, no service, no injection, no hooks.** ETW and public counters are the only
   sources. If a question cannot be answered from them, the answer is "not from user mode".
4. **Never read a file's contents.** Paths, sizes, offsets, operation kinds, statuses. That is
   the whole surface.
5. **A trace session never outlives the process.** Stop it on every exit path (normal, Ctrl+C,
   console close, crash handler where possible); on start, stop a stale session with our name
   before creating a new one. Leaking a kernel logger is the one bug that hurts the operator
   after we are gone.
6. **Never elevate silently.** `--elevate` (or the window's explicit prompt) relaunches through
   UAC; every other path runs in the tier it has and says which.
7. **Numbers are the kernel's or they are labelled derived.** Attribution heuristics (System
   writeback → last writer, tape append → session) are marked `attributed`, and the raw
   figure stays visible.
8. **Mirror the family, do not reinvent it.** Options, formatting, JSON style, MCP framing,
   spool lines, the rail, chips, `--shot`, `--shortcut`, `--make-icon`, `--where`,
   `--selftest`: copy the shape from `C:\facet` and `C:\GPUz`, then adapt. When in doubt open
   `C:\facet\facet.cpp` (console modes, MCP, selftest), `C:\facet\facets.cpp` (the trie and
   the fold), `C:\facet\facet_gui.cpp` (rail, chips, table, driver seam), `C:\GPUz\vramtop.cpp`
   (watch loop, stamp, spool, NDJSON, ANSI rendering).

## 3. Stage gates

Work proceeds in the stages of `docs/ROADMAP.md`, each with a **falsifier**: a measurement that,
if it fails, means the design is wrong and the stage stops rather than being patched around.
A stage is done when `--selftest` is green, the oracle (`tests/`) is green for that stage, the
devlog has the numbers, and there is one commit named `everywho 0.X.0 — <what landed>` ending
with the attribution trailer the harness requires.

- **Stage 0** counters tier, identity, report, JSON, selftest. *Falsifier:* the identity layer
  cannot name the working directory of 95 % of the top-20 I/O processes on the reference box.
- **Stage 1** the ETW session and decoder, the oracle. *Falsifier:* planted writes are not
  attributed byte-exact, or events are lost below 100 MB/s of 4 KB writes with 4 MB of buffers.
- **Stage 2** the fold, rates, watch TUI, stamp, spool, tapes, MCP.
- **Stage 3** the window.
- **Stage 4** the seams: fusor lane, facet/everywhen handoffs, the `muster` columns.

## 4. Build and test discipline

- `build.bat` only. `/W4` and **zero warnings**; a warning is a bug until proven cosmetic.
- `everywho --selftest` before every commit. It must pass unelevated (offline checks + the
  counters tier) and, when run elevated, exercise a real session on a planted workload.
- The GUI is verified by `tests/drive.exe`, which posts real window messages and reads the
  window's event log (`EVERYWHO_LOG=FILE`, the `WM_APP+7` pick seam, exactly as in facet),
  never by eyeballing a screenshot alone. Screenshots come from `--shot`.
- Measure before claiming: event rates, lost events, decode cost per event, memory at 1 M
  distinct paths, the fold's cost per event. Put the numbers in the devlog.
- The oracle is ripgrep's role in everywhere: `tests/plant` writes known bytes from known
  processes to known paths at known times; everywho must report them exactly. Build the
  oracle before the engine (Stage 1 begins with it).

## 5. This machine

- Windows 11 24H2 (26100), x64, 225 % DPI (size windows in physical pixels scaled by the
  system DPI — see facet's devlog for the postage-stamp trap), i9-9900K, one NVMe system disk
  plus D:, an RTX 4070 Ti SUPER, Everything 1.4.1 running, WSL Ubuntu with Docker inside.
- `sudo` for Windows is **disabled** here. To test the ETW tier from an agent shell, start an
  elevated pwsh (`Start-Process pwsh -Verb RunAs -ArgumentList '-NoExit'`) and drive it, or use
  `everywho --elevate --out FILE` which relaunches and writes its report to FILE.
- Kernel loggers already running include the *Circular Kernel Context Logger*; the
  *NT Kernel Logger* name is free but we do not take it (see `docs/ETW.md` §1): everywho uses
  a system-logger-mode session named `everywho`.
- Shell paths use forward slashes in every command (`C:/Intellect_AI_tools/everywho/...`);
  the harness blocks PowerShell `Remove-Item` under tool folders — delete with Git Bash `rm`.
- A running exe cannot be overwritten by the linker; a running one can be **renamed** aside.
- Never ask the operator what an earlier session decided or measured: `C:\everywhen\everywhen.exe
  search --hours N --query Q` finds it in the transcripts.

## 6. Writing rules for this repo

- Comments explain *why* and name the trap they avoid; the code says what.
- Every mode prints for agents first: JSON is a single line with stable field names, errors
  are objects with an `error` string, exit codes mean something (0 ok · 1 args · 2 collector
  error, JSON still emitted · 3 selftest failed · 4 needs elevation for what was asked).
- The devlog is a lab notebook: date, what was tried, what was measured, what was decided.
  Traps go there the day they bite.
- Docs describe what exists; planned things are marked planned. The README's "Status" line
  changes with the first working build.

## 7. Refusals (say no, and say why)

- A minifilter, a kernel driver, or DLL injection to see more: no — ETW is the boundary.
- Reading or hashing file contents "to identify" them: no — names only.
- A resident daemon that keeps a session open "for later": no by default; `--spool` runs for
  as long as it is asked and stops on exit.
- Alerting, blocking, or "security" verdicts: no — everywho measures; judgement is the
  operator's.
- Downscaling the README screenshot with bicubic to save bytes: no — it doubled the file
  (facet devlog, 2026-09-01).
