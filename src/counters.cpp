// everywho · counters.cpp — the counters tier. One NtQuerySystemInformation call lists every
// process with its cumulative read/write/other transfer and operation counts (no handles, any
// user); PDH's PhysicalDisk counters give per-disk rates, queue depth and busy time.
#include "counters.h"

#include "sys.h"

#include <winternl.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cstddef>
#include <unordered_map>

namespace everywho {

namespace {

// SYSTEM_PROCESS_INFORMATION, the whole record (winternl.h reserves most of it), x64 layout.
struct SPI_FULL {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};
static_assert(offsetof(SPI_FULL, UniqueProcessId) == 80, "SYSTEM_PROCESS_INFORMATION x64 layout");
static_assert(offsetof(SPI_FULL, ReadOperationCount) == 208, "SYSTEM_PROCESS_INFORMATION x64 layout");
static_assert(sizeof(SPI_FULL) == 256, "SYSTEM_PROCESS_INFORMATION x64 layout");

typedef NTSTATUS(NTAPI* PFN_NtQuerySystemInformation)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

constexpr NTSTATUS kInfoLengthMismatch = (NTSTATUS)0xC0000004L;

}  // namespace

bool sample_processes(std::vector<ProcSample>& out, std::string* err) {
    static PFN_NtQuerySystemInformation f =
        reinterpret_cast<PFN_NtQuerySystemInformation>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (!f) {
        if (err) *err = "NtQuerySystemInformation not found";
        return false;
    }
    thread_local std::vector<uint8_t> buf(1u << 20);   // reused across ticks; grows to the box's size once
    ULONG need = 0;
    NTSTATUS st = 0;
    for (int tries = 0; tries < 8; ++tries) {
        st = f(SystemProcessInformation, buf.data(), (ULONG)buf.size(), &need);
        if (st == kInfoLengthMismatch) {
            buf.resize((size_t)need + 65536);
            continue;
        }
        break;
    }
    if (st != 0) {
        if (err) *err = ssprintf("NtQuerySystemInformation failed (0x%08lx)", (unsigned long)st);
        return false;
    }
    out.clear();
    size_t off = 0;
    for (;;) {
        if (off + sizeof(SPI_FULL) > buf.size()) break;
        const auto* p = reinterpret_cast<const SPI_FULL*>(buf.data() + off);
        ProcSample s;
        s.pid = (uint32_t)(uintptr_t)p->UniqueProcessId;
        s.ppid = (uint32_t)(uintptr_t)p->InheritedFromUniqueProcessId;
        s.session_id = p->SessionId;
        s.threads = p->NumberOfThreads;
        s.create_ft = (uint64_t)p->CreateTime.QuadPart;
        s.read_bytes = (uint64_t)p->ReadTransferCount.QuadPart;
        s.write_bytes = (uint64_t)p->WriteTransferCount.QuadPart;
        s.other_bytes = (uint64_t)p->OtherTransferCount.QuadPart;
        s.read_ops = (uint64_t)p->ReadOperationCount.QuadPart;
        s.write_ops = (uint64_t)p->WriteOperationCount.QuadPart;
        s.other_ops = (uint64_t)p->OtherOperationCount.QuadPart;
        s.working_set = (uint64_t)p->WorkingSetSize;
        if (p->ImageName.Buffer && p->ImageName.Length) s.name.assign(p->ImageName.Buffer, p->ImageName.Length / sizeof(wchar_t));
        out.push_back(std::move(s));
        if (!p->NextEntryOffset) break;
        off += p->NextEntryOffset;
    }
    return true;
}

// ---- PDH: PhysicalDisk(*) ----

struct DiskCounters::Impl {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c[6] = {};
    bool open_ok = false;
};

namespace {
const wchar_t* kDiskPaths[6] = {
    L"\\PhysicalDisk(*)\\Disk Read Bytes/sec",
    L"\\PhysicalDisk(*)\\Disk Write Bytes/sec",
    L"\\PhysicalDisk(*)\\Disk Reads/sec",
    L"\\PhysicalDisk(*)\\Disk Writes/sec",
    L"\\PhysicalDisk(*)\\Current Disk Queue Length",
    L"\\PhysicalDisk(*)\\% Idle Time",
};

// "1 C:" → disk 1, letters "C"; "0 D: E:" → disk 0, letters "DE"
void parse_instance(const std::wstring& inst, VolumeStat& v) {
    v.instance = inst;
    size_t i = 0;
    uint32_t disk = 0;
    bool have_disk = false;
    while (i < inst.size() && inst[i] >= L'0' && inst[i] <= L'9') {
        disk = disk * 10 + (uint32_t)(inst[i] - L'0');
        have_disk = true;
        ++i;
    }
    if (have_disk) v.disk = disk;
    v.letters.clear();
    for (; i < inst.size(); ++i)
        if (iswalpha(inst[i]) && i + 1 < inst.size() && inst[i + 1] == L':') v.letters += (wchar_t)towupper(inst[i]);
    v.letter = v.letters.empty() ? 0 : v.letters[0];
}
}  // namespace

DiskCounters::DiskCounters() : p_(new Impl) {}
DiskCounters::~DiskCounters() {
    if (p_->q) PdhCloseQuery(p_->q);
    delete p_;
}

bool DiskCounters::open(std::string* err) {
    if (PdhOpenQueryW(nullptr, 0, &p_->q) != ERROR_SUCCESS) {
        if (err) *err = "PdhOpenQuery failed";
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        const PDH_STATUS st = PdhAddEnglishCounterW(p_->q, kDiskPaths[i], 0, &p_->c[i]);
        if (st != ERROR_SUCCESS) {
            if (err) *err = ssprintf("PdhAddEnglishCounter(%s) failed (0x%08lx)", narrow(kDiskPaths[i]).c_str(), (unsigned long)st);
            return false;
        }
    }
    PdhCollectQueryData(p_->q);   // prime: rate counters need two samples
    p_->open_ok = true;
    return true;
}

bool DiskCounters::ok() const { return p_->open_ok; }

bool DiskCounters::collect(std::vector<VolumeStat>& out, std::string* err) {
    out.clear();
    if (!p_->open_ok) return false;
    const PDH_STATUS st = PdhCollectQueryData(p_->q);
    if (st != ERROR_SUCCESS) {
        if (err) *err = ssprintf("PdhCollectQueryData failed (0x%08lx)", (unsigned long)st);
        return false;
    }
    std::unordered_map<std::wstring, size_t> index;
    std::vector<uint8_t> buf;
    for (int i = 0; i < 6; ++i) {
        DWORD bytes = 0, count = 0;
        PDH_STATUS s = PdhGetFormattedCounterArrayW(p_->c[i], PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
        if (s != PDH_MORE_DATA && s != ERROR_SUCCESS) continue;   // a counter with no data yet (first interval) is skipped, not fatal
        buf.resize(bytes + 64);
        s = PdhGetFormattedCounterArrayW(p_->c[i], PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data()));
        if (s != ERROR_SUCCESS) continue;
        const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        for (DWORD k = 0; k < count; ++k) {
            if (!items[k].szName) continue;
            const std::wstring name(items[k].szName);
            if (name == L"_Total") continue;
            if (items[k].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[k].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            auto it = index.find(name);
            if (it == index.end()) {
                VolumeStat v;
                parse_instance(name, v);
                out.push_back(std::move(v));
                it = index.emplace(name, out.size() - 1).first;
            }
            VolumeStat& v = out[it->second];
            const double val = items[k].FmtValue.doubleValue;
            switch (i) {
                case 0: v.read_bps = val; break;
                case 1: v.write_bps = val; break;
                case 2: v.read_iops = val; break;
                case 3: v.write_iops = val; break;
                case 4: v.queue = val; break;
                case 5: v.busy_pct = val > 100.0 ? 0.0 : 100.0 - val; break;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const VolumeStat& a, const VolumeStat& b) { return a.disk < b.disk; });
    return true;
}

std::vector<std::pair<wchar_t, uint32_t>> volume_disks() {
    std::vector<std::pair<wchar_t, uint32_t>> out;
    for (wchar_t L = L'A'; L <= L'Z'; ++L) {
        const wchar_t root[] = { L, L':', L'\\', 0 };
        const UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        const wchar_t dev[] = { L'\\', L'\\', L'.', L'\\', L, L':', 0 };
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER sdn{};
        DWORD n = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof sdn, &n, nullptr)) out.emplace_back(L, (uint32_t)sdn.DeviceNumber);
        CloseHandle(h);
    }
    return out;
}

}  // namespace everywho
