// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/renderer_vulkan/present/anti_alias_pass.h"
#include "video_core/renderer_vulkan/present/fxaa.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// FXAA followed by a temporal resolve against the previous output. There are no motion
/// vectors, so the history is clipped to the current neighbourhood instead of reprojected.
class TAA final : public AntiAliasPass {
public:
    explicit TAA(const Device& device, MemoryAllocator& allocator, size_t image_count, VkExtent2D extent);
    ~TAA() override;

    void Draw(const Device& device, Scheduler& scheduler, size_t image_index, VkImage* inout_image, VkImageView* inout_image_view) override;

private:
    void CreateImages(const Device& device, MemoryAllocator& allocator);
    void CreateRenderPasses(const Device& device);
    void CreateSampler(const Device& device);
    void CreateShaders(const Device& device);
    void CreateDescriptorPool(const Device& device);
    void CreateDescriptorSetLayouts(const Device& device);
    void CreateDescriptorSets(const Device& device);
    void CreatePipelineLayouts(const Device& device);
    void CreatePipelines(const Device& device);
    void UpdateDescriptorSets(const Device& device, VkImageView current_view, VkImageView history_view, size_t image_index);
    void UploadImages(const Device& device, Scheduler& scheduler);

    struct Image {
        vk::DescriptorSets descriptor_sets{};
        vk::Framebuffer framebuffer{};
        vk::Image image{};
        vk::ImageView image_view{};
    };
    FXAA m_fxaa;
    std::vector<Image> m_dynamic_images{};
    const VkExtent2D m_extent;
    const u32 m_image_count;
    vk::ShaderModule m_vertex_shader{};
    vk::ShaderModule m_fragment_shader{};
    vk::DescriptorPool m_descriptor_pool{};
    vk::DescriptorSetLayout m_descriptor_set_layout{};
    vk::PipelineLayout m_pipeline_layout{};
    vk::Pipeline m_pipeline{};
    vk::RenderPass m_renderpass{};
    vk::Sampler m_sampler{};
    size_t m_history_index{};
    bool m_has_history{};
    bool m_images_ready{};
};

} // namespace Vulkan
