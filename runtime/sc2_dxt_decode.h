// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SC2_DXT_DECODE_H
#define SC2_DXT_DECODE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sc2 {

namespace detail {

constexpr uint8_t k5To8[32] = {
    0, 8, 16, 24, 32, 41, 49, 57,
    65, 74, 82, 90, 98, 106, 115, 123,
    131, 139, 148, 156, 164, 172, 180, 189,
    197, 205, 213, 222, 230, 238, 246, 255
};

constexpr uint8_t k6To8[64] = {
    0, 4, 8, 12, 16, 20, 24, 28,
    32, 36, 40, 44, 48, 52, 56, 60,
    64, 68, 72, 76, 80, 85, 89, 93,
    97, 101, 105, 109, 113, 117, 121, 125,
    129, 133, 137, 141, 145, 149, 153, 157,
    161, 165, 170, 174, 178, 182, 186, 190,
    194, 198, 202, 206, 210, 214, 218, 222,
    226, 230, 234, 238, 242, 246, 250, 255
};

inline uint32_t packBgr(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint32_t>(b) |
           (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(r) << 16u);
}

inline void writeBgraBytes(uint8_t *dst, uint32_t bgr, uint8_t a)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const uint32_t pixel = bgr | (static_cast<uint32_t>(a) << 24u);
    std::memcpy(dst, &pixel, 4u);
#else
    dst[0] = static_cast<uint8_t>(bgr & 0xffu);
    dst[1] = static_cast<uint8_t>((bgr >> 8u) & 0xffu);
    dst[2] = static_cast<uint8_t>((bgr >> 16u) & 0xffu);
    dst[3] = a;
#endif
}

inline void decodeColorEndpoints(const uint8_t *colour, bool dxt1, uint32_t bgr_lut[4], bool &transparent)
{
    const uint16_t c0 = static_cast<uint16_t>(colour[0] | (static_cast<uint16_t>(colour[1]) << 8u));
    const uint16_t c1 = static_cast<uint16_t>(colour[2] | (static_cast<uint16_t>(colour[3]) << 8u));
    const uint32_t r0 = k5To8[(c0 >> 11u) & 31u];
    const uint32_t g0 = k6To8[(c0 >> 5u) & 63u];
    const uint32_t b0 = k5To8[c0 & 31u];
    const uint32_t r1 = k5To8[(c1 >> 11u) & 31u];
    const uint32_t g1 = k6To8[(c1 >> 5u) & 63u];
    const uint32_t b1 = k5To8[c1 & 31u];

    bgr_lut[0] = packBgr(static_cast<uint8_t>(r0), static_cast<uint8_t>(g0), static_cast<uint8_t>(b0));
    bgr_lut[1] = packBgr(static_cast<uint8_t>(r1), static_cast<uint8_t>(g1), static_cast<uint8_t>(b1));
    if (!dxt1 || c0 > c1) {
        transparent = false;
        const uint8_t r2 = static_cast<uint8_t>(((2u * r0 + r1) * 21846u) >> 16u);
        const uint8_t g2 = static_cast<uint8_t>(((2u * g0 + g1) * 21846u) >> 16u);
        const uint8_t b2 = static_cast<uint8_t>(((2u * b0 + b1) * 21846u) >> 16u);
        const uint8_t r3 = static_cast<uint8_t>(((r0 + 2u * r1) * 21846u) >> 16u);
        const uint8_t g3 = static_cast<uint8_t>(((g0 + 2u * g1) * 21846u) >> 16u);
        const uint8_t b3 = static_cast<uint8_t>(((b0 + 2u * b1) * 21846u) >> 16u);
        bgr_lut[2] = packBgr(r2, g2, b2);
        bgr_lut[3] = packBgr(r3, g3, b3);
    } else {
        transparent = true;
        const uint8_t r2 = static_cast<uint8_t>((r0 + r1) >> 1u);
        const uint8_t g2 = static_cast<uint8_t>((g0 + g1) >> 1u);
        const uint8_t b2 = static_cast<uint8_t>((b0 + b1) >> 1u);
        bgr_lut[2] = packBgr(r2, g2, b2);
        bgr_lut[3] = 0u;
    }
}

} // namespace detail

