// SPDX-License-Identifier: GPL-3.0-or-later
#include "sc2_cpu_affinity.h"
#include "sc2_dxt_decode.h"
#include "sc2_texture_cache.h"

extern "C" {
#include "d3d_texture_model.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

void decodeBlockLevelReference(const uint8_t *src, uint32_t width, uint32_t height,
    uint32_t format_byte, std::vector<uint8_t> &out)
{
    out.assign(static_cast<size_t>(width) * height * 4u, 0u);
    const bool dxt1 = format_byte == 0x0cu;
    const bool dxt3 = format_byte == 0x0eu;
    const uint32_t block_bytes = dxt1 ? 8u : 16u;
    const uint32_t blocks_x = (width + 3u) / 4u, blocks_y = (height + 3u) / 4u;
    const auto expand = [](uint16_t c, uint8_t rgb[3]) {
        rgb[0] = static_cast<uint8_t>(((c >> 11) & 31u) * 255u / 31u);
        rgb[1] = static_cast<uint8_t>(((c >> 5) & 63u) * 255u / 63u);
        rgb[2] = static_cast<uint8_t>((c & 31u) * 255u / 31u);
    };
    for (uint32_t by = 0u; by < blocks_y; ++by) {
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            const uint8_t *block = src + (static_cast<size_t>(by) * blocks_x + bx) * block_bytes;
            uint8_t alpha[16];
            const uint8_t *colour = block;
            if (dxt3) {
                for (int i = 0; i < 16; ++i) {
                    const uint8_t nibble = (block[i / 2] >> ((i & 1) * 4)) & 15u;
                    alpha[i] = static_cast<uint8_t>(nibble * 17u);
                }
                colour = block + 8;
            } else if (!dxt1) {
                uint8_t a[8] = {block[0], block[1]};
                if (a[0] > a[1]) {
                    for (int i = 1; i < 7; ++i) a[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
                } else {
                    for (int i = 1; i < 5; ++i) a[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
                    a[6] = 0u;
                    a[7] = 255u;
                }
                uint64_t bits = 0u;
                for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7u];
                colour = block + 8;
            } else {
                std::memset(alpha, 255, sizeof alpha);
            }
            const uint16_t c0 = static_cast<uint16_t>(colour[0] | colour[1] << 8);
            const uint16_t c1 = static_cast<uint16_t>(colour[2] | colour[3] << 8);
            uint8_t rgb[4][3];
            expand(c0, rgb[0]);
            expand(c1, rgb[1]);
            bool transparent = false;
            for (int k = 0; k < 3; ++k) {
                if (!dxt1 || c0 > c1) {
                    rgb[2][k] = static_cast<uint8_t>((2 * rgb[0][k] + rgb[1][k]) / 3);
                    rgb[3][k] = static_cast<uint8_t>((rgb[0][k] + 2 * rgb[1][k]) / 3);
                } else {
                    rgb[2][k] = static_cast<uint8_t>((rgb[0][k] + rgb[1][k]) / 2);
                    rgb[3][k] = 0u;
                    transparent = true;
                }
            }
            const uint32_t indices = static_cast<uint32_t>(colour[4] | colour[5] << 8 | colour[6] << 16) |
                static_cast<uint32_t>(colour[7]) << 24;
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4u + (i & 3), y = by * 4u + (i >> 2);
                if (x >= width || y >= height) continue;
                const uint32_t index = (indices >> (2 * i)) & 3u;
                uint8_t *texel = out.data() + (static_cast<size_t>(y) * width + x) * 4u;
                texel[0] = rgb[index][2];
                texel[1] = rgb[index][1];
                texel[2] = rgb[index][0];
                texel[3] = transparent && index == 3u ? 0u : alpha[i];
            }
        }
    }
}

