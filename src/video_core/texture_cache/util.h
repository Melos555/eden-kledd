// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/scratch_buffer.h"

#include "video_core/surface.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

using Tegra::Texture::TICEntry;

using LevelArray = std::array<u32, MAX_MIP_LEVELS>;

struct OverlapResult {
    GPUVAddr gpu_addr;
    VAddr cpu_addr;
    SubresourceExtent resources;
};

[[nodiscard]] u32 CalculateGuestSizeInBytes(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateUnswizzledSizeBytes(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateConvertedSizeBytes(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateLayerStride(const ImageInfo& info) noexcept;

[[nodiscard]] u32 CalculateLayerSize(const ImageInfo& info) noexcept;

[[nodiscard]] LevelArray CalculateMipLevelOffsets(const ImageInfo& info) noexcept;

[[nodiscard]] LevelArray CalculateMipLevelSizes(const ImageInfo& info) noexcept;

[[nodiscard]] boost::container::small_vector<u32, 16> CalculateSliceOffsets(const ImageInfo& info);

[[nodiscard]] boost::container::small_vector<SubresourceBase, 16> CalculateSliceSubresources(
    const ImageInfo& info);

[[nodiscard]] u32 CalculateLevelStrideAlignment(const ImageInfo& info, u32 level);

[[nodiscard]] VideoCore::Surface::PixelFormat PixelFormatFromTIC(
    const Tegra::Texture::TICEntry& config) noexcept;

[[nodiscard]] ImageViewType RenderTargetImageViewType(const ImageInfo& info) noexcept;

[[nodiscard]] boost::container::small_vector<ImageCopy, 16> MakeShrinkImageCopies(
    const ImageInfo& dst, const ImageInfo& src, SubresourceBase base, u32 up_scale = 1,
    u32 down_shift = 0);

[[nodiscard]] boost::container::small_vector<ImageCopy, 16> MakeReinterpretImageCopies(
    const ImageInfo& src, u32 up_scale = 1, u32 down_shift = 0);

[[nodiscard]] bool IsValidEntry(const Tegra::MemoryManager& gpu_memory, const TICEntry& config);

[[nodiscard]] boost::container::small_vector<BufferImageCopy, 16> UnswizzleImage(
    Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, const ImageInfo& info,
    std::span<const u8> input, std::span<u8> output);

/// Unswizzles only the swizzle blocks of a single-level, single-layer block linear image that
/// intersect `ranges` (byte offsets from gpu_addr, half-open). Each run of consecutive blocks
/// along X is decoded as its own small block linear texture into `output`, and one copy per run
/// covers its texels. Returns nullopt when the image's layout isn't one this handles, so the
/// caller uploads the whole image instead.
[[nodiscard]] std::optional<boost::container::small_vector<BufferImageCopy, 16>>
UnswizzleImageRanges(Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, const ImageInfo& info,
                     u64 guest_size_bytes, std::span<const std::pair<u64, u64>> ranges,
                     std::vector<u8>& output, std::vector<u8>& scratch);

/// How the guest pages of a single-level, single-layer block linear image map onto texel boxes:
/// page p covers page_texels starting at PageOffset(p). Only exists when every page is exactly
/// one such box (whole swizzle blocks, consecutive along X, rows of blocks a whole number of
/// pages), which is what a sparse image needs to bind one host tile per guest page.
struct SparsePageLayout {
    Extent3D page_texels;
    u32 pages_x;
    u32 pages_y;
    u32 pages_z;
    u64 level_bytes;

    [[nodiscard]] u32 NumPages() const noexcept {
        return pages_x * pages_y * pages_z;
    }

    [[nodiscard]] Offset3D PageOffset(u32 page) const noexcept {
        return Offset3D{
            .x = static_cast<s32>((page % pages_x) * page_texels.width),
            .y = static_cast<s32>((page / pages_x % pages_y) * page_texels.height),
            .z = static_cast<s32>((page / pages_x / pages_y) * page_texels.depth),
        };
    }
};

[[nodiscard]] std::optional<SparsePageLayout> ComputeSparsePageLayout(const ImageInfo& info,
                                                                     u64 page_size);

void ConvertImage(std::span<const u8> input, const ImageInfo& info, std::span<u8> output,
                  std::span<BufferImageCopy> copies);

[[nodiscard]] boost::container::small_vector<BufferImageCopy, 16> FullDownloadCopies(
    const ImageInfo& info);

[[nodiscard]] Extent3D MipSize(Extent3D size, u32 level);

[[nodiscard]] Extent3D MipBlockSize(const ImageInfo& info, u32 level);

[[nodiscard]] boost::container::small_vector<SwizzleParameters, 16> FullUploadSwizzles(
    const ImageInfo& info);

void SwizzleImage(Tegra::MemoryManager& gpu_memory, GPUVAddr gpu_addr, const ImageInfo& info,
                  std::span<const BufferImageCopy> copies, std::span<const u8> memory,
                  Common::ScratchBuffer<u8>& tmp_buffer);

[[nodiscard]] bool IsBlockLinearSizeCompatible(const ImageInfo& new_info,
                                               const ImageInfo& overlap_info, u32 new_level,
                                               u32 overlap_level, bool strict_size) noexcept;

[[nodiscard]] bool IsPitchLinearSameSize(const ImageInfo& lhs, const ImageInfo& rhs,
                                         bool strict_size) noexcept;

[[nodiscard]] bool IsBlockLinearSizeCompatibleBPPRelaxed(const ImageInfo& lhs, const ImageInfo& rhs,
                                                         u32 lhs_level, u32 rhs_level) noexcept;

[[nodiscard]] std::optional<OverlapResult> ResolveOverlap(const ImageInfo& new_info,
                                                          GPUVAddr gpu_addr, VAddr cpu_addr,
                                                          const ImageBase& overlap,
                                                          bool strict_size, bool broken_views,
                                                          bool native_bgr);

[[nodiscard]] bool IsLayerStrideCompatible(const ImageInfo& lhs, const ImageInfo& rhs);

[[nodiscard]] std::optional<SubresourceBase> FindSubresource(const ImageInfo& candidate,
                                                             const ImageBase& image,
                                                             GPUVAddr candidate_addr,
                                                             RelaxedOptions options,
                                                             bool broken_views, bool native_bgr);

[[nodiscard]] bool IsSubresource(const ImageInfo& candidate, const ImageBase& image,
                                 GPUVAddr candidate_addr, RelaxedOptions options, bool broken_views,
                                 bool native_bgr);

[[nodiscard]] bool IsSubCopy(const ImageInfo& candidate, const ImageBase& image,
                             GPUVAddr candidate_addr);

void DeduceBlitImages(ImageInfo& dst_info, ImageInfo& src_info, const ImageBase* dst,
                      const ImageBase* src);

[[nodiscard]] u32 MapSizeBytes(const ImageBase& image);

} // namespace VideoCommon
