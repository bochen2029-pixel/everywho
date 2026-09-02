# ETW — the kernel stream everywho consumes

This is the collector's specification: how the session is created, which events are enabled,
how each is decoded, how names and processes are resolved, and what to verify on the first
build. Everything here is documented Windows behaviour (the classic *NT Kernel Logger* MOF
providers); the field orders are given so the decoder can be written offline, and **Stage 1
verifies every layout against TDH on the running OS** (`everywho --where --verify-layouts`)
before trusting it. Where memory of the exact layout could be off, the field order is what to
verify, not the concept.

## §1 · The session

- API: `StartTraceW` with an `EVENT_TRACE_PROPERTIES` block (allocate the struct plus room for
  the logger name and, for file mode, the file name; we use real-time mode only).
- **Mode**: `EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE`. System-logger mode
  (Windows 8+) lets a session with *our own name* (`everywho`) receive the kernel providers,
  instead of taking the singleton `NT Kernel Logger` name that WPR, xperf and other tools
  fight over. Up to eight system loggers may exist; this box already runs one
  (*Circular Kernel Context Logger*). If system-logger mode fails (older OS), fall back to the
  `KERNEL_LOGGER_NAME` session and say so in `--where`.
- **Enable flags** (`EnableFlags`): `EVENT_TRACE_FLAG_PROCESS | EVENT_TRACE_FLAG_THREAD |
  EVENT_TRACE_FLAG_FILE_IO_INIT | EVENT_TRACE_FLAG_FILE_IO | EVENT_TRACE_FLAG_DISK_FILE_IO |
  EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_DISK_IO_INIT`. `DISK_FILE_IO` is what produces the
  file-name rundown (`FileIo_Name`) for files already open at session start; `FILE_IO_INIT`
  produces the create/read/write/delete/rename events with their thread; `FILE_IO` produces the
  completions (`FileIo_OpEnd`) with status.
- **Clock**: `Wnode.ClientContext = 1` (QPC). Buffers: `BufferSize = 64` (KB), `MinimumBuffers =
  64`, `MaximumBuffers = 256`, `FlushTimer = 1`. Tune from `EventsLost`, which
  `ControlTraceW(…, EVENT_TRACE_CONTROL_QUERY)` reports every tick.
- **Privilege**: enable `SeSystemProfilePrivilege` in the process token with
  `AdjustTokenPrivileges` before `StartTrace`; if the privilege is absent (not admin, not in
  *Performance Log Users*), `StartTrace` returns `ERROR_ACCESS_DENIED` → the counters tier.
- **Stale sessions**: on start, `ControlTraceW(0, L"everywho", &props, EVENT_TRACE_CONTROL_STOP)`
  first; a crashed everywho must never leave a kernel logger running. On every exit path
  (normal, `SetConsoleCtrlHandler`, the window's `WM_DESTROY`, an unhandled-exception filter
  as a last resort) stop the session.
- **Consumer**: `EVENT_TRACE_LOGFILEW` with `LoggerName = L"everywho"`, `ProcessTraceMode =
  PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP`,
  an `EventRecordCallback`; `OpenTraceW` then `ProcessTraceW` on a dedicated thread (it blocks
  until the session stops). The callback runs on that thread: decode, fold, return. Never block.

## §2 · The events

Header first: `EVENT_RECORD.EventHeader` carries `ProviderId` (the MOF class GUID),
`EventDescriptor.Opcode` (the MOF *EventType*), `EventDescriptor.Version`, `ProcessId`,
`ThreadId` (valid for most kernel events; `0xFFFFFFFF` when not), `TimeStamp` (QPC), and
`Flags` with `EVENT_HEADER_FLAG_64_BIT_HEADER` → pointer size 8. `UserData`/`UserDataLength` is
the payload, laid out in MOF field order with natural sizes; `pointer` fields are 8 bytes on
this platform; strings are inline NUL-terminated (`string` = UTF-16, `ansi` = 8-bit).

### FileIo — provider GUID `{90cbdc39-4a3e-11d1-84f4-0000f80464e3}`

