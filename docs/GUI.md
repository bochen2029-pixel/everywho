# The window — everywho-gui.exe

facet's window with vramtop's band: a volume band on top, a facet rail on the left you click, a
live table on the right, chips that compile into the filter, a status line that always says the
tier and the loss.

```
┌ everywho — etw · C: 48 MB/s write ─────────────────────────────────────────────────────────┐
│ [filter box: name glob / dir / pid ……………………………………]   ● live · 3,412 ev/s · 0 lost   [P][F][T]│
│ chips:  [not C:\Users\user\AppData\Local\Google\Chrome\ ×] [only node.exe ×]                  │
│ C:  file r 12.1  w 48.3   disk r 0.4  w 41.0   q 1.2  busy 61%  ▂▃▅▇█▇▅▃▂▁▂▃▅▇█  (60 s)      │
│ D:  idle                                                                                      │
├──────────────────────┬────────────────────────────────────────────────────────────────────────┤
│ ▾ WHO            41MB│ Process        Path                                  Write   Read   Ops │
│  node.exe ×3    41MB │ node.exe       C:\Users\…\C--facet\6ebfe…jsonl        41 MB  0.3 MB 1.2k│
│   claude-code C--facet│ Everything.exe C:\Users\…\Everything\Everything.db     3.1 MB    0 B   88 │
│  Everything.exe 3.1MB│ chrome.exe     C:\Users\…\Default\Cache\f_00012a      1.2 MB    0 B   12 │
│  chrome.exe ×14 2.2MB│ …                                                                     │
│  System         1.9MB│                                                                        │
│ ▾ WHERE              │                                                                        │
│  C:\Users\user\.claude\projects\C--facet\  41MB                                               │
│  C:\Users\user\AppData\Local\             5.3MB                                               │
│ ▾ OPERATIONS         │                                                                        │
│  write   1,301       │                                                                        │
│  read    9,812       │                                                                        │
│ ▾ EXTENSIONS         │                                                                        │
│ ▾ VOLUMES            │                                                                        │
├──────────────────────┴────────────────────────────────────────────────────────────────────────┤
│ FILTER  name:node.exe  !path:"C:\…\Chrome\"   ·   Ctrl+L filter · Esc clear · P pause · F freeze · Ctrl+T pin │
└───────────────────────────────────────────────────────────────────────────────────────────────┘
```

## Parts

- **Filter box** (a subclassed EDIT, like facet's query box): free text that becomes filters —
  `node.exe` (name glob), `C:\dir` (a directory), `pid:1234`, `ops:write,create`, `vol:C`. The
  caret starts at the end; `WS_CLIPCHILDREN` on the parent so the box never blanks.
- **Chips bar**: every pick is a chip; `×` removes; Esc clears; right-click a `not …` chip to pin
  it as a standing exclude in `everywho.ini`. Chips wrap onto rows.
- **Volume band**: one row per volume with file and disk rates, queue, busy %, and a 60 s
  sparkline (write as columns, busy as the line — vramtop's convention).
- **Rail**: WHO (process rows with harness · project · session under agents), WHERE (directory
  tree, facet's rules: drive + folder entries, 5 % expansion, chain collapse), OPERATIONS,
  EXTENSIONS, VOLUMES. Left-click = only, right-click = not, headers fold. Share bars behind
  labels are by bytes in the current window.
- **Table**: the live top-N of (process, path) pairs by the current sort, or, in `g` mode, one
  row per file. Click a column header to sort; right-click a cell for the same per-column picks
  as facet (only/not this process, this folder level, this extension, this size class of I/O);
  double-click a path opens the containing folder; Ctrl+C copies the path; Ctrl+Shift+C copies
  the filter line.
- **Status**: the compiled filter, tier, events/s, lost events (amber), and the key hints.

## Behaviour

- Collection runs on the collector thread; the tick thread swaps snapshots at `--interval`
  (1 s); the UI paints the latest snapshot on `WM_APP_SNAP`, like vramtop. **Pause** stops
  the UI from taking new snapshots; **freeze** keeps the displayed snapshot but keeps the
  history rings growing so the sparkline continues.
- The window never blocks on the collector; a stuck `ProcessTrace` shows as a stale tally with
  its age.
- Elevation: when started unelevated the band shows counters only and the rail shows WHO only;
  a single button in the top bar ("see what: relaunch elevated") runs the UAC relaunch. Nothing
  else prompts.
- DPI: sized in physical pixels from `GetDpiForSystem`; minimum 720×460 logical; fonts scaled.
- Icon from the exe's resource (`--make-icon` exports it); `--shortcut` writes the Start Menu
  entry; `--ini` keeps a second profile.

## The test seam (mandatory, as in facet)

- `EVERYWHO_LOG=FILE`: the window appends one line per event: clicks with hit-test results,
  picks, filter submits, snapshots taken (with tier, events, lost), pause/freeze toggles.
- `WM_APP+7` (`wParam` = column, `lParam` = pick index) applies a cell pick to the selected row
  without the modal menu; `WM_APP+8` (`wParam` = rail row index, `lParam` = 0 only / 1 not)
  applies a rail pick; `WM_APP+9` toggles pause (0) / freeze (1).
- `--no-activate` opens the window without stealing focus (scripted runs); `--shot FILE.png`
  renders once after the first snapshot and exits.
- `tests/drive.cpp` (C++): starts `everywho-gui.exe` with a scratch `--ini`, runs a planted
  workload in a child process, posts the picks, captures with `PrintWindow`, reads the log, and
  checks the invariants (the planted process appears in WHO with its bytes; `only` narrows to
  it; `not` removes it; freeze keeps the numbers while the band moves).
