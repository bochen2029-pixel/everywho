// everywho · rates.h — interval accounting: ticks, moving windows, history rings for sparklines.
// Header-only, pure std. Rates are bytes per second over the *measured* interval, never the nominal.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace everywho {

// A fixed-capacity ring of samples (vramtop keeps 360 = 6 minutes at 1 s).
template <typename T, size_t N>
struct Ring {
    T v[N] = {};
    size_t head = 0, count = 0;
    void push(const T& x) {
        v[head] = x;
        head = (head + 1) % N;
        if (count < N) ++count;
    }
    // i = 0 is the oldest kept sample
    const T& at(size_t i) const { return v[(head + N - count + i) % N]; }
    size_t size() const { return count; }
};

struct RatePoint {
    float file_read_bps = 0, file_write_bps = 0, disk_read_bps = 0, disk_write_bps = 0;
    float busy_pct = 0, queue = 0;
};

// One volume's (or process's) history: per-tick points plus the running sums for moving windows.
struct History {
    Ring<RatePoint, 360> ticks;
    void push(const RatePoint& p) { ticks.push(p); }
    // mean over the last `seconds` ticks (1, 10, 60), by field
    RatePoint window(size_t seconds) const {
        RatePoint acc;
        const size_t n = (std::min)(seconds, ticks.size());   // parenthesised: immune to a stray min macro
        if (!n) return acc;
        for (size_t i = ticks.size() - n; i < ticks.size(); ++i) {
            const RatePoint& p = ticks.at(i);
            acc.file_read_bps += p.file_read_bps;
            acc.file_write_bps += p.file_write_bps;
            acc.disk_read_bps += p.disk_read_bps;
            acc.disk_write_bps += p.disk_write_bps;
            acc.busy_pct += p.busy_pct;
            acc.queue += p.queue;
        }
        const float k = 1.0f / (float)n;
        acc.file_read_bps *= k; acc.file_write_bps *= k; acc.disk_read_bps *= k; acc.disk_write_bps *= k;
        acc.busy_pct *= k; acc.queue *= k;
        return acc;
    }
};

// bytes over a measured interval → bytes per second
inline double rate(uint64_t bytes, double interval_ms) {
    return interval_ms > 0 ? (double)bytes * 1000.0 / interval_ms : 0.0;
}

// Interval bookkeeping for a cumulative counter (process I/O counters, disk performance):
// delta since the last tick, robust to counter resets (a smaller value = reset → delta 0).
struct Delta64 {
    uint64_t last = 0;
    bool primed = false;
    uint64_t step(uint64_t now) {
        const uint64_t d = (primed && now >= last) ? now - last : 0;
        last = now;
        primed = true;
        return d;
    }
};

// Response-time percentiles for DiskIo (HighResResponseTime): a small reservoir per interval.
struct Percentiles {
    std::vector<uint32_t> us;                 // microseconds, at most cap
    size_t cap = 4096, seen = 0;
    void push(uint32_t v) {
        ++seen;
        if (us.size() < cap) us.push_back(v);
        else us[(seen * 2654435761u) % cap] = v;   // reservoir-ish replacement, cheap
    }
    double p(double q) {                      // q in [0,1]
        if (us.empty()) return 0.0;
        std::vector<uint32_t> s = us;
        std::sort(s.begin(), s.end());
        const size_t i = (std::min)(s.size() - 1, (size_t)(q * (double)(s.size() - 1) + 0.5));
        return s[i] / 1000.0;                 // ms
    }
    void reset() { us.clear(); seen = 0; }
};

}  // namespace everywho
