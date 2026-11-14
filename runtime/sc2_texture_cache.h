// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SC2_TEXTURE_CACHE_H
#define SC2_TEXTURE_CACHE_H

#include <cstdint>

namespace sc2 {

// Only flush the active render pass before updating a linear_bgra (0x12) texture if
// draws already queued into THIS un-flushed segment sampled this exact cached texture.
inline bool ShouldFlushLinearBgraUpdate(uint64_t entry_last_segment_id, uint64_t current_segment_id,
    bool segment_has_draws)
{
    return segment_has_draws && entry_last_segment_id == current_segment_id;
}

// LRU texture cache slot selection with empty-slot preference.
inline uint32_t SelectTextureEvictionSlot(const uint64_t *last_used, const bool *used, uint32_t count,
    uint32_t &cursor)
{
    if (count == 0u) return 0u;
    for (uint32_t probe = 0u; probe < count; ++probe) {
        const uint32_t slot = (cursor + probe) % count;
        if (!used[slot]) {
            cursor = (slot + 1u) % count;
            return slot;
        }
    }
    uint32_t best = cursor % count;
    uint64_t oldest = last_used[best];
    for (uint32_t probe = 1u; probe < count; ++probe) {
        const uint32_t slot = (cursor + probe) % count;
        if (last_used[slot] < oldest) {
            oldest = last_used[slot];
            best = slot;
        }
    }
    cursor = (best + 1u) % count;
    return best;
}

} // namespace sc2

#endif
