// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <array>
#include <format>
#include <optional>
#include <span>
#include <memory>
#include <utility>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>
#include <boost/container/small_vector.hpp>
#include <bit>
#include <numeric>
#include "common/bit_util.h"
#include "common/settings.h"

#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/gpu_logging/gpu_logging.h"

#include "video_core/engines/fermi_2d.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/textures/decoders.h"

namespace Vulkan {

using Tegra::Engines::Fermi2D;
using Tegra::Texture::SwizzleSource;
using Tegra::Texture::TextureMipmapFilter;
using VideoCommon::BufferImageCopy;
using VideoCommon::ImageFlagBits;
using VideoCommon::ImageInfo;
using VideoCommon::ImageType;
using VideoCommon::SubresourceRange;
using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::HasAlpha;
using VideoCore::Surface::IsPixelFormatASTC;
using VideoCore::Surface::IsPixelFormatInteger;
using VideoCore::Surface::IsPixelFormatSRGB;
using VideoCore::Surface::SurfaceType;

namespace {
constexpr bool ENABLE_MSAA_TILER_RESOLVE = true;
constexpr bool ENABLE_MSAA_RESOLVE_CONSUME = true;
constexpr bool ENABLE_MSAA_COLOR_DISCARD = true;
constexpr bool ENABLE_MSAA_DEPTH_STENCIL_DISCARD = true;

[[nodiscard]] constexpr bool NeedsExplicitBorderColorFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        return true;
    default:
        return false;
    }
}

constexpr VkBorderColor ConvertBorderColor(const std::array<float, 4>& color) {
    if (color == std::array<float, 4>{0, 0, 0, 0}) {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    } else if (color == std::array<float, 4>{0, 0, 0, 1}) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else if (color == std::array<float, 4>{1, 1, 1, 1}) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    if (color[0] + color[1] + color[2] > 1.35f) {
        // If color elements are brighter than roughly 0.5 average, use white border
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    } else if (color[3] > 0.5f) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
}

[[nodiscard]] VkImageType ConvertImageType(const ImageType type) {
    switch (type) {
    case ImageType::e1D:
        return VK_IMAGE_TYPE_1D;
    case ImageType::e2D:
    case ImageType::Linear:
        return VK_IMAGE_TYPE_2D;
    case ImageType::e3D:
        return VK_IMAGE_TYPE_3D;
    case ImageType::Buffer:
        break;
    }
    ASSERT_MSG(false, "Invalid image type={}", type);
    return {};
}

[[nodiscard]] VkSampleCountFlagBits ConvertSampleCount(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return VK_SAMPLE_COUNT_1_BIT;
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    default:
        ASSERT_MSG(false, "Invalid number of samples={}", num_samples);
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

[[nodiscard]] VkImageUsageFlags ImageUsageFlags(const MaxwellToVK::FormatInfo& info,
                                                PixelFormat format, bool allow_storage = true) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info.attachable) {
        switch (VideoCore::Surface::GetFormatType(format)) {
        case VideoCore::Surface::SurfaceType::ColorTexture:
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            break;
        case VideoCore::Surface::SurfaceType::Depth:
        case VideoCore::Surface::SurfaceType::Stencil:
        case VideoCore::Surface::SurfaceType::DepthStencil:
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            break;
        default:
            ASSERT_MSG(false, "Invalid surface type");
            break;
        }
    }
    if (info.storage && allow_storage) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return usage;
}

[[nodiscard]] bool WillUseAcceleratedAstcDecode(const Device& device, const ImageInfo& info) {
    if (!IsPixelFormatASTC(info.format) || device.IsOptimalAstcSupported()) {
        return false;
    }
    if (Settings::values.accelerate_astc.GetValue() != Settings::AstcDecodeMode::Gpu) {
        return false;
    }
    const auto recompression = Settings::values.astc_recompression.GetValue();
    // eden-kledd: "BC7 (GPU)" decodes and encodes on the GPU; "BC7 (CPU, slow)" never does
    const bool gpu_bc7 = recompression == Settings::AstcRecompression::Bc7;
    return (recompression == Settings::AstcRecompression::Uncompressed || gpu_bc7) &&
           info.size.depth == 1;
}

[[nodiscard]] bool UsesAstcStorageViews(const Device& device, const ImageInfo& info) {
    // The RGBA8 storage views are only valid when the image itself is RGBA8 (uncompressed)
    return WillUseAcceleratedAstcDecode(device, info) &&
           Settings::values.astc_recompression.GetValue() ==
               Settings::AstcRecompression::Uncompressed;
}

[[nodiscard]] VkImageCreateInfo MakeImageCreateInfo(const Device& device, const ImageInfo& info,
                                                    std::optional<VkFormat> format_override = {}) {
    auto format_info =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, info.format);
    if (format_override) {
        format_info.format = *format_override;
        format_info.attachable = false;
        format_info.storage = true;
    }
    VkImageCreateFlags flags{};
    if (info.type == ImageType::e2D && info.resources.layers >= 6 &&
        info.size.width == info.size.height && !device.HasBrokenCubeImageCompatibility()) {
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (info.type == ImageType::e3D) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
    const bool allow_storage =
        info.num_samples == 1 || device.IsStorageImageMultisampleSupported();
    return VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = format_info.format,
        .extent{
            .width = info.size.width >> samples_x,
            .height = info.size.height >> samples_y,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = ConvertSampleCount(info.num_samples),
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ImageUsageFlags(format_info, info.format, allow_storage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}
[[nodiscard]] VkImageCreateInfo MakeMsaaScratchImageCreateInfo(const Device& device,
                                                               const ImageInfo& info,
                                                               VkImageUsageFlags usage) {
    ImageInfo temp_info = info;
    temp_info.num_samples = 1;
    VkImageCreateInfo image_ci = MakeImageCreateInfo(device, temp_info);
    image_ci.format =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, info.format).format;
    image_ci.usage = usage;
    return image_ci;
}

[[nodiscard]] vk::Image MakeImage(const Device& device, const MemoryAllocator& allocator,
                                  const ImageInfo& info, std::span<const VkFormat> view_formats,
                                  std::optional<VkFormat> format_override = {}) {
    if (info.type == ImageType::Buffer) {
        return vk::Image{};
    }
    VkImageCreateInfo image_ci = MakeImageCreateInfo(device, info, format_override);
    const VkImageFormatListCreateInfo image_format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (view_formats.size() > 1) {
        image_ci.flags |=
            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

        const bool has_storage_compatible_view =
            std::any_of(view_formats.begin(), view_formats.end(), [&device](VkFormat view_format) {
                return device.IsFormatSupported(view_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                                                FormatType::Optimal);
            });
        if (has_storage_compatible_view) {
            image_ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }

        if (device.IsKhrImageFormatListSupported()) {
            image_ci.pNext = &image_format_list;
        }
    }
    return allocator.CreateImage(image_ci);
}

[[nodiscard]] vk::ImageView MakeStorageView(const vk::Device& device, u32 level, VkImage image,
                                            VkFormat format) {
    static constexpr VkImageViewUsageCreateInfo storage_image_view_usage_create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
    };
    return device.CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &storage_image_view_usage_create_info,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = level,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    });
}

[[nodiscard]] VkImageAspectFlags ImageAspectMask(PixelFormat format) {
    switch (VideoCore::Surface::GetFormatType(format)) {
    case VideoCore::Surface::SurfaceType::ColorTexture:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    case VideoCore::Surface::SurfaceType::Depth:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VideoCore::Surface::SurfaceType::Stencil:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VideoCore::Surface::SurfaceType::DepthStencil:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        ASSERT_MSG(false, "Invalid surface type");
        return VkImageAspectFlags{};
    }
}

[[nodiscard]] bool IsLdrAstcFormat(VkFormat format) {
    return format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}

[[nodiscard]] VkImageAspectFlags ImageViewAspectMask(const VideoCommon::ImageViewInfo& info) {
    if (info.IsRenderTarget()) {
        return ImageAspectMask(info.format);
    }
    bool any_r =
        std::ranges::any_of(info.Swizzle(), [](SwizzleSource s) { return s == SwizzleSource::R; });
    switch (info.format) {
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::D32_FLOAT_S8_UINT:
        // R = depth, G = stencil
        return any_r ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    case PixelFormat::S8_UINT_D24_UNORM:
        // R = stencil, G = depth
        return any_r ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormat::D16_UNORM:
    case PixelFormat::D32_FLOAT:
    case PixelFormat::X8_D24_UNORM:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormat::S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

[[nodiscard]] VkComponentSwizzle ComponentSwizzle(SwizzleSource swizzle) {
    switch (swizzle) {
    case SwizzleSource::Zero:
        return VK_COMPONENT_SWIZZLE_ZERO;
    case SwizzleSource::R:
        return VK_COMPONENT_SWIZZLE_R;
    case SwizzleSource::G:
        return VK_COMPONENT_SWIZZLE_G;
    case SwizzleSource::B:
        return VK_COMPONENT_SWIZZLE_B;
    case SwizzleSource::A:
        return VK_COMPONENT_SWIZZLE_A;
    case SwizzleSource::OneFloat:
    case SwizzleSource::OneInt:
        return VK_COMPONENT_SWIZZLE_ONE;
    }
    ASSERT_MSG(false, "Invalid swizzle={}", swizzle);
    return VK_COMPONENT_SWIZZLE_ZERO;
}

void SanitizeDepthStencilSwizzle(std::array<SwizzleSource, 4>& swizzle,
                                 bool supports_depth_stencil_swizzle_one) {
    if (supports_depth_stencil_swizzle_one) {
        return;
    }
    std::replace_if(swizzle.begin(), swizzle.end(),
                    [](SwizzleSource value) {
                        return value == SwizzleSource::OneFloat ||
                               value == SwizzleSource::OneInt;
                    },
                    SwizzleSource::Zero);
}

[[nodiscard]] VkImageViewType ImageViewType(Shader::TextureType type) {
    switch (type) {
    case Shader::TextureType::Color1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case Shader::TextureType::Color2D:
    case Shader::TextureType::Color2DRect:
        return VK_IMAGE_VIEW_TYPE_2D;
    case Shader::TextureType::ColorCube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case Shader::TextureType::Color3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case Shader::TextureType::ColorArray1D:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case Shader::TextureType::ColorArray2D:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case Shader::TextureType::ColorArrayCube:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case Shader::TextureType::Buffer:
        ASSERT_MSG(false, "Texture buffers can't be image views");
        return VK_IMAGE_VIEW_TYPE_1D;
    }
    ASSERT_MSG(false, "Invalid image view type={}", type);
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkImageViewType ImageViewType(VideoCommon::ImageViewType type) {
    switch (type) {
    case VideoCommon::ImageViewType::e1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::Rect:
        return VK_IMAGE_VIEW_TYPE_2D;
    case VideoCommon::ImageViewType::Cube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case VideoCommon::ImageViewType::e3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case VideoCommon::ImageViewType::e1DArray:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case VideoCommon::ImageViewType::e2DArray:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case VideoCommon::ImageViewType::CubeArray:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case VideoCommon::ImageViewType::Buffer:
        ASSERT_MSG(false, "Texture buffers can't be image views");
        return VK_IMAGE_VIEW_TYPE_1D;
    }
    ASSERT_MSG(false, "Invalid image view type={}", type);
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkImageSubresourceLayers MakeImageSubresourceLayers(
    VideoCommon::SubresourceLayers subresource, VkImageAspectFlags aspect_mask) {
    return VkImageSubresourceLayers{
        .aspectMask = aspect_mask,
        .mipLevel = static_cast<u32>(subresource.base_level),
        .baseArrayLayer = static_cast<u32>(subresource.base_layer),
        .layerCount = static_cast<u32>(subresource.num_layers),
    };
}

[[nodiscard]] VkOffset3D MakeOffset3D(VideoCommon::Offset3D offset3d) {
    return VkOffset3D{
        .x = offset3d.x,
        .y = offset3d.y,
        .z = offset3d.z,
    };
}

[[nodiscard]] VkExtent3D MakeExtent3D(VideoCommon::Extent3D extent3d) {
    return VkExtent3D{
        .width = static_cast<u32>(extent3d.width),
        .height = static_cast<u32>(extent3d.height),
        .depth = static_cast<u32>(extent3d.depth),
    };
}

[[nodiscard]] VkImageCopy MakeImageCopy(const VideoCommon::ImageCopy& copy,
                                        VkImageAspectFlags aspect_mask) noexcept {
    return VkImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(copy.src_subresource, aspect_mask),
        .srcOffset = MakeOffset3D(copy.src_offset),
        .dstSubresource = MakeImageSubresourceLayers(copy.dst_subresource, aspect_mask),
        .dstOffset = MakeOffset3D(copy.dst_offset),
        .extent = MakeExtent3D(copy.extent),
    };
}

[[nodiscard]] VkBufferImageCopy MakeBufferImageCopy(const VideoCommon::ImageCopy& copy, bool is_src,
                                                    VkImageAspectFlags aspect_mask) noexcept {
    return VkBufferImageCopy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = MakeImageSubresourceLayers(
            is_src ? copy.src_subresource : copy.dst_subresource, aspect_mask),
        .imageOffset = MakeOffset3D(is_src ? copy.src_offset : copy.dst_offset),
        .imageExtent = MakeExtent3D(copy.extent),
    };
}

[[maybe_unused]] [[nodiscard]] boost::container::small_vector<VkBufferCopy, 16>
TransformBufferCopies(std::span<const VideoCommon::BufferCopy> copies, size_t buffer_offset) {
    boost::container::small_vector<VkBufferCopy, 16> result(copies.size());
    std::ranges::transform(
        copies, result.begin(), [buffer_offset](const VideoCommon::BufferCopy& copy) {
            return VkBufferCopy{
                .srcOffset = static_cast<VkDeviceSize>(copy.src_offset + buffer_offset),
                .dstOffset = static_cast<VkDeviceSize>(copy.dst_offset),
                .size = static_cast<VkDeviceSize>(copy.size),
            };
        });
    return result;
}

[[nodiscard]] boost::container::small_vector<VkBufferImageCopy, 16> TransformBufferImageCopies(
    std::span<const BufferImageCopy> copies, size_t buffer_offset, VkImageAspectFlags aspect_mask) {
    struct Maker {
        VkBufferImageCopy operator()(const BufferImageCopy& copy) const {
            return VkBufferImageCopy{
                .bufferOffset = copy.buffer_offset + buffer_offset,
                .bufferRowLength = copy.buffer_row_length,
                .bufferImageHeight = copy.buffer_image_height,
                .imageSubresource =
                    {
                        .aspectMask = aspect_mask,
                        .mipLevel = static_cast<u32>(copy.image_subresource.base_level),
                        .baseArrayLayer = static_cast<u32>(copy.image_subresource.base_layer),
                        .layerCount = static_cast<u32>(copy.image_subresource.num_layers),
                    },
                .imageOffset =
                    {
                        .x = copy.image_offset.x,
                        .y = copy.image_offset.y,
                        .z = copy.image_offset.z,
                    },
                .imageExtent =
                    {
                        .width = copy.image_extent.width,
                        .height = copy.image_extent.height,
                        .depth = copy.image_extent.depth,
                    },
            };
        }
        size_t buffer_offset;
        VkImageAspectFlags aspect_mask;
    };
    if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        boost::container::small_vector<VkBufferImageCopy, 16> result(copies.size() * 2);
        std::ranges::transform(copies, result.begin(),
                               Maker{buffer_offset, VK_IMAGE_ASPECT_DEPTH_BIT});
        std::ranges::transform(copies, result.begin() + copies.size(),
                               Maker{buffer_offset, VK_IMAGE_ASPECT_STENCIL_BIT});
        return result;
    } else {
        boost::container::small_vector<VkBufferImageCopy, 16> result(copies.size());
        std::ranges::transform(copies, result.begin(), Maker{buffer_offset, aspect_mask});
        return result;
    }
}

[[nodiscard]] VkImageSubresourceRange MakeSubresourceRange(VkImageAspectFlags aspect_mask,
                                                           const SubresourceRange& range) {
    return VkImageSubresourceRange{
        .aspectMask = aspect_mask,
        .baseMipLevel = static_cast<u32>(range.base.level),
        .levelCount = static_cast<u32>(range.extent.levels),
        .baseArrayLayer = static_cast<u32>(range.base.layer),
        .layerCount = static_cast<u32>(range.extent.layers),
    };
}

[[nodiscard]] VkImageSubresourceRange MakeSubresourceRange(const ImageView* image_view) {
    SubresourceRange range = image_view->range;
    if (True(image_view->flags & VideoCommon::ImageViewFlagBits::Slice)) {
        // Slice image views always affect a single layer, but their subresource range corresponds
        // to the slice. Override the value to affect a single layer.
        range.base.layer = 0;
        range.extent.layers = 1;
    }
    return MakeSubresourceRange(ImageAspectMask(image_view->format), range);
}

[[nodiscard]] VkImageSubresourceLayers MakeSubresourceLayers(const ImageView* image_view) {
    return VkImageSubresourceLayers{
        .aspectMask = ImageAspectMask(image_view->format),
        .mipLevel = static_cast<u32>(image_view->range.base.level),
        .baseArrayLayer = static_cast<u32>(image_view->range.base.layer),
        .layerCount = static_cast<u32>(image_view->range.extent.layers),
    };
}

[[nodiscard]] SwizzleSource ConvertGreenRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::G:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapBlueRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::R:
        return SwizzleSource::B;
    case SwizzleSource::B:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapGreenRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::R:
        return SwizzleSource::G;
    case SwizzleSource::G:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapSpecial(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::A:
        return SwizzleSource::R;
    case SwizzleSource::R:
        return SwizzleSource::A;
    case SwizzleSource::G:
        return SwizzleSource::B;
    case SwizzleSource::B:
        return SwizzleSource::G;
    default:
        return value;
    }
}
struct RangedBarrierRange {
    u32 min_mip = (std::numeric_limits<u32>::max)();
    u32 max_mip = (std::numeric_limits<u32>::min)();
    u32 min_layer = (std::numeric_limits<u32>::max)();
    u32 max_layer = (std::numeric_limits<u32>::min)();

