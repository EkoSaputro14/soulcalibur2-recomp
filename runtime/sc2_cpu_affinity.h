// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SC2_CPU_AFFINITY_H
#define SC2_CPU_AFFINITY_H

#include <algorithm>
#include <vector>

namespace sc2 {

struct CpuFreqInfo {
    bool online = true;
    long info_max_khz = 0;
    long scaling_max_khz = 0;
};

inline long effectiveCoreFreqKhz(const CpuFreqInfo &core)
{
    if (!core.online) return 0;
    if (core.info_max_khz > 0 && core.scaling_max_khz > 0) {
        return std::min(core.info_max_khz, core.scaling_max_khz);
    }
    return core.info_max_khz > 0 ? core.info_max_khz : core.scaling_max_khz;
}

// Selects active CPU cores using effective frequency (min(cpuinfo_max_freq, scaling_max_freq)).
// 1) Picks all online cores within >= 70% of the best current effective frequency, which naturally
//    includes fast big+mid clusters (e.g. 1.80 GHz vs 2.30 GHz = 78.3%) while avoiding pure
//    low-power cores when big cores are unthrottled, AND automatically migrating away from
//    thermally throttled prime cores (e.g. 1.21 GHz vs 1.80 GHz = 67.2%).
// 2) Ensures at least min(4, available) highest-effective-frequency online cores are included
//    so guest fibers, audio, video decode, Vulkan driver, and shader threads do not starve on 2 cores.
inline std::vector<int> SelectActiveCores(const std::vector<CpuFreqInfo> &cores)
{
    const size_t n = cores.size();
    std::vector<long> effective(n, 0);
    long best = 0;
    int available = 0;
    for (size_t i = 0; i < n; ++i) {
        effective[i] = effectiveCoreFreqKhz(cores[i]);
        if (effective[i] > 0) {
            ++available;
            if (effective[i] > best) best = effective[i];
        }
    }
    std::vector<int> selected;
    if (best <= 0 || available == 0) return selected;

    std::vector<bool> picked(n, false);
    int picked_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (effective[i] > 0 && effective[i] * 100 >= best * 70) {
            picked[i] = true;
            ++picked_count;
        }
    }

    const int min_cores = std::min(4, available);
    while (picked_count < min_cores) {
        long next_best = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!picked[i] && effective[i] > next_best) next_best = effective[i];
        }
        if (next_best <= 0) break;
        for (size_t i = 0; i < n; ++i) {
            if (!picked[i] && effective[i] == next_best) {
                picked[i] = true;
                ++picked_count;
            }
        }
    }

    selected.reserve(static_cast<size_t>(picked_count));
    for (size_t i = 0; i < n; ++i) {
        if (picked[i]) selected.push_back(static_cast<int>(i));
    }
    return selected;
}

} // namespace sc2

#endif