| event (opcode) | class · V | fields, in order | we use it for |
|---|---|---|---|
| Name 0 · FileCreate 32 · FileDelete 35 · FileRundown 36 | `FileIo_Name` V2 | `FileObject: ptr` (the *FileKey*), `FileName: string` | FileKey → name map; the rundown at start names already-open files |
| Create 64 | `FileIo_Create` V2/V3 | `IrpPtr: ptr`, `FileObject: ptr`, `TTID: uint32`, `CreateOptions: uint32`, `FileAttributes: uint32`, `ShareAccess: uint32`, `OpenPath: string` | FileObject → name; creation vs open (from `CreateOptions` disposition bits, verified at OpEnd) |
| Read 67 · Write 68 | `FileIo_ReadWrite` V2/V3 | `Offset: uint64`, `IrpPtr: ptr`, `FileObject: ptr`, `FileKey: ptr`, `TTID: uint32`, `IoSize: uint32`, `IoFlags: uint32` | the bytes; `IoFlags` tells paging / non-cached / synchronous |
| SetInfo 69 · Delete 70 · Rename 71 · QueryInfo 74 · FSControl 75 | `FileIo_Info` V2/V3 | `IrpPtr: ptr`, `FileObject: ptr`, `FileKey: ptr`, `ExtraInfo: ptr`, `TTID: uint32`, `InfoClass: uint32` | deletes and renames (the new name arrives as a later `FileIo_Name`/`Create` for the same key) |
| Cleanup 65 · Close 66 · Flush 73 | `FileIo_SimpleOp` V2/V3 | `IrpPtr: ptr`, `FileObject: ptr`, `FileKey: ptr`, `TTID: uint32` | drop the FileObject mapping on Close; flushes count as ops |
| DirEnum 72 · DirNotify 77 | `FileIo_DirEnum` V2/V3 | `IrpPtr: ptr`, `FileObject: ptr`, `FileKey: ptr`, `TTID: uint32`, `Length: uint32`, `InfoClass: uint32`, `FileIndex: uint32`, `FileName: string` | directory enumeration storms (a walker, an indexer) |
| OpEnd 76 | `FileIo_OpEnd` V2/V3 | `IrpPtr: ptr`, `ExtraInfo: ptr`, `NtStatus: uint32` | pair with the Irp: failed creates are not files; completion latency |

### DiskIo — provider GUID `{3d6fa8d4-fe05-11d0-9dda-00c04fd7ba7c}`

| event (opcode) | class · V | fields, in order | we use it for |
|---|---|---|---|
| Read 10 · Write 11 | `DiskIo_TypeGroup1` V2/V3 | `DiskNumber: uint32`, `IrpFlags: uint32`, `TransferSize: uint32`, `Reserved: uint32`, `ByteOffset: uint64`, `FileObject: ptr`, `Irp: ptr`, `HighResResponseTime: uint64`, `IssuingThreadId: uint32` (V3) | physical bytes per disk, response time, who issued it (tid → pid), which file (FileObject) |
| ReadInit 12 · WriteInit 13 · FlushInit 15 | `DiskIo_TypeGroup2` V2/V3 | `Irp: ptr`, `IssuingThreadId: uint32` (V3) | the initiating thread when TypeGroup1 completes in another context |
| FlushBuffers 14 | `DiskIo_TypeGroup3` | `DiskNumber`, `IrpFlags`, `HighResResponseTime`, `Irp` | flush storms (databases, fsync-happy tools) |

### Process — provider GUID `{3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c}`

| event (opcode) | class · V | fields, in order | notes |
|---|---|---|---|
| Start 1 · End 2 · DCStart 3 · DCEnd 4 · Defunct 39 | `Process_TypeGroup1` V3/V4 | `UniqueProcessKey: ptr`, `ProcessId: uint32`, `ParentId: uint32`, `SessionId: uint32`, `ExitStatus: int32`, `DirectoryTableBase: ptr`, `Flags: uint32` (V4), `UserSID: sid`, `ImageFileName: ansi`, `CommandLine: string`, `PackageFullName: string` (V4), `ApplicationId: string` (V4) | the `sid` object is stored as a `TOKEN_USER`-shaped blob: two pointer-sized values, then the SID (`8 + 4 × SubAuthorityCount` bytes); when the first pointer-sized value is 0 only that value is present. **Verify with TDH** — this is the one layout most often got wrong. |

`DCStart` events are the rundown (existing processes) emitted right after the session starts;
`Start` is a live creation. `ImageFileName` is the short name; the full path and the working
directory come from enrichment (`who.h`).

### Thread — provider GUID `{3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c}`