    void AddLayers(const VkImageSubresourceLayers& layers) {
        min_mip = (std::min)(min_mip, layers.mipLevel);
        max_mip = (std::max)(max_mip, layers.mipLevel + 1);
        min_layer = (std::min)(min_layer, layers.baseArrayLayer);
        max_layer = (std::max)(max_layer, layers.baseArrayLayer + layers.layerCount);
    }

    VkImageSubresourceRange SubresourceRange(VkImageAspectFlags aspect_mask) const noexcept {
        return VkImageSubresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = min_mip,
            .levelCount = max_mip - min_mip,
            .baseArrayLayer = min_layer,
            .layerCount = max_layer - min_layer,
        };
    }
};
void CopyBufferToImage(vk::CommandBuffer cmdbuf, VkBuffer src_buffer, VkImage image,
                       VkImageAspectFlags aspect_mask, bool is_initialized,
                       std::span<const VkBufferImageCopy> copies) {
    static constexpr VkAccessFlags WRITE_ACCESS_FLAGS =
                                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkAccessFlags READ_ACCESS_FLAGS = VK_ACCESS_SHADER_READ_BIT |
                                                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    //  Compute exact mip/layer range being written to
    RangedBarrierRange range;
    for (const auto& region : copies) {
        range.AddLayers(region.imageSubresource);
    }
    const VkImageSubresourceRange subresource_range = range.SubresourceRange(aspect_mask);

    const VkImageMemoryBarrier read_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = WRITE_ACCESS_FLAGS,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = is_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = subresource_range,
    };

    const VkImageMemoryBarrier write_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = WRITE_ACCESS_FLAGS | READ_ACCESS_FLAGS,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = subresource_range,
    };

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           read_barrier);
    cmdbuf.CopyBufferToImage(src_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies);
    // TODO: Move this to another API
    cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, nullptr, nullptr, write_barrier);
}

[[nodiscard]] VkImageBlit MakeImageBlit(const Region2D& dst_region, const Region2D& src_region,
                                        const VkImageSubresourceLayers& dst_layers,
                                        const VkImageSubresourceLayers& src_layers) {
    return VkImageBlit{
        .srcSubresource = src_layers,
        .srcOffsets =
            {
                {
                    .x = src_region.start.x,
                    .y = src_region.start.y,
                    .z = 0,
                },
                {
                    .x = src_region.end.x,
                    .y = src_region.end.y,
                    .z = 1,
                },
            },
        .dstSubresource = dst_layers,
        .dstOffsets =
            {
                {
                    .x = dst_region.start.x,
                    .y = dst_region.start.y,
                    .z = 0,
                },
                {
                    .x = dst_region.end.x,
                    .y = dst_region.end.y,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] bool HaveSameExtent(const Region2D& dst_region, const Region2D& src_region) {
    return dst_region.end.x - dst_region.start.x == src_region.end.x - src_region.start.x &&
           dst_region.end.y - dst_region.start.y == src_region.end.y - src_region.start.y;
}

[[nodiscard]] VkImageResolve MakeImageResolve(const Region2D& dst_region,
                                              const Region2D& src_region,
                                              const VkImageSubresourceLayers& dst_layers,
                                              const VkImageSubresourceLayers& src_layers) {
    return VkImageResolve{
        .srcSubresource = src_layers,
        .srcOffset =
            {
                .x = src_region.start.x,
                .y = src_region.start.y,
                .z = 0,
            },
        .dstSubresource = dst_layers,
        .dstOffset =
            {
                .x = dst_region.start.x,
                .y = dst_region.start.y,
                .z = 0,
            },
        .extent =
            {
                .width = static_cast<u32>(dst_region.end.x - dst_region.start.x),
                .height = static_cast<u32>(dst_region.end.y - dst_region.start.y),
                .depth = 1,
            },
    };
}

void TryTransformSwizzleIfNeeded(PixelFormat format, std::array<SwizzleSource, 4>& swizzle,
                                 bool emulate_bgr565, bool emulate_a4b4g4r4) {
    switch (format) {
    case PixelFormat::A1B5G5R5_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapBlueRed);
        break;
    case PixelFormat::B5G6R5_UNORM:
        if (emulate_bgr565) {
            std::ranges::transform(swizzle, swizzle.begin(), SwapBlueRed);
        }
        break;
    case PixelFormat::A5B5G5R1_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapSpecial);
        break;
    case PixelFormat::G4R4_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapGreenRed);
        break;
    case PixelFormat::A4B4G4R4_UNORM:
        if (emulate_a4b4g4r4) {
            std::ranges::reverse(swizzle);
        }
        break;
    default:
        break;
    }
}



[[nodiscard]] VkFormat Format(Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        break;
    case Shader::ImageFormat::R8_SINT:
        return VK_FORMAT_R8_SINT;
    case Shader::ImageFormat::R8_UINT:
        return VK_FORMAT_R8_UINT;
    case Shader::ImageFormat::R16_UINT:
        return VK_FORMAT_R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return VK_FORMAT_R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return VK_FORMAT_R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return VK_FORMAT_R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return VK_FORMAT_R32G32B32A32_UINT;
    }
    ASSERT_MSG(false, "Invalid image format={}", format);
    return VK_FORMAT_R32_UINT;
}

void BlitScale(Scheduler& scheduler, VkImage src_image, VkImage dst_image, const ImageInfo& info,
               VkImageAspectFlags aspect_mask, const Settings::ResolutionScalingInfo& resolution,
               bool up_scaling = true) {
    const bool is_2d = info.type == ImageType::e2D;
    const auto resources = info.resources;
    const VkExtent2D extent{
        .width = info.size.width,
        .height = info.size.height,
    };
    // Depth and integer formats must use NEAREST filter for blits.
    const bool is_color{aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT};
    const bool is_bilinear{is_color && !IsPixelFormatInteger(info.format)};
    const VkFilter vk_filter = is_bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, extent, resources, aspect_mask, resolution, is_2d,
                      vk_filter, up_scaling](vk::CommandBuffer cmdbuf) {
        const u32 scaled_width = resolution.ScaleUp(extent.width);
        const u32 scaled_height = is_2d ? resolution.ScaleUp(extent.height) : extent.height;
        const VkOffset2D src_size{
            .x = static_cast<s32>(up_scaling ? extent.width : scaled_width),
            .y = static_cast<s32>(up_scaling ? extent.height : scaled_height),
        };
        const VkOffset2D dst_size{
            .x = static_cast<s32>(up_scaling ? scaled_width : extent.width),
            .y = static_cast<s32>(up_scaling ? scaled_height : extent.height),
        };
        boost::container::small_vector<VkImageBlit, 4> regions;
        regions.reserve(resources.levels);
        for (s32 level = 0; level < resources.levels; level++) {
            regions.push_back({
                .srcSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(level),
                    .baseArrayLayer = 0,
                    .layerCount = static_cast<u32>(resources.layers),
                },
                .srcOffsets{
                    {
                        .x = 0,
                        .y = 0,
                        .z = 0,
                    },
                    {
                        .x = (std::max)(1, src_size.x >> level),
                        .y = (std::max)(1, src_size.y >> level),
                        .z = 1,
                    },
                },
                .dstSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(level),
                    .baseArrayLayer = 0,
                    .layerCount = static_cast<u32>(resources.layers),
                },
                .dstOffsets{
                    {
                        .x = 0,
                        .y = 0,
                        .z = 0,
                    },
                    {
                        .x = (std::max)(1, dst_size.x >> level),
                        .y = (std::max)(1, dst_size.y >> level),
                        .z = 1,
                    },
                },
            });
        }
        const VkImageSubresourceRange subresource_range{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array read_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = subresource_range,
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // Discard contents
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = subresource_range,
            },
        };
        const std::array write_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = subresource_range,
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = subresource_range,
            },
        };
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, nullptr, nullptr, read_barriers);
        cmdbuf.BlitImage(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions, vk_filter);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                       0, nullptr, nullptr, write_barriers);
    });
}
} // Anonymous namespace

TextureCacheRuntime::TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                         MemoryAllocator& memory_allocator_,
                                         StagingBufferPool& staging_buffer_pool_,
                                         BlitImageHelper& blit_image_helper_,
                                         RenderPassCache& render_pass_cache_,
                                         DescriptorPool& descriptor_pool,
                                         ComputePassDescriptorQueue& compute_pass_descriptor_queue)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, sparse_tiles{device_},
      blit_image_helper{blit_image_helper_}, render_pass_cache{render_pass_cache_},
      resolution{Settings::values.resolution_info} {
    if (Settings::values.accelerate_astc.GetValue() == Settings::AstcDecodeMode::Gpu) {
        astc_decoder_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                  compute_pass_descriptor_queue, memory_allocator);
    }
    if (!device.IsKhrImageFormatListSupported()) {
        return;
    }
    for (size_t index_a = 0; index_a < VideoCore::Surface::MaxPixelFormat; index_a++) {
        const auto image_format = static_cast<PixelFormat>(index_a);
        if (IsPixelFormatASTC(image_format) && !device.IsOptimalAstcSupported()) {
            view_formats[index_a].push_back(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
        }
        for (size_t index_b = 0; index_b < VideoCore::Surface::MaxPixelFormat; index_b++) {
            const auto view_format = static_cast<PixelFormat>(index_b);
            if (VideoCore::Surface::IsViewCompatible(image_format, view_format, false, true)) {
                const auto view_info =
                    MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, view_format);
                view_formats[index_a].push_back(view_info.format);
            }
        }
    }

    bl3d_unswizzle_pass.emplace(device, scheduler, descriptor_pool,
                               staging_buffer_pool, compute_pass_descriptor_queue);
}

void TextureCacheRuntime::Finish() {
    scheduler.Finish();
}

StagingBufferRef TextureCacheRuntime::UploadStagingBuffer(size_t size, bool deferred) {
    return staging_buffer_pool.Request(size, MemoryUsage::Upload, deferred);
}

StagingBufferRef TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_buffer_pool.Request(size, MemoryUsage::Download, deferred);
}

void TextureCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_buffer_pool.FreeDeferred(ref);
}

bool TextureCacheRuntime::ShouldReinterpret(Image& dst, Image& src) {
    if (VideoCore::Surface::GetFormatType(dst.info.format) ==
            VideoCore::Surface::SurfaceType::DepthStencil &&
        !device.IsExtShaderStencilExportSupported()) {
        return true;
    }

    if (VideoCore::Surface::GetFormatType(src.info.format) ==
            VideoCore::Surface::SurfaceType::DepthStencil &&
        !device.IsExtShaderStencilExportSupported()) {
        return true;
    }

    if (dst.info.format == PixelFormat::D32_FLOAT_S8_UINT ||
        src.info.format == PixelFormat::D32_FLOAT_S8_UINT) {
        return true;
    }
    return false;
}

VkBuffer TextureCacheRuntime::GetTemporaryBuffer(size_t needed_size) {
    const auto level = (8 * sizeof(size_t)) - std::countl_zero(needed_size - 1ULL);
    if (buffers[level]) {
        return *buffers[level];
    }
    const auto new_size = Common::NextPow2(needed_size);
    static constexpr VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    const VkBufferCreateInfo temp_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = new_size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    buffers[level] = memory_allocator.CreateBuffer(temp_ci, MemoryUsage::DeviceLocal);
    return *buffers[level];
}

VkImageView TextureCacheRuntime::GetOrCreateResolveShadow(VkImage msaa_image, VkFormat format,
                                                          VkExtent2D extent, u32 layers,
                                                          VkImageAspectFlags aspect_mask) {
    ResolveShadow& shadow = resolve_shadows[msaa_image];
    if (shadow.image && shadow.format == format && shadow.extent.width == extent.width &&
        shadow.extent.height == extent.height && shadow.layers == layers &&
        shadow.aspect_mask == aspect_mask) {
        return *shadow.view;
    }
    VkImageUsageFlags shadow_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
        shadow_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        shadow_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (shadow.image) {
        pending_resolve_shadows.emplace_back(scheduler.CurrentTick(), std::move(shadow));
    }
    shadow.image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = shadow_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    shadow.view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *shadow.image,
        .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{},
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = layers,
        },
    });
    shadow.format = format;
    shadow.extent = extent;
    shadow.layers = layers;
    shadow.aspect_mask = aspect_mask;
    shadow.up_to_date = false;
    return *shadow.view;
}

const TextureCacheRuntime::ResolveShadow* TextureCacheRuntime::GetValidResolveShadow(
    VkImage msaa_image) const {
    const auto it = resolve_shadows.find(msaa_image);
    if (it == resolve_shadows.end() || !it->second.up_to_date || !it->second.image) {
        return nullptr;
    }
    return &it->second;
}

void TextureCacheRuntime::MarkResolveShadowUpToDate(VkImage msaa_image) {
    const auto it = resolve_shadows.find(msaa_image);
    if (it != resolve_shadows.end()) {
        it->second.up_to_date = true;
    }
}

void TextureCacheRuntime::InvalidateResolveShadow(VkImage msaa_image) {
    const auto it = resolve_shadows.find(msaa_image);
    if (it != resolve_shadows.end()) {
        it->second.up_to_date = false;
    }
}

void TextureCacheRuntime::EraseResolveShadow(VkImage msaa_image) {
    const auto it = resolve_shadows.find(msaa_image);
    if (it == resolve_shadows.end()) {
        return;
    }
    if (it->second.image) {
        pending_resolve_shadows.emplace_back(scheduler.CurrentTick(), std::move(it->second));
    }
    resolve_shadows.erase(it);
}

void TextureCacheRuntime::BarrierFeedbackLoop() {
    scheduler.RequestOutsideRenderPassOperationContext();
}

