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

// Selects active CPU cores for the main game + inline presenter thread:
// 1) On heterogeneous big.LITTLE / DynamIQ SoCs (max_info_max > min_info_max), cores with
//    info_max_khz > min_info_max are out-of-order performance cores (Big / Mid / Prime), while
//    cores at min_info_max are in-order LITTLE efficiency cores (e.g. Cortex-A53/A55).
// 2) Whenever >= 2 performance cores are online, restricts selection strictly to performance
//    cores so Android EAS never migrates the single-threaded main loop onto slow LITTLE cores,
//    and never evicts 2-big-core clusters (2+6 SoCs) into LITTLE-only masks when scaling_max_freq
//    thermally throttles (eliminating 2-second 6/8 <-> 8/8 affinity oscillation).
// 3) On 3-tier SoCs (e.g. 1 Prime + 3 Mid + 4 LITTLE), if the single Prime core throttles below
//    70% of the Mid cluster's effective frequency, selects the unthrottled Mid performance cores.
inline std::vector<int> SelectActiveCores(const std::vector<CpuFreqInfo> &cores)
{
    const size_t n = cores.size();
    std::vector<long> effective(n, 0);
    long best_all = 0;
    int available = 0;
    long min_info_max = 0;
    long max_info_max = 0;

    for (size_t i = 0; i < n; ++i) {
        effective[i] = effectiveCoreFreqKhz(cores[i]);
        if (effective[i] > 0) {
            ++available;
            if (effective[i] > best_all) best_all = effective[i];
        }
        const long hw_max = cores[i].info_max_khz > 0 ? cores[i].info_max_khz : effective[i];
        if (hw_max > 0) {
            if (min_info_max == 0 || hw_max < min_info_max) min_info_max = hw_max;
            if (hw_max > max_info_max) max_info_max = hw_max;
        }
    }

    std::vector<int> selected;
    if (best_all <= 0 || available == 0) return selected;

    int perf_online = 0;
    long best_perf = 0;
    if (max_info_max > min_info_max) {
        for (size_t i = 0; i < n; ++i) {
            const long hw_max = cores[i].info_max_khz > 0 ? cores[i].info_max_khz : effective[i];
            if (effective[i] > 0 && hw_max > min_info_max) {
                ++perf_online;
                if (effective[i] > best_perf) best_perf = effective[i];
            }
        }
    }

    const bool use_perf_only = (perf_online >= 2 && best_perf > 0);
    const long reference_best = use_perf_only ? best_perf : best_all;

    auto isCandidate = [&](size_t i) -> bool {
        if (effective[i] <= 0) return false;
        if (!use_perf_only) return true;
        const long hw_max = cores[i].info_max_khz > 0 ? cores[i].info_max_khz : effective[i];
        return hw_max > min_info_max;
    };

    std::vector<bool> picked(n, false);
    int picked_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (isCandidate(i) && effective[i] * 100 >= reference_best * 70) {
            picked[i] = true;
            ++picked_count;
        }
    }

    const int min_cores = use_perf_only ? std::min(2, perf_online) : std::min(4, available);
    while (picked_count < min_cores) {
        long next_best = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!picked[i] && isCandidate(i) && effective[i] > next_best) next_best = effective[i];
        }
        if (next_best <= 0) break;
        for (size_t i = 0; i < n; ++i) {
            if (!picked[i] && isCandidate(i) && effective[i] == next_best) {
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
