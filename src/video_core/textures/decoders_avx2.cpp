// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <span>

#include <immintrin.h> // AVX2

#include "common/assert.h"
#include "common/div_ceil.h"
#include "video_core/textures/decoders.h"
#include "video_core/textures/decoders_avx2.h"

namespace Tegra::Texture {
namespace {

template <u32 mask>
constexpr u32 Pdep(u32 value) {
    u32 result = 0;
    u32 m = mask;
    for (u32 bit = 1; m; bit += bit) {
        if (value & bit) {
            result |= m & (~m + 1);
        }
        m &= m - 1;
    }
    return result;
}

template <u32 mask, u32 incr_amount>
void IncrPdep(u32& value) {
    static constexpr u32 swizzled_incr = Pdep<mask>(incr_amount);
    value = ((value | ~mask) + swizzled_incr) & mask;
}

template <u32 BYTES_PER_PIXEL>
__attribute__((target("avx2"))) void UnswizzleGobPermuteAVX2Impl(std::span<u8> output,
                                                                  std::span<const u8> input,
                                                                  u32 width, u32 height, u32 depth,
                                                                  u32 block_height,
                                                                  u32 block_depth, u32 stride) {
    static_assert(32 % BYTES_PER_PIXEL == 0, "BYTES_PER_PIXEL must evenly divide 32");

    static constexpr u32 RUN_BYTES = 32;
    static constexpr u32 PIXELS_PER_RUN = RUN_BYTES / BYTES_PER_PIXEL;

    const u32 pitch = width * BYTES_PER_PIXEL;
    const u32 gobs_in_x = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT);
    const u32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const u32 slice_size =
        Common::DivCeilLog2(height, block_height + GOB_SIZE_Y_SHIFT) * block_size;
    const u32 block_height_mask = (1U << block_height) - 1;
    const u32 block_depth_mask = (1U << block_depth) - 1;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    for (u32 slice = 0; slice < depth; ++slice) {
        const u32 offset_z = (slice >> block_depth) * slice_size +
                             ((slice & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));

        for (u32 line = 0; line < height; ++line) {
            const u32 swizzled_y = Pdep<SWIZZLE_Y_BITS>(line);
            const u32 block_y = line >> GOB_SIZE_Y_SHIFT;
            const u32 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 swizzled_x = 0;
            u32 column = 0;

            for (; column + PIXELS_PER_RUN <= width;
                 column += PIXELS_PER_RUN, IncrPdep<SWIZZLE_X_BITS, RUN_BYTES>(swizzled_x)) {

                const u32 x = column * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;
                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 swizzled_offset = base_swizzled_offset + (swizzled_x | swizzled_y);
                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                u8* const dst = &output[unswizzled_offset];
                const u8* const src = &input[swizzled_offset];

                const __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
                const __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 32));

                __m256i val = _mm256_castsi128_si256(lo);
                val = _mm256_inserti128_si256(val, hi, 1);

                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), val);
            }

            if constexpr (BYTES_PER_PIXEL <= 16) {
                constexpr u32 PIXELS_PER_16 = 16 / BYTES_PER_PIXEL;
                if (column + PIXELS_PER_16 <= width) {
                    const u32 x = column * BYTES_PER_PIXEL;
                    const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;
                    const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                    const u32 swizzled_offset = base_swizzled_offset + (swizzled_x | swizzled_y);
                    const u32 unswizzled_offset =
                        slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                    u8* const dst = &output[unswizzled_offset];
                    const u8* const src = &input[swizzled_offset];

                    const __m128i val = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), val);

                    column += PIXELS_PER_16;
                    IncrPdep<SWIZZLE_X_BITS, 16>(swizzled_x);
                }
            }

            for (; column < width;
                 ++column, IncrPdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {

                const u32 x = column * BYTES_PER_PIXEL;
                const u32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;
                const u32 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 swizzled_offset = base_swizzled_offset + (swizzled_x | swizzled_y);
                const u32 unswizzled_offset =
                    slice * pitch * height + line * pitch + column * BYTES_PER_PIXEL;

                u8* const dst = &output[unswizzled_offset];
                const u8* const src = &input[swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
    }
}

} // Anonymous namespace

void UnswizzleGobPermuteAVX2(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                             u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                             u32 stride) {
    switch (bytes_per_pixel) {
    case 1:
        return UnswizzleGobPermuteAVX2Impl<1>(output, input, width, height, depth, block_height,
                                              block_depth, stride);
    case 2:
        return UnswizzleGobPermuteAVX2Impl<2>(output, input, width, height, depth, block_height,
                                              block_depth, stride);
    case 4:
        return UnswizzleGobPermuteAVX2Impl<4>(output, input, width, height, depth, block_height,
                                              block_depth, stride);
    case 8:
        return UnswizzleGobPermuteAVX2Impl<8>(output, input, width, height, depth, block_height,
                                              block_depth, stride);
    case 16:
        return UnswizzleGobPermuteAVX2Impl<16>(output, input, width, height, depth, block_height,
                                               block_depth, stride);
    default:
        UNREACHABLE();
    }
}

} // namespace Tegra::Texture