void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    dst.PrepareSparseWrite();
    if (ENABLE_MSAA_RESOLVE_CONSUME) {
        InvalidateResolveShadow(dst.Handle());
    }
    boost::container::small_vector<VkBufferImageCopy, 16> vk_in_copies(copies.size());
    boost::container::small_vector<VkBufferImageCopy, 16> vk_out_copies(copies.size());
    const VkImageAspectFlags src_aspect_mask = src.AspectMask();
    const VkImageAspectFlags dst_aspect_mask = dst.AspectMask();

    const auto bpp_in = BytesPerBlock(src.info.format) / DefaultBlockWidth(src.info.format);
    const auto bpp_out = BytesPerBlock(dst.info.format) / DefaultBlockWidth(dst.info.format);
    std::ranges::transform(copies, vk_in_copies.begin(),
                           [src_aspect_mask, bpp_in, bpp_out](const auto& copy) {
                               auto copy2 = copy;
                               copy2.src_offset.x = (bpp_out * copy.src_offset.x) / bpp_in;
                               copy2.extent.width = (bpp_out * copy.extent.width) / bpp_in;
                               return MakeBufferImageCopy(copy2, true, src_aspect_mask);
                           });
    std::ranges::transform(copies, vk_out_copies.begin(), [dst_aspect_mask](const auto& copy) {
        return MakeBufferImageCopy(copy, false, dst_aspect_mask);
    });
    const u32 img_bpp = BytesPerBlock(dst.info.format);
    size_t total_size = 0;
    for (const auto& copy : copies) {
        total_size += copy.extent.width * copy.extent.height * copy.extent.depth * img_bpp;
    }
    const VkBuffer copy_buffer = GetTemporaryBuffer(total_size);
    const VkImage dst_image = dst.Handle();
    const VkImage src_image = src.Handle();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, copy_buffer, src_aspect_mask, dst_aspect_mask,
                      vk_in_copies, vk_out_copies](vk::CommandBuffer cmdbuf) {
        RangedBarrierRange dst_range;
        RangedBarrierRange src_range;
        for (const VkBufferImageCopy& copy : vk_in_copies) {
            src_range.AddLayers(copy.imageSubresource);
        }
        for (const VkBufferImageCopy& copy : vk_out_copies) {
            dst_range.AddLayers(copy.imageSubresource);
        }
        static constexpr VkMemoryBarrier READ_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        const std::array pre_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(src_aspect_mask),
            },
        };
        const std::array middle_in_barrier{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = 0,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(src_aspect_mask),
            },
        };
        const std::array middle_out_barrier{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(dst_aspect_mask),
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(dst_aspect_mask),
            },
        };
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, {}, {}, pre_barriers);

        cmdbuf.CopyImageToBuffer(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                                 vk_in_copies);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                       0, WRITE_BARRIER, nullptr, middle_in_barrier);

        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, READ_BARRIER, {}, middle_out_barrier);
        cmdbuf.CopyBufferToImage(copy_buffer, dst_image, VK_IMAGE_LAYOUT_GENERAL, vk_out_copies);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                       0, {}, {}, post_barriers);
    });
}

void TextureCacheRuntime::BlitImage(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation operation) {
    const VkImageAspectFlags aspect_mask = ImageAspectMask(src.format);
    const bool is_dst_msaa = dst.Samples() != VK_SAMPLE_COUNT_1_BIT;
    const bool is_src_msaa = src.Samples() != VK_SAMPLE_COUNT_1_BIT;
    if (aspect_mask != ImageAspectMask(dst.format)) {
        UNIMPLEMENTED_MSG("Incompatible blit from format {} to {}", src.format, dst.format);
        return;
    }
    if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT && !is_src_msaa && !is_dst_msaa) {
        blit_image_helper.BlitColor(dst_framebuffer, src, dst_region, src_region, filter,
                                    operation);
        return;
    }
    ASSERT(src.format == dst.format);
    if (is_src_msaa && !is_dst_msaa &&
        (aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
        if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
            UNIMPLEMENTED_MSG("Stencil-only MSAA resolve is not supported");
            return;
        }
        blit_image_helper.ResolveDepthStencil(dst_framebuffer, src, dst_region, src_region);
        return;
    }
    if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const auto format = src.format;
        const auto can_blit_depth_stencil = [this, format] {
            switch (format) {
            case VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT:
            case VideoCore::Surface::PixelFormat::S8_UINT_D24_UNORM:
                return device.IsBlitDepth24Stencil8Supported();
            case VideoCore::Surface::PixelFormat::D32_FLOAT_S8_UINT:
                return device.IsBlitDepth32Stencil8Supported();
            default:
                UNREACHABLE();
            }
        }();
        // Use shader-based depth/stencil blits if hardware doesn't support the format
        // Note: MSAA resolves (MSAA->single) use vkCmdResolveImage which works fine
        if (!can_blit_depth_stencil) {
            UNIMPLEMENTED_IF(is_src_msaa || is_dst_msaa);
            blit_image_helper.BlitDepthStencil(dst_framebuffer, src, dst_region, src_region,
                                               filter, operation);
            return;
        }
    }
    ASSERT(!(is_dst_msaa && !is_src_msaa));
    ASSERT(operation == Fermi2D::Operation::SrcCopy);

    const bool is_msaa_to_msaa = is_src_msaa && is_dst_msaa;
    if (is_msaa_to_msaa && aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
        blit_image_helper.BlitColorMSAA(dst_framebuffer, src, dst_region, src_region);
        return;
    }
    if (is_msaa_to_msaa) {
        blit_image_helper.BlitDepthStencilMSAA(dst_framebuffer, src, dst_region, src_region);
        return;
    }

    const bool is_resolve = is_src_msaa && !is_dst_msaa;
    if (is_resolve && !HaveSameExtent(dst_region, src_region)) {
        blit_image_helper.BlitColorMSAA(dst_framebuffer, src, dst_region, src_region);
        return;
    }

    const VkImage dst_image = dst.ImageHandle();
    const VkImage src_image = src.ImageHandle();
    const VkImageSubresourceLayers dst_layers = MakeSubresourceLayers(&dst);
    const VkImageSubresourceLayers src_layers = MakeSubresourceLayers(&src);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([filter, dst_region, src_region, dst_image, src_image, dst_layers, src_layers,
                      aspect_mask, is_resolve](vk::CommandBuffer cmdbuf) {
        const std::array read_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange{
                    .aspectMask = aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{
                    .aspectMask = aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        VkImageMemoryBarrier write_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, nullptr, nullptr, read_barriers);
        if (is_resolve) {
            cmdbuf.ResolveImage(src_image, VK_IMAGE_LAYOUT_GENERAL, dst_image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                MakeImageResolve(dst_region, src_region, dst_layers, src_layers));
        } else {
            const bool is_linear = filter == Fermi2D::Filter::Bilinear;
            const VkFilter vk_filter = is_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            cmdbuf.BlitImage(
                src_image, VK_IMAGE_LAYOUT_GENERAL, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                MakeImageBlit(dst_region, src_region, dst_layers, src_layers), vk_filter);
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                       0, write_barrier);
    });
}

void TextureCacheRuntime::ConvertImage(Framebuffer* dst, ImageView& dst_view, ImageView& src_view) {
    if (!dst->RenderPass()) {
        return;
    }

    switch (dst_view.format) {
    case PixelFormat::R16_UNORM:
        if (src_view.format == PixelFormat::D16_UNORM) {
            return blit_image_helper.ConvertD16ToR16(dst, src_view);
        }
        break;
    case PixelFormat::A8B8G8R8_SRGB:
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::B8G8R8A8_UNORM:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::A8B8G8R8_UNORM:
        if (src_view.format == PixelFormat::S8_UINT_D24_UNORM) {
            return blit_image_helper.ConvertD24S8ToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::D24_UNORM_S8_UINT) {
            return blit_image_helper.ConvertS8D24ToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::R32_FLOAT:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32ToR32(dst, src_view);
        }
        break;
    case PixelFormat::D16_UNORM:
        if (src_view.format == PixelFormat::R16_UNORM) {
            return blit_image_helper.ConvertR16ToD16(dst, src_view);
        }
        break;
    case PixelFormat::S8_UINT_D24_UNORM:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM) {
            return blit_image_helper.ConvertABGR8ToD24S8(dst, src_view);
        }
        break;
    case PixelFormat::D32_FLOAT:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM ||
            src_view.format == PixelFormat::A8B8G8R8_SRGB ||
            src_view.format == PixelFormat::B8G8R8A8_SRGB) {
            return blit_image_helper.ConvertABGR8ToD32F(dst, src_view);
        }
        if (src_view.format == PixelFormat::R32_FLOAT) {
            return blit_image_helper.ConvertR32ToD32(dst, src_view);
        }
        break;
    case PixelFormat::D24_UNORM_S8_UINT:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM ||
            src_view.format == PixelFormat::A8B8G8R8_SRGB ||
            src_view.format == PixelFormat::B8G8R8A8_SRGB) {
            return blit_image_helper.ConvertABGR8ToD24S8(dst, src_view);
        }
        break;
    default:
        break;
    }

    if (src_view.format == PixelFormat::D32_FLOAT &&
        VideoCore::Surface::GetFormatType(dst_view.format) == SurfaceType::ColorTexture &&
        (dst_view.format == PixelFormat::B5G6R5_UNORM ||
         Settings::values.fix_bloom_effects.GetValue())) {
        const Region2D region{
            .start = {0, 0},
            .end = {static_cast<s32>(dst->RenderArea().width),
                    static_cast<s32>(dst->RenderArea().height)},
        };
        return blit_image_helper.BlitColor(dst, src_view, region, region,
                                        Tegra::Engines::Fermi2D::Filter::Point,
                                        Tegra::Engines::Fermi2D::Operation::SrcCopy);
    }

    LOG_DEBUG(Render_Vulkan, "Unimplemented texture conversion from {} to {} format type", src_view.format, dst_view.format);
}

VkFormat TextureCacheRuntime::GetSupportedFormat(VkFormat requested_format,
                                                VkFormatFeatureFlags required_features) const {
    if (requested_format == VK_FORMAT_A8B8G8R8_SRGB_PACK32 &&
        (required_features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        // Force valid depth format when sRGB requested in depth context
        return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return requested_format;
}

// Helper functions for format compatibility checks
bool TextureCacheRuntime::IsFormatDitherable(PixelFormat format) {
    switch (format) {
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::B8G8R8A8_SRGB:
    case PixelFormat::A8B8G8R8_SRGB:
        return true;
    default:
        return false;
    }
}

bool TextureCacheRuntime::IsFormatScalable(PixelFormat format) {
    switch (format) {
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::R16G16B16A16_FLOAT:
    case PixelFormat::R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

void TextureCacheRuntime::CopyImage(Image& dst, Image& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    dst.PrepareSparseWrite();
    if (ENABLE_MSAA_RESOLVE_CONSUME) {
        InvalidateResolveShadow(dst.Handle());
    }
    // As per the size-compatible formats section of vulkan, copy manually via ReinterpretImage
    // these images that aren't size-compatible
    if (BytesPerBlock(src.info.format) != BytesPerBlock(dst.info.format)) {
#ifdef _WIN32
        // On Windows, linear images cause device loss when used in image copies.
        // Tested with TitleID: 0x010067300059A00 (Mario + Rabbids Kingdom Battle)
        if (src.info.type == ImageType::Linear || dst.info.type == ImageType::Linear) {
            return;
        }
#endif
        auto oneCopy = VideoCommon::ImageCopy{
            .src_offset = VideoCommon::Offset3D(0, 0, 0),
            .dst_offset = VideoCommon::Offset3D(0, 0, 0),
            .extent = dst.info.size
        };
        return ReinterpretImage(dst, src, std::span{&oneCopy, 1});
    }
    boost::container::small_vector<VkImageCopy, 16> vk_copies(copies.size());
    const VkImageAspectFlags aspect_mask = dst.AspectMask();
    ASSERT(aspect_mask == src.AspectMask());

    std::ranges::transform(copies, vk_copies.begin(), [aspect_mask](const auto& copy) {
        return MakeImageCopy(copy, aspect_mask);
    });
    const VkImage dst_image = dst.Handle();
    const VkImage src_image = src.Handle();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, aspect_mask, vk_copies](vk::CommandBuffer cmdbuf) {
        RangedBarrierRange dst_range;
        RangedBarrierRange src_range;
        for (const VkImageCopy& copy : vk_copies) {
            dst_range.AddLayers(copy.dstSubresource);
            src_range.AddLayers(copy.srcSubresource);
        }
        const std::array pre_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(aspect_mask),
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(aspect_mask),
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = 0,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_range.SubresourceRange(aspect_mask),
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_range.SubresourceRange(aspect_mask),
            },
        };
        cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, nullptr, nullptr, pre_barriers);
        cmdbuf.CopyImage(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk_copies);
        cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, nullptr, nullptr, post_barriers);
    });
}