void testCpuAffinity()
{
    // Case 1: 6x 1.80 GHz LITTLE + 2x 2.30 GHz BIG unthrottled -> selects ONLY the 2x 2.30 GHz performance cores
    // so the single-threaded Main Game + Inline Presenter Thread is never migrated onto slow LITTLE cores.
    std::vector<sc2::CpuFreqInfo> unthrottled(8);
    for (int i = 0; i < 6; ++i) unthrottled[i] = {true, 1804800, 1804800};
    for (int i = 6; i < 8; ++i) unthrottled[i] = {true, 2304000, 2304000};
    const auto c1 = sc2::SelectActiveCores(unthrottled);
    CHECK((c1 == std::vector<int>{6, 7}));

    // Case 2: 2+6 SoC when BIG cluster thermally throttles (2.30 GHz hardware throttled to 1.21 GHz scaling_max_freq)
    // -> stays on the 2x out-of-order BIG cores ({6, 7}) instead of evicting to in-order LITTLE cores or oscillating 6/8 <-> 8/8.
    std::vector<sc2::CpuFreqInfo> throttled = unthrottled;
    throttled[6].scaling_max_khz = 1209600;
    throttled[7].scaling_max_khz = 1209600;
    const auto c2 = sc2::SelectActiveCores(throttled);
    CHECK((c2 == std::vector<int>{6, 7}));

    // Case 3: Flagship 1x 2.8 GHz + 3x 2.4 GHz + 4x 1.8 GHz -> picks 4 fast cores, skips slow 4x 1.8 GHz.
    std::vector<sc2::CpuFreqInfo> flagship = {
        {true, 1800000, 1800000}, {true, 1800000, 1800000},
        {true, 1800000, 1800000}, {true, 1800000, 1800000},
        {true, 2400000, 2400000}, {true, 2400000, 2400000},
        {true, 2400000, 2400000}, {true, 2800000, 2800000},
    };
    const auto c3 = sc2::SelectActiveCores(flagship);
    CHECK((c3 == std::vector<int>{4, 5, 6, 7}));

    // Case 3b: Flagship 1+3+4 when single Prime core throttles to 1.4 GHz -> selects 3 unthrottled Mid performance cores.
    std::vector<sc2::CpuFreqInfo> flagship_throttled = flagship;
    flagship_throttled[7].scaling_max_khz = 1400000;
    const auto c3b = sc2::SelectActiveCores(flagship_throttled);
    CHECK((c3b == std::vector<int>{4, 5, 6}));

    // Case 4: Only 1 performance core online -> falls back to incluir LITTLE cores so >= 2 cores are available.
    std::vector<sc2::CpuFreqInfo> one_fast_only = {
        {true, 1400000, 1400000}, {true, 1400000, 1400000},
        {true, 1400000, 1400000}, {true, 1400000, 1400000},
        {true, 2400000, 2400000}, {false, 2400000, 2400000},
    };
    const auto c4 = sc2::SelectActiveCores(one_fast_only);
    CHECK(c4.size() == 5u);

    // Case 5: Unknown/zero frequencies -> empty.
    const auto c5 = sc2::SelectActiveCores({{true, 0, 0}, {false, 1800000, 1800000}});
    CHECK(c5.empty());
}

void testDxtDecodeExact()
{
    // Exhaustive check on reciprocal divisions vs exact integer divides for all 0..255 byte pairs.
    for (uint32_t a = 0u; a < 256u; ++a) {
        CHECK(sc2::detail::k5To8[a & 31u] == static_cast<uint8_t>((a & 31u) * 255u / 31u));
        CHECK(sc2::detail::k6To8[a & 63u] == static_cast<uint8_t>((a & 63u) * 255u / 63u));
        for (uint32_t b = 0u; b < 256u; b += 17u) {
            CHECK(static_cast<uint8_t>(((2u * a + b) * 21846u) >> 16u) == static_cast<uint8_t>((2u * a + b) / 3u));
            for (uint32_t w = 1u; w < 7u; ++w) {
                CHECK(static_cast<uint8_t>((((7u - w) * a + w * b) * 149797u) >> 20u) ==
                      static_cast<uint8_t>(((7u - w) * a + w * b) / 7u));
            }
            for (uint32_t w = 1u; w < 5u; ++w) {
                CHECK(static_cast<uint8_t>((((5u - w) * a + w * b) * 52429u) >> 18u) ==
                      static_cast<uint8_t>(((5u - w) * a + w * b) / 5u));
            }
        }
    }

    const uint32_t formats[] = {0x0cu, 0x0eu, 0x0fu};
    const uint32_t dims[][2] = {{1u, 1u}, {2u, 2u}, {3u, 5u}, {6u, 7u}, {16u, 16u}};
    std::vector<uint8_t> block_data(16u * 16u);
    uint32_t rng = 0x12345678u;
    for (size_t i = 0u; i < block_data.size(); ++i) {
        rng = rng * 1664525u + 1013904223u;
        block_data[i] = static_cast<uint8_t>(rng >> 24u);
    }

    std::vector<uint8_t> ref_out, fast_out;
    for (uint32_t fmt : formats) {
        for (const auto &dim : dims) {
            // Test both endpoint orderings on first block by flipping bytes.
            for (int flip = 0; flip < 2; ++flip) {
                block_data[0] ^= static_cast<uint8_t>(flip ? 0xffu : 0u);
                block_data[8] ^= static_cast<uint8_t>(flip ? 0xffu : 0u);
                decodeBlockLevelReference(block_data.data(), dim[0], dim[1], fmt, ref_out);
                sc2::decodeBlockLevelFast(block_data.data(), dim[0], dim[1], fmt, fast_out);
                CHECK(ref_out == fast_out);
            }
        }
    }
}

