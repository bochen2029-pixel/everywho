// everywho · counters.h — the counters tier: every process's cumulative I/O from one
// SystemProcessInformation call (no handles, every user's processes, unelevated), and
// per-physical-disk rates from PDH's PhysicalDisk counters (English names, locale-proof).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "who.h"
#include "where.h"

namespace everywho {

// One pass over the kernel's process list. False with err when the query fails.
bool sample_processes(std::vector<ProcSample>& out, std::string* err);

// Per-disk rates through PDH. open() primes; each collect() returns the rates since the
// previous collect (the first collect after open returns nothing: rate counters need two samples).
class DiskCounters {
public:
    DiskCounters();
    ~DiskCounters();
    DiskCounters(const DiskCounters&) = delete;
    DiskCounters& operator=(const DiskCounters&) = delete;
    bool open(std::string* err);
    bool collect(std::vector<VolumeStat>& out, std::string* err);
    bool ok() const;
    struct Impl;
private:
    Impl* p_;
};

// Drive letter → physical disk number, from the volume's device number (unelevated).
std::vector<std::pair<wchar_t, uint32_t>> volume_disks();

}  // namespace everywho
