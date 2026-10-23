// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/common_types.h"

#include "video_core/host_shaders/taa_frag_spv.h"
#include "video_core/host_shaders/taa_vert_spv.h"
#include "video_core/renderer_vulkan/present/taa.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

TAA::TAA(const Device& device, MemoryAllocator& allocator, size_t image_count, VkExtent2D extent)
    : m_fxaa(device, allocator, image_count, extent)
    , m_extent(extent)
    , m_image_count(u32(image_count))
{
    CreateImages(device, allocator);
    CreateRenderPasses(device);
    CreateSampler(device);
    CreateShaders(device);
    CreateDescriptorPool(device);
    CreateDescriptorSetLayouts(device);
    CreateDescriptorSets(device);
    CreatePipelineLayouts(device);
    CreatePipelines(device);
}

TAA::~TAA() = default;

void TAA::CreateImages(const Device& device, MemoryAllocator& allocator) {
    for (u32 i = 0; i < m_image_count; i++) {
        Image& image = m_dynamic_images.emplace_back();
        image.image = CreateWrappedImage(allocator, m_extent, VK_FORMAT_R16G16B16A16_SFLOAT);
        image.image_view = CreateWrappedImageView(device, image.image, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
}

void TAA::CreateRenderPasses(const Device& device) {
    m_renderpass = CreateWrappedRenderPass(device, VK_FORMAT_R16G16B16A16_SFLOAT);

    for (auto& image : m_dynamic_images) {
        image.framebuffer =
            CreateWrappedFramebuffer(device, m_renderpass, image.image_view, m_extent);
    }
}

void TAA::CreateSampler(const Device& device) {
    m_sampler = CreateWrappedSampler(device);
}

void TAA::CreateShaders(const Device& device) {
    m_vertex_shader = CreateWrappedShaderModule(device, TAA_VERT_SPV);
    m_fragment_shader = CreateWrappedShaderModule(device, TAA_FRAG_SPV);
}

void TAA::CreateDescriptorPool(const Device& device) {
    // 2 descriptors (current frame, history), 1 descriptor set per image
    m_descriptor_pool = CreateWrappedDescriptorPool(device, 2 * m_image_count, m_image_count);
}

void TAA::CreateDescriptorSetLayouts(const Device& device) {
    m_descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
}

void TAA::CreateDescriptorSets(const Device& device) {
    VkDescriptorSetLayout layout = *m_descriptor_set_layout;

    for (auto& images : m_dynamic_images) {
        images.descriptor_sets = CreateWrappedDescriptorSets(m_descriptor_pool, {layout});
    }
}

void TAA::CreatePipelineLayouts(const Device& device) {
    m_pipeline_layout = CreateWrappedPipelineLayout(device, m_descriptor_set_layout);
}

void TAA::CreatePipelines(const Device& device) {
    m_pipeline = CreateWrappedPipeline(device, m_renderpass, m_pipeline_layout,
                                       std::tie(m_vertex_shader, m_fragment_shader));
}

void TAA::UpdateDescriptorSets(const Device& device, VkImageView current_view,
                               VkImageView history_view, size_t image_index) {
    Image& image = m_dynamic_images[image_index];
    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkWriteDescriptorSet> updates;
    image_infos.reserve(2);

    updates.push_back(CreateWriteDescriptorSet(image_infos, *m_sampler, current_view, image.descriptor_sets[0], 0));
    updates.push_back(CreateWriteDescriptorSet(image_infos, *m_sampler, history_view, image.descriptor_sets[0], 1));

    device.GetLogical().UpdateDescriptorSets(updates, {});
}

void TAA::UploadImages(const Device& device, Scheduler& scheduler) {
    if (m_images_ready) {
        return;
    }

    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        for (auto& image : m_dynamic_images) {
            ClearColorImage(cmdbuf, *image.image);
        }
    });
    scheduler.Finish();

    m_images_ready = true;
}

void TAA::Draw(const Device& device, Scheduler& scheduler, size_t image_index, VkImage* inout_image, VkImageView* inout_image_view) {
    // FXAA smooths edges within the frame, the temporal pass then steadies them across frames
    m_fxaa.Draw(device, scheduler, image_index, inout_image, inout_image_view);

    // The previous output is the history; on the first frame (or if it would alias the image
    // being written) the current frame stands in for it, which resolves to the current frame
    const bool use_history = m_has_history && m_history_index != image_index;
    const Image& image{m_dynamic_images[image_index]};
    const VkImage input_image{*inout_image};
    const VkImage history_image{use_history ? *m_dynamic_images[m_history_index].image : input_image};
    const VkImageView history_view{use_history ? *m_dynamic_images[m_history_index].image_view
                                               : *inout_image_view};
    const VkImage output_image{*image.image};
    const VkDescriptorSet descriptor_set{image.descriptor_sets[0]};
    const VkFramebuffer framebuffer{*image.framebuffer};
    const VkRenderPass renderpass{*m_renderpass};
    const VkPipeline pipeline{*m_pipeline};
    const VkPipelineLayout layout{*m_pipeline_layout};
    const VkExtent2D extent{m_extent};

    UploadImages(device, scheduler);
    UpdateDescriptorSets(device, *inout_image_view, history_view, image_index);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        TransitionImageLayout(cmdbuf, input_image, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, history_image, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, output_image, VK_IMAGE_LAYOUT_GENERAL);
        BeginRenderPass(cmdbuf, renderpass, framebuffer, extent);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set, {});
        cmdbuf.Draw(3, 1, 0, 0);
        cmdbuf.EndRenderPass();
        TransitionImageLayout(cmdbuf, output_image, VK_IMAGE_LAYOUT_GENERAL);
    });

    m_history_index = image_index;
    m_has_history = true;

    *inout_image = *image.image;
    *inout_image_view = *image.image_view;
}

} // namespace Vulkan
