#pragma once

// Offline targets y(h) = m_{t+h} - m_t in ticks, kept apart from the feature code: it reads
// the whole mid series of a day, which features must never see. Event horizons count book
// events; clock horizons use the mid prevailing at t + h. NaN when the horizon passes the
// end of the series or a mid is undefined.

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace hft::strategy {

struct Horizons {
    std::vector<std::uint32_t> events;    // e.g. 10, 100, 1000
    std::vector<std::uint64_t> clock_ns;  // e.g. 1e7, 1e8, 1e9, 1e10
    std::size_t count() const { return events.size() + clock_ns.size(); }
};

// out: n rows of h.count() targets, row-major.
inline void label(const std::vector<std::uint64_t>& ts, const std::vector<double>& mid,
                  const Horizons& h, std::vector<double>& out) {
    const std::size_t n = ts.size(), w = h.count();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    out.assign(n * w, nan);
    for (std::size_t c = 0; c < h.events.size(); ++c)
        for (std::size_t i = 0; i + h.events[c] < n; ++i)
            out[i * w + c] = mid[i + h.events[c]] - mid[i];
    for (std::size_t c = 0; c < h.clock_ns.size(); ++c) {
        // j is the last event at or before ts[i] + h; it only moves forward.
        std::size_t j = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t until = ts[i] + h.clock_ns[c];
            if (until > ts[n - 1]) break;
            if (j < i) j = i;
            while (j + 1 < n && ts[j + 1] <= until) ++j;
            out[i * w + h.events.size() + c] = mid[j] - mid[i];
        }
    }
}

}  // namespace hft::strategy
