// everywho · handles.h — open file objects by process, from user mode: the rundown the ETW
// stream cannot give for files opened before the session started, and the answer to
// "who has this open?" (--open PATH). Elevated: other users' handles need PROCESS_DUP_HANDLE.
//
// Mechanism: NtQuerySystemInformation(SystemExtendedHandleInformation) lists every handle with
// its owner pid, the kernel Object address and the type index; for File-type objects the Object
// address IS the FILE_OBJECT pointer the kernel's FileIo events carry as FileObject, so the two
// worlds join on that number without a name lookup. Names come from DuplicateHandle into this
// process + GetFinalPathNameByHandle (or NtQueryObject ObjectNameInformation) on a worker
// thread with a timeout, because querying a synchronous pipe or a stuck network handle can hang
// the caller (Process Explorer's known trap; skip GrantedAccess 0x0012019f and pipe types).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace everywho {

struct OpenFile {
    uint32_t pid = 0;
    uint64_t object = 0;          // kernel FILE_OBJECT address (joins with RawEvent::file_object)
    uint32_t handle = 0;          // in the owner process
    uint32_t access = 0;          // GrantedAccess
    std::wstring path;            // DOS path when resolvable; empty when the name query was skipped or timed out
    bool directory = false;
};

struct HandleScanStats {
    uint32_t handles_total = 0;   // every handle in the system
    uint32_t file_objects = 0;    // File-type handles seen
    uint32_t named = 0;           // names resolved
    uint32_t skipped_unsafe = 0;  // pipes / known-hanging access masks, not queried
    uint32_t timed_out = 0;       // name queries abandoned after the timeout
    uint32_t denied = 0;          // processes we could not open
    double ms = 0.0;
};

// One pass over the handle table. cb receives every File-type object (named or not).
// name_timeout_ms bounds each name query; 200 ms is generous on NTFS, hostile to a hung pipe.
bool scan_open_files(const std::function<void(const OpenFile&)>& cb, HandleScanStats& stats, uint32_t name_timeout_ms = 200,
                     std::string* err = nullptr);

// --open PATH|DIR: every process holding a handle to the path or anything below it (case-folded prefix match).
std::vector<OpenFile> who_has_open(std::wstring_view path_or_dir, HandleScanStats& stats, std::string* err = nullptr);

// The rundown complement for the ETW tier: called once after the session starts, feeds
// FileObject -> name into the collector's map so reads and writes on long-held handles
// (databases, caches, log files) are named from the first event. Returns the count fed.
size_t prime_file_object_names(const std::function<void(uint64_t object, uint32_t pid, std::wstring_view path)>& feed,
                               HandleScanStats& stats);

}  // namespace everywho