`Thread_TypeGroup1` V3 (Start 1 · End 2 · DCStart 3 · DCEnd 4): `ProcessId: uint32`,
`TThreadId: uint32`, then stack/TEB/affinity fields we skip. Only the first two matter: the
`tid → pid` table for events whose header pid is `-1` and for `IssuingThreadId`.

### Event-record identity used by the decoder

Match on `(ProviderId, Opcode)`; select the field list by `Version` (use the highest known
layout ≤ the event's version; if the payload is shorter than the layout, treat trailing fields
as absent rather than reading past `UserDataLength`). The decoder never trusts a string to be
terminated: it scans to the end of the payload.

## §3 · Names

- `OpenPath` and rundown names are **NT paths**. Build the device table once at start:
  for each drive letter `QueryDosDeviceW(L"C:")` → `\Device\HarddiskVolume3`; also map
  `\??\C:\` → `C:\`, `\SystemRoot\` → `%SystemRoot%\`, `\Device\Mup\server\share\` →
  `\\server\share\`, volume GUID paths through `GetVolumePathNamesForVolumeNameW`. Refresh
  the table on `WM_DEVICECHANGE` (window) or every 30 s (console).
- Non-file devices (`\Device\NamedPipe\…`, `\Device\Mailslot\…`, `\Device\Afd`, `\Device\Null`,
  `\Device\HarddiskVolumeShadowCopy…`) are classified into a *devices* bucket, counted,
  never inserted into the directory tree.
- Two maps, both bounded: `FileObject → (name, last_writer_pid, last_reader_pid, flags)`,
  dropped on `Close`; `FileKey → name` from the rundown and from every `Create` (the key is the
  file, the object is the handle). A `FileIo_ReadWrite` looks up `FileObject` first, then
  `FileKey`; a miss counts as *unnamed* bytes on the process, reported as such.
- Renames: the `Rename` info event marks the FileObject; the next `FileIo_Name` for its key
  carries the new name; keep the old name in `renamed_to` for the file table entry.
- Deletes: `FileDelete 35` (name class) and `Delete 70` (info class) both mark the file
  deleted; the path stays in the table for the window.

## §4 · Attribution

- **Header pid** is authoritative when not `-1`. Otherwise `tid → pid` from the thread table.
- **DiskIo** completes on arbitrary threads: use `IssuingThreadId` (V3+; on V2 pair the Irp with
  the `*Init` event's thread) → pid.
- **System (pid 4)**: `FileIo_ReadWrite` with paging I/O flags on a FileObject whose
  `last_writer_pid` is a user-mode process → `attributed_write` to that process, tagged
  `writeback`; raw stays on System. Memory-mapped writes surface only this way.
- **vmmem / vmmemWSL** → `Wsl`; distro unknown from user mode (reported as `WSL`).
- **Agent session by tape append** (`ARCHITECTURE.md` §3): a `FileWrite` whose name matches
  `<home>\.claude\projects\<slug>\<uuid>.jsonl` sets the writer's `agent = {claude-code, slug,
  uuid}`; `…\subagents\agent-<id>.jsonl` adds `subagent`.

## §5 · Rates, windows, loss

- The tick thread closes an interval every second: swaps the fold's live counters into the
  snapshot, computes bytes/s over the measured interval length (not the nominal 1000 ms), and
  pushes volume and top-process rings for sparklines (360 points, like vramtop).
- `EventsLost` from the session query is copied into every snapshot; a non-zero value flips
  the status line to amber and the JSON `session.lost` field. Buffer counts are raised
  automatically once (×2) on the first loss; the change is logged.
- Decode cost is measured (`session.decode_us_per_event`) by QPC around the callback, averaged
  per interval — the number that guards the performance falsifier.

## §6 · Verification on this OS (Stage 1, before trusting a single number)

`everywho --where --verify-layouts` opens the schema for every `(provider, opcode, version)` we
decode through `TdhGetEventInformation` on a live session's first occurrence of each, prints
the property names and offsets TDH reports next to `etw_layouts.h`'s expectation, and marks
any divergence. Divergence is a build-breaking finding: fix the table, re-run, then proceed.
TDH is used only here; the hot path decodes from the tables.

## §7 · Coexistence

- Process Monitor uses its own driver, not ETW: no conflict.
- WPR / xperf / PerfView use the *NT Kernel Logger* name: we do not, so they coexist; if the OS
  refuses a second system logger (limit 8), `--where` lists the ones present (as `logman query
  -ets` does) and everywho falls back to counters.
- Everything, Defender, the search indexer, backup jobs: they are *subjects* of everywho, and the
  clearest demo of it.

## §8 · Quirks to expect

- A file opened before the session started has no `Create`; its first read is nameless until
  the rundown names its `FileKey` (a few hundred ms after start) — the one-shot window begins
  *after* rundown completes.
- `IoSize` on a `FileWrite` is the requested size; the completion (`OpEnd`) may carry a shorter
  actual; count the request, note failures by status.
- `TTID` in the payload equals the header thread for FileIo events; keep the payload one when the
  header is `-1`.
- Very long paths (> 32 KB) exist in principle; the decoder bounds string reads at the payload.
- Volume letters change (USB, mounts): the device table refresh handles it; names decoded
  before a refresh keep the letter they had, which is the truthful history.

## §9 · The manifest alternative (ADR-013) — verified on the reference box, 2026-09-02

Windows 8+ also exposes the kernel's file, disk and process activity through **manifest-based
providers** that a *private* session enables with `EnableTraceEx2` (no system-logger slot, no
`NT Kernel Logger` name, still `SeSystemProfilePrivilege`). `logman query providers` on this box:

| provider | GUID | what it replaces |
|---|---|---|
| `Microsoft-Windows-Kernel-File` | `{EDD08927-9CC4-4E65-B970-C2560FB5C289}` | FileIo_* |
| `Microsoft-Windows-Kernel-Disk` | `{C7BDE69A-E1E0-4177-B6EF-283AD1525271}` | DiskIo_* |
| `Microsoft-Windows-Kernel-Process` | `{22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}` | Process_/Thread_TypeGroup1 |
| `Microsoft-Windows-Kernel-Network` | `{7DD42A49-5329-4832-8DFD-43D979153A88}` | (the network band, later) |
| `Microsoft-Windows-Kernel-Registry` | `{70EB4F03-C1DE-4F73-A051-33D13D5413BD}` | (the registry facet, later) |

Kernel-File keywords (the `MatchAnyKeyword` mask): `FILENAME 0x10` · `FILEIO 0x20` ·
`OP_END 0x40` · `CREATE 0x80` · `READ 0x100` · `WRITE 0x200` · `DELETE_PATH 0x400` ·
`RENAME_SETLINK_PATH 0x800` · `CREATE_NEW_FILE 0x1000`. Level `Informational (4)`.

What changes against the classic backend:

- **Decoding**: real TDH schemas per event id and version; the decoder can still cache offsets
  per `(id, version)` and read fixed prefixes directly, so the hot path stays table-driven.
- **Paths on delete and rename**: `DeletePath` / `RenamePath` / `SetLinkPath` events carry the
  file path itself, so deletes and renames no longer depend on the FileObject map. `Create`
  carries the name as before; `Read` / `Write` still carry only FileObject / FileKey.
- **No open-file rundown**: nothing names files already open at session start. `handles.h`
  fills that gap in both backends (one scan of the handle table after start, ADR-014), and with
  the manifest backend it is the *only* source of those names — which is why Stage 1b measures
  the nameless share on the oracle before deciding.
- **Process events** come with image name, command line and the creating process; threads via
  the same provider. Rundown of existing processes: `EnableTraceEx2` with
  `EVENT_CONTROL_CODE_CAPTURE_STATE` asks the provider to emit its current state.
- **Lifecycle**: a private session name, `ControlTrace(STOP)` on exit, no coexistence concerns
  beyond the ordinary session limit (64).

The `RawEvent` shape is unchanged; a second decoder feeds the same fold. `--where` reports which
backend is active and why.

## §3 addendum · naming files that were already open

Whichever backend runs, one pass of `scan_open_files` (`handles.h`) immediately after the
session starts feeds `FileObject → (pid, name)` into the collector's map: the kernel object
address in the handle table is the same value the FileIo events carry. Long-held files
(Everything's database, browser caches, log files, a session tape kept open by a harness) are
then named from their first read or write instead of after their next open. The scan runs on a
worker thread with a per-handle timeout and skips known-hanging handle classes; the collector
never waits for it — events that arrive before the scan finishes are named retroactively at
the next tick from the map.
