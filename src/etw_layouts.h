// everywho · etw_layouts.h — the classic NT Kernel Logger events we decode: provider GUIDs,
// opcodes (MOF EventTypes), and field lists in MOF order. Offsets are computed at runtime from
// the field sizes and the event's pointer size (EVENT_HEADER_FLAG_64_BIT_HEADER), so one table
// serves both header widths. Stage 1 verifies every table against TDH on the running OS
// (`everywho --where --verify-layouts`, ETW.md §6) before a single number is trusted.
//
// Field orders follow the documented MOF classes (FileIo_*, DiskIo_*, Process_TypeGroup1,
// Thread_TypeGroup1). Where versions differ, the highest known layout <= the event's version is
// used; a payload shorter than its layout yields ABSENT trailing fields, never a read past
// UserDataLength.
#pragma once
#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX          // windows.h's min/max macros would clobber std::min/max in every header after this one
#endif
#include <windows.h>
#include <cstring>

namespace everywho::etw {

// ---- providers (MOF class GUIDs) ----
constexpr GUID kFileIoGuid  = { 0x90cbdc39, 0x4a3e, 0x11d1, { 0x84, 0xf4, 0x00, 0x00, 0xf8, 0x04, 0x64, 0xe3 } };
constexpr GUID kDiskIoGuid  = { 0x3d6fa8d4, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
constexpr GUID kProcessGuid = { 0x3d6fa8d0, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };
constexpr GUID kThreadGuid  = { 0x3d6fa8d1, 0xfe05, 0x11d0, { 0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c } };

// ---- opcodes (MOF EventType) ----
namespace fileio {
constexpr uint8_t Name = 0, FileCreate = 32, FileDelete = 35, FileRundown = 36;      // FileIo_Name
constexpr uint8_t Create = 64;                                                        // FileIo_Create
constexpr uint8_t Cleanup = 65, Close = 66, Flush = 73;                               // FileIo_SimpleOp
constexpr uint8_t Read = 67, Write = 68;                                              // FileIo_ReadWrite
constexpr uint8_t SetInfo = 69, Delete = 70, Rename = 71, QueryInfo = 74, FSControl = 75;   // FileIo_Info
constexpr uint8_t DirEnum = 72, DirNotify = 77;                                       // FileIo_DirEnum
constexpr uint8_t OpEnd = 76;                                                         // FileIo_OpEnd
}  // namespace fileio
namespace diskio {
constexpr uint8_t Read = 10, Write = 11;                        // DiskIo_TypeGroup1
constexpr uint8_t ReadInit = 12, WriteInit = 13, FlushInit = 15;   // DiskIo_TypeGroup2
constexpr uint8_t FlushBuffers = 14;                            // DiskIo_TypeGroup3
}  // namespace diskio
namespace process {
constexpr uint8_t Start = 1, End = 2, DCStart = 3, DCEnd = 4, Defunct = 39;   // Process_TypeGroup1
}
namespace thread {
constexpr uint8_t Start = 1, End = 2, DCStart = 3, DCEnd = 4;                 // Thread_TypeGroup1
}

// ---- FileIo_ReadWrite.IoFlags bits we interpret (from the MOF documentation) ----
constexpr uint32_t kIoPaging = 0x1;         // paging I/O (writeback, mapped files)
constexpr uint32_t kIoNonCached = 0x2;      // FILE_FLAG_NO_BUFFERING path
constexpr uint32_t kIoSynchronous = 0x4;

// ---- field types ----
enum class FieldType : uint8_t {
    U8, U16, U32, U64, I32,
    Ptr,        // pointer-sized: 4 or 8 by the event header
    WStr,       // inline NUL-terminated UTF-16
    AStr,       // inline NUL-terminated 8-bit
    Sid,        // TOKEN_USER-shaped: ptr, ptr, then a SID (8 + 4 * SubAuthorityCount); a first ptr of 0 means "no SID, only that ptr"
};

struct Field {
    const char* name;
    FieldType type;
};

struct Layout {
    const GUID* provider;
    uint8_t opcode_first, opcode_last;   // inclusive range sharing this class layout
    uint8_t version;                     // the MOF version this table describes
    const Field* fields;
    size_t nfields;
};

// ---- the tables (one per MOF class + version we support) ----
inline constexpr Field kFileIoName[] = { { "FileObject", FieldType::Ptr }, { "FileName", FieldType::WStr } };
inline constexpr Field kFileIoCreateV3[] = {
    { "IrpPtr", FieldType::Ptr }, { "FileObject", FieldType::Ptr }, { "TTID", FieldType::U32 }, { "CreateOptions", FieldType::U32 },
    { "FileAttributes", FieldType::U32 }, { "ShareAccess", FieldType::U32 }, { "OpenPath", FieldType::WStr },
};
inline constexpr Field kFileIoReadWriteV3[] = {
    { "Offset", FieldType::U64 }, { "IrpPtr", FieldType::Ptr }, { "FileObject", FieldType::Ptr }, { "FileKey", FieldType::Ptr },
    { "TTID", FieldType::U32 }, { "IoSize", FieldType::U32 }, { "IoFlags", FieldType::U32 },
};
inline constexpr Field kFileIoInfoV3[] = {
    { "IrpPtr", FieldType::Ptr }, { "FileObject", FieldType::Ptr }, { "FileKey", FieldType::Ptr }, { "ExtraInfo", FieldType::Ptr },
    { "TTID", FieldType::U32 }, { "InfoClass", FieldType::U32 },
};
inline constexpr Field kFileIoSimpleOpV3[] = {
    { "IrpPtr", FieldType::Ptr }, { "FileObject", FieldType::Ptr }, { "FileKey", FieldType::Ptr }, { "TTID", FieldType::U32 },
};
inline constexpr Field kFileIoDirEnumV3[] = {
    { "IrpPtr", FieldType::Ptr }, { "FileObject", FieldType::Ptr }, { "FileKey", FieldType::Ptr }, { "TTID", FieldType::U32 },
    { "Length", FieldType::U32 }, { "InfoClass", FieldType::U32 }, { "FileIndex", FieldType::U32 }, { "FileName", FieldType::WStr },
};
inline constexpr Field kFileIoOpEndV3[] = { { "IrpPtr", FieldType::Ptr }, { "ExtraInfo", FieldType::Ptr }, { "NtStatus", FieldType::U32 } };
inline constexpr Field kDiskIoGroup1V3[] = {
    { "DiskNumber", FieldType::U32 }, { "IrpFlags", FieldType::U32 }, { "TransferSize", FieldType::U32 }, { "Reserved", FieldType::U32 },
    { "ByteOffset", FieldType::U64 }, { "FileObject", FieldType::Ptr }, { "Irp", FieldType::Ptr }, { "HighResResponseTime", FieldType::U64 },
    { "IssuingThreadId", FieldType::U32 },
};
inline constexpr Field kDiskIoGroup2V3[] = { { "Irp", FieldType::Ptr }, { "IssuingThreadId", FieldType::U32 } };
inline constexpr Field kDiskIoGroup3V2[] = {
    { "DiskNumber", FieldType::U32 }, { "IrpFlags", FieldType::U32 }, { "HighResResponseTime", FieldType::U64 }, { "Irp", FieldType::Ptr },
};
inline constexpr Field kProcessV4[] = {
    { "UniqueProcessKey", FieldType::Ptr }, { "ProcessId", FieldType::U32 }, { "ParentId", FieldType::U32 }, { "SessionId", FieldType::U32 },
    { "ExitStatus", FieldType::I32 }, { "DirectoryTableBase", FieldType::Ptr }, { "Flags", FieldType::U32 }, { "UserSID", FieldType::Sid },
    { "ImageFileName", FieldType::AStr }, { "CommandLine", FieldType::WStr }, { "PackageFullName", FieldType::WStr }, { "ApplicationId", FieldType::WStr },
};
inline constexpr Field kProcessV3[] = {
    { "UniqueProcessKey", FieldType::Ptr }, { "ProcessId", FieldType::U32 }, { "ParentId", FieldType::U32 }, { "SessionId", FieldType::U32 },
    { "ExitStatus", FieldType::I32 }, { "DirectoryTableBase", FieldType::Ptr }, { "UserSID", FieldType::Sid },
    { "ImageFileName", FieldType::AStr }, { "CommandLine", FieldType::WStr },
};
inline constexpr Field kThreadV3[] = { { "ProcessId", FieldType::U32 }, { "TThreadId", FieldType::U32 } };   // the rest is skipped

inline constexpr Layout kLayouts[] = {
    { &kFileIoGuid, fileio::Name, fileio::Name, 2, kFileIoName, 2 },
    { &kFileIoGuid, fileio::FileCreate, fileio::FileRundown, 2, kFileIoName, 2 },
    { &kFileIoGuid, fileio::Create, fileio::Create, 2, kFileIoCreateV3, 7 },
    { &kFileIoGuid, fileio::Cleanup, fileio::Close, 2, kFileIoSimpleOpV3, 4 },
    { &kFileIoGuid, fileio::Flush, fileio::Flush, 2, kFileIoSimpleOpV3, 4 },
    { &kFileIoGuid, fileio::Read, fileio::Write, 2, kFileIoReadWriteV3, 7 },
    { &kFileIoGuid, fileio::SetInfo, fileio::Rename, 2, kFileIoInfoV3, 6 },
    { &kFileIoGuid, fileio::QueryInfo, fileio::FSControl, 2, kFileIoInfoV3, 6 },
    { &kFileIoGuid, fileio::DirEnum, fileio::DirEnum, 2, kFileIoDirEnumV3, 8 },
    { &kFileIoGuid, fileio::DirNotify, fileio::DirNotify, 2, kFileIoDirEnumV3, 8 },
    { &kFileIoGuid, fileio::OpEnd, fileio::OpEnd, 2, kFileIoOpEndV3, 3 },
    { &kDiskIoGuid, diskio::Read, diskio::Write, 3, kDiskIoGroup1V3, 9 },
    { &kDiskIoGuid, diskio::Read, diskio::Write, 2, kDiskIoGroup1V3, 8 },     // V2: no IssuingThreadId
    { &kDiskIoGuid, diskio::ReadInit, diskio::WriteInit, 3, kDiskIoGroup2V3, 2 },
    { &kDiskIoGuid, diskio::FlushInit, diskio::FlushInit, 3, kDiskIoGroup2V3, 2 },
    { &kDiskIoGuid, diskio::FlushBuffers, diskio::FlushBuffers, 2, kDiskIoGroup3V2, 4 },
    { &kProcessGuid, process::Start, process::DCEnd, 4, kProcessV4, 12 },
    { &kProcessGuid, process::Start, process::DCEnd, 3, kProcessV3, 9 },
    { &kProcessGuid, process::Defunct, process::Defunct, 4, kProcessV4, 12 },
    { &kThreadGuid, thread::Start, thread::DCEnd, 3, kThreadV3, 2 },
};
constexpr size_t kLayoutCount = sizeof(kLayouts) / sizeof(kLayouts[0]);

// The highest table with version <= the event's, for (provider, opcode); nullptr if none.
inline const Layout* find_layout(const GUID& provider, uint8_t opcode, uint8_t version) {
    const Layout* best = nullptr;
    for (size_t i = 0; i < kLayoutCount; ++i) {
        const Layout& l = kLayouts[i];
        if (!IsEqualGUID(*l.provider, provider) || opcode < l.opcode_first || opcode > l.opcode_last) continue;
        if (l.version > version) continue;
        if (!best || l.version > best->version) best = &l;
    }
    return best;
}

// A decoded view over one payload: field i → (offset, present). Built once per event; the hot
// path caches (layout, pointer size) → offsets for fixed-size prefixes and only walks strings.
struct Cursor {
    const uint8_t* data = nullptr;
    size_t len = 0;
    size_t off = 0;
    size_t ptr_size = 8;
    bool ok() const { return off <= len; }
    bool u32(uint32_t& v) { if (off + 4 > len) return false; memcpy(&v, data + off, 4); off += 4; return true; }
    bool u64(uint64_t& v) { if (off + 8 > len) return false; memcpy(&v, data + off, 8); off += 8; return true; }
    bool i32(int32_t& v) { if (off + 4 > len) return false; memcpy(&v, data + off, 4); off += 4; return true; }
    bool ptr(uint64_t& v) {
        if (off + ptr_size > len) return false;
        v = 0;
        memcpy(&v, data + off, ptr_size);
        off += ptr_size;
        return true;
    }
    // inline UTF-16 string: (pointer, length in chars) — bounded by the payload, terminator optional
    bool wstr(const wchar_t*& s, size_t& n) {
        if (off + 2 > len) return false;
        s = reinterpret_cast<const wchar_t*>(data + off);
        const size_t max = (len - off) / 2;
        n = 0;
        while (n < max && s[n]) ++n;
        off += (n + (n < max ? 1 : 0)) * 2;
        return true;
    }
    bool astr(const char*& s, size_t& n) {
        if (off >= len) return false;
        s = reinterpret_cast<const char*>(data + off);
        const size_t max = len - off;
        n = 0;
        while (n < max && s[n]) ++n;
        off += n + (n < max ? 1 : 0);
        return true;
    }
    // the TOKEN_USER-shaped SID blob (ETW.md §2); returns the SID pointer (may be null) — VERIFY WITH TDH
    bool sid(const uint8_t*& psid, size_t& n) {
        uint64_t first = 0;
        if (!ptr(first)) return false;
        psid = nullptr;
        n = 0;
        if (first == 0) return true;   // no SID present
        uint64_t second = 0;
        if (!ptr(second)) return false;
        if (off + 8 > len) return false;
        const uint8_t subs = data[off + 1];   // SID: Revision, SubAuthorityCount, IdentifierAuthority[6], SubAuthority[subs]
        n = 8 + 4 * (size_t)subs;
        if (off + n > len) return false;
        psid = data + off;
        off += n;
        return true;
    }
};

}  // namespace everywho::etw