void testTextureUnswizzleExact()
{
    const uint32_t sizes[][2] = {{1u, 1u}, {64u, 64u}, {512u, 128u}, {128u, 512u}, {4096u, 1u}};
    const uint32_t bpts[] = {1u, 2u, 3u, 4u};
    for (uint32_t bpt : bpts) {
        for (const auto &sz : sizes) {
            const uint32_t w = sz[0], h = sz[1];
            const size_t total = static_cast<size_t>(w) * h * bpt;
            std::vector<uint8_t> src(total), dst_fast(total, 0u), dst_ref(total, 0u);
            for (size_t i = 0u; i < total; ++i) src[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xffu);
            CHECK(recomp_d3d_texture_unswizzle(src.data(), dst_fast.data(), w, h, bpt));
            uint32_t mx = 0u, my = 0u;
            recomp_d3d_texture_swizzle_masks(w, h, &mx, &my);
            for (uint32_t y = 0u; y < h; ++y) {
                for (uint32_t x = 0u; x < w; ++x) {
                    const uint32_t so = recomp_d3d_texture_swizzle_offset(x, y, mx, my);
                    std::memcpy(dst_ref.data() + (static_cast<size_t>(y) * w + x) * bpt,
                        src.data() + static_cast<size_t>(so) * bpt, bpt);
                }
            }
            CHECK(dst_fast == dst_ref);
        }
    }
}

void testTextureCachePolicy()
{
    // Do not flush render pass when entry has not been sampled in the active segment,
    // or when segment has no queued draws.
    CHECK(!sc2::ShouldFlushLinearBgraUpdate(1u, 2u, true));
    CHECK(!sc2::ShouldFlushLinearBgraUpdate(2u, 2u, false));
    // Must flush when active segment already queued at least one draw sampling this texture.
    CHECK(sc2::ShouldFlushLinearBgraUpdate(2u, 2u, true));

    // Linear BGRA row hashing: ignores pitch padding bytes and detects any active texel modification.
    std::vector<uint8_t> contiguous(16u * 4u, 0x5au);
    std::vector<uint8_t> padded(32u * 4u, 0xffu);
    for (uint32_t y = 0u; y < 4u; ++y) {
        std::memcpy(padded.data() + y * 32u, contiguous.data() + y * 16u, 16u);
    }
    const uint64_t h1 = sc2::HashLinearBgraRows(contiguous.data(), 16u, 4u, 16u);
    const uint64_t h2 = sc2::HashLinearBgraRows(padded.data(), 16u, 4u, 32u);
    CHECK(h1 == h2);
    padded[2u * 32u + 7u] ^= 0x01u;
    const uint64_t h3 = sc2::HashLinearBgraRows(padded.data(), 16u, 4u, 32u);
    CHECK(h1 != h3);

    // LRU slot selection: prefers unused slots first, then picks strictly lowest last_used.
    bool used[4] = {true, false, true, true};
    uint64_t last_used[4] = {10u, 0u, 3u, 8u};
    uint32_t cursor = 2u;
    CHECK(sc2::SelectTextureEvictionSlot(last_used, used, 4u, cursor) == 1u);
    used[1] = true;
    last_used[1] = 20u;
    cursor = 0u;
    CHECK(sc2::SelectTextureEvictionSlot(last_used, used, 4u, cursor) == 2u);
    CHECK(cursor == 3u);
}

} // namespace

int main()
{
    testCpuAffinity();
    testDxtDecodeExact();
    testTextureUnswizzleExact();
    testTextureCachePolicy();
    if (g_failures != 0) {
        std::fprintf(stderr, "sc2_perf_test: %d checks failed\n", g_failures);
        return 1;
    }
    std::printf("sc2_perf_test: ALL PASSED\n");
    return 0;
}