void TextureCacheRuntime::CopyImageMSAA(Image& dst, Image& src,
                                        std::span<const VideoCommon::ImageCopy> copies) {
    dst.PrepareSparseWrite();
    const bool msaa_to_non_msaa = src.info.num_samples > 1 && dst.info.num_samples == 1;
    const u32 num_samples = msaa_to_non_msaa ? src.info.num_samples : dst.info.num_samples;
    if (ENABLE_MSAA_RESOLVE_CONSUME && msaa_to_non_msaa && copies.size() == 1 &&
        src.info.format == dst.info.format) {
        const VideoCommon::ImageCopy& copy = copies.front();
        const ResolveShadow* const shadow = GetValidResolveShadow(src.Handle());
        if (shadow != nullptr && shadow->aspect_mask == dst.AspectMask() &&
            copy.src_offset.x == 0 && copy.src_offset.y == 0 &&
            copy.src_subresource.base_level == 0 &&
            static_cast<u32>(copy.extent.width) <= shadow->extent.width &&
            static_cast<u32>(copy.extent.height) <= shadow->extent.height &&
            static_cast<u32>(copy.src_subresource.base_layer + copy.src_subresource.num_layers) <=
                shadow->layers) {
            const VkImageAspectFlags aspect_mask = shadow->aspect_mask;
            VkPipelineStageFlags attachment_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkAccessFlags attachment_write = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkAccessFlags attachment_read_write =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
                attachment_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                attachment_write = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                attachment_read_write = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            }
            const VkImage shadow_image = *shadow->image;
            const VkImage dst_image = dst.Handle();
            const VkImageCopy region{
                .srcSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = 0,
                    .baseArrayLayer = static_cast<u32>(copy.src_subresource.base_layer),
                    .layerCount = static_cast<u32>(copy.src_subresource.num_layers),
                },
                .srcOffset = {0, 0, 0},
                .dstSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(copy.dst_subresource.base_level),
                    .baseArrayLayer = static_cast<u32>(copy.dst_subresource.base_layer),
                    .layerCount = static_cast<u32>(copy.dst_subresource.num_layers),
                },
                .dstOffset = {copy.dst_offset.x, copy.dst_offset.y, copy.dst_offset.z},
                .extent = {copy.extent.width, copy.extent.height, 1},
            };
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([shadow_image, dst_image, region, aspect_mask, attachment_stage,
                              attachment_write,
                              attachment_read_write](vk::CommandBuffer cmdbuf) {
                const std::array pre_barriers{
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = attachment_write,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = shadow_image,
                        .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                          VK_REMAINING_ARRAY_LAYERS},
                    },
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | attachment_write |
                                         VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = dst_image,
                        .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                          VK_REMAINING_ARRAY_LAYERS},
                    },
                };
                const std::array post_barriers{
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = 0,
                        .dstAccessMask = 0,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = shadow_image,
                        .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                          VK_REMAINING_ARRAY_LAYERS},
                    },
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | attachment_read_write |
                                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = dst_image,
                        .subresourceRange{aspect_mask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                          VK_REMAINING_ARRAY_LAYERS},
                    },
                };
                cmdbuf.PipelineBarrier(attachment_stage | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, nullptr,
                                       pre_barriers);
                cmdbuf.CopyImage(shadow_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, nullptr,
                                       post_barriers);
            });
            return;
        }
    }
    const VkImageAspectFlags dst_aspect_mask = dst.AspectMask();
    if ((dst_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
        const bool copies_stencil = (dst_aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                                    device.IsExtShaderStencilExportSupported();
        blit_image_helper.CopyMSAADepth(render_pass_cache, dst.Handle(), dst.info.format,
                                        src.Handle(), src.info.format, num_samples, copies,
                                        copies_stencil, msaa_to_non_msaa);
        return;
    }
    if ((dst_aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
        UNIMPLEMENTED_MSG("Copying images with different samples is not supported.");
        return;
    }
    blit_image_helper.CopyMSAA(render_pass_cache, dst.Handle(), dst.info.format, src.Handle(),
                               src.info.format, num_samples, copies, msaa_to_non_msaa);
}

u64 TextureCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 TextureCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool TextureCacheRuntime::CanDownloadMsaa(const VideoCommon::ImageInfo& info) const {
    const VkImageAspectFlags aspect_mask = ImageAspectMask(info.format);
    if ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        return true;
    }
    if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0) {
        return false;
    }
    if ((aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
        return device.IsExtShaderStencilExportSupported();
    }
    return true;
}

bool TextureCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

void TextureCacheRuntime::FlushDeferredClear() {
    scheduler.FlushDeferredClear();
}

VkImage TextureCacheRuntime::AcquireMsaaScratchImage(const VkImageCreateInfo& image_ci) {
    const MsaaScratchKey key{
        .format = image_ci.format,
        .type = image_ci.imageType,
        .width = image_ci.extent.width,
        .height = image_ci.extent.height,
        .depth = image_ci.extent.depth,
        .levels = image_ci.mipLevels,
        .layers = image_ci.arrayLayers,
        .usage = image_ci.usage,
        .flags = image_ci.flags,
    };
    for (MsaaScratchImage& scratch : msaa_scratch_images) {
        if (scratch.key != key || !scheduler.IsFree(scratch.tick)) {
            continue;
        }
        scratch.tick = (std::numeric_limits<u64>::max)();
        scratch.unused_frames = 0;
        return *scratch.image;
    }
    MsaaScratchImage scratch{
        .key = key,
        .image = memory_allocator.CreateImage(image_ci),
        .tick = (std::numeric_limits<u64>::max)(),
        .unused_frames = 0,
    };
    const VkImage handle = *scratch.image;
    msaa_scratch_images.push_back(std::move(scratch));
    return handle;
}

void TextureCacheRuntime::ReleaseMsaaScratchImage(VkImage image) {
    for (MsaaScratchImage& scratch : msaa_scratch_images) {
        if (*scratch.image == image) {
            scratch.tick = scheduler.CurrentTick();
            return;
        }
    }
}

void TextureCacheRuntime::TickFrame() {
    sparse_tiles.Tick(scheduler.GetMasterSemaphore().KnownGpuTick());
    static constexpr u32 MAX_UNUSED_SCRATCH_FRAMES = 60;
    std::erase_if(msaa_scratch_images, [this](MsaaScratchImage& scratch) {
        if (!scheduler.IsFree(scratch.tick)) {
            scratch.unused_frames = 0;
            return false;
        }
        return ++scratch.unused_frames > MAX_UNUSED_SCRATCH_FRAMES;
    });
    std::erase_if(pending_resolve_shadows, [this](const auto& pending) {
        return scheduler.IsFree(pending.first);
    });
    sentenced_unswizzle_buffers.Tick();
}

// Is this even needed? I wasn't sure if the buffer was cleared when done with,
// someone who knows how these systems works better than me could probably explain
void TextureCacheRuntime::ReleaseSparseUnswizzleBuffer(Image& image) {
    if (image.has_compute_unswizzle_buffer) {
        sentenced_unswizzle_buffers.Push(std::move(image.compute_unswizzle_buffer));
        image.has_compute_unswizzle_buffer = false;
        image.compute_unswizzle_buffer_size = 0;
        image.compute_unswizzle_buffer_is_zero = false;
    }
}

Image::Image(TextureCacheRuntime& runtime_, const ImageInfo& info_, GPUVAddr gpu_addr_,
             VAddr cpu_addr_)
    : VideoCommon::ImageBase(info_, gpu_addr_, cpu_addr_), scheduler{&runtime_.scheduler},
      runtime{&runtime_},
      aspect_mask(ImageAspectMask(info.format)) {
    if (IsPixelFormatASTC(info.format) && !runtime->device.IsOptimalAstcSupported()) {
        switch (Settings::values.accelerate_astc.GetValue()) {
        case Settings::AstcDecodeMode::Gpu:
            if (WillUseAcceleratedAstcDecode(runtime->device, info)) {
                flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
            }
            break;
        case Settings::AstcDecodeMode::CpuAsynchronous:
            flags |= VideoCommon::ImageFlagBits::AsynchronousDecode;
            break;
        default:
            break;
        }
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    if (IsPixelFormatBCn(info.format) && !runtime->device.IsOptimalBcnSupported()) {
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    if (IsPixelFormatBCn(info.format) && info.type == ImageType::e3D && info.resources.levels == 1 && info.resources.layers == 1 && info.is_sparse) {
        flags |= VideoCommon::ImageFlagBits::SparseUpload;
    }
    // The flags above decide whether the image can be sparse resident.
    if (!TryCreateSparseResident()) {
        original_image = MakeImage(runtime->device, runtime->memory_allocator, info,
                                   runtime->ViewFormats(info.format));
    }
    if (runtime->device.HasDebuggingToolAttached()) {
        original_image.SetObjectNameEXT(VideoCommon::Name(*this).c_str());
    }
    current_image = &Image::original_image;
    storage_image_views.resize(info.resources.levels);
    if (UsesAstcStorageViews(runtime->device, info)) {
        const auto& device = runtime->device.GetLogical();
        const VkFormat storage_format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        for (s32 level = 0; level < info.resources.levels; ++level) {
            storage_image_views[level] =
                MakeStorageView(device, level, *original_image, storage_format);
        }
    }
}

SparseTilePool::SparseTilePool(const Device& device_) : device{device_} {}

SparseTilePool::~SparseTilePool() {
    const auto& dld = device.GetDispatchLoader();
    for (const Chunk& chunk : chunks) {
        if (chunk.buffer) {
            dld.vkDestroyBuffer(*device.GetLogical(), chunk.buffer, nullptr);
        }
    }
}

std::optional<SparseTilePool::Tile> SparseTilePool::Allocate(u32 memory_type) {
    std::scoped_lock lock{mutex};
    for (u32 index = 0; index < chunks.size(); ++index) {
        Chunk& chunk = chunks[index];
        if (chunk.memory_type == memory_type && !chunk.free_slots.empty()) {
            const u32 slot = chunk.free_slots.back();
            chunk.free_slots.pop_back();
            return Tile{.memory = *chunk.memory, .offset = slot * TileSize, .chunk = index};
        }
    }
    Chunk chunk{.memory_type = memory_type};
    try {
        chunk.memory = device.GetLogical().AllocateMemory(VkMemoryAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = TileSize * ChunkTiles,
            .memoryTypeIndex = memory_type,
        });
    } catch (const vk::Exception& exception) {
        LOG_ERROR(Render_Vulkan, "Sparse tile memory allocation failed: {}", exception.what());
        return std::nullopt;
    }
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = TileSize * ChunkTiles,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    const auto& dld = device.GetDispatchLoader();
    if (dld.vkCreateBuffer(*device.GetLogical(), &buffer_ci, nullptr, &chunk.buffer) ==
        VK_SUCCESS) {
        const VkMemoryRequirements requirements =
            device.GetLogical().GetBufferMemoryRequirements(chunk.buffer);
        if ((requirements.memoryTypeBits & (1U << memory_type)) != 0 &&
            requirements.size <= TileSize * ChunkTiles) {
            vk::Check(dld.vkBindBufferMemory(*device.GetLogical(), chunk.buffer, *chunk.memory, 0));
        } else {
            dld.vkDestroyBuffer(*device.GetLogical(), chunk.buffer, nullptr);
            chunk.buffer = VK_NULL_HANDLE;
        }
    } else {
        chunk.buffer = VK_NULL_HANDLE;
    }
    chunk.free_slots.reserve(ChunkTiles);
    for (u32 slot = ChunkTiles; slot-- > 1;) {
        chunk.free_slots.push_back(slot);
    }
    const u32 index = static_cast<u32>(chunks.size());
    const VkDeviceMemory memory = *chunk.memory;
    chunks.push_back(std::move(chunk));
    return Tile{.memory = memory, .offset = 0, .chunk = index};
}

std::optional<SparseTilePool::Tile> SparseTilePool::ZeroRegion(u32 memory_type, u32 tiles,
                                                              Scheduler& scheduler) {
    std::scoped_lock lock{mutex};
    for (const ZeroRegionEntry& entry : zero_regions) {
        if (entry.memory_type == memory_type && entry.tiles >= tiles) {
            return Tile{.memory = *entry.memory, .offset = 0, .chunk = ~0U};
        }
    }
    const VkDeviceSize size = TileSize * tiles;
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    // Zero it on the GPU through a buffer over the same memory.
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkBuffer buffer{};
    if (dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &buffer) != VK_SUCCESS) {
        return std::nullopt;
    }
    const VkMemoryRequirements requirements =
        device.GetLogical().GetBufferMemoryRequirements(buffer);
    if ((requirements.memoryTypeBits & (1U << memory_type)) == 0 || requirements.size > size) {
        dld.vkDestroyBuffer(logical, buffer, nullptr);
        return std::nullopt;
    }
    vk::DeviceMemory memory;
    try {
        memory = device.GetLogical().AllocateMemory(VkMemoryAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = size,
            .memoryTypeIndex = memory_type,
        });
    } catch (const vk::Exception&) {
        dld.vkDestroyBuffer(logical, buffer, nullptr);
        return std::nullopt;
    }
    vk::Check(dld.vkBindBufferMemory(logical, buffer, *memory, 0));
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer, size](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, size, 0);
    });
    scheduler.Finish();
    dld.vkDestroyBuffer(logical, buffer, nullptr);
    const VkDeviceMemory handle = *memory;
    // Smaller regions stay alive: images created earlier are bound to them.
    zero_regions.push_back(ZeroRegionEntry{
        .memory_type = memory_type, .tiles = tiles, .memory = std::move(memory)});
    return Tile{.memory = handle, .offset = 0, .chunk = ~0U};
}

void SparseTilePool::Free(const Tile& tile, u64 tick) {
    std::scoped_lock lock{mutex};
    pending_free.emplace_back(tile, tick);
}

void SparseTilePool::RecordZeroFill(const Tile& tile, Scheduler& scheduler) {
    VkBuffer buffer{};
    {
        std::scoped_lock lock{mutex};
        buffer = tile.chunk < chunks.size() ? chunks[tile.chunk].buffer : VK_NULL_HANDLE;
    }
    if (!buffer) {
        return;
    }
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    scheduler.RecordWithUploadBuffer(
        [buffer, offset = tile.offset, barrier](vk::CommandBuffer, vk::CommandBuffer upload) {
            upload.FillBuffer(buffer, offset, TileSize, 0);
            upload.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier);
        });
}

void SparseTilePool::Tick(u64 completed_tick) {
    std::scoped_lock lock{mutex};
    while (!pending_free.empty() && pending_free.front().second <= completed_tick) {
        const Tile& tile = pending_free.front().first;
        chunks[tile.chunk].free_slots.push_back(static_cast<u32>(tile.offset / TileSize));
        pending_free.pop_front();
    }
}

namespace {
// Sparse residency is decided per image. Each distinct outcome (reason, format, type, whether the
// image has mips or layers) is logged its 1st, 10th, 100th... time, so a tester's log shows where
// residency applies and how often, without a line per texture.
class SparseResidencyLog {
public:
    /// Returns how many times this outcome has happened when it should be logged now, else 0.
    u64 Count(const std::string& key) {
        std::scoped_lock lock{mutex};
        auto it = counts.find(key);
        if (it == counts.end()) {
            if (counts.size() >= MaxKeys) {
                return 0;
            }
            it = counts.emplace(key, 0).first;
        }
        const u64 count = ++it->second;
        u64 power = 1;
        while (power < count) {
            power *= 10;
        }
        return power == count ? count : 0;
    }

private:
    static constexpr size_t MaxKeys = 256;
    std::mutex mutex;
    std::unordered_map<std::string, u64> counts;
};

SparseResidencyLog& GetSparseResidencyLog() {
    static SparseResidencyLog log;
    return log;
}

template <typename DetailFn>
void ReportSparse(const ImageInfo& info, GPUVAddr gpu_addr, std::string_view outcome,
                  DetailFn&& detail) {
    const std::string key =
        fmt::format("{}|{}|{}|{}|{}", outcome, info.format, info.type, info.resources.levels > 1,
                    info.resources.layers > 1);
    const u64 count = GetSparseResidencyLog().Count(key);
    if (count == 0) {
        return;
    }
    // The detail can be costly (it may query the driver), so only the first occurrence has one.
    const std::string text = count == 1 ? detail() : std::string{};
    LOG_INFO(Render_Vulkan,
             "Sparse texture {} (x{}): {} {} {}x{}x{} levels={} layers={} block={},{},{} "
             "spacing={} at {:#x}{}{}",
             outcome, count, info.format, info.type, info.size.width, info.size.height,
             info.size.depth, info.resources.levels, info.resources.layers, info.block.width,
             info.block.height, info.block.depth, info.tile_width_spacing, gpu_addr,
             text.empty() ? "" : ": ", text);
}

/// Logs once what the GPU supports for sparse residency, and why it can't be used if it can't.
void LogSparseDeviceOnce(const Device& device) {
    static std::once_flag once;
    std::call_once(once, [&device] {
        const auto& properties = device.GetSparseProperties();
        LOG_INFO(Render_Vulkan,
                 "Sparse residency support: binding={} image2D={} image3D={} aliased={} "
                 "nonResidentStrict={} standardShape2D={} standardShape3D={}",
                 device.IsSparseBindingSupported(), device.IsSparseResidencyImageSupported(false),
                 device.IsSparseResidencyImageSupported(true),
                 device.IsSparseResidencyAliasedSupported(),
                 device.IsSparseResidencyNonResidentStrict(),
                 properties.residencyStandard2DBlockShape != VK_FALSE,
                 properties.residencyStandard3DBlockShape != VK_FALSE);
        std::string problems;
        const auto add = [&problems](std::string_view problem) {
            problems += problems.empty() ? "" : "; ";
            problems += problem;
        };
        if (!device.IsSparseBindingSupported()) {
            add("no sparse binding on the graphics queue");
        } else {
            if (!device.IsSparseResidencyImageSupported(false)) {
                add("no sparse residency for 2D images");
            }
            if (!device.IsSparseResidencyImageSupported(true)) {
                add("no sparse residency for 3D images");
            }
        }
        if (!device.IsSparseResidencyAliasedSupported() &&
            !device.IsSparseResidencyNonResidentStrict()) {
            add("neither aliased binding nor zero reads from unbound regions");
        }
        if (properties.residencyStandard2DBlockShape == VK_FALSE) {
            add("non-standard 2D sparse tile shapes");
        }
        if (properties.residencyStandard3DBlockShape == VK_FALSE) {
            add("non-standard 3D sparse tile shapes");
        }
        if (!problems.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "Sparse textures will use regular images where affected: {}", problems);
        }
    });
}

/// For a sparse image with mip levels or array layers: whether its guest pages would map onto
/// the host's sparse tiles, level by level, with the host's mip tail and the layer stride.
std::string AnalyzeSparseMipsLayers(const Device& device, const ImageInfo& info,
                                    const VkImageCreateInfo& image_ci) {
    constexpr u64 page_size = SparseTilePool::TileSize;
    const auto format_properties = device.GetPhysical().GetSparseImageFormatProperties(
        image_ci.format, image_ci.imageType, image_ci.samples, image_ci.usage, image_ci.tiling);
    if (format_properties.size() != 1) {
        return fmt::format("host lists {} sparse formats", format_properties.size());
    }
    const VkExtent3D granularity = format_properties[0].imageGranularity;
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    VkImage handle{};
    if (dld.vkCreateImage(logical, &image_ci, nullptr, &handle) != VK_SUCCESS) {
        return "host can't create it as a sparse image";
    }
    u32 num_requirements = 0;
    dld.vkGetImageSparseMemoryRequirements(logical, handle, &num_requirements, nullptr);
    std::vector<VkSparseImageMemoryRequirements> requirements(num_requirements);
    dld.vkGetImageSparseMemoryRequirements(logical, handle, &num_requirements,
                                           requirements.data());
    dld.vkDestroyImage(logical, handle, nullptr);
    if (requirements.size() != 1) {
        return fmt::format("host lists {} sparse memory requirements", requirements.size());
    }
    const VkSparseImageMemoryRequirements& host = requirements[0];
    const u32 levels = static_cast<u32>(info.resources.levels);
    const u32 tail_level = (std::min)(host.imageMipTailFirstLod, levels);
    bool fits = true;
    std::string guest;
    for (u32 level = 0; level < tail_level; ++level) {
        const auto layout = VideoCommon::ComputeSparseLevelLayout(info, level, page_size);
        const bool match = layout && layout->page_texels.width == granularity.width &&
                           layout->page_texels.height == granularity.height &&
                           layout->page_texels.depth == granularity.depth;
        fits = fits && match;
        if (match) {
            guest += fmt::format(" L{}:ok", level);
        } else if (layout) {
            guest += fmt::format(" L{}:{}x{}x{}", level, layout->page_texels.width,
                                 layout->page_texels.height, layout->page_texels.depth);
        } else {
            guest += fmt::format(" L{}:unaligned", level);
        }
    }
    if (tail_level < levels) {
        const u64 tail_offset = VideoCommon::CalculateMipLevelOffsets(info)[tail_level];
        const bool aligned = tail_offset % page_size == 0;
        fits = fits && aligned;
        guest += fmt::format(" tail@{:#x}{}", tail_offset, aligned ? "" : "(unaligned)");
    }
    if (info.resources.layers > 1) {
        const u64 stride = VideoCommon::CalculateLayerStride(info);
        const bool aligned = stride % page_size == 0;
        fits = fits && aligned;
        guest += fmt::format(" layer_stride={:#x}{}", stride, aligned ? "" : "(unaligned)");
    }
    return fmt::format("would {}fit; host tile {}x{}x{}{}, host tail from level {} ({:#x} "
                       "bytes, stride {:#x}{}); guest:{}",
                       fits ? "" : "not ", granularity.width, granularity.height,
                       granularity.depth,
                       (format_properties[0].flags &
                        VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT) != 0
                           ? " (non-standard)"
                           : "",
                       host.imageMipTailFirstLod, host.imageMipTailSize,
                       host.imageMipTailStride,
                       (host.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) !=
                               0
                           ? ", single tail"
                           : "",
                       guest);
}
} // Anonymous namespace