// Fast LUT-based DXT1 (0x0c) / DXT3 (0x0e) / DXT5 (0x0f) decoder producing BGRA8 bytes.
// Matches reference decodeBlockLevel bit-for-bit without per-channel integer divisions or re-allocating `out`.
inline void decodeBlockLevelFast(const uint8_t *src, uint32_t width, uint32_t height, uint32_t format_byte,
    std::vector<uint8_t> &out)
{
    const size_t total_bytes = static_cast<size_t>(width) * height * 4u;
    out.resize(total_bytes);
    if (width == 0u || height == 0u) return;

    const bool dxt1 = format_byte == 0x0cu;
    const bool dxt3 = format_byte == 0x0eu;
    const uint32_t block_bytes = dxt1 ? 8u : 16u;
    const uint32_t blocks_x = (width + 3u) / 4u;
    const uint32_t blocks_y = (height + 3u) / 4u;
    const size_t row_stride = static_cast<size_t>(width) * 4u;
    uint8_t *dst_base = out.data();

    for (uint32_t by = 0u; by < blocks_y; ++by) {
        const uint32_t y_base = by * 4u;
        const uint32_t valid_rows = height - y_base < 4u ? height - y_base : 4u;
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            const uint32_t x_base = bx * 4u;
            const uint32_t valid_cols = width - x_base < 4u ? width - x_base : 4u;
            const bool full_block = (valid_rows == 4u && valid_cols == 4u);
            const uint8_t *block = src + (static_cast<size_t>(by) * blocks_x + bx) * block_bytes;

            uint32_t bgr_lut[4];
            bool transparent = false;
            if (dxt1) {
                detail::decodeColorEndpoints(block, true, bgr_lut, transparent);
                const uint32_t indices = static_cast<uint32_t>(block[4]) |
                    (static_cast<uint32_t>(block[5]) << 8u) |
                    (static_cast<uint32_t>(block[6]) << 16u) |
                    (static_cast<uint32_t>(block[7]) << 24u);
                const uint32_t a_lut[4] = {255u, 255u, 255u, transparent ? 0u : 255u};
                uint8_t *row_ptr = dst_base + static_cast<size_t>(y_base) * row_stride + static_cast<size_t>(x_base) * 4u;
                if (full_block) {
                    for (uint32_t r = 0u; r < 4u; ++r) {
                        const uint32_t row_idx = indices >> (r * 8u);
                        for (uint32_t c = 0u; c < 4u; ++c) {
                            const uint32_t idx = (row_idx >> (c * 2u)) & 3u;
                            detail::writeBgraBytes(row_ptr + c * 4u, bgr_lut[idx], static_cast<uint8_t>(a_lut[idx]));
                        }
                        row_ptr += row_stride;
                    }
                } else {
                    for (uint32_t r = 0u; r < valid_rows; ++r) {
                        const uint32_t row_idx = indices >> (r * 8u);
                        for (uint32_t c = 0u; c < valid_cols; ++c) {
                            const uint32_t idx = (row_idx >> (c * 2u)) & 3u;
                            detail::writeBgraBytes(row_ptr + c * 4u, bgr_lut[idx], static_cast<uint8_t>(a_lut[idx]));
                        }
                        row_ptr += row_stride;
                    }
                }
            } else if (dxt3) {
                detail::decodeColorEndpoints(block + 8u, false, bgr_lut, transparent);
                const uint32_t indices = static_cast<uint32_t>(block[12]) |
                    (static_cast<uint32_t>(block[13]) << 8u) |
                    (static_cast<uint32_t>(block[14]) << 16u) |
                    (static_cast<uint32_t>(block[15]) << 24u);
                uint8_t *row_ptr = dst_base + static_cast<size_t>(y_base) * row_stride + static_cast<size_t>(x_base) * 4u;
                const uint32_t rows = full_block ? 4u : valid_rows;
                const uint32_t cols = full_block ? 4u : valid_cols;
                for (uint32_t r = 0u; r < rows; ++r) {
                    const uint32_t row_idx = indices >> (r * 8u);
                    const uint16_t alpha_row = static_cast<uint16_t>(block[r * 2u]) |
                        (static_cast<uint16_t>(block[r * 2u + 1u]) << 8u);
                    for (uint32_t c = 0u; c < cols; ++c) {
                        const uint32_t idx = (row_idx >> (c * 2u)) & 3u;
                        const uint8_t a = static_cast<uint8_t>(((alpha_row >> (c * 4u)) & 15u) * 17u);
                        detail::writeBgraBytes(row_ptr + c * 4u, bgr_lut[idx], a);
                    }
                    row_ptr += row_stride;
                }
            } else {
                uint8_t a[8];
                a[0] = block[0];
                a[1] = block[1];
                const uint32_t a0 = a[0];
                const uint32_t a1 = a[1];
                if (a0 > a1) {
                    a[2] = static_cast<uint8_t>(((6u * a0 + 1u * a1) * 149797u) >> 20u);
                    a[3] = static_cast<uint8_t>(((5u * a0 + 2u * a1) * 149797u) >> 20u);
                    a[4] = static_cast<uint8_t>(((4u * a0 + 3u * a1) * 149797u) >> 20u);
                    a[5] = static_cast<uint8_t>(((3u * a0 + 4u * a1) * 149797u) >> 20u);
                    a[6] = static_cast<uint8_t>(((2u * a0 + 5u * a1) * 149797u) >> 20u);
                    a[7] = static_cast<uint8_t>(((1u * a0 + 6u * a1) * 149797u) >> 20u);
                } else {
                    a[2] = static_cast<uint8_t>(((4u * a0 + 1u * a1) * 52429u) >> 18u);
                    a[3] = static_cast<uint8_t>(((3u * a0 + 2u * a1) * 52429u) >> 18u);
                    a[4] = static_cast<uint8_t>(((2u * a0 + 3u * a1) * 52429u) >> 18u);
                    a[5] = static_cast<uint8_t>(((1u * a0 + 4u * a1) * 52429u) >> 18u);
                    a[6] = 0u;
                    a[7] = 255u;
                }
                const uint64_t alpha_bits =
                    static_cast<uint64_t>(block[2]) |
                    (static_cast<uint64_t>(block[3]) << 8u) |
                    (static_cast<uint64_t>(block[4]) << 16u) |
                    (static_cast<uint64_t>(block[5]) << 24u) |
                    (static_cast<uint64_t>(block[6]) << 32u) |
                    (static_cast<uint64_t>(block[7]) << 40u);
                detail::decodeColorEndpoints(block + 8u, false, bgr_lut, transparent);
                const uint32_t indices = static_cast<uint32_t>(block[12]) |
                    (static_cast<uint32_t>(block[13]) << 8u) |
                    (static_cast<uint32_t>(block[14]) << 16u) |
                    (static_cast<uint32_t>(block[15]) << 24u);
                uint8_t *row_ptr = dst_base + static_cast<size_t>(y_base) * row_stride + static_cast<size_t>(x_base) * 4u;
                const uint32_t rows = full_block ? 4u : valid_rows;
                const uint32_t cols = full_block ? 4u : valid_cols;
                for (uint32_t r = 0u; r < rows; ++r) {
                    const uint32_t row_idx = indices >> (r * 8u);
                    const uint32_t row_alpha = static_cast<uint32_t>((alpha_bits >> (r * 12u)) & 0xfffu);
                    for (uint32_t c = 0u; c < cols; ++c) {
                        const uint32_t idx = (row_idx >> (c * 2u)) & 3u;
                        const uint8_t alpha = a[(row_alpha >> (c * 3u)) & 7u];
                        detail::writeBgraBytes(row_ptr + c * 4u, bgr_lut[idx], alpha);
                    }
                    row_ptr += row_stride;
                }
            }
        }
    }
}

} // namespace sc2

#endif
