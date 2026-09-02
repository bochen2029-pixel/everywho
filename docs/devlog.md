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