bool Image::TryCreateSparseResident() {
    // eden-kledd: part of Nomad's sparse texture fixes (lab box, takes effect for new textures)
    if (!info.is_sparse || !Settings::values.kledd_sparse_fixes.GetValue()) {
        return false;
    }
    const Device& device = runtime->device;
    LogSparseDeviceOnce(device);
    const auto no_detail = [] { return std::string{}; };
    const auto reject = [this](std::string_view reason, auto&& detail) {
        ReportSparse(info, gpu_addr, reason, detail);
        return false;
    };
    const bool is_3d = info.type == ImageType::e3D;
    // Compressed formats only: they can't be render targets, so the GPU writes them only through
    // copies and reinterpreting views, which PrepareSparseWrite guards.
    if (info.type != ImageType::e2D && !is_3d) {
        return reject("not resident: not 2D or 3D", no_detail);
    }
    if (info.rescaleable) {
        return reject("not resident: resolution-scaled", no_detail);
    }
    if (VideoCore::Surface::DefaultBlockWidth(info.format) == 1) {
        return reject("not resident: uncompressed", no_detail);
    }
    if (True(flags & (VideoCommon::ImageFlagBits::Converted |
                      VideoCommon::ImageFlagBits::AcceleratedUpload |
                      VideoCommon::ImageFlagBits::AsynchronousDecode))) {
        return reject("not resident: converted or decoded on upload", no_detail);
    }
    if (!device.IsSparseResidencyImageSupported(is_3d)) {
        return reject("not resident: GPU lacks residency for this type", no_detail);
    }
    // Pages without data must read as zero. Unbound regions don't on every driver that says
    // so (RADV, 3D BC images on RDNA3), so they're bound to a shared zero tile where aliasing is
    // supported, and left unbound only where it isn't but the device promises zeros.
    const bool use_zero_tile = device.IsSparseResidencyAliasedSupported();
    if (!use_zero_tile && !device.IsSparseResidencyNonResidentStrict()) {
        return reject("not resident: GPU can't make unbound pages read zero", no_detail);
    }
    VkImageCreateInfo image_ci = MakeImageCreateInfo(device, info, std::nullopt);
    // Sparse images can't be 2D-array or cube compatible.
    image_ci.flags &= ~(VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT |
                        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    image_ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (use_zero_tile) {
        image_ci.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    }
    const auto layout = VideoCommon::ComputeSparsePageLayout(info, SparseTilePool::TileSize);
    if (!layout) {
        if (info.num_samples != 1) {
            return reject("not resident: multisampled", no_detail);
        }
        if (info.resources.levels > 1 || info.resources.layers > 1) {
            return reject("not resident: has mip levels or layers (unsupported yet)",
                          [&] { return AnalyzeSparseMipsLayers(device, info, image_ci); });
        }
        return reject("not resident: guest pages aren't whole texel boxes", no_detail);
    }
    const std::span<const VkFormat> view_formats = runtime->ViewFormats(info.format);
    const VkImageFormatListCreateInfo image_format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (view_formats.size() > 1) {
        image_ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (device.IsKhrImageFormatListSupported()) {
            image_ci.pNext = &image_format_list;
        }
    }
    // The device's sparse block must be exactly the box one guest page covers.
    const auto format_properties = device.GetPhysical().GetSparseImageFormatProperties(
        image_ci.format, image_ci.imageType, image_ci.samples, image_ci.usage, image_ci.tiling);
    if (format_properties.size() != 1) {
        return reject("not resident: host has no single sparse format", [&] {
            return fmt::format("{} entries", format_properties.size());
        });
    }
    if (format_properties[0].imageGranularity.width != layout->page_texels.width ||
        format_properties[0].imageGranularity.height != layout->page_texels.height ||
        format_properties[0].imageGranularity.depth != layout->page_texels.depth) {
        return reject("not resident: host tile shape differs", [&] {
            const VkExtent3D host = format_properties[0].imageGranularity;
            return fmt::format("host {}x{}x{}{}, guest page {}x{}x{}", host.width, host.height,
                               host.depth,
                               (format_properties[0].flags &
                                VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT) != 0
                                   ? " (non-standard)"
                                   : "",
                               layout->page_texels.width, layout->page_texels.height,
                               layout->page_texels.depth);
        });
    }
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    VkImage handle{};
    if (dld.vkCreateImage(logical, &image_ci, nullptr, &handle) != VK_SUCCESS) {
        return reject("not resident: host can't create the sparse image", no_detail);
    }
    VkMemoryRequirements requirements{};
    dld.vkGetImageMemoryRequirements(logical, handle, &requirements);
    u32 num_sparse_requirements = 0;
    dld.vkGetImageSparseMemoryRequirements(logical, handle, &num_sparse_requirements, nullptr);
    std::vector<VkSparseImageMemoryRequirements> sparse_requirements(num_sparse_requirements);
    dld.vkGetImageSparseMemoryRequirements(logical, handle, &num_sparse_requirements,
                                           sparse_requirements.data());
    // Level 0 must not be part of a mip tail (which needs opaque binds).
    const bool level0_in_tail = std::ranges::any_of(
        sparse_requirements, [](const VkSparseImageMemoryRequirements& requirement) {
            return requirement.imageMipTailFirstLod == 0;
        });
    const auto memory_properties = device.GetPhysical().GetMemoryProperties().memoryProperties;
    std::optional<u32> memory_type;
    for (u32 type = 0; type < memory_properties.memoryTypeCount; ++type) {
        if ((requirements.memoryTypeBits & (1U << type)) != 0 &&
            (memory_properties.memoryTypes[type].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            memory_type = type;
            break;
        }
    }
    if (num_sparse_requirements == 0 || level0_in_tail || !memory_type ||
        SparseTilePool::TileSize % requirements.alignment != 0) {
        dld.vkDestroyImage(logical, handle, nullptr);
        return reject("not resident: host memory requirements don't fit", [&] {
            return fmt::format("sparse requirements={} level0_in_tail={} device_local={} "
                               "alignment={:#x}",
                               num_sparse_requirements, level0_in_tail, memory_type.has_value(),
                               requirements.alignment);
        });
    }
    original_image = vk::Image(handle, image_ci.usage, logical, runtime->memory_allocator.Handle(),
                               VK_NULL_HANDLE, dld);
    sparse = std::make_unique<SparseState>();
    sparse->layout = *layout;
    sparse->memory_type = *memory_type;
    sparse->tiles.resize(layout->NumPages());
    if (use_zero_tile) {
        // Zeros are bound in the largest boxes a zero region of at most MaxZeroTiles covers.
        constexpr u32 MaxZeroTiles = 256; // 16 MiB
        const u32 slab_pages = layout->pages_x * layout->pages_y;
        sparse->zero_box_pages = slab_pages <= MaxZeroTiles      ? slab_pages
                                 : layout->pages_x <= MaxZeroTiles ? layout->pages_x
                                                                   : 1;
        sparse->zero_tile =
            runtime->sparse_tiles.ZeroRegion(*memory_type, sparse->zero_box_pages, *scheduler);
        if (!sparse->zero_tile && !device.IsSparseResidencyNonResidentStrict()) {
            // No zero tile and no promise about unbound reads: use a regular image.
            sparse.reset();
            original_image.reset();
            return reject("not resident: no zero tile memory", no_detail);
        }
    }
    ReportSparse(info, gpu_addr, "resident", [&] {
        // The general per-level layout should agree with the one in use; a mismatch would need
        // fixing before mips and layers can rely on it.
        const auto level0 =
            VideoCommon::ComputeSparseLevelLayout(info, 0, SparseTilePool::TileSize);
        const bool agrees = level0 && level0->page_texels.width == layout->page_texels.width &&
                            level0->page_texels.height == layout->page_texels.height &&
                            level0->page_texels.depth == layout->page_texels.depth &&
                            level0->NumPages() == layout->NumPages();
        return fmt::format("{} pages of {}x{}x{} texels{}", layout->NumPages(),
                           layout->page_texels.width, layout->page_texels.height,
                           layout->page_texels.depth,
                           agrees ? "" : " (per-level layout disagrees)");
    });
    return true;
}

void Image::UpdateResidency(std::span<const u32> bind, std::span<const u32> unbind) {
    const auto& layout = sparse->layout;
    const auto make_bind = [&](u32 page, VkDeviceMemory memory, VkDeviceSize offset) {
        const VideoCommon::Offset3D origin = layout.PageOffset(page);
        return VkSparseImageMemoryBind{
            .subresource = {.aspectMask = aspect_mask, .mipLevel = 0, .arrayLayer = 0},
            .offset = {origin.x, origin.y, origin.z},
            .extent =
                {
                    (std::min)(layout.page_texels.width, info.size.width - origin.x),
                    (std::min)(layout.page_texels.height, info.size.height - origin.y),
                    (std::min)(layout.page_texels.depth,
                               (info.type == ImageType::e3D ? info.size.depth : 1U) - origin.z),
                },
            .memory = memory,
            .memoryOffset = offset,
            .flags = 0,
        };
    };
    const auto in_image = [&](u32 page) {
        const VideoCommon::Offset3D origin = layout.PageOffset(page);
        const u32 depth = info.type == ImageType::e3D ? info.size.depth : 1U;
        return page < sparse->tiles.size() && static_cast<u32>(origin.x) < info.size.width &&
               static_cast<u32>(origin.y) < info.size.height && static_cast<u32>(origin.z) < depth;
    };
    std::vector<VkSparseImageMemoryBind> binds;
    const u64 free_tick = scheduler->CurrentTick();
    for (const u32 page : unbind) {
        if (!in_image(page) || !sparse->tiles[page]) {
            continue;
        }
        if (sparse->materialized) {
            // Keeps its tile, which reads as zero like an unmapped page.
            runtime->sparse_tiles.RecordZeroFill(*sparse->tiles[page], *scheduler);
            continue;
        }
        binds.push_back(sparse->zero_tile
                            ? make_bind(page, sparse->zero_tile->memory, 0)
                            : make_bind(page, VK_NULL_HANDLE, 0));
        runtime->sparse_tiles.Free(*sparse->tiles[page], free_tick);
        sparse->tiles[page].reset();
    }
    if (sparse->zero_tile && !sparse->zero_bound) {
        // First update: zeros everywhere, in big boxes, before the tiles go over them.
        sparse->zero_bound = true;
        const u32 width = info.size.width;
        const u32 height = info.size.height;
        const u32 depth = info.type == ImageType::e3D ? info.size.depth : 1U;
        const u32 box_w = sparse->zero_box_pages >= layout.pages_x ? width : layout.page_texels.width;
        const u32 box_h = sparse->zero_box_pages >= layout.pages_x * layout.pages_y
                              ? height
                              : layout.page_texels.height;
        const u32 box_d = layout.page_texels.depth;
        std::vector<VkSparseImageMemoryBind> zero_binds;
        for (u32 z = 0; z < depth; z += box_d) {
            for (u32 y = 0; y < height; y += box_h) {
                for (u32 x = 0; x < width; x += box_w) {
                    zero_binds.push_back(VkSparseImageMemoryBind{
                        .subresource = {.aspectMask = aspect_mask, .mipLevel = 0, .arrayLayer = 0},
                        .offset = {static_cast<s32>(x), static_cast<s32>(y), static_cast<s32>(z)},
                        .extent = {(std::min)(box_w, width - x), (std::min)(box_h, height - y),
                                   (std::min)(box_d, depth - z)},
                        .memory = sparse->zero_tile->memory,
                        .memoryOffset = 0,
                        .flags = 0,
                    });
                }
            }
        }
        SubmitSparseBinds(zero_binds);
    }
    for (const u32 page : bind) {
        if (!in_image(page) || sparse->tiles[page]) {
            continue;
        }
        const auto tile = runtime->sparse_tiles.Allocate(sparse->memory_type);
        if (!tile) {
            break; // out of memory: the remaining pages read as zero
        }
        binds.push_back(make_bind(page, tile->memory, tile->offset));
        sparse->tiles[page] = tile;
    }
    if (!binds.empty()) {
        SubmitSparseBinds(binds);
    }
}

u64 Image::ResidentBytes() const noexcept {
    if (!sparse) {
        return 0;
    }
    return SparseTilePool::TileSize *
           std::ranges::count_if(sparse->tiles, [](const auto& tile) { return tile.has_value(); });
}

void Image::UploadSparsePages(const StagingBufferRef& map,
                              std::span<const VideoCommon::BufferImageCopy> copies) {
    sparse->uploading_pages = true;
    UploadMemory(map, copies);
    sparse->uploading_pages = false;
}

void Image::PrepareSparseWrite() {
    if (!sparse || !sparse->zero_tile || sparse->materialized || sparse->uploading_pages) {
        return;
    }
    sparse->materialized = true;
    const auto& layout = sparse->layout;
    std::vector<VkSparseImageMemoryBind> binds;
    const u32 depth = info.type == ImageType::e3D ? info.size.depth : 1U;
    for (u32 page = 0; page < sparse->tiles.size(); ++page) {
        const VideoCommon::Offset3D origin = layout.PageOffset(page);
        if (sparse->tiles[page] || static_cast<u32>(origin.x) >= info.size.width ||
            static_cast<u32>(origin.y) >= info.size.height ||
            static_cast<u32>(origin.z) >= depth) {
            continue;
        }
        const auto tile = runtime->sparse_tiles.Allocate(sparse->memory_type);
        if (!tile) {
            LOG_ERROR(Render_Vulkan, "Out of memory for the tiles of a written sparse image");
            break;
        }
        runtime->sparse_tiles.RecordZeroFill(*tile, *scheduler);
        sparse->tiles[page] = tile;
        binds.push_back(VkSparseImageMemoryBind{
            .subresource = {.aspectMask = aspect_mask, .mipLevel = 0, .arrayLayer = 0},
            .offset = {origin.x, origin.y, origin.z},
            .extent = {(std::min)(layout.page_texels.width, info.size.width - origin.x),
                       (std::min)(layout.page_texels.height, info.size.height - origin.y),
                       (std::min)(layout.page_texels.depth, depth - origin.z)},
            .memory = tile->memory,
            .memoryOffset = tile->offset,
            .flags = 0,
        });
    }
    LOG_DEBUG(Render_Vulkan, "Sparse image {:#x} is written by the GPU: {} pages get own tiles",
              gpu_addr, binds.size());
    if (!binds.empty()) {
        SubmitSparseBinds(binds);
    }
}

void Image::SubmitSparseBinds(std::span<const VkSparseImageMemoryBind> binds) {
    // The work already submitted may still sample the image: an unbound tile would vanish under
    // it, and a newly bound one would show its old contents until the upload recorded next runs.
    // So the binds wait for that work, and the fence below for the binds.
    const VkSemaphore timeline = scheduler->GetMasterSemaphore().TimelineHandle();
    const u64 last_submitted = scheduler->CurrentTick() - 1;
    if (!timeline) {
        scheduler->Finish();
    }
    const bool wait_timeline = timeline && last_submitted > 0;
    const VkTimelineSemaphoreSubmitInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &last_submitted,
        .signalSemaphoreValueCount = 0,
        .pSignalSemaphoreValues = nullptr,
    };
    const VkSparseImageMemoryBindInfo image_bind{
        .image = *original_image,
        .bindCount = static_cast<u32>(binds.size()),
        .pBinds = binds.data(),
    };
    const VkBindSparseInfo bind_info{
        .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
        .pNext = wait_timeline ? &timeline_info : nullptr,
        .waitSemaphoreCount = wait_timeline ? 1U : 0U,
        .pWaitSemaphores = wait_timeline ? &timeline : nullptr,
        .bufferBindCount = 0,
        .pBufferBinds = nullptr,
        .imageOpaqueBindCount = 0,
        .pImageOpaqueBinds = nullptr,
        .imageBindCount = 1,
        .pImageBinds = &image_bind,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vk::Fence fence = runtime->device.GetLogical().CreateFence(VkFenceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    });
    VkResult result = VK_ERROR_UNKNOWN;
    {
        std::scoped_lock lock{scheduler->submit_mutex};
        result = runtime->device.GetGraphicsQueue().BindSparse(bind_info, *fence);
    }
    if (result == VK_SUCCESS) {
        fence.Wait();
    } else {
        LOG_ERROR(Render_Vulkan, "vkQueueBindSparse failed: {}", static_cast<int>(result));
    }
}

Image::Image(const VideoCommon::NullImageParams& params) : VideoCommon::ImageBase{params} {}

Image::~Image() {
    if (sparse && runtime != nullptr) {
        // Nothing submitted after now can use the image: its tiles are free once the GPU gets
        // past the current tick.
        const u64 tick = scheduler->CurrentTick();
        for (const auto& tile : sparse->tiles) {
            if (tile) {
                runtime->sparse_tiles.Free(*tile, tick);
            }
        }
    }
    if (ENABLE_MSAA_RESOLVE_CONSUME && runtime != nullptr) {
        if (original_image) {
            runtime->EraseResolveShadow(*original_image);
        }
        if (scaled_image) {
            runtime->EraseResolveShadow(*scaled_image);
        }
    }
}

void Image::AllocateComputeUnswizzleBuffer(u32 max_slices) {
    using VideoCore::Surface::BytesPerBlock;

    const u32 block_bytes  = BytesPerBlock(info.format); // 8 for BC1, 16 for BC6H
    const u32 block_width  = VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 block_height = VideoCore::Surface::DefaultBlockHeight(info.format);

    // BCn is 4x4x1 blocks
    const u32 blocks_x = (info.size.width  + block_width  - 1) / block_width;
    const u32 blocks_y = (info.size.height + block_height - 1) / block_height;
    const u32 blocks_z = (std::min)(max_slices, info.size.depth);

    const u64 block_count =
        static_cast<u64>(blocks_x) *
        static_cast<u64>(blocks_y) *
        static_cast<u64>(blocks_z);

    const VkDeviceSize required_size = block_count * block_bytes;
    if (has_compute_unswizzle_buffer && required_size <= compute_unswizzle_buffer_size) {
        return;
    }

    compute_unswizzle_buffer_size = required_size;

    VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = compute_unswizzle_buffer_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };

    compute_unswizzle_buffer =
        runtime->memory_allocator.CreateBuffer(ci, MemoryUsage::DeviceLocal);

    has_compute_unswizzle_buffer = true;
    compute_unswizzle_buffer_is_zero = false;
}

