// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SC2_TEXTURE_CACHE_H
#define SC2_TEXTURE_CACHE_H

#include <cstdint>
#include <cstring>

namespace sc2 {

// Only flush the active render pass before updating a linear_bgra (0x12) texture if
// draws already queued into THIS un-flushed segment sampled this exact cached texture.
inline bool ShouldFlushLinearBgraUpdate(uint64_t entry_last_segment_id, uint64_t current_segment_id,
    bool segment_has_draws)
{
    return segment_has_draws && entry_last_segment_id == current_segment_id;
}

// Fast 64-bit word hash over active texel rows of a linear_bgra (0x12) texture so unchanged
// linear_bgra textures skip both render-pass flushes and GPU transfer uploads.
inline uint64_t HashLinearBgraRows(const void *bytes, uint32_t row_bytes, uint32_t height, uint32_t pitch)
{
    if (bytes == nullptr || row_bytes == 0u || height == 0u) return 0u;
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ (static_cast<uint64_t>(row_bytes) * height);
    if (pitch == row_bytes) {
        const size_t total = static_cast<size_t>(row_bytes) * height;
        const size_t words = total / 8u;
        for (size_t i = 0u; i < words; ++i) {
            uint64_t w;
            std::memcpy(&w, data + i * 8u, sizeof w);
            hash = (hash ^ w) * 0x100000001b3ull;
        }
        for (size_t i = words * 8u; i < total; ++i) {
            hash = (hash ^ data[i]) * 0x100000001b3ull;
        }
    } else {
        const uint32_t words_per_row = row_bytes / 8u;
        for (uint32_t y = 0u; y < height; ++y) {
            const uint8_t *row = data + static_cast<size_t>(y) * pitch;
            for (uint32_t i = 0u; i < words_per_row; ++i) {
                uint64_t w;
                std::memcpy(&w, row + static_cast<size_t>(i) * 8u, sizeof w);
                hash = (hash ^ w) * 0x100000001b3ull;
            }
            for (uint32_t i = words_per_row * 8u; i < row_bytes; ++i) {
                hash = (hash ^ row[i]) * 0x100000001b3ull;
            }
        }
    }
    return hash == 0u ? 1u : hash;
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
