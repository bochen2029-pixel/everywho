# everywho — development log

Running notes, newest at the bottom. Decisions, measurements, test results, traps.

## 2026-09-02 · the blueprint

- Why this tool, in one line: Windows accounts for every I/O in the kernel and offers no live
  per-tenant pivot of it; a desktop running an agent swarm needs one. facet answers *where
  things landed*; everywho answers *who is landing them now*.
- Reference box facts that shaped the design: Windows 11 24H2 (26100) x64; `sudo` disabled
  (elevation = UAC relaunch or an elevated pwsh); 27 ETW sessions already running including
  the *Circular Kernel Context Logger*, the *NT Kernel Logger* name free — hence a
  system-logger-mode session named `everywho` (ADR-002); facet at 0.4 with `--grep`,
  everywhen with `--paths` / `locate`, everywhere with `--files-from` — the tape contract is
  in place on all three, so everywho speaks it from day one (ADR-010).
- The two-tier model (ADR-003) exists because the *what* is privileged and the tool must still
  be useful to an unelevated agent: who + how much always, what when allowed.
- Decode-from-tables with TDH verification (ADR-004) is the decision most likely to need a
  correction on first contact with the running OS: the Process event's SID blob and the V2/V3
  field differences are called out in ETW.md as *verify first*.
- Everything in `src/` is a contract, not an implementation. `app_util.h` and `tape.h` are
  lifted from facet and are real; the rest declares what Stage 0–2 must define.
- The contract headers were compiled alone under the project flags (/std:c++20 /W4 /WX) before
  the first commit, with a smoke of the header-only parts (layout lookup + cursor bounds, tape
  parsing, rate windows). It caught one real trap immediately: `etw_layouts.h` pulls in
  `windows.h`, whose `min`/`max` macros clobbered `std::min` in `rates.h`. Fixed twice over —
  `NOMINMAX` before the include, and `(std::min)(…)` parenthesised so any include order works.
  Rule for the implementation: every new header goes through the same lone-compile check.
- Next: Stage 0 per ROADMAP.md. Build the oracle before the engine in Stage 1.

## 2026-09-02 · the fork review, folded in before a line of code

- A forked session brainstormed the same tool as `iowho` (C:\facet\docs\next.md): shorter, but
  it named the manifest providers, a treemap, files-per-minute, `--paths PID|NAME`, a handle
  finder (`handlewho`), a port finder, and a thin fleet board. Taken into this blueprint: the
  manifest backend as a measured alternative (ADR-013, ETW.md §9, `EtwConfig::backend`), the
  handle scan as the open-file rundown and `--open` (ADR-014, `src/handles.h`), files/min as a
  first-class rate, the treemap toggle in the window, and the inherit rule for process trees
  (ADR-015). Left where they are: portwho (a network band later, ROADMAP "After") and the fleet
  board (that is `muster`, the tool after this one). The effort estimate is now written down.
- `next.md` in facet now points here as the canonical design; everywho is the name.

## 2026-09-02 · Stage 0 built — 0.1.0, the counters tier

- Sources: `sys.h` (the Windows layer), `who.cpp` (identity), `counters.cpp` (the process list +
  PDH), `where.cpp` (the fold, per process and per volume for now), `shell.cpp` (icon,
  shortcut), `everywho.cpp` (every mode). One build, zero warnings under /W4 /WX.
- The counters tier turned out simpler than the blueprint assumed: `SystemProcessInformation`
  carries every process's cumulative read / write / other transfer and operation counts in the
  same record as its identity — one syscall for the whole box, no handles, every user's
  processes, unelevated. Per-process handles are opened once per new pid, for enrichment only
  (image path; command line via ProcessCommandLineInformation, which needs no VM_READ; the
  working directory from the PEB, which does; the user from the token).
- PDH `\PhysicalDisk(*)` with English counter names gives media-level rates per disk; the
  instance names ("1 C:", "0 D:", "2 E:") carry the letters. `IOCTL_STORAGE_GET_DEVICE_NUMBER`
  on `\.\C:` confirms the letter → disk map without elevation.
- Measured on the reference box: 268 processes listed; the Stage 0 falsifier held at 100 %
  (working directory readable for 20 of the top 20 own-user I/O processes; 148 of 148 own-user
  processes overall); an 8 MB planted write shows as 8.0 MB in the next tick; 3 PDH instances
  with letters; the report, JSON, NDJSON, stamp, spool and MCP all answer.
- The identity finding that reshaped a rule: on this box `claude.exe` is two things. Five are
  the Claude desktop app's Electron processes (WindowsApps package, `--type=renderer|gpu-process
  |utility`, cwd `system32`); two are the Claude Code CLI under `AppData\Roaming\Claude\claude-code
  \<ver>\`, driven with `--output-format stream-json … --resume=<uuid>`, cwd = the project. The
  rule now tells them apart by image path and arguments, reads the session id from `--resume=`
  / `--session-id` (rule `cmdline`), and the report groups by name *and* harness so the app and
  the CLI never share a row. The agent shell running this session is itself elevated, so the
  ETW tier can be tested from here in Stage 1.
- Header changes against the blueprint, recorded here as the CLAUDE.md asks: `classify_image`
  gained the image path; `enumerate_now` returns bool; `ProcSample` moved into `who.h`;
  `VolumeStat` gained media rates and the PDH instance; `IoCounters` gained `other_bytes`,
  `ops_other`; `Fold::snapshot(sort, cumulative, window_ms)`; `DevicePaths::set_table` for tests.
- Icon bootstrapped (`--make-icon`, then the .rc), `--shortcut` writes the Start Menu entry to
  `everywho.exe -w` until the window exists.

## 2026-09-02 · `--about`: the family's self-description contract (for peek)

- `everywho --about` prints the organ's card as one JSON object (organ, version, path, purpose,
  stage, verbs with examples, mcp{command,args,tools,register}, health{ok,tier,elevated,
  profile_privilege,processes,disks,detail}, docs, tape). peek 0.3 reads it for `peek env`,
  spawns the `mcp` command behind `peek --mcp`, and builds `peek fleet` from `-j --agents`.