void Image::UploadMemory(VkBuffer buffer, VkDeviceSize offset,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    PrepareSparseWrite();
    // TODO: Move this to another API
    if (ENABLE_MSAA_RESOLVE_CONSUME && runtime != nullptr) {
        runtime->InvalidateResolveShadow(Handle());
    }
    const bool is_rescaled = True(flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ScaleDown(true);
    }

    const bool msaa_upload_is_depth = (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const bool wants_msaa_upload = info.num_samples > 1
        && ((aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) != 0 || msaa_upload_is_depth);

    if (wants_msaa_upload) {
        const bool msaa_upload_copies_stencil =
            msaa_upload_is_depth && (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
            runtime->device.IsExtShaderStencilExportSupported();
        VkImageAspectFlags upload_aspect_mask = aspect_mask;
        if (msaa_upload_is_depth) {
            upload_aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (msaa_upload_copies_stencil) {
                upload_aspect_mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }
        const VkImageCreateInfo image_ci = MakeMsaaScratchImageCreateInfo(
            runtime->device, info, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const VkImage temp_vk_image = runtime->AcquireMsaaScratchImage(image_ci);

        scheduler->RequestOutsideRenderPassOperationContext();
        auto vk_copies = TransformBufferImageCopies(copies, offset, upload_aspect_mask);
        const VkBuffer src_buffer = buffer;
        const VkImageAspectFlags vk_aspect_mask = upload_aspect_mask;

        scheduler->Record([src_buffer, temp_vk_image, vk_aspect_mask,
                           vk_copies](vk::CommandBuffer cmdbuf) {
            CopyBufferToImage(cmdbuf, src_buffer, temp_vk_image, vk_aspect_mask, false, vk_copies);
        });

        const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
        boost::container::small_vector<VideoCommon::ImageCopy, 16> image_copies;
        image_copies.reserve(copies.size());
        for (const auto& copy : copies) {
            VideoCommon::ImageCopy image_copy{};
            image_copy.src_offset = {0, 0, 0};
            image_copy.dst_offset = {copy.image_offset.x >> samples_x,
                                     copy.image_offset.y >> samples_y, copy.image_offset.z};
            image_copy.src_subresource = copy.image_subresource;
            image_copy.dst_subresource = copy.image_subresource;
            image_copy.extent = {copy.image_extent.width >> samples_x,
                                 copy.image_extent.height >> samples_y, copy.image_extent.depth};
            image_copies.push_back(image_copy);
        }

        if (msaa_upload_is_depth) {
            runtime->blit_image_helper.CopyMSAADepth(runtime->render_pass_cache, Handle(),
                                                     info.format, temp_vk_image, info.format,
                                                     info.num_samples,
                                                     {image_copies.data(), image_copies.size()},
                                                     msaa_upload_copies_stencil, false);
        } else {
            runtime->blit_image_helper.CopyMSAA(runtime->render_pass_cache, Handle(), info.format,
                                                temp_vk_image, info.format, info.num_samples,
                                                {image_copies.data(), image_copies.size()}, false);
        }
        initialized = true;
        runtime->ReleaseMsaaScratchImage(temp_vk_image);

        if (is_rescaled) {
            ScaleUp();
        }
        return;
    }

    if (info.num_samples > 1) {
        if (is_rescaled) {
            ScaleUp();
        }
        return;
    }

    scheduler->RequestOutsideRenderPassOperationContext();
    auto vk_copies = TransformBufferImageCopies(copies, offset, aspect_mask);
    const VkBuffer src_buffer = buffer;
    const VkImage vk_image = *original_image;
    const VkImageAspectFlags vk_aspect_mask = aspect_mask;
    const bool was_initialized = std::exchange(initialized, true);

    scheduler->Record([src_buffer, vk_image, vk_aspect_mask, was_initialized,
                       vk_copies](vk::CommandBuffer cmdbuf) {
        CopyBufferToImage(cmdbuf, src_buffer, vk_image, vk_aspect_mask, was_initialized, vk_copies);
    });

    if (is_rescaled) {
        ScaleUp();
    }
}

void Image::UploadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    UploadMemory(map.buffer, map.offset, copies);
}

void Image::DownloadMemory(VkBuffer buffer, size_t offset,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    std::array buffer_handles{
        buffer,
    };
    std::array buffer_offsets{
        offset,
    };
    DownloadMemory(buffer_handles, buffer_offsets, copies);
}

void Image::DownloadMemory(std::span<VkBuffer> buffers_span, std::span<size_t> offsets_span,
                            std::span<const VideoCommon::BufferImageCopy> copies) {
    const bool is_rescaled = True(flags & ImageFlagBits::Rescaled);
    // eden-kledd: split readbacks of a rescaled image copy straight from the scaled image (the
    // caller passes scaled coordinates and downsamples on the CPU). Scaling down first goes
    // through a helper blit on GPUs that can't blit depth-stencil (e.g. AMD with D32S8), and
    // that blit can drop the stencil, which holds the object IDs.
    const bool kledd_from_scaled = is_rescaled && kledd_copy_scaled && kledd_stencil_split != 0;
    if (is_rescaled && !kledd_from_scaled) {
        ScaleDown();
    }

    if (info.num_samples > 1) {
        if (runtime->CanDownloadMsaa(info)) {
            const bool msaa_download_is_depth =
                (aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
            const bool msaa_download_copies_stencil =
                msaa_download_is_depth && (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
                runtime->device.IsExtShaderStencilExportSupported();
            VkImageUsageFlags scratch_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (msaa_download_is_depth) {
                scratch_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            } else {
                scratch_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            const VkImageCreateInfo image_ci =
                MakeMsaaScratchImageCreateInfo(runtime->device, info, scratch_usage);
            const VkImage temp_vk_image = runtime->AcquireMsaaScratchImage(image_ci);

            const VkImageAspectFlags temp_aspect_mask = aspect_mask;
            const VkAccessFlags attachment_access =
                msaa_download_is_depth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                                       : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            const VkPipelineStageFlags attachment_stage =
                msaa_download_is_depth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                                       : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

            scheduler->RequestOutsideRenderPassOperationContext();
            scheduler->Record([temp_vk_image, temp_aspect_mask, attachment_access,
                               attachment_stage](vk::CommandBuffer cmdbuf) {
                const VkImageMemoryBarrier init_barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = attachment_access,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = temp_vk_image,
                    .subresourceRange{
                        .aspectMask = temp_aspect_mask,
                        .baseMipLevel = 0,
                        .levelCount = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, attachment_stage, 0,
                                       init_barrier);
            });

            boost::container::small_vector<VideoCommon::ImageCopy, 16> image_copies;
            image_copies.reserve(copies.size());
            for (const auto& copy : copies) {
                VideoCommon::ImageCopy image_copy;
                image_copy.src_offset = copy.image_offset;
                image_copy.dst_offset = copy.image_offset;
                image_copy.src_subresource = copy.image_subresource;
                image_copy.dst_subresource = copy.image_subresource;
                image_copy.extent = copy.image_extent;
                image_copies.push_back(image_copy);
            }

            if (msaa_download_is_depth) {
                runtime->blit_image_helper.CopyMSAADepth(
                    runtime->render_pass_cache, temp_vk_image, info.format, Handle(), info.format,
                    info.num_samples, {image_copies.data(), image_copies.size()},
                    msaa_download_copies_stencil, true);
            } else {
                runtime->blit_image_helper.CopyMSAA(runtime->render_pass_cache, temp_vk_image,
                                                    info.format, Handle(), info.format,
                                                    info.num_samples,
                                                    {image_copies.data(), image_copies.size()},
                                                    true);
            }

            boost::container::small_vector<VkBuffer, 8> buffers_vector{};
            boost::container::small_vector<boost::container::small_vector<VkBufferImageCopy, 16>, 8>
                vk_copies;
            for (size_t index = 0; index < buffers_span.size(); index++) {
                buffers_vector.emplace_back(buffers_span[index]);
                vk_copies.emplace_back(
                    TransformBufferImageCopies(copies, offsets_span[index], aspect_mask));
            }

            scheduler->RequestOutsideRenderPassOperationContext();
            scheduler->Record([buffers = std::move(buffers_vector), image = temp_vk_image,
                               aspect_mask_ = aspect_mask, vk_copies](vk::CommandBuffer cmdbuf) {
                const VkImageMemoryBarrier read_barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange{
                        .aspectMask = aspect_mask_,
                        .baseMipLevel = 0,
                        .levelCount = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
                cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, read_barrier);

                for (size_t index = 0; index < buffers.size(); index++) {
                    cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[index],
                                             vk_copies[index]);
                }

                const VkMemoryBarrier memory_write_barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                };
                const VkImageMemoryBarrier image_write_barrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image,
                    .subresourceRange{
                        .aspectMask = aspect_mask_,
                        .baseMipLevel = 0,
                        .levelCount = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                       0, memory_write_barrier, nullptr, image_write_barrier);
            });
            runtime->ReleaseMsaaScratchImage(temp_vk_image);
        }
        if (is_rescaled) {
            ScaleUp(true);
        }
        return;
    } else {
        boost::container::small_vector<VkBuffer, 8> buffers_vector{};
        boost::container::small_vector<boost::container::small_vector<VkBufferImageCopy, 16>, 8>
            vk_copies;
        const bool kledd_split =
            kledd_stencil_split != 0 &&
            aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        for (size_t index = 0; index < buffers_span.size(); index++) {
            buffers_vector.emplace_back(buffers_span[index]);
            if (kledd_split) {
                // eden-kledd: copying both aspects to the same buffer offset makes the regions
                // overlap (undefined in Vulkan). Put stencil in its own region instead.
                auto depth_copies = TransformBufferImageCopies(copies, offsets_span[index],
                                                               VK_IMAGE_ASPECT_DEPTH_BIT);
                auto stencil_copies = TransformBufferImageCopies(copies, offsets_span[index],
                                                                 VK_IMAGE_ASPECT_STENCIL_BIT);
                for (size_t i = 0; i < copies.size(); ++i) {
                    stencil_copies[i].bufferOffset =
                        offsets_span[index] + kledd_stencil_split + copies[i].buffer_offset / 4;
                }
                for (const auto& copy : stencil_copies) {
                    depth_copies.push_back(copy);
                }
                vk_copies.emplace_back(std::move(depth_copies));
            } else {
                vk_copies.emplace_back(
                    TransformBufferImageCopies(copies, offsets_span[index], aspect_mask));
            }
        }
        scheduler->RequestOutsideRenderPassOperationContext();
        const VkImage src_image = kledd_from_scaled ? *scaled_image : *original_image;
        scheduler->Record([buffers = std::move(buffers_vector), image = src_image,
                           aspect_mask_ = aspect_mask, vk_copies](vk::CommandBuffer cmdbuf) {
            const VkImageMemoryBarrier read_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = aspect_mask_,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, read_barrier);

            for (size_t index = 0; index < buffers.size(); index++) {
                cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[index],
                                         vk_copies[index]);
            }

            const VkMemoryBarrier memory_write_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            const VkImageMemoryBarrier image_write_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = aspect_mask_,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, memory_write_barrier, nullptr, image_write_barrier);
        });
    }

    if (is_rescaled && !kledd_from_scaled) {
        ScaleUp(true);
    }
}

bool Image::KleddCanSplitDepthStencil() const {
    if (info.num_samples != 1) {
        return false;
    }
    if (info.format != PixelFormat::S8_UINT_D24_UNORM &&
        info.format != PixelFormat::D24_UNORM_S8_UINT) {
        return false;
    }
    if (aspect_mask != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        return false;
    }
    const auto host = MaxwellToVK::SurfaceFormat(runtime->device, FormatType::Optimal, false,
                                                 info.format);
    // D32_SFLOAT_S8_UINT is what GPUs without D24S8 support (e.g. AMD) get instead.
    return host.format == VK_FORMAT_D24_UNORM_S8_UINT ||
           host.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool Image::KleddDepthIsFloat() const {
    const auto host = MaxwellToVK::SurfaceFormat(runtime->device, FormatType::Optimal, false,
                                                 info.format);
    return host.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

void Image::DownloadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    std::array buffers{
        map.buffer,
    };
    std::array offsets{
        static_cast<size_t>(map.offset),
    };
    DownloadMemory(buffers, offsets, copies);
}

VkImageView Image::StorageImageView(s32 level) noexcept {
    PrepareSparseWrite();
    auto& view = storage_image_views[level];
    if (!view) {
        auto format_info =
            MaxwellToVK::SurfaceFormat(runtime->device, FormatType::Optimal, true, info.format);
        if (UsesAstcStorageViews(runtime->device, info)) {
            format_info.format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        }
        view = MakeStorageView(runtime->device.GetLogical(), level, *(this->*current_image),
                               format_info.format);
    }
    return *view;
}

bool Image::IsRescaled() const noexcept {
    return True(flags & ImageFlagBits::Rescaled);
}

bool Image::ScaleUp(bool ignore) {
    const auto& resolution = runtime->resolution;
    if (!resolution.active) {
        return false;
    }
    if (True(flags & ImageFlagBits::Rescaled)) {
        return false;
    }
    ASSERT(info.type != ImageType::Linear);
    flags |= ImageFlagBits::Rescaled;
    has_scaled = true;
    if (!scaled_image) {
        const bool is_2d = info.type == ImageType::e2D;
        const u32 scaled_width = resolution.ScaleUp(info.size.width);
        const u32 scaled_height = is_2d ? resolution.ScaleUp(info.size.height) : info.size.height;
        auto scaled_info = info;
        scaled_info.size.width = scaled_width;
        scaled_info.size.height = scaled_height;
        scaled_image = MakeImage(runtime->device, runtime->memory_allocator, scaled_info,
                                 runtime->ViewFormats(info.format));
        ignore = false;
    }
    current_image = &Image::scaled_image;
    if (ignore) {
        return true;
    }
    if (aspect_mask == 0) {
        aspect_mask = ImageAspectMask(info.format);
    }
    if (NeedsScaleHelper()) {
        if (!BlitScaleHelper(true)) {
            current_image = &Image::original_image;
            return false;
        }
    } else {
        BlitScale(*scheduler, *original_image, *scaled_image, info, aspect_mask, resolution);
    }
    return true;
}

bool Image::ScaleDown(bool ignore) {
    const auto& resolution = runtime->resolution;
    if (!resolution.active) {
        return false;
    }
    if (False(flags & ImageFlagBits::Rescaled)) {
        return false;
    }
    ASSERT(info.type != ImageType::Linear);
    flags &= ~ImageFlagBits::Rescaled;
    current_image = &Image::original_image;
    if (ignore) {
        return true;
    }
    if (aspect_mask == 0) {
        aspect_mask = ImageAspectMask(info.format);
    }
    if (NeedsScaleHelper()) {
        if (!BlitScaleHelper(false)) {
            current_image = &Image::scaled_image;
            flags |= ImageFlagBits::Rescaled;
            return false;
        }
    } else {
        BlitScale(*scheduler, *scaled_image, *original_image, info, aspect_mask, resolution, false);
    }
    return true;
}

bool Image::BlitScaleHelper(bool scale_up) {
    using namespace VideoCommon;
    static constexpr auto BLIT_OPERATION = Tegra::Engines::Fermi2D::Operation::SrcCopy;
    const bool is_color{aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT};
    const bool is_bilinear{is_color && !IsPixelFormatInteger(info.format)};
    const auto operation = is_bilinear ? Tegra::Engines::Fermi2D::Filter::Bilinear
                                       : Tegra::Engines::Fermi2D::Filter::Point;

    const bool is_2d = info.type == ImageType::e2D;
    const auto& resolution = runtime->resolution;
    const u32 scaled_width = resolution.ScaleUp(info.size.width);
    const u32 scaled_height = is_2d ? resolution.ScaleUp(info.size.height) : info.size.height;
    std::unique_ptr<ImageView>& blit_view = scale_up ? scale_view : normal_view;
    std::optional<Framebuffer>& blit_framebuffer = scale_up ? scale_framebuffer : normal_framebuffer;
    if (!blit_view) {
        const auto view_info = ImageViewInfo(ImageViewType::e2D, info.format);
        blit_view = std::make_unique<ImageView>(*runtime, view_info, NULL_IMAGE_ID, *this);
    }

    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
    const u32 src_width = (scale_up ? info.size.width : scaled_width) >> samples_x;
    const u32 src_height = (scale_up ? info.size.height : scaled_height) >> samples_y;
    const u32 dst_width = (scale_up ? scaled_width : info.size.width) >> samples_x;
    const u32 dst_height = (scale_up ? scaled_height : info.size.height) >> samples_y;
    const Region2D src_region{
        .start = {0, 0},
        .end = {s32(src_width), s32(src_height)},
    };
    const Region2D dst_region{
        .start = {0, 0},
        .end = {s32(dst_width), s32(dst_height)},
    };
    const VkExtent2D extent{
        .width = (std::max)(scaled_width, info.size.width) >> samples_x,
        .height = (std::max)(scaled_height, info.size.height) >> samples_y,
    };

    auto* view_ptr = blit_view.get();
    if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
        if (!blit_framebuffer)
            blit_framebuffer.emplace(*runtime, view_ptr, nullptr, extent, scale_up);
        if (info.num_samples > 1) {
            runtime->blit_image_helper.BlitColorMSAA(&*blit_framebuffer, *blit_view,
                dst_region, src_region);
        } else {
            runtime->blit_image_helper.BlitColor(&*blit_framebuffer, *blit_view,
                dst_region, src_region, operation, BLIT_OPERATION);
        }
    } else if ((aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
        if (!blit_framebuffer)
            blit_framebuffer.emplace(*runtime, nullptr, view_ptr, extent, scale_up);
        const bool has_stencil = (aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        const bool can_blit_stencil =
            has_stencil && runtime->device.IsExtShaderStencilExportSupported();
        if (info.num_samples > 1) {
            runtime->blit_image_helper.BlitDepthStencilMSAA(&*blit_framebuffer, *blit_view,
                dst_region, src_region);
        } else if (can_blit_stencil) {
            runtime->blit_image_helper.BlitDepthStencil(&*blit_framebuffer, *blit_view,
                dst_region, src_region, operation, BLIT_OPERATION);
        } else {
            runtime->blit_image_helper.BlitDepth(&*blit_framebuffer, *blit_view, dst_region,
                                                 src_region);
        }
    } else {
        // TODO: Use helper blits where applicable
        flags &= ~ImageFlagBits::Rescaled;
        LOG_ERROR(Render_Vulkan, "Device does not support scaling format {}", info.format);
        return false;
    }
    return true;
}

bool Image::NeedsScaleHelper() const {
    const auto& device = runtime->device;
    if (info.num_samples > 1) {
        return true;
    }
    static constexpr auto OPTIMAL_FORMAT = FormatType::Optimal;
    const auto vk_format =
        MaxwellToVK::SurfaceFormat(device, OPTIMAL_FORMAT, false, info.format).format;
    const auto blit_usage = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool needs_blit_helper = !device.IsFormatSupported(vk_format, blit_usage, OPTIMAL_FORMAT);
    return needs_blit_helper;
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
                     ImageId image_id_, Image& image)
    : VideoCommon::ImageViewBase{info, image.info, image_id_, image.gpu_addr},
      device{&runtime.device}, image_handle{image.Handle()},
      samples(ConvertSampleCount(image.info.num_samples)) {
    using Shader::TextureType;

    if (image.IsSparseResident() && image.info.type == VideoCommon::ImageType::e3D &&
        info.type != VideoCommon::ImageViewType::e3D) {
        // Sparse images can't be 2D-array compatible, so 2D views of a 3D one are invalid.
        LOG_WARNING(Render_Vulkan, "2D view of a sparse resident 3D image at {:#x}",
                    image.gpu_addr);
    }

    const VkImageAspectFlags aspect_mask = ImageViewAspectMask(info);
    std::array<SwizzleSource, 4> swizzle{
        SwizzleSource::R,
        SwizzleSource::G,
        SwizzleSource::B,
        SwizzleSource::A,
    };
    if (!info.IsRenderTarget()) {
        swizzle = info.Swizzle();
        TryTransformSwizzleIfNeeded(format, swizzle,
                        device->MustEmulateBGR565(),
                        !device->IsExt4444FormatsSupported());
        if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
            std::ranges::transform(swizzle, swizzle.begin(), ConvertGreenRed);
            SanitizeDepthStencilSwizzle(swizzle, device->SupportsDepthStencilSwizzleOne());
        }
    }
    auto format_info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    if (device->ApiVersion() >= VK_API_VERSION_1_3) {
        const VkFormatProperties3 properties3 =
            device->GetPhysical().GetFormatProperties3(format_info.format);
        supports_depth_comparison =
            (properties3.optimalTilingFeatures &
             VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
        supports_minmax_filter = (properties3.optimalTilingFeatures &
                                  VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT) != 0;
    } else {
        supports_depth_comparison = true;
        supports_minmax_filter =
            (device->GetPhysical().GetFormatProperties(format_info.format).optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT) != 0;
    }
    requires_border_color_format = NeedsExplicitBorderColorFormat(format_info.format);
    swizzle_mapping = VkComponentMapping{
        .r = ComponentSwizzle(swizzle[0]),
        .g = ComponentSwizzle(swizzle[1]),
        .b = ComponentSwizzle(swizzle[2]),
        .a = ComponentSwizzle(swizzle[3]),
    };
    has_identity_swizzle = swizzle[0] == SwizzleSource::R && swizzle[1] == SwizzleSource::G &&
                           swizzle[2] == SwizzleSource::B && swizzle[3] == SwizzleSource::A;
    const VkImageUsageFlags requested_view_usage = ImageUsageFlags(format_info, format);
    const VkImageUsageFlags image_usage = image.UsageFlags();
    const VkImageUsageFlags clamped_view_usage = requested_view_usage & image_usage;
    const VkImageViewUsageCreateInfo image_view_usage{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = clamped_view_usage,
    };
    const VkImageViewASTCDecodeModeEXT astc_decode_mode{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT,
        .pNext = &image_view_usage,
        .decodeMode = VK_FORMAT_R8G8B8A8_UNORM,
    };
    const void* view_next = &image_view_usage;
    if (device->IsExtAstcDecodeModeSupported() && IsLdrAstcFormat(format_info.format)) {
        view_next = &astc_decode_mode;
    }
    const VkImageViewCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = view_next,
        .flags = 0,
        .image = image.Handle(),
        .viewType = VkImageViewType{},
        .format = format_info.format,
        .components = swizzle_mapping,
        .subresourceRange = MakeSubresourceRange(aspect_mask, info.range),
    };
    const auto create = [&](TextureType tex_type, std::optional<u32> num_layers) {
        VkImageViewCreateInfo ci{create_info};
        ci.viewType = ImageViewType(tex_type);
        if (num_layers) {
            ci.subresourceRange.layerCount = *num_layers;
        }
        vk::ImageView handle = device->GetLogical().CreateImageView(ci);
        if (device->HasDebuggingToolAttached()) {
            handle.SetObjectNameEXT(VideoCommon::Name(*this, gpu_addr).c_str());
        }
        image_views[static_cast<size_t>(tex_type)] = std::move(handle);
    };
    switch (info.type) {
    case VideoCommon::ImageViewType::e1D:
    case VideoCommon::ImageViewType::e1DArray:
        create(TextureType::Color1D, 1);
        create(TextureType::ColorArray1D, std::nullopt);
        render_target = Handle(TextureType::ColorArray1D);
        break;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::e2DArray:
    case VideoCommon::ImageViewType::Rect:
        create(TextureType::Color2D, 1);
        create(TextureType::ColorArray2D, std::nullopt);
        render_target = Handle(Shader::TextureType::ColorArray2D);
        break;
    case VideoCommon::ImageViewType::e3D:
        create(TextureType::Color3D, std::nullopt);
        render_target = Handle(Shader::TextureType::Color3D);
        break;
    case VideoCommon::ImageViewType::Cube:
    case VideoCommon::ImageViewType::CubeArray:
        create(TextureType::ColorCube, 6);
        create(TextureType::ColorArrayCube, std::nullopt);
        break;
    case VideoCommon::ImageViewType::Buffer:
        ASSERT(false);
        break;
    }
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info, ImageId image_id_, Image& image, const SlotVector<Image>& slot_imgs)
    : ImageView{runtime, info, image_id_, image}
{
    slot_images = &slot_imgs;
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view_info, GPUVAddr gpu_addr_)
    : VideoCommon::ImageViewBase{info, view_info, gpu_addr_},
      buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::NullImageViewParams& params)
    : VideoCommon::ImageViewBase{params}, device{&runtime.device} {
    if (device->HasNullDescriptor()) {
        return;
    }

    // Handle fallback for devices without nullDescriptor
    ImageInfo info{};
    info.format = PixelFormat::A8B8G8R8_UNORM;

    null_image = MakeImage(*device, runtime.memory_allocator, info, {});
    image_handle = *null_image;
    for (u32 i = 0; i < Shader::NUM_TEXTURE_TYPES; i++) {
        image_views[i] = MakeView(VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

ImageView::~ImageView() = default;

VkImageView ImageView::DepthView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (depth_view) {
        return *depth_view;
    }
    const auto& info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    depth_view = MakeView(info.format, VK_IMAGE_ASPECT_DEPTH_BIT);
    return *depth_view;
}

VkImageView ImageView::StencilView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (stencil_view) {
        return *stencil_view;
    }
    const auto& info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    stencil_view = MakeView(info.format, VK_IMAGE_ASPECT_STENCIL_BIT);
    return *stencil_view;
}

VkImageView ImageView::ColorView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (color_view) {
        return *color_view;
    }
    color_view = MakeView(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    return *color_view;
}

VkImageView ImageView::StorageView(Shader::TextureType texture_type,
                                   Shader::ImageFormat image_format) {
    if (image_handle) {
        if (image_format == Shader::ImageFormat::Typeless) {
            if (!typeless_storage_view) {
                auto info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
                typeless_storage_view = MakeView(info.format, VK_IMAGE_ASPECT_COLOR_BIT, texture_type);
            }
            return *typeless_storage_view;
        }
        const bool is_signed = image_format == Shader::ImageFormat::R8_SINT
            || image_format == Shader::ImageFormat::R16_SINT;
        if (!storage_views)
            storage_views.emplace();
        auto& views{is_signed ? storage_views->signeds : storage_views->unsigneds};
        auto& view{views[size_t(texture_type)]};
        if (!view)
            view = MakeView(Format(image_format), VK_IMAGE_ASPECT_COLOR_BIT, texture_type);
        return *view;
    }
    return VK_NULL_HANDLE;
}

bool ImageView::IsRescaled() const noexcept {
    return (*slot_images)[image_id].IsRescaled();
}

vk::ImageView ImageView::MakeView(VkFormat vk_format, VkImageAspectFlags aspect_mask,
                                  std::optional<Shader::TextureType> texture_type) {
    VkImageViewType view_type = ImageViewType(type);
    VkImageSubresourceRange subresource_range = MakeSubresourceRange(aspect_mask, range);
    if (texture_type) {
        view_type = ImageViewType(*texture_type);
        switch (view_type) {
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            break;
        default:
            subresource_range.layerCount = 1;
            break;
        }
    }
    return device->GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image_handle,
        .viewType = view_type,
        .format = vk_format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = subresource_range,
    });
}

