// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/texture_cache/accelerated_swizzle.h"

namespace VideoCommon {
struct SwizzleParameters;
}

namespace Vulkan {

using VideoCommon::Accelerated::BlockLinearSwizzle3DParams;

class Device;
class StagingBufferPool;
class Scheduler;
class Image;
struct StagingBufferRef;

class ComputePass {
public:
    explicit ComputePass(const Device& device, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                         vk::Span<VkDescriptorSetLayoutBinding> bindings,
                         vk::Span<VkDescriptorUpdateTemplateEntry> templates,
                         const DescriptorBankInfo& bank_info,
                         vk::Span<VkPushConstantRange> push_constants, std::span<const u32> code,
                         std::optional<u32> optional_subgroup_size = std::nullopt);
    ~ComputePass();

protected:
    const Device& device;
    vk::DescriptorUpdateTemplate descriptor_template;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;

private:
    vk::ShaderModule module;
};

class Uint8Pass final : public ComputePass {
public:
    explicit Uint8Pass(const Device& device_, Scheduler& scheduler_,
                       DescriptorPool& descriptor_pool_, StagingBufferPool& staging_buffer_pool_,
                       ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~Uint8Pass();

    /// Assemble uint8 indices into an uint16 index buffer
    /// Returns a pair with the staging buffer, and the offset where the assembled data is
    std::pair<VkBuffer, VkDeviceSize> Assemble(u32 num_vertices, VkBuffer src_buffer,
                                               u32 src_offset);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class QuadIndexedPass final : public ComputePass {
public:
    explicit QuadIndexedPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~QuadIndexedPass();

    std::pair<VkBuffer, VkDeviceSize> Assemble(
        Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format, u32 num_vertices,
        u32 base_vertex, VkBuffer src_buffer, u32 src_offset, bool is_strip);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class ConditionalRenderingResolvePass final : public ComputePass {
public:
    explicit ConditionalRenderingResolvePass(
        const Device& device_, Scheduler& scheduler_, DescriptorPool& descriptor_pool_,
        ComputePassDescriptorQueue& compute_pass_descriptor_queue_);

    void Resolve(VkBuffer dst_buffer, VkBuffer src_buffer, u32 src_offset, bool compare_to_zero);

private:
    Scheduler& scheduler;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class QueriesPrefixScanPass final : public ComputePass {
public:
    explicit QueriesPrefixScanPass(const Device& device_, Scheduler& scheduler_,
                                   DescriptorPool& descriptor_pool_,
                                   ComputePassDescriptorQueue& compute_pass_descriptor_queue_);

    void Run(VkBuffer accumulation_buffer, VkBuffer dst_buffer, VkBuffer src_buffer,
             size_t number_of_sums, size_t min_accumulation_limit, size_t max_accumulation_limit);

private:
    Scheduler& scheduler;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

/// eden-kledd: encodes an RGBA8 storage image into BC7 blocks in a storage buffer (mode 6)
class BC7EncodePass final : public ComputePass {
public:
    explicit BC7EncodePass(const Device& device_, Scheduler& scheduler_,
                           DescriptorPool& descriptor_pool_,
                           ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~BC7EncodePass();

    /// Records the encode of one mip level. Must be called outside a render pass with the
    /// source view in GENERAL layout. Writes blocks starting at out_offset (in bytes).
    void Encode(VkImageView src_view, u32 width, u32 height, u32 layers, VkBuffer out_buffer,
                VkDeviceSize out_offset, VkDeviceSize out_size);

private:
    Scheduler& scheduler;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

class ASTCDecoderPass final : public ComputePass {
public:
    explicit ASTCDecoderPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_,
                             MemoryAllocator& memory_allocator_);
    ~ASTCDecoderPass();

    void Assemble(Image& image, const StagingBufferRef& map,
                  std::span<const VideoCommon::SwizzleParameters> swizzles);

private:
    /// eden-kledd: decode ASTC into a temporary RGBA8 image, then encode BC7 on the GPU
    void AssembleBC7(Image& image, const StagingBufferRef& map,
                     std::span<const VideoCommon::SwizzleParameters> swizzles);

    void RecordDecode(Image& image, const StagingBufferRef& map,
                      const VideoCommon::SwizzleParameters& swizzle, VkImageView dst_view);

    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
    MemoryAllocator& memory_allocator;
    DescriptorPool& descriptor_pool;
    std::optional<BC7EncodePass> bc7_encode_pass;
};

class BlockLinearUnswizzle3DPass final : public ComputePass {
public:
    explicit BlockLinearUnswizzle3DPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_);
    ~BlockLinearUnswizzle3DPass();

    void Unswizzle(Image& image,
                   const StagingBufferRef& swizzled,
                   std::span<const VideoCommon::SwizzleParameters> swizzles,
                   u32 z_src_start, u32 z_image_start, u32 z_count,
                   std::span<const u8> slice_has_data,
                   bool image_already_uploaded);

    void UnswizzleChunk(
        Image &image,
        const BlockLinearSwizzle3DParams &params,
        u32 blocks_x, u32 blocks_y,
        u32 z_src, u32 z_dst, u32 z_count,
        u32 src_buffer_byte_bias);

    void UnswizzleZeroChunk(
        Image &image,
        u32 blocks_x, u32 blocks_y,
        u32 bytes_per_block,
        u32 z_dst, u32 z_count);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    ComputePassDescriptorQueue& compute_pass_descriptor_queue;
};

} // namespace Vulkan
