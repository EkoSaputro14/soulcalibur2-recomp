// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SC2_PERF_METRICS_H
#define SC2_PERF_METRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompD3dPresenterPerf {
    uint64_t draw_ns;
    uint64_t upload_ns;
    uint64_t tex_decode_ns;
    uint64_t flush_ns;
    uint64_t present_wait_ns;
    uint64_t present_total_ns;
    uint32_t draws;
    uint32_t uploads;
    uint32_t dxt_decodes;
    uint32_t flushes;
} RecompD3dPresenterPerf;

void recomp_d3d_presenter_take_perf(RecompD3dPresenterPerf *out);

#ifdef __cplusplus
}
#endif

#endif