CustomBorderColorBudget::~CustomBorderColorBudget() {
    Release();
}

CustomBorderColorBudget::CustomBorderColorBudget(CustomBorderColorBudget&& rhs) noexcept
    : device_ptr{std::exchange(rhs.device_ptr, nullptr)}, held{std::exchange(rhs.held, 0)} {}

CustomBorderColorBudget& CustomBorderColorBudget::operator=(
    CustomBorderColorBudget&& rhs) noexcept {
    if (this != &rhs) {
        Release();
        device_ptr = std::exchange(rhs.device_ptr, nullptr);
        held = std::exchange(rhs.held, 0);
    }
    return *this;
}

bool CustomBorderColorBudget::TryAcquire(const Device& device, size_t count) {
    if (!device.TryReserveCustomBorderColorSamplers(count)) {
        return false;
    }
    device_ptr = &device;
    held += count;
    return true;
}

void CustomBorderColorBudget::Release() noexcept {
    if (device_ptr != nullptr) {
        device_ptr->ReleaseCustomBorderColorSamplers(held);
    }
    device_ptr = nullptr;
    held = 0;
}

Sampler::Sampler(TextureCacheRuntime& runtime, const Tegra::Texture::TSCEntry& tsc) {
    const auto& device = runtime.device;
    device_ptr = &device;
    border_color = tsc.BorderColor();
    srgb_border_color = tsc.SrgbBorderColor();

    const f32 max_anisotropy = std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f);
    default_anisotropy = static_cast<f32>(1U << tsc.max_anisotropy);

    const VkFilter mag_filter{MaxwellToVK::Sampler::Filter(tsc.mag_filter)};
    const VkFilter min_filter{MaxwellToVK::Sampler::Filter(tsc.min_filter)};
    const VkSamplerMipmapMode mipmap_mode{MaxwellToVK::Sampler::MipmapMode(tsc.mipmap_filter)};

    const VkSamplerAddressMode wrap_u{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_u, tsc.mag_filter)};
    const VkSamplerAddressMode wrap_v{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_v, tsc.mag_filter)};
    const VkSamplerAddressMode wrap_p{
        MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_p, tsc.mag_filter)};
    const bool samples_border = wrap_u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                wrap_v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                wrap_p == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;

    reduction_mode = MaxwellToVK::SamplerReduction(tsc.reduction_filter);

    has_added_anisotropy = max_anisotropy > default_anisotropy;
    has_linear_filtering = mag_filter == VK_FILTER_LINEAR || min_filter == VK_FILTER_LINEAR ||
                           mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
    has_depth_comparison = tsc.depth_compare_enabled != 0;
    has_minmax_reduction = reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;
    has_srgb_border_color = tsc.srgb_conversion != 0 && srgb_border_color != border_color;
    if (has_minmax_reduction && !device.IsExtSamplerFilterMinmaxSupported()) {
        LOG_WARNING(Render_Vulkan, "VK_EXT_sampler_filter_minmax is required");
        has_minmax_reduction = false;
    }
    has_custom_border_colors = samples_border && device.IsCustomBorderColorUsable();
    needs_swizzle_mapping = has_custom_border_colors && device.NeedsBorderColorSwizzleMapping();

    if (has_custom_border_colors && GPU::Logging::IsActive()) {
        GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
            "VK_EXT_custom_border_color", "Sampler::Sampler");
    }
    if (device.IsExtBorderColorSwizzleSupported() && GPU::Logging::IsActive()) {
        GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
            "VK_EXT_border_color_swizzle", "Sampler::Sampler");
    }

    f32 min_lod = 0.0f;
    f32 max_lod = 0.25f;
    if (tsc.mipmap_filter != TextureMipmapFilter::None) {
        min_lod = tsc.MinLod();
        max_lod = tsc.MaxLod();
    }
    base_ci = VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = mag_filter,
        .minFilter = min_filter,
        .mipmapMode = mipmap_mode,
        .addressModeU = wrap_u,
        .addressModeV = wrap_v,
        .addressModeW = wrap_p,
        .mipLodBias = tsc.LodBias(),
        .anisotropyEnable = static_cast<VkBool32>(max_anisotropy > 1.0f),
        .maxAnisotropy = max_anisotropy,
        .compareEnable = static_cast<VkBool32>(tsc.depth_compare_enabled),
        .compareOp = MaxwellToVK::Sampler::DepthCompareFunction(tsc.depth_compare_func),
        .minLod = min_lod,
        .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT,
        .unnormalizedCoordinates = VK_FALSE,
    };
    variants.reserve(1);
    Emplace(VariantKey{});
}

Sampler::VariantKey Sampler::MakeKey(const ImageView& image_view, bool is_depth) const noexcept {
    VariantKey key{};
    key.reduce_anisotropy = has_added_anisotropy && !image_view.SupportsAnisotropy();
    key.force_nearest = has_linear_filtering && IsPixelFormatInteger(image_view.format);
    key.drop_reduction = has_minmax_reduction && !image_view.SupportsMinmaxFilter();
    key.drop_custom_border = has_custom_border_colors && image_view.RequiresBorderColorFormat();
    key.srgb_border = has_srgb_border_color && IsPixelFormatSRGB(image_view.format);
    if (needs_swizzle_mapping && !key.drop_custom_border && !image_view.HasIdentitySwizzle()) {
        const VkComponentMapping& mapping = image_view.Swizzle();
        key.swizzle = {mapping.r, mapping.g, mapping.b, mapping.a};
    }
    return key;
}

VkSampler Sampler::Find(const VariantKey& key) const noexcept {
    const auto it = std::ranges::find(variants, key, &Variant::key);
    if (it == variants.end()) {
        return VK_NULL_HANDLE;
    }
    return *it->sampler;
}

VkSampler Sampler::Emplace(VariantKey key) {
    bool custom_border = has_custom_border_colors && !key.drop_custom_border;
    if (custom_border && !custom_border_color_budget.TryAcquire(*device_ptr, 1)) {
        static bool warned_budget = false;
        if (!warned_budget) {
            warned_budget = true;
        }
        custom_border = false;
        key.drop_custom_border = true;
        key.swizzle = {};
        if (const VkSampler existing = Find(key); existing != VK_NULL_HANDLE) {
            return existing;
        }
    }

    std::array<float, 4> color = border_color;
    if (key.srgb_border) {
        color = srgb_border_color;
    }
    const VkSamplerCustomBorderColorCreateInfoEXT border_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .pNext = nullptr,
        .customBorderColor = std::bit_cast<VkClearColorValue>(color),
        .format = VK_FORMAT_UNDEFINED,
    };
    const VkSamplerBorderColorComponentMappingCreateInfoEXT mapping_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT,
        .pNext = &border_ci,
        .components{key.swizzle[0], key.swizzle[1], key.swizzle[2], key.swizzle[3]},
        .srgb = VK_FALSE,
    };
    const void* chain = nullptr;
    if (custom_border) {
        chain = &border_ci;
        if (key.HasSwizzle()) {
            chain = &mapping_ci;
        }
    }
    const VkSamplerReductionModeCreateInfoEXT reduction_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .pNext = chain,
        .reductionMode = reduction_mode,
    };
    if (has_minmax_reduction && !key.drop_reduction) {
        chain = &reduction_ci;
    }

    VkSamplerCreateInfo create_info = base_ci;
    create_info.pNext = chain;
    if (key.force_nearest) {
        create_info.magFilter = VK_FILTER_NEAREST;
        create_info.minFilter = VK_FILTER_NEAREST;
        create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        create_info.anisotropyEnable = VK_FALSE;
        create_info.maxAnisotropy = 1.0f;
    } else if (key.reduce_anisotropy) {
        create_info.anisotropyEnable = static_cast<VkBool32>(default_anisotropy > 1.0f);
        create_info.maxAnisotropy = default_anisotropy;
    }
    if (!custom_border) {
        create_info.borderColor = ConvertBorderColor(color);
    }
    variants.push_back(Variant{
        .key = key,
        .sampler = device_ptr->GetLogical().CreateSampler(create_info),
    });
    return *variants.back().sampler;
}

VkSampler Sampler::HandleFor(const ImageView& image_view, bool is_depth) {
    VariantKey key = MakeKey(image_view, is_depth);
    if (variants.size() >= MAX_VARIANTS) {
        key.srgb_border = false;
        key.swizzle = {};
    }
    if (const VkSampler existing = Find(key); existing != VK_NULL_HANDLE) {
        return existing;
    }
    return Emplace(key);
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key)
    : render_area{VkExtent2D{
          .width = key.size.width,
          .height = key.size.height,
      }} {
    CreateFramebuffer(runtime, color_buffers, depth_buffer, key.is_rescaled);
    if (runtime.device.HasDebuggingToolAttached()) {
        framebuffer.SetObjectNameEXT(VideoCommon::Name(key).c_str());
    }
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, ImageView* color_buffer,
                         ImageView* depth_buffer, VkExtent2D extent, bool is_rescaled_)
    : render_area{extent} {
    std::array<ImageView*, NUM_RT> color_buffers{color_buffer};
    CreateFramebuffer(runtime, color_buffers, depth_buffer, is_rescaled_);
}

Framebuffer::~Framebuffer() = default;

void Framebuffer::CreateFramebuffer(TextureCacheRuntime& runtime,
                                    std::span<ImageView*, NUM_RT> color_buffers,
                                    ImageView* depth_buffer, bool is_rescaled_) {
    boost::container::small_vector<VkImageView, NUM_RT * 2 + 2> attachments;
    RenderPassKey renderpass_key{};
    s32 num_layers = 1;

    is_rescaled = is_rescaled_;
    const auto& resolution = runtime.resolution;

    u32 width = (std::numeric_limits<u32>::max)();
    u32 height = (std::numeric_limits<u32>::max)();
    for (size_t index = 0; index < NUM_RT; ++index) {
        const ImageView* const color_buffer = color_buffers[index];
        if (!color_buffer) {
            renderpass_key.color_formats[index] = PixelFormat::Invalid;
            continue;
        }
        width = (std::min)(width, is_rescaled ? resolution.ScaleUp(color_buffer->size.width)
                                            : color_buffer->size.width);
        height = (std::min)(height, is_rescaled ? resolution.ScaleUp(color_buffer->size.height)
                                              : color_buffer->size.height);
        attachments.push_back(color_buffer->RenderTarget());
        renderpass_key.color_formats[index] = color_buffer->format;
        num_layers = (std::max)(num_layers, color_buffer->range.extent.layers);
        images[num_images] = color_buffer->ImageHandle();
        image_ranges[num_images] = MakeSubresourceRange(color_buffer);
        rt_map[index] = num_images;
        samples = color_buffer->Samples();
        ++num_images;
    }
    const size_t num_colors = attachments.size();
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageAspectFlags depth_aspect_mask = 0;
    if (depth_buffer) {
        width = (std::min)(width, is_rescaled ? resolution.ScaleUp(depth_buffer->size.width)
                                            : depth_buffer->size.width);
        height = (std::min)(height, is_rescaled ? resolution.ScaleUp(depth_buffer->size.height)
                                              : depth_buffer->size.height);
        attachments.push_back(depth_buffer->RenderTarget());
        renderpass_key.depth_format = depth_buffer->format;
        num_layers = (std::max)(num_layers, depth_buffer->range.extent.layers);
        images[num_images] = depth_buffer->ImageHandle();
        const VkImageSubresourceRange subresource_range = MakeSubresourceRange(depth_buffer);
        image_ranges[num_images] = subresource_range;
        samples = depth_buffer->Samples();
        ++num_images;
        has_depth = (subresource_range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        has_stencil = (subresource_range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        depth_image = depth_buffer->ImageHandle();
        depth_aspect_mask = subresource_range.aspectMask;
    } else {
        renderpass_key.depth_format = PixelFormat::Invalid;
    }
    renderpass_key.samples = samples;
    const bool do_resolve_color = ENABLE_MSAA_TILER_RESOLVE &&
        samples != VK_SAMPLE_COUNT_1_BIT && num_colors > 0 && runtime.device.IsTiler();
    renderpass_key.resolve_color = do_resolve_color;

    const bool do_resolve_depth_stencil = ENABLE_MSAA_TILER_RESOLVE &&
        samples != VK_SAMPLE_COUNT_1_BIT && depth_image != VK_NULL_HANDLE &&
        runtime.device.IsTiler() &&
        SupportsDepthStencilResolve(runtime.device, renderpass_key.depth_format);
    renderpass_key.resolve_depth_stencil = do_resolve_depth_stencil;

    discard_msaa_color =
        ENABLE_MSAA_RESOLVE_CONSUME && ENABLE_MSAA_COLOR_DISCARD && do_resolve_color;
    discard_msaa_depth_stencil = ENABLE_MSAA_RESOLVE_CONSUME &&
                                 ENABLE_MSAA_DEPTH_STENCIL_DISCARD && do_resolve_depth_stencil;

    renderpass = runtime.render_pass_cache.Get(renderpass_key);
    render_pass_key = renderpass_key;
    render_pass_cache = &runtime.render_pass_cache;
    runtime_ptr = &runtime;
    render_area.width = (std::min)(render_area.width, width);
    render_area.height = (std::min)(render_area.height, height);

    if (do_resolve_color) {
        const u32 layers = static_cast<u32>((std::max)(num_layers, 1));
        for (size_t index = 0; index < NUM_RT; ++index) {
            const PixelFormat format = renderpass_key.color_formats[index];
            if (format == PixelFormat::Invalid) {
                continue;
            }
            const VkFormat vk_format =
                MaxwellToVK::SurfaceFormat(runtime.device, FormatType::Optimal, true, format).format;
            const VkImage msaa_image = images[rt_map[index]];
            attachments.push_back(runtime.GetOrCreateResolveShadow(
                msaa_image, vk_format, render_area, layers, VK_IMAGE_ASPECT_COLOR_BIT));
            resolve_shadow_images[num_resolve_shadows++] = msaa_image;
        }
    }

    if (do_resolve_depth_stencil) {
        const u32 layers = static_cast<u32>((std::max)(num_layers, 1));
        const VkFormat vk_format =
            MaxwellToVK::SurfaceFormat(runtime.device, FormatType::Optimal, true,
                                       renderpass_key.depth_format)
                .format;
        attachments.push_back(runtime.GetOrCreateResolveShadow(depth_image, vk_format, render_area,
                                                               layers, depth_aspect_mask));
        resolve_shadow_images[num_resolve_shadows++] = depth_image;
    }

    num_color_buffers = static_cast<u32>(num_colors);
    framebuffer = runtime.device.GetLogical().CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = renderpass,
        .attachmentCount = static_cast<u32>(attachments.size()),
        .pAttachments = attachments.data(),
        .width = render_area.width,
        .height = render_area.height,
        .layers = static_cast<u32>((std::max)(num_layers, 1)),
    });
}

void Framebuffer::MarkResolveShadowsUpToDate() const {
    if (runtime_ptr == nullptr) {
        return;
    }
    for (u32 index = 0; index < num_resolve_shadows; ++index) {
        runtime_ptr->MarkResolveShadowUpToDate(resolve_shadow_images[index]);
    }
}

VkRenderPass Framebuffer::RenderPassVariant(u32 color_clear_mask, bool depth_stencil_clear,
                                            u32 color_discard_mask,
                                            bool depth_stencil_discard) const {
    if (color_clear_mask == 0 && !depth_stencil_clear && color_discard_mask == 0 &&
        !depth_stencil_discard) {
        return renderpass;
    }
    static_assert(NUM_RT <= 8);
    const u32 variant_key = color_clear_mask | (color_discard_mask << 8) |
                            (static_cast<u32>(depth_stencil_clear) << 16) |
                            (static_cast<u32>(depth_stencil_discard) << 17);
    for (u32 index = 0; index < num_memoized_variants; ++index) {
        if (variant_keys[index] == variant_key) {
            return variant_render_passes[index];
        }
    }
    RenderPassKey key = render_pass_key;
    key.color_clear_mask = color_clear_mask;
    key.depth_stencil_clear = depth_stencil_clear;
    key.color_discard_mask = color_discard_mask;
    key.depth_stencil_discard = depth_stencil_discard;
    const VkRenderPass variant = render_pass_cache->Get(key);
    if (num_memoized_variants < variant_keys.size()) {
        variant_keys[num_memoized_variants] = variant_key;
        variant_render_passes[num_memoized_variants] = variant;
        ++num_memoized_variants;
    }
    return variant;
}

void TextureCacheRuntime::AccelerateImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles,
    u32 z_src_start, u32 z_image_start, u32 z_count,
    std::span<const u8> slice_has_data,
    bool image_already_uploaded) {

    if (IsPixelFormatASTC(image.info.format)) {
        return astc_decoder_pass->Assemble(image, map, swizzles);
    }

    if (bl3d_unswizzle_pass && IsPixelFormatBCn(image.info.format) && image.info.type == ImageType::e3D && image.info.resources.levels == 1 && image.info.resources.layers == 1) {
        return bl3d_unswizzle_pass->Unswizzle(image, map, swizzles,
            z_src_start, z_image_start, z_count,
            slice_has_data, image_already_uploaded);
    }

    ASSERT(false);
}

void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    if (!image.ExchangeInitialization()) {
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_NONE,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.Handle(),
            .subresourceRange{
                .aspectMask = image.AspectMask(),
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   vk::PIPELINE_STAGE_GRAPHICS_COMPUTE, 0, barrier);
        });
    }
}

void TextureCacheRuntime::SynchronizePendingGpuWrite(Image& image) {
    if (!image.has_pending_gpu_write_sync) {
        return;
    }
    image.has_pending_gpu_write_sync = false;

    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.Handle(),
        .subresourceRange = {image.AspectMask(), 0, 1, 0, 1},
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               vk::PIPELINE_STAGE_GRAPHICS_COMPUTE, 0, barrier);
    });
}

} // namespace Vulkan
