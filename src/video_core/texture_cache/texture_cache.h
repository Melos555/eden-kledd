// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <bit>
#include <condition_variable>
#include <cstring>
#include "common/container/unordered_map.h"
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/kledd_frame_stats.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/guest_memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/texture_cache/util.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/textures/decoders.h"

namespace VideoCommon {

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using Tegra::Texture::UnswizzleTexture;
using VideoCore::Surface::GetFormatType;
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::SurfaceType;
using namespace Common::Literals;

template <class P>
TextureCache<P>::TextureCache(Runtime& runtime_, Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : runtime{runtime_}, device_memory{device_memory_} {
    // Configure null sampler
    TSCEntry sampler_descriptor{};
    sampler_descriptor.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mag_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
    sampler_descriptor.cubemap_anisotropy.Assign(1);

    // These values were chosen based on typical peak swizzle data sizes seen in some titles
    constexpr size_t SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 8_MiB;
    constexpr size_t UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 1_MiB;
    swizzle_data_buffer.resize_destructive(SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);
    unswizzle_data_buffer.resize_destructive(UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);

    // Make sure the first index is reserved for the null resources
    // This way the null resource becomes a compile time constant
    void(slot_images.insert(NullImageParams{}));
    void(slot_image_views.insert(runtime, NullImageViewParams{}));
    void(slot_samplers.insert(runtime, sampler_descriptor));

    if constexpr (HAS_DEVICE_MEMORY_INFO) {
        const s64 device_local_memory = static_cast<s64>(runtime.GetDeviceLocalMemory());
        const s64 min_spacing_expected = device_local_memory - 1_GiB;
        const s64 min_spacing_critical = device_local_memory - 512_MiB;
        const s64 mem_threshold = (std::min)(device_local_memory, TARGET_THRESHOLD);
        const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
        const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
        expected_memory = static_cast<u64>(
            (std::max)((std::min)(device_local_memory - min_vacancy_expected, min_spacing_expected),
                     DEFAULT_EXPECTED_MEMORY));
        critical_memory = static_cast<u64>(
            (std::max)((std::min)(device_local_memory - min_vacancy_critical, min_spacing_critical),
                     DEFAULT_CRITICAL_MEMORY));
        minimum_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
    } else {
        expected_memory = DEFAULT_EXPECTED_MEMORY + 512_MiB;
        critical_memory = DEFAULT_CRITICAL_MEMORY + 1_GiB;
        minimum_memory = 0;
    }

    async_unswizzle_mode = Settings::values.async_unswizzle_mode.GetValue();

    if (async_unswizzle_mode != Settings::AsyncUnswizzleMode::Off) {
        switch (Settings::values.gpu_unswizzle_chunk_size.GetValue()) {
            case Settings::GpuUnswizzleChunk::VeryLow: async_unswizzle_slices_per_batch = 32; break;
            case Settings::GpuUnswizzleChunk::Low:     async_unswizzle_slices_per_batch = 64; break;
            case Settings::GpuUnswizzleChunk::Normal:  async_unswizzle_slices_per_batch = 128; break;
            case Settings::GpuUnswizzleChunk::Medium:  async_unswizzle_slices_per_batch = 256; break;
            case Settings::GpuUnswizzleChunk::High:    async_unswizzle_slices_per_batch = 512; break;
            case Settings::GpuUnswizzleChunk::Off:     async_unswizzle_slices_per_batch = 0; break;
            default:                                   async_unswizzle_slices_per_batch = 32;
        }
    } else {
        async_unswizzle_slices_per_batch = 0;
    }
}

template <class P>
void TextureCache<P>::RunGarbageCollector() {
    bool high_priority_mode = false;
    bool aggressive_mode = false;
    u64 ticks_to_destroy = 0;
    size_t num_iterations = 0;
    const auto Configure = [&](bool allow_aggressive) {
        high_priority_mode = total_used_memory >= expected_memory;
        aggressive_mode = allow_aggressive && total_used_memory >= critical_memory;
        ticks_to_destroy = aggressive_mode ? 10ULL : high_priority_mode ? 25ULL : 50ULL;
        if (Settings::values.kledd_gentle_gc.GetValue()) {
            // eden-kledd: 10 frames is ~0.1 s at 100 FPS, so turning the camera away and back
            // deletes and re-uploads the same textures (thrashing). Keep them a few seconds.
            ticks_to_destroy = aggressive_mode ? 240ULL : high_priority_mode ? 480ULL : 900ULL;
        }
        if (const u32 seconds = Settings::values.kledd_gc_seconds.GetValue(); seconds > 0) {
            // eden-kledd: count real seconds instead of frames, so the frame rate (60 vs 120)
            // and long pauses like the scan text don't change how long textures survive.
            ticks_to_destroy = KleddTicksForSeconds(aggressive_mode     ? seconds
                                                    : high_priority_mode ? seconds * 2
                                                                         : seconds * 4);
        }
        num_iterations = aggressive_mode ? 40 : (high_priority_mode ? 20 : 10);
    };
    const auto Cleanup = [this, &num_iterations, &high_priority_mode, &aggressive_mode](ImageId image_id) {
        if (num_iterations == 0) {
            return true;
        }
        --num_iterations;
        auto& image = slot_images[image_id];
        if (True(image.flags & ImageFlagBits::IsDecoding)) {
            return false;
        }
        const bool must_download = IsDownloadable(image) && False(image.flags & ImageFlagBits::BadOverlap);
        if ((!aggressive_mode && True(image.flags & ImageFlagBits::CostlyLoad)) || (!high_priority_mode && must_download)) {
            return false;
        }
        Common::Kledd::GetFrameStats().gc_deleted.fetch_add(1, std::memory_order_relaxed);
        if (must_download) {
            Common::Kledd::GetFrameStats().gc_sync_downloads.fetch_add(1,
                                                                      std::memory_order_relaxed);
            auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            runtime.Finish();
            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span, swizzle_data_buffer);
        }
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, image_id);
        }
        UnregisterImage(image_id);
        DeleteImage(image_id, image.scale_tick > frame_tick + 5);
        if (aggressive_mode && total_used_memory < critical_memory) {
            num_iterations >>= 2;
            aggressive_mode = false;
        } else if (high_priority_mode && total_used_memory < expected_memory) {
            num_iterations >>= 1;
            high_priority_mode = false;
        }
        return false;
    };
    Configure(false);
    lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, Cleanup);
    if (total_used_memory >= critical_memory) {
        Configure(true);
        lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, Cleanup);
    }
}

template <class P>
void TextureCache<P>::TickFrame() {
    KleddLogLongFrame();
    {
        const auto now = std::chrono::steady_clock::now();
        kledd_tick_times.emplace_back(frame_tick, now);
        while (!kledd_tick_times.empty() &&
               now - kledd_tick_times.front().second > std::chrono::seconds(500)) {
            kledd_tick_times.pop_front();
        }
    }
    // If we can obtain the memory info, use it instead of the estimate.
    if (runtime.CanReportMemoryUsage()) {
        total_used_memory = runtime.GetDeviceMemoryUsage();
    }
    if (total_used_memory > minimum_memory) {
        RunGarbageCollector();
    }
    sentenced_images.Tick();
    sentenced_framebuffers.Tick();
    sentenced_image_view.Tick();
    TickAsyncDecode();
    TickAsyncUnswizzle();

    runtime.TickFrame();
    ++frame_tick;

    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        for (auto& buffer : async_buffers_death_ring) {
            runtime.FreeDeferredStagingBuffer(buffer);
        }
        async_buffers_death_ring.clear();
    }
}

template <class P>
u64 TextureCache<P>::KleddTicksForSeconds(u32 seconds) const {
    // Frames that passed since `seconds` ago. If the history is shorter than that, keep all.
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
    const auto it = std::upper_bound(
        kledd_tick_times.begin(), kledd_tick_times.end(), cutoff,
        [](const auto& time, const auto& entry) { return time < entry.second; });
    if (it == kledd_tick_times.begin()) {
        return frame_tick;
    }
    const u64 tick_then = std::prev(it)->first;
    return frame_tick - (std::min)(tick_then, frame_tick);
}

template <class P>
void TextureCache<P>::KleddLogLongFrame() {
    auto& s = Common::Kledd::GetFrameStats();
    const auto take = [](auto& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    const u32 created = take(s.images_created);
    const u32 deleted = take(s.images_deleted);
    const u32 gc_deleted = take(s.gc_deleted);
    const u32 gc_sync = take(s.gc_sync_downloads);
    const u32 join_dropped = take(s.join_dropped_gpu);
    const u32 uploads = take(s.uploads);
    const u64 upload_bytes = take(s.upload_bytes);
    const u32 decodes = take(s.async_decodes);
    const u32 unswizzles = take(s.async_unswizzles);
    const u32 gfx_pipelines = take(s.graphics_pipelines);
    const u32 cmp_pipelines = take(s.compute_pipelines);

    const auto now = std::chrono::steady_clock::now();
    const auto last = std::exchange(kledd_last_frame, now);
    if (!Settings::values.kledd_frame_log.GetValue() ||
        last == std::chrono::steady_clock::time_point{}) {
        return;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (ms < 50) {
        return;
    }
    constexpr u64 MiB = 1024 * 1024;
    LOG_INFO(HW_GPU,
             "[Kledd] long frame {} ms | images new {} deleted {} (gc {}, gc sync downloads {}, "
             "join drops {}) | uploads {} ({} MiB) async decode {} unswizzle {} | pipelines "
             "built gfx {} compute {} | texture memory {}/{}/{} MiB (used/expected/critical)",
             ms, created, deleted, gc_deleted, gc_sync, join_dropped, uploads, upload_bytes / MiB,
             decodes, unswizzles, gfx_pipelines, cmp_pipelines, total_used_memory / MiB,
             expected_memory / MiB, critical_memory / MiB);
}

template <class P>
const typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) const noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(u32 index) noexcept {
    // Not compute!
    const auto image_view_id = VisitImageView(index, false);
    return slot_image_views[image_view_id];
}

template <class P>
void TextureCache<P>::MarkModification(ImageId id) noexcept {
    MarkModification(slot_images[id]);
}

template <class P>
void TextureCache<P>::FillImageViews(std::span<ImageViewInOut> views, bool compute, bool blacklist) {
    bool has_blacklisted = false;
    do {
        has_deleted_images = false;
        if (blacklist) {
            has_blacklisted = false;
        }
        for (ImageViewInOut& view : views) {
            view.id = VisitImageView(view.index, compute);
            if (blacklist) {
                if (view.blacklist && view.id != NULL_IMAGE_VIEW_ID) {
                    const ImageViewBase& image_view = slot_image_views[view.id];
                    auto& image = slot_images[image_view.image_id];
                    has_blacklisted |= ScaleDown(image);
                    image.scale_rating = 0;
                }
            }
        }
    } while (has_deleted_images || (blacklist && has_blacklisted));
}

template <class P>
void TextureCache<P>::CheckFeedbackLoop(std::span<const ImageViewInOut> views) {
    if (!Settings::values.barrier_feedback_loops.GetValue()) {
        return;
    }

    if (render_targets_serial == last_feedback_loop_serial &&
        texture_bindings_serial == last_feedback_texture_serial) {
        if (last_feedback_loop_result) {
            runtime.BarrierFeedbackLoop();
        }
        return;
    }

    if (rt_active_mask == 0) {
        last_feedback_loop_serial = render_targets_serial;
        last_feedback_texture_serial = texture_bindings_serial;
        last_feedback_loop_result = false;
        return;
    }
    const u32 depth_bit = 1u << NUM_RT;
    const bool depth_active = (rt_active_mask & depth_bit) != 0;

    const bool requires_barrier = [&] {
        for (const auto& view : views) {
            if (!view.id) {
                continue;
            }

            {
                bool is_continue = false;
                for (size_t i = 0; i < 8; ++i)
                    is_continue |= (rt_active_mask & (1u << i)) && view.id == render_targets.color_buffer_ids[i];
                if (is_continue)
                    continue;
            }

            if (depth_active && view.id == render_targets.depth_buffer_id)
                continue;

            const ImageId view_image_id = slot_image_views[view.id].image_id;
            {
                bool is_continue = false;
                for (size_t i = 0; i < 8; ++i)
                    is_continue |= (rt_active_mask & (1u << i)) && view_image_id == rt_image_id[i];
                if (is_continue)
                    continue;
            }
            if (depth_active && view_image_id == rt_depth_image_id) {
                return true;
            }
        }

        return false;
    }();

    last_feedback_loop_serial = render_targets_serial;
    last_feedback_texture_serial = texture_bindings_serial;
    last_feedback_loop_result = requires_barrier;
    if (requires_barrier) {
        runtime.BarrierFeedbackLoop();
    }
}

template <class P>
typename P::Sampler* TextureCache<P>::GetSampler(u32 index, bool compute) {
    return &slot_samplers[GetSamplerId(index, compute)];
}

template <class P>
SamplerId TextureCache<P>::GetSamplerId(u32 index, bool compute) {
    auto& table = compute ? channel_state->compute_sampler_table : channel_state->graphics_sampler_table;
    if (index > table.current_limit) {
        LOG_DEBUG(HW_GPU, "Invalid sampler index={}", index);
        return NULL_SAMPLER_ID;
    }
    auto const map_index = index | (compute ? Common::SlotId::TAGGED_VALUE : 0);
    auto const [descriptor, is_new] = table.Read(*gpu_memory, index);
    if (is_new) {
        auto const id = FindSampler(descriptor, compute);
        channel_state->sampler_ids.insert_or_assign(map_index, id);
        return id;
    }
    return channel_state->sampler_ids.find(map_index)->second;
}

template <class P>
const typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) const noexcept {
    return slot_samplers[id];
}

template <class P>
typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) noexcept {
    return slot_samplers[id];
}

template <class P>
void TextureCache<P>::SynchronizeDescriptors(bool compute) {
    if (compute) {
        const bool linked_tsc = kepler_compute->launch_description.linked_tsc;
        const u32 tic_limit = kepler_compute->regs.tic.limit;
        const u32 tsc_limit = linked_tsc ? tic_limit : kepler_compute->regs.tsc.limit;
        bool bindings_changed = false;
        if (channel_state->compute_sampler_table.Synchronize(kepler_compute->regs.tsc.Address(), tsc_limit))
            bindings_changed = true;
        if (channel_state->compute_image_table.Synchronize(kepler_compute->regs.tic.Address(), tic_limit))
            bindings_changed = true;
        if (bindings_changed) {
            ++texture_bindings_serial;
        }
    } else {
        const bool linked_tsc = maxwell3d->regs.sampler_binding == Tegra::Engines::Maxwell3D::Regs::SamplerBinding::ViaHeaderBinding;
        const u32 tic_limit = maxwell3d->regs.tex_header.limit;
        const u32 tsc_limit = linked_tsc ? tic_limit : maxwell3d->regs.tex_sampler.limit;
        bool bindings_changed = false;
        if (channel_state->graphics_sampler_table.Synchronize(maxwell3d->regs.tex_sampler.Address(), tsc_limit))
            bindings_changed = true;
        if (channel_state->graphics_image_table.Synchronize(maxwell3d->regs.tex_header.Address(), tic_limit))
            bindings_changed = true;
        if (bindings_changed) {
            ++texture_bindings_serial;
        }
    }
}

template <class P>
bool TextureCache<P>::RescaleRenderTargets() {
    auto& flags = maxwell3d->dirty.flags;
    u32 scale_rating = 0;
    bool rescaled = false;
    std::array<ImageId, NUM_RT> tmp_color_images{};
    ImageId tmp_depth_image{};
    do {
        flags[Dirty::RenderTargets] = false;

        has_deleted_images = false;
        // Render target control is used on all render targets, so force look ups when this one is
        // up
        const bool force = flags[Dirty::RenderTargetControl];
        flags[Dirty::RenderTargetControl] = false;

        scale_rating = 0;
        bool any_rescaled = false;
        bool can_rescale = true;
        const auto check_rescale = [&](ImageViewId view_id, ImageId& id_save) {
            if (view_id != NULL_IMAGE_VIEW_ID && view_id != ImageViewId{}) {
                const auto& view = slot_image_views[view_id];
                const auto image_id = view.image_id;
                id_save = image_id;
                auto& image = slot_images[image_id];
                can_rescale &= ImageCanRescale(image);
                any_rescaled |= True(image.flags & ImageFlagBits::Rescaled) ||
                                GetFormatType(image.info.format) != SurfaceType::ColorTexture;
                scale_rating = std::max<u32>(scale_rating, image.scale_tick <= frame_tick
                                                               ? image.scale_rating + 1U
                                                               : image.scale_rating);
            } else {
                id_save = CORRUPT_ID;
            }
        };
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            if (flags[Dirty::ColorBuffer0 + index] || force) {
                flags[Dirty::ColorBuffer0 + index] = false;
                BindRenderTarget(&color_buffer_id, FindColorBuffer(index));
            }
            check_rescale(color_buffer_id, tmp_color_images[index]);
        }
        if (flags[Dirty::ZetaBuffer] || force) {
            flags[Dirty::ZetaBuffer] = false;
            BindRenderTarget(&render_targets.depth_buffer_id, FindDepthBuffer());
        }
        check_rescale(render_targets.depth_buffer_id, tmp_depth_image);

        if (can_rescale) {
            rescaled = any_rescaled || scale_rating >= 2;
            const auto scale_up = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleUp(image);
                }
            };
            if (rescaled) {
                for (size_t index = 0; index < NUM_RT; ++index) {
                    scale_up(tmp_color_images[index]);
                }
                scale_up(tmp_depth_image);
                scale_rating = 2;
            }
        } else {
            rescaled = false;
            const auto scale_down = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleDown(image);
                }
            };
            for (size_t index = 0; index < NUM_RT; ++index) {
                scale_down(tmp_color_images[index]);
            }
            scale_down(tmp_depth_image);
            scale_rating = 1;
        }
    } while (has_deleted_images);
    const auto set_rating = [this, scale_rating](ImageId image_id) {
        if (image_id != CORRUPT_ID) {
            Image& image = slot_images[image_id];
            image.scale_rating = scale_rating;
            if (image.scale_tick <= frame_tick) {
                image.scale_tick = frame_tick + 1;
            }
        }
    };
    for (size_t index = 0; index < NUM_RT; ++index) {
        set_rating(tmp_color_images[index]);
    }
    set_rating(tmp_depth_image);

    return rescaled;
}

template <class P>
void TextureCache<P>::UpdateRenderTargets(bool is_clear) {
    using namespace VideoCommon::Dirty;
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::RenderTargets]) {
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id));
        }
        const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;
        PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id));
        return;
    }

    const VideoCommon::RenderTargets previous_render_targets = render_targets;
    const bool rescaled = RescaleRenderTargets();
    if (is_rescaling != rescaled) {
        flags[Dirty::RescaleViewports] = true;
        flags[Dirty::RescaleScissors] = true;
        is_rescaling = rescaled;
    }

    for (size_t index = 0; index < NUM_RT; ++index) {
        ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
        PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id));
    }
    const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;

    PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id));

    rt_active_mask = 0;
    rt_image_id = {};
    for (size_t i = 0; i < rt_image_id.size(); ++i) {
        if (ImageViewId const view = render_targets.color_buffer_ids[i]; view) {
            rt_active_mask |= 1u << i;
            rt_image_id[i] = slot_image_views[view].image_id;
        }
    }
    if (depth_buffer_id) {
        rt_active_mask |= (1u << NUM_RT);
        rt_depth_image_id = slot_image_views[depth_buffer_id].image_id;
    } else {
        rt_depth_image_id = ImageId{};
    }

    for (size_t index = 0; index < NUM_RT; ++index) {
        render_targets.draw_buffers[index] = static_cast<u8>(maxwell3d->regs.rt_control.Map(index));
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (is_rescaling) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    render_targets.size = Extent2D{
        (maxwell3d->regs.surface_clip.width * up_scale) >> down_shift,
        (maxwell3d->regs.surface_clip.height * up_scale) >> down_shift,
    };
    render_targets.is_rescaled = is_rescaling;

    if (render_targets != previous_render_targets) {
        ++render_targets_serial;
    }

    flags[Dirty::DepthBiasGlobal] = true;
}

template <class P>
typename P::Framebuffer* TextureCache<P>::GetFramebuffer() {
    if (last_framebuffer_id && last_framebuffer_serial == render_targets_serial) {
        return &slot_framebuffers[last_framebuffer_id];
    }
    const FramebufferId framebuffer_id = GetFramebufferId(render_targets);
    last_framebuffer_id = framebuffer_id;
    last_framebuffer_serial = render_targets_serial;
    return &slot_framebuffers[framebuffer_id];
}

template <class P>
ImageViewId TextureCache<P>::VisitImageView(u32 index, bool compute) {
    auto& table = compute ? channel_state->compute_image_table : channel_state->graphics_image_table;
    if (index > table.current_limit) {
        LOG_DEBUG(HW_GPU, "Invalid image view index={}", index);
        return NULL_IMAGE_VIEW_ID;
    }
    auto const map_index = index | (compute ? Common::SlotId::TAGGED_VALUE : 0);
    // Is new (on the tegra engine side)?
    auto const [descriptor, is_new] = table.Read(*gpu_memory, index);
    if (is_new) {
        if (IsValidEntry(*gpu_memory, descriptor)) {
            // Is new (registered view) on the texture cache side?
            const auto [pair, is_new_tc] = channel_state->image_views.try_emplace(descriptor);
            if (is_new_tc)
                pair->second = CreateImageView(descriptor);
            PrepareImageView(pair->second, false, false);
            channel_state->image_view_ids.insert_or_assign(map_index, pair->second);
            return pair->second;
        }
        channel_state->image_view_ids.insert_or_assign(map_index, NULL_IMAGE_VIEW_ID);
        return NULL_IMAGE_VIEW_ID;
    }
    auto const it = channel_state->image_view_ids.find(map_index);
    if (it->second != NULL_IMAGE_VIEW_ID)
        PrepareImageView(it->second, false, false);
    return it->second;
}

template <class P>
FramebufferId TextureCache<P>::GetFramebufferId(const RenderTargets& key) {
    const auto [pair, is_new] = framebuffers.try_emplace(key);
    FramebufferId& framebuffer_id = pair->second;
    if (!is_new) {
        return framebuffer_id;
    }
    std::array<ImageView*, NUM_RT> color_buffers;
    std::ranges::transform(key.color_buffer_ids, color_buffers.begin(), [this](ImageViewId id) {
        return id ? &slot_image_views[id] : nullptr;
    });
    ImageView* const depth_buffer = key.depth_buffer_id ? &slot_image_views[key.depth_buffer_id] : nullptr;
    runtime.FlushDeferredClear();
    framebuffer_id = slot_framebuffers.insert(runtime, color_buffers, depth_buffer, key);
    return framebuffer_id;
}

template <class P>
void TextureCache<P>::WriteMemory(DAddr cpu_addr, size_t size) {
    ForEachImageInRegion(cpu_addr, size, [this](ImageId image_id, Image& image) {
        if (True(image.flags & ImageFlagBits::CpuModified)) {
            return;
        }
        image.flags |= ImageFlagBits::CpuModified;
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, image_id);
        }
    });
}

template <class P>
bool TextureCache<P>::IsDownloadable(const ImageBase& image) const noexcept {
    if (!image.IsSafeGpuCopy()) {
        return false;
    }
    if (image.info.num_samples == 1) {
        return true;
    }
    if constexpr (P::HAS_MSAA_DOWNLOADS) {
        return runtime.CanDownloadMsaa(image.info);
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::DownloadMemory(DAddr cpu_addr, size_t size) {
    boost::container::small_vector<ImageId, 16> images;
    ForEachImageInRegion(cpu_addr, size, [this, &images](ImageId image_id, ImageBase& image) {
        if (!IsDownloadable(image)) {
            return;
        }
        image.flags &= ~ImageFlagBits::GpuModified;
        images.push_back(image_id);
    });
    if (images.empty()) {
        return;
    }
    std::ranges::sort(images, [this](ImageId lhs, ImageId rhs) {
        return slot_images[lhs].modification_tick < slot_images[rhs].modification_tick;
    });
    for (const ImageId image_id : images) {
        Image& image = slot_images[image_id];
        if (image.KleddCanSplitDepthStencil()) {
            KleddDownloadDepthStencil(image);
            continue;
        }
        auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
        const auto copies = FullDownloadCopies(image.info);
        image.DownloadMemory(map, copies);
        runtime.Finish();
        SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span,
                     swizzle_data_buffer);
    }
}

namespace KleddDetail {
/// Depth part of a downloaded texel as 24-bit unorm. Host D32_SFLOAT depth (GPUs without D24S8)
/// arrives as a 32-bit float and is converted; host D24 depth is already in the low 24 bits.
inline u32 KleddDepth24(u32 word, bool depth_float) {
    if (!depth_float) {
        return word & 0x00FFFFFFu;
    }
    float depth;
    std::memcpy(&depth, &word, sizeof(depth));
    if (!(depth > 0.0f)) {
        return 0; // also catches NaN
    }
    if (depth >= 1.0f) {
        return 0x00FFFFFFu;
    }
    return static_cast<u32>(static_cast<double>(depth) * 16777215.0 + 0.5);
}

/// Scaled coordinate of the texel under the centre of a guest texel: floor((guest + 0.5) * scale)
inline u32 KleddScaledCenter(u32 guest, u32 up_scale, u32 down_shift) {
    return (((guest * 2 + 1) * up_scale) >> down_shift) / 2;
}
} // namespace KleddDetail

template <class P>
void TextureCache<P>::KleddDownloadDepthStencil(Image& image) {
    // Download depth (4 bytes/texel, D24 in the low bits) and stencil (1 byte/texel) to separate
    // regions of the staging buffer, then rebuild the guest's packed 32-bit layout on the CPU.
    const size_t depth_bytes = image.unswizzled_size_bytes;
    const size_t split = Common::AlignUp(depth_bytes, 256);
    auto map = runtime.DownloadStagingBuffer(split + depth_bytes / 4 + 256);
    const auto copies = FullDownloadCopies(image.info);
    image.kledd_stencil_split = split;
    image.DownloadMemory(map, copies);
    image.kledd_stencil_split = 0;
    runtime.Finish();

    u8* const base = map.mapped_span.data();
    const bool stencil_low = image.info.format == PixelFormat::S8_UINT_D24_UNORM;
    const bool depth_float = image.KleddDepthIsFloat();
    for (const auto& copy : copies) {
        const u32 width = copy.image_extent.width;
        const u32 height = copy.image_extent.height;
        const u32 depth = copy.image_extent.depth;
        const u32 row = copy.buffer_row_length != 0 ? copy.buffer_row_length : width;
        const u32 rows = copy.buffer_image_height != 0 ? copy.buffer_image_height : height;
        const u32 layers = static_cast<u32>(copy.image_subresource.num_layers);
        for (u32 layer = 0; layer < layers * depth; ++layer) {
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const size_t texel = (static_cast<size_t>(layer) * rows + y) * row + x;
                    u8* const word_ptr = base + copy.buffer_offset + texel * 4;
                    u32 word;
                    std::memcpy(&word, word_ptr, sizeof(word));
                    const u32 d24 = KleddDetail::KleddDepth24(word, depth_float);
                    const u32 s8 = base[split + copy.buffer_offset / 4 + texel];
                    word = stencil_low ? ((d24 << 8) | s8) : (d24 | (s8 << 24));
                    std::memcpy(word_ptr, &word, sizeof(word));
                }
            }
        }
    }
    SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies,
                 map.mapped_span.subspan(0, depth_bytes), swizzle_data_buffer);
    if (Settings::values.kledd_dma_readback_log.GetValue()) {
        LOG_INFO(HW_GPU, "KLEDD ds-download {}x{} fmt={} bytes={}", image.info.size.width,
                 image.info.size.height, static_cast<u32>(image.info.format), depth_bytes);
    }
}

template <class P>
std::optional<VideoCore::RasterizerDownloadArea> TextureCache<P>::GetFlushArea(DAddr cpu_addr,
                                                                               u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> area{};
    ForEachImageInRegion(cpu_addr, size, [&](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return;
        }
        if (!area) {
            area.emplace();
            area->start_address = cpu_addr;
            area->end_address = cpu_addr + size;
            area->preemtive = true;
        }
        area->start_address = (std::min)(area->start_address, image.cpu_addr);
        area->end_address = (std::max)(area->end_address, image.cpu_addr_end);
        for (auto image_view_id : image.image_view_ids) {
            auto& image_view = slot_image_views[image_view_id];
            image_view.flags |= ImageViewFlagBits::PreemtiveDownload;
        }
        area->preemtive &= image.info.forced_flushed;
        image.info.forced_flushed = true;
    });
    return area;
}

template <class P>
void TextureCache<P>::UnmapMemory(DAddr cpu_addr, size_t size) {
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, id);
        }
        UnregisterImage(id);
        DeleteImage(id);
    }
}

template <class P>
void TextureCache<P>::UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size) {
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegionGPU(as_id, gpu_addr, size,
                            [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        CancelPendingUnswizzle(id);
        image.flags &= ~ImageFlagBits::IsDecoding;
        if (False(image.flags & ImageFlagBits::CpuModified)) {
            image.flags |= ImageFlagBits::CpuModified;
            if (True(image.flags & ImageFlagBits::Tracked)) {
                UntrackImage(image, id);
            }
        }
        if (True(image.flags & ImageFlagBits::Remapped)) {
            continue;
        }
        image.flags |= ImageFlagBits::Remapped;
    }
}

template <class P>
bool TextureCache<P>::BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                                const Tegra::Engines::Fermi2D::Surface& src,
                                const Tegra::Engines::Fermi2D::Config& copy) {
    const auto result = GetBlitImages(dst, src, copy);
    if (!result) {
        return false;
    }
    const BlitImages images = *result;
    const ImageId dst_id = images.dst_id;
    const ImageId src_id = images.src_id;

    PrepareImage(src_id, false, false);
    PrepareImage(dst_id, true, false);

    Image& dst_image = slot_images[dst_id];
    Image& src_image = slot_images[src_id];
    bool is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
    bool is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);

    const bool is_resolve = src_image.info.num_samples != 1 && dst_image.info.num_samples == 1;
    if (is_src_rescaled != is_dst_rescaled) {
        if (ImageCanRescale(src_image)) {
            ScaleUp(src_image);
            is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
            if (is_resolve) {
                dst_image.info.rescaleable = true;
                for (const auto& alias : dst_image.aliased_images) {
                    Image& other_image = slot_images[alias.id];
                    other_image.info.rescaleable = true;
                }
            }
        }
        if (ImageCanRescale(dst_image)) {
            ScaleUp(dst_image);
            is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
        }
    }
    if (is_resolve && (is_src_rescaled != is_dst_rescaled)) {
        // A resolve requires both images to be the same dimensions. Resize down if needed.
        ScaleDown(src_image);
        ScaleDown(dst_image);
        is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
        is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
    }
    const auto& resolution = Settings::values.resolution_info;
    const auto scale_region = [&](Region2D& region) {
        region.start.x = resolution.ScaleUp(region.start.x);
        region.start.y = resolution.ScaleUp(region.start.y);
        region.end.x = resolution.ScaleUp(region.end.x);
        region.end.y = resolution.ScaleUp(region.end.y);
    };

    // TODO: Deduplicate
    const std::optional src_base = src_image.TryFindBase(src.Address());
    const SubresourceRange src_range{.base = src_base.value(), .extent = {1, 1}};
    const ImageViewInfo src_view_info(ImageViewType::e2D, images.src_format, src_range);
    const auto [src_framebuffer_id, src_view_id] = RenderTargetFromImage(src_id, src_view_info);
    const auto [src_samples_x, src_samples_y] = SamplesLog2(src_image.info.num_samples);
    Region2D src_region{
        Offset2D{.x = copy.src_x0 >> src_samples_x, .y = copy.src_y0 >> src_samples_y},
        Offset2D{.x = copy.src_x1 >> src_samples_x, .y = copy.src_y1 >> src_samples_y},
    };
    if (is_src_rescaled) {
        scale_region(src_region);
    }

    const std::optional dst_base = dst_image.TryFindBase(dst.Address());
    const SubresourceRange dst_range{.base = dst_base.value(), .extent = {1, 1}};
    const ImageViewInfo dst_view_info(ImageViewType::e2D, images.dst_format, dst_range);
    const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
    const auto [dst_samples_x, dst_samples_y] = SamplesLog2(dst_image.info.num_samples);
    Region2D dst_region{
        Offset2D{.x = copy.dst_x0 >> dst_samples_x, .y = copy.dst_y0 >> dst_samples_y},
        Offset2D{.x = copy.dst_x1 >> dst_samples_x, .y = copy.dst_y1 >> dst_samples_y},
    };
    if (is_dst_rescaled) {
        scale_region(dst_region);
    }

    // Always call this after src_framebuffer_id was queried, as the address might be invalidated.
    Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
    if constexpr (FRAMEBUFFER_BLITS) {
        // OpenGL blits from framebuffers, not images
        Framebuffer* const src_framebuffer = &slot_framebuffers[src_framebuffer_id];
        runtime.BlitFramebuffer(dst_framebuffer, src_framebuffer, dst_region, src_region,
                                copy.filter, copy.operation);
    } else {
        // Vulkan can blit images, but it lacks format reinterpretations
        // Provide a framebuffer in case it's necessary
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        runtime.BlitImage(dst_framebuffer, dst_view, src_view, dst_region, src_region, copy.filter,
                          copy.operation);
    }
    return true;
}

template <class P>
std::pair<typename P::ImageView*, bool> TextureCache<P>::TryFindFramebufferImageView(
    const Tegra::FramebufferConfig& config, DAddr cpu_addr) {
    // TODO: Properly implement this
    const auto it = page_table.find(cpu_addr >> YUZU_PAGEBITS);
    if (it == page_table.end()) {
        return {};
    }
    const auto& image_map_ids = it->second;
    boost::container::small_vector<ImageId, 4> valid_image_ids;
    for (const ImageMapId map_id : image_map_ids) {
        const ImageMapView& map = slot_map_views[map_id];
        const ImageBase& image = slot_images[map.image_id];
        if (image.cpu_addr != cpu_addr) {
            continue;
        }
        if (image.image_view_ids.empty()) {
            continue;
        }
        valid_image_ids.push_back(map.image_id);
    }

    const auto view_format = [&]() {
        switch (config.pixel_format) {
        case Service::android::PixelFormat::Rgb565:
            return PixelFormat::R5G6B5_UNORM;
        case Service::android::PixelFormat::Bgra8888:
            return PixelFormat::B8G8R8A8_UNORM;
        default:
            return PixelFormat::A8B8G8R8_UNORM;
        }
    }();

    const auto GetImageViewForFramebuffer = [&](ImageId image_id) {
        ImageViewInfo info{ImageViewType::e2D, view_format};
        if (config.blending == Tegra::BlendMode::Opaque) {
            info.x_source = static_cast<u8>(SwizzleSource::R);
            info.y_source = static_cast<u8>(SwizzleSource::G);
            info.z_source = static_cast<u8>(SwizzleSource::B);
            info.w_source = static_cast<u8>(SwizzleSource::OneFloat);
        }
        return std::make_pair(&slot_image_views[FindOrEmplaceImageView(image_id, info)],
                              slot_images[image_id].IsRescaled());
    };

    if (valid_image_ids.size() == 1) [[likely]] {
        return GetImageViewForFramebuffer(valid_image_ids.front());
    }

    if (valid_image_ids.size() > 0) [[unlikely]] {
        auto most_recent = std::ranges::max_element(valid_image_ids, [&](auto a, auto b) {
            return slot_images[a].modification_tick < slot_images[b].modification_tick;
        });
        return GetImageViewForFramebuffer(*most_recent);
    }

    return {};
}

template <class P>
bool TextureCache<P>::HasUncommittedFlushes() const noexcept {
    return !uncommitted_downloads.empty();
}

template <class P>
bool TextureCache<P>::ShouldWaitAsyncFlushes() const noexcept {
    return !committed_downloads.empty() && !committed_downloads.front().empty();
}

template <class P>
void TextureCache<P>::CommitAsyncFlushes() {
    // This is intentionally passing the value by copy
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        auto& download_ids = uncommitted_downloads;
        if (download_ids.empty()) {
            committed_downloads.emplace_back(std::move(uncommitted_downloads));
            uncommitted_downloads.clear();
            async_buffers.emplace_back(std::move(uncommitted_async_buffers));
            uncommitted_async_buffers.clear();
            return;
        }
        size_t total_size_bytes = 0;
        size_t last_async_buffer_id = uncommitted_async_buffers.size();
        bool any_none_dma = false;
        for (PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                total_size_bytes +=
                    Common::AlignUp(slot_images[download_info.object_id].unswizzled_size_bytes, 64);
                any_none_dma = true;
                download_info.async_buffer_id = last_async_buffer_id;
            }
        }

        if (any_none_dma) {
            auto download_map = runtime.DownloadStagingBuffer(total_size_bytes, true);
            for (const PendingDownload& download_info : download_ids) {
                if (download_info.is_swizzle) {
                    Image& image = slot_images[download_info.object_id];
                    const auto copies = FullDownloadCopies(image.info);
                    image.DownloadMemory(download_map, copies);
                    download_map.offset += Common::AlignUp(image.unswizzled_size_bytes, 64);
                }
            }
            uncommitted_async_buffers.emplace_back(download_map);
        }

        async_buffers.emplace_back(std::move(uncommitted_async_buffers));
        uncommitted_async_buffers.clear();
    }
    committed_downloads.emplace_back(std::move(uncommitted_downloads));
    uncommitted_downloads.clear();
}

template <class P>
void TextureCache<P>::PopAsyncFlushes() {
    if (committed_downloads.empty()) {
        return;
    }
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            async_buffers.pop_front();
            return;
        }
        auto download_map = std::move(async_buffers.front());
        for (size_t i = download_ids.size(); i > 0; i--) {
            auto& download_info = download_ids[i - 1];
            auto& download_buffer = download_map[download_info.async_buffer_id];
            if (download_info.is_swizzle) {
                const ImageBase& image = slot_images[download_info.object_id];
                const auto copies = FullDownloadCopies(image.info);
                download_buffer.offset -= Common::AlignUp(image.unswizzled_size_bytes, 64);
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, download_span, swizzle_data_buffer);
            } else {
                const BufferDownload& buffer_info = slot_buffer_downloads[download_info.object_id];
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                if (buffer_info.kledd_ds && buffer_info.kledd_scaled) {
                    u8* const base = download_span.data();
                    u8* const out = base + buffer_info.kledd_out_offset;
                    for (u32 y = 0; y < buffer_info.kledd_height; ++y) {
                        const u32 sy = std::clamp(
                            KleddDetail::KleddScaledCenter(buffer_info.kledd_guest_y + y,
                                                           buffer_info.kledd_up_scale,
                                                           buffer_info.kledd_down_shift),
                            buffer_info.kledd_scaled_y,
                            buffer_info.kledd_scaled_y + buffer_info.kledd_src_height - 1) -
                            buffer_info.kledd_scaled_y;
                        for (u32 x = 0; x < buffer_info.kledd_width; ++x) {
                            const u32 sx = std::clamp(
                                KleddDetail::KleddScaledCenter(buffer_info.kledd_guest_x + x,
                                                               buffer_info.kledd_up_scale,
                                                               buffer_info.kledd_down_shift),
                                buffer_info.kledd_scaled_x,
                                buffer_info.kledd_scaled_x + buffer_info.kledd_src_width - 1) -
                                buffer_info.kledd_scaled_x;
                            const size_t src_texel =
                                static_cast<size_t>(sy) * buffer_info.kledd_src_width + sx;
                            u32 word;
                            std::memcpy(&word, base + src_texel * 4, sizeof(word));
                            const u32 d24 =
                                KleddDetail::KleddDepth24(word, buffer_info.kledd_depth_float);
                            const u32 s8 = base[buffer_info.kledd_split + src_texel];
                            word = buffer_info.kledd_stencil_low ? ((d24 << 8) | s8)
                                                                 : (d24 | (s8 << 24));
                            const size_t texel =
                                static_cast<size_t>(y) * buffer_info.kledd_row + x;
                            std::memcpy(out + texel * 4, &word, sizeof(word));
                        }
                    }
                    gpu_memory->WriteBlock(buffer_info.address, out, buffer_info.size);
                } else if (buffer_info.kledd_ds) {
                    u8* const base = download_span.data();
                    for (u32 y = 0; y < buffer_info.kledd_height; ++y) {
                        for (u32 x = 0; x < buffer_info.kledd_width; ++x) {
                            const size_t texel =
                                static_cast<size_t>(y) * buffer_info.kledd_row + x;
                            u8* const word_ptr = base + texel * 4;
                            u32 word;
                            std::memcpy(&word, word_ptr, sizeof(word));
                            const u32 d24 =
                                KleddDetail::KleddDepth24(word, buffer_info.kledd_depth_float);
                            const u32 s8 = base[buffer_info.kledd_split + texel];
                            word = buffer_info.kledd_stencil_low ? ((d24 << 8) | s8)
                                                                 : (d24 | (s8 << 24));
                            std::memcpy(word_ptr, &word, sizeof(word));
                        }
                    }
                    gpu_memory->WriteBlock(buffer_info.address, download_span.data(),
                                           buffer_info.size);
                } else {
                    gpu_memory->WriteBlockUnsafe(buffer_info.address, download_span.data(),
                                                 buffer_info.size);
                }
                slot_buffer_downloads.erase(download_info.object_id);
            }
        }
        for (auto& download_buffer : download_map) {
            async_buffers_death_ring.emplace_back(download_buffer);
        }
        committed_downloads.pop_front();
        async_buffers.pop_front();
    } else {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            return;
        }
        size_t total_size_bytes = 0;
        for (const PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                total_size_bytes += slot_images[download_info.object_id].unswizzled_size_bytes;
            }
        }
        auto download_map = runtime.DownloadStagingBuffer(total_size_bytes);
        const size_t original_offset = download_map.offset;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle) {
                continue;
            }
            Image& image = slot_images[download_info.object_id];
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(download_map, copies);
            download_map.offset += image.unswizzled_size_bytes;
        }
        // Wait for downloads to finish
        runtime.Finish();
        download_map.offset = original_offset;
        std::span<u8> download_span = download_map.mapped_span;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle) {
                continue;
            }
            const ImageBase& image = slot_images[download_info.object_id];
            const auto copies = FullDownloadCopies(image.info);
            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, download_span, swizzle_data_buffer);
            download_map.offset += image.unswizzled_size_bytes;
            download_span = download_span.subspan(image.unswizzled_size_bytes);
        }
        committed_downloads.pop_front();
    }
}

template <class P>
ImageId TextureCache<P>::DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload) {
    const ImageInfo dst_info(operand);
    kledd_last_dma = {};
    const ImageId dst_id = FindDMAImage(dst_info, operand.address);
    if (!dst_id) {
        kledd_last_dma.reason = 1;
        return NULL_IMAGE_ID;
    }
    auto& image = slot_images[dst_id];
    kledd_last_dma.image_index = dst_id.index;
    kledd_last_dma.flags = static_cast<u32>(image.flags);
    kledd_last_dma.width = image.info.size.width;
    kledd_last_dma.height = image.info.size.height;
    kledd_last_dma.format = static_cast<u32>(image.info.format);
    kledd_last_dma.tick = image.modification_tick;
    // eden-kledd: the scan readback fix is always on, so readbacks always take the GPU path
    const bool kledd_fix = !is_upload;
    if (kledd_fix && True(image.flags & ImageFlagBits::CpuModified)) {
        // Guest memory is newer than the host image: let the CPU path copy it.
        kledd_last_dma.reason = 6;
        return NULL_IMAGE_ID;
    }
    if (!kledd_fix && False(image.flags & ImageFlagBits::GpuModified)) {
        // No need to waste time on an image that's synced with guest
        kledd_last_dma.reason = 2;
        return NULL_IMAGE_ID;
    }
    if (image.info.type == ImageType::e3D) {
        // Don't accelerate 3D images.
        kledd_last_dma.reason = 3;
        return NULL_IMAGE_ID;
    }
    if (!kledd_fix && !is_upload && !image.info.dma_downloaded) {
        // Force a full sync.
        image.info.dma_downloaded = true;
        kledd_last_dma.reason = 4;
        return NULL_IMAGE_ID;
    }
    const auto base = image.TryFindBase(operand.address);
    if (!base) {
        kledd_last_dma.reason = 5;
        return NULL_IMAGE_ID;
    }
    return dst_id;
}

template <class P>
bool TextureCache<P>::IsRescaling() const noexcept {
    return is_rescaling;
}

template <class P>
bool TextureCache<P>::IsRescaling(const ImageViewBase& image_view) const noexcept {
    if (image_view.type == ImageViewType::Buffer) {
        return false;
    }
    const ImageBase& image = slot_images[image_view.image_id];
    return True(image.flags & ImageFlagBits::Rescaled);
}

template <class P>
bool TextureCache<P>::IsRegionGpuModified(DAddr addr, size_t size) {
    bool is_modified = false;
    ForEachImageInRegion(addr, size, [&is_modified](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return false;
        }
        is_modified = true;
        return true;
    });
    return is_modified;
}

template <class P>
std::pair<typename TextureCache<P>::Image*, BufferImageCopy> TextureCache<P>::DmaBufferImageCopy(
    const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
    const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image) {
    const auto [level, base] = PrepareDmaImage(image_id, image_operand.address, modifies_image);
    auto* image = &slot_images[image_id];
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    const u32 bpp = VideoCore::Surface::BytesPerBlock(image->info.format);
    const auto convert = [old_bpp = image_operand.bytes_per_pixel, bpp](u32 value) {
        return (old_bpp * value) / bpp;
    };
    const u32 base_x = convert(image_operand.params.origin.x.Value());
    const u32 base_y = image_operand.params.origin.y.Value();
    const u32 length_x = convert(copy_info.length_x);
    const u32 length_y = copy_info.length_y;

    const BufferImageCopy copy{
        .buffer_offset = 0,
        .buffer_size = buffer_size,
        .buffer_row_length = convert(buffer_operand.pitch),
        .buffer_image_height = buffer_operand.height,
        .image_subresource =
            {
                .base_level = static_cast<s32>(level),
                .base_layer = static_cast<s32>(base),
                .num_layers = 1,
            },
        .image_offset =
            {
                .x = static_cast<s32>(base_x),
                .y = static_cast<s32>(base_y),
                .z = 0,
            },
        .image_extent =
            {
                .width = length_x,
                .height = length_y,
                .depth = 1,
            },
    };
    return {image, copy};
}

template <class P>
bool TextureCache<P>::KleddDownloadDepthStencilToGuest(Image* image, const BufferImageCopy& copy,
                                                       GPUVAddr address, size_t size) {
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        if (!image->KleddCanSplitDepthStencil()) {
            return false;
        }
        const u32 row =
            copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
        // If the image is rescaled, copy the patch at its scaled size straight from the scaled
        // image; it is point-sampled down to the guest size when the download lands.
        const auto& resolution = Settings::values.resolution_info;
        const u32 x = static_cast<u32>(copy.image_offset.x);
        const u32 y = static_cast<u32>(copy.image_offset.y);
        const u32 scaled_x = resolution.ScaleUp(x);
        const u32 scaled_y = resolution.ScaleUp(y);
        const u32 src_width = resolution.ScaleUp(x + copy.image_extent.width) - scaled_x;
        const u32 src_height = resolution.ScaleUp(y + copy.image_extent.height) - scaled_y;
        const bool scaled = True(image->flags & ImageFlagBits::Rescaled) && resolution.active &&
                            src_width != 0 && src_height != 0;

        BufferDownload download{address, size};
        BufferImageCopy src_copy = copy;
        size_t depth_bytes = size;
        if (scaled) {
            depth_bytes = static_cast<size_t>(src_width) * src_height * 4;
            src_copy.buffer_offset = 0;
            src_copy.buffer_size = depth_bytes;
            src_copy.buffer_row_length = 0;
            src_copy.buffer_image_height = 0;
            src_copy.image_offset.x = static_cast<s32>(scaled_x);
            src_copy.image_offset.y = static_cast<s32>(scaled_y);
            src_copy.image_extent.width = src_width;
            src_copy.image_extent.height = src_height;
            download.kledd_scaled = true;
            download.kledd_src_width = src_width;
            download.kledd_src_height = src_height;
            download.kledd_guest_x = x;
            download.kledd_guest_y = y;
            download.kledd_scaled_x = scaled_x;
            download.kledd_scaled_y = scaled_y;
            download.kledd_up_scale = resolution.up_scale;
            download.kledd_down_shift = resolution.down_shift;
        }
        const size_t split = Common::AlignUp(depth_bytes, 256);
        const size_t out_offset = Common::AlignUp(split + depth_bytes / 4, 256);
        auto staging = runtime.DownloadStagingBuffer(out_offset + (scaled ? size : 0) + 256, true);
        std::array buffers{staging.buffer};
        std::array<size_t, 1> offsets{static_cast<size_t>(staging.offset)};
        image->kledd_stencil_split = split;
        image->kledd_copy_scaled = scaled;
        image->DownloadMemory(buffers, offsets, std::span<const BufferImageCopy>(&src_copy, 1));
        image->kledd_copy_scaled = false;
        image->kledd_stencil_split = 0;

        download.kledd_ds = true;
        download.kledd_stencil_low = image->info.format == PixelFormat::S8_UINT_D24_UNORM;
        download.kledd_depth_float = image->KleddDepthIsFloat();
        download.kledd_split = split;
        download.kledd_width = copy.image_extent.width;
        download.kledd_height = copy.image_extent.height;
        download.kledd_row = row;
        download.kledd_out_offset = out_offset;
        auto slot = slot_buffer_downloads.insert(download);
        const PendingDownload pending{false, uncommitted_async_buffers.size(), slot};
        uncommitted_downloads.emplace_back(pending);
        uncommitted_async_buffers.emplace_back(staging);
        if (Settings::values.kledd_dma_readback_log.GetValue()) {
            LOG_INFO(HW_GPU, "KLEDD ds-dma {}x{} row={} dst={:#x} size={} depth={} scaled={}x{}",
                     copy.image_extent.width, copy.image_extent.height, row, address, size,
                     download.kledd_depth_float ? "f32" : "d24", download.kledd_src_width,
                     download.kledd_src_height);
        }
        return true;
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::DownloadImageIntoBuffer(typename TextureCache<P>::Image* image,
                                              typename TextureCache<P>::BufferType buffer,
                                              size_t buffer_offset,
                                              std::span<const VideoCommon::BufferImageCopy> copies,
                                              GPUVAddr address, size_t size) {
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const BufferDownload new_buffer_download{address, size};
        auto slot = slot_buffer_downloads.insert(new_buffer_download);
        const PendingDownload new_download{false, uncommitted_async_buffers.size(), slot};
        uncommitted_downloads.emplace_back(new_download);
        auto download_map = runtime.DownloadStagingBuffer(size, true);
        uncommitted_async_buffers.emplace_back(download_map);
        std::array buffers{
            buffer,
            download_map.buffer,
        };
        std::array<size_t, 2> buffer_offsets{
            buffer_offset,
            download_map.offset,
        };
        image->DownloadMemory(buffers, buffer_offsets, copies);
    } else {
        image->DownloadMemory(buffer, buffer_offset, copies);
    }
}

template <class P>
void TextureCache<P>::RefreshContents(Image& image, ImageId image_id) {
    if (False(image.flags & ImageFlagBits::CpuModified)) {
        // Only upload modified images
        return;
    }

    image.flags &= ~ImageFlagBits::CpuModified;

    TrackImage(image, image_id);

    if (image.info.num_samples > 1 && !runtime.CanUploadMSAA()) {
        LOG_WARNING(HW_GPU, "MSAA image uploads are not implemented");
        runtime.TransitionImageLayout(image);
        return;
    }
    auto& kledd_stats = Common::Kledd::GetFrameStats();
    if (True(image.flags & ImageFlagBits::AsynchronousDecode)) {
        kledd_stats.async_decodes.fetch_add(1, std::memory_order_relaxed);
        QueueAsyncDecode(image, image_id);
        return;
    }

    if (True(image.flags & ImageFlagBits::SparseUpload)) {
        kledd_stats.async_unswizzles.fetch_add(1, std::memory_order_relaxed);
        if ( async_unswizzle_mode == Settings::AsyncUnswizzleMode::Off ) {
            ProcessSparseTexture(image, image_id);
        }
        else {
            QueueAsyncUnswizzle(image, image_id);
        }
        return;
    }
    kledd_stats.uploads.fetch_add(1, std::memory_order_relaxed);
    kledd_stats.upload_bytes.fetch_add(MapSizeBytes(image), std::memory_order_relaxed);

    auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
    UploadImageContents(image, staging);
    runtime.InsertUploadMemoryBarrier();
}

template <class P>
template <typename StagingBuffer>
void TextureCache<P>::UploadImageContents(Image& image, StagingBuffer& staging) {
    const std::span<u8> mapped_span = staging.mapped_span;
    const GPUVAddr gpu_addr = image.gpu_addr;

    if (True(image.flags & ImageFlagBits::AcceleratedUpload)) {
        gpu_memory->ReadBlock(gpu_addr, mapped_span.data(), mapped_span.size_bytes(),
                              VideoCommon::CacheType::NoTextureCache);
        const auto uploads = FullUploadSwizzles(image.info);
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0, 0, {}, false);
        return;
    }

    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> swizzle_data(
        *gpu_memory, gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);
    if (True(image.flags & ImageFlagBits::Converted)) {
        unswizzle_data_buffer.resize_destructive(image.unswizzled_size_bytes);
        auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, unswizzle_data_buffer);
        ConvertImage(unswizzle_data_buffer, image.info, mapped_span, copies);
        image.UploadMemory(staging, copies);
    } else {
        const auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data, mapped_span);
        image.UploadMemory(staging, copies);
    }
}

template <class P>
ImageViewId TextureCache<P>::CreateImageView(const TICEntry& config) {
    const ImageInfo info(config);
    if (info.type == ImageType::Buffer) {
        const ImageViewInfo view_info(config, 0);
        return slot_image_views.insert(runtime, info, view_info, config.Address());
    }
    const u32 layer_offset = config.BaseLayer() * info.layer_stride;
    const GPUVAddr image_gpu_addr = config.Address() - layer_offset;
    const ImageId image_id = FindOrInsertImage(info, image_gpu_addr);
    if (!image_id) {
        return NULL_IMAGE_VIEW_ID;
    }
    ImageBase& image = slot_images[image_id];
    const SubresourceBase base = image.TryFindBase(config.Address()).value();
    ASSERT(base.level == 0);
    const ImageViewInfo view_info(config, base.layer);
    const ImageViewId image_view_id = FindOrEmplaceImageView(image_id, view_info);
    ImageViewBase& image_view = slot_image_views[image_view_id];
    image_view.flags |= ImageViewFlagBits::Strong;
    image.flags |= ImageFlagBits::Strong;
    return image_view_id;
}

template <class P>
ImageId TextureCache<P>::FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                           RelaxedOptions options) {
    if (const ImageId image_id = FindImage(info, gpu_addr, options); image_id) {
        return image_id;
    }
    return InsertImage(info, gpu_addr, options);
}

template <class P>
ImageId TextureCache<P>::FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                   RelaxedOptions options) {
    std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
        if (!cpu_addr) {
            return ImageId{};
        }
    }
    const bool broken_views =
        runtime.HasBrokenTextureViewFormats() || True(options & RelaxedOptions::ForceBrokenViews);
    const bool native_bgr = runtime.HasNativeBgr();
    const bool flexible_formats = True(options & RelaxedOptions::Format);
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = False(options & RelaxedOptions::Size) &&
                                     True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, broken_views, native_bgr)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return !flexible_formats && existing.format == info.format;
            }
        } else if (IsSubresource(info, existing_image, gpu_addr, options, broken_views,
                                 native_bgr)) {
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return !flexible_formats && existing_image.info.format == info.format;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    kledd_last_dma.candidates = static_cast<u32>(image_ids.size());
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
bool TextureCache<P>::ImageCanRescale(ImageBase& image) {
    if (!image.info.rescaleable) {
        return false;
    }
    if (Settings::values.resolution_info.downscale && !image.info.downscaleable) {
        return false;
    }
    if (True(image.flags & (ImageFlagBits::Rescaled | ImageFlagBits::CheckingRescalable))) {
        return true;
    }
    if (True(image.flags & ImageFlagBits::IsRescalable)) {
        return true;
    }
    image.flags |= ImageFlagBits::CheckingRescalable;
    for (const auto& alias : image.aliased_images) {
        Image& other_image = slot_images[alias.id];
        if (!ImageCanRescale(other_image)) {
            image.flags &= ~ImageFlagBits::CheckingRescalable;
            return false;
        }
    }
    image.flags &= ~ImageFlagBits::CheckingRescalable;
    image.flags |= ImageFlagBits::IsRescalable;
    return true;
}

template <class P>
void TextureCache<P>::InvalidateScale(Image& image) {
    if (image.scale_tick <= frame_tick) {
        image.scale_tick = frame_tick + 1;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);
    for (const ImageViewId image_view_id : image_view_ids) {
        sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        slot_image_views.erase(image_view_id);
    }
    image.image_view_ids.clear();
    image.image_view_infos.clear();
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];

        if constexpr (ENABLE_VALIDATION)
            for (auto& e : channel_info.image_view_ids)
                e.second = CORRUPT_ID;
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    has_deleted_images = true;
}

template <class P>
u64 TextureCache<P>::GetScaledImageSizeBytes(const ImageBase& image) {
    const u64 scale_up = static_cast<u64>(Settings::values.resolution_info.up_scale *
                                          Settings::values.resolution_info.up_scale);
    const u64 down_shift = static_cast<u64>(Settings::values.resolution_info.down_shift +
                                            Settings::values.resolution_info.down_shift);
    const u64 image_size_bytes =
        static_cast<u64>((std::max)(image.guest_size_bytes, image.unswizzled_size_bytes));
    const u64 tentative_size = (image_size_bytes * scale_up) >> down_shift;
    const u64 fitted_size = Common::AlignUp(tentative_size, 1024);
    return fitted_size;
}

template <class P>
void TextureCache<P>::QueueAsyncDecode(Image& image, ImageId image_id) {
    UNIMPLEMENTED_IF(False(image.flags & ImageFlagBits::Converted));
    LOG_INFO(HW_GPU, "Queuing async texture decode");

    image.flags |= ImageFlagBits::IsDecoding;
    auto decode = std::make_unique<AsyncDecodeContext>();
    auto* decode_ptr = decode.get();
    decode->image_id = image_id;
    async_decodes.push_back(std::move(decode));

    std::vector<u8> local_unswizzle_data_buffer(image.unswizzled_size_bytes, 0);
    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> swizzle_data(*gpu_memory, image.gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);
    auto copies = UnswizzleImage(*gpu_memory, image.gpu_addr, image.info, swizzle_data, local_unswizzle_data_buffer);
    const size_t out_size = MapSizeBytes(image);

    auto func = [out_size, copies, info = image.info,
                 input = std::move(local_unswizzle_data_buffer),
                 async_decode = decode_ptr]() mutable {
        async_decode->decoded_data.resize_destructive(out_size);
        std::span copies_span{copies.data(), copies.size()};
        ConvertImage(input, info, async_decode->decoded_data, copies_span);

        // TODO: Do we need this lock?
        std::unique_lock lock{async_decode->mutex};
        async_decode->copies = std::move(copies);
        async_decode->complete = true;
    };
    texture_decode_worker.QueueWork(std::move(func));
}

template <class P>
void TextureCache<P>::QueueAsyncUnswizzle(Image& image, ImageId image_id) {
    if (True(image.flags & ImageFlagBits::IsDecoding)) {
        return;
    }

    if (unswizzle_queue.size() >= MAX_ASYNC_UNSWIZZLE_TASKS) {
        EvictOldestUnswizzleTask();
    }

    image.flags |= ImageFlagBits::IsDecoding;
    image.flags |= ImageFlagBits::AcceleratedUpload;

    unswizzle_queue.push_back({
        .info = image.info,
        .image_id = image_id,
        .is_cpu = async_unswizzle_mode == Settings::AsyncUnswizzleMode::Cpu,
    });
}

template <class P>
void TextureCache<P>::EvictOldestUnswizzleTask() {
    if (unswizzle_queue.empty()) {
        return;
    }
    const ImageId task_image_id = unswizzle_queue.front().image_id;
    Image& image = slot_images[task_image_id];
    CancelPendingUnswizzle(task_image_id);
    image.flags &= ~ImageFlagBits::IsDecoding;

    if (False(image.flags & ImageFlagBits::CpuModified)) {
        image.flags |= ImageFlagBits::CpuModified;
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, task_image_id);
        }
    }
}

template <class P>
void TextureCache<P>::ProcessSparseTexture(Image& image, ImageId image_id) {
    if (True(image.flags & ImageFlagBits::IsDecoding)) {
        return;
    }

    image.flags |= ImageFlagBits::IsDecoding;
    image.flags |= ImageFlagBits::AcceleratedUpload;

    PendingUnswizzle local_task{
        .info = image.info,
        .image_id = image_id,
        .is_cpu = false,
    };

    while (True(image.flags & ImageFlagBits::IsDecoding)) {
        TickAsyncUnswizzleGpu(local_task, image);
    }
}

template <class P>
void TextureCache<P>::TickAsyncDecode() {
    bool has_uploads{};
    auto i = async_decodes.begin();
    while (i != async_decodes.end()) {
        auto* async_decode = i->get();
        std::unique_lock lock{async_decode->mutex};
        if (!async_decode->complete) {
            ++i;
            continue;
        }
        Image& image = slot_images[async_decode->image_id];
        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
        std::memcpy(staging.mapped_span.data(), async_decode->decoded_data.data(),
                    async_decode->decoded_data.size());
        image.UploadMemory(staging, async_decode->copies);
        image.flags &= ~ImageFlagBits::IsDecoding;
        has_uploads = true;
        i = async_decodes.erase(i);
    }
    if (has_uploads) {
        runtime.InsertUploadMemoryBarrier();
    }
}

template <class P>
void TextureCache<P>::TickAsyncUnswizzle() {
    if (unswizzle_queue.empty()) {
        return;
    }

    const auto frame_deadline = Common::SteadyClock::Now() + async_unswizzle_frame_budget;

    do {
        PendingUnswizzle& task = unswizzle_queue.front();
        const ImageId task_image_id = task.image_id;
        Image& image = slot_images[task.image_id];

        if (task.is_cpu) {
            TickAsyncUnswizzleCpu(task, image);
        } else {
            TickAsyncUnswizzleGpu(task, image);
        }

        if (!unswizzle_queue.empty() && unswizzle_queue.front().image_id == task_image_id) {
            std::rotate(unswizzle_queue.begin(), unswizzle_queue.begin() + 1,
                        unswizzle_queue.end());
        }
    } while (!unswizzle_queue.empty() &&
             Common::SteadyClock::Now() < frame_deadline);
}

template <class P>
void TextureCache<P>::InitSparseUnswizzleTracking(PendingUnswizzle& task, Image& image) {
    const u32 total_slices_init = image.info.size.depth;

    task.is_sparse = True(image.flags & ImageFlagBits::Sparse);
    task.active_z_start = 0;
    task.active_z_end = total_slices_init;
    if (!task.is_sparse) {
        return;
    }

    const auto uploads = FullUploadSwizzles(task.info);
    const auto sp = VideoCommon::Accelerated::MakeBlockLinearSwizzle3DParams(uploads[0], task.info);
    task.swizzled_slice_size = sp.slice_size;
    task.swizzle_block_depth = sp.block_depth;

    const auto segs = gpu_memory->GetSubmappedRange(image.gpu_addr, image.guest_size_bytes);
    task.sparse_segments.assign(segs.begin(), segs.end());
    task.segment_scan_cursor = 0;

    task.slice_has_data.assign(image.info.size.depth, 0u);

    if (image.info.size.depth > 1 && !image.slice_offsets.empty()) {
        const u32 depth = static_cast<u32>(image.info.size.depth);
        const u32 group_slices = 1u << task.swizzle_block_depth;
        const u64 group_byte_size = task.swizzled_slice_size;

        size_t seg_idx = 0;
        for (u32 g_start = 0; g_start < depth; g_start += group_slices) {
            const u32 g_end = (std::min)(g_start + group_slices, depth);
            const u64 group_abs_start = image.slice_offsets[g_start];
            const u64 group_abs_end =
                (g_end < image.slice_offsets.size())
                    ? static_cast<u64>(image.slice_offsets[g_end])
                    : group_abs_start + group_byte_size;

            while (seg_idx < task.sparse_segments.size()) {
                const auto& [seg_gpu_addr, seg_size] = task.sparse_segments[seg_idx];
                const u64 seg_start = seg_gpu_addr - image.gpu_addr;
                const u64 seg_end = seg_start + seg_size;
                if (seg_end <= group_abs_start) {
                    ++seg_idx;
                    continue;
                }
                break;
            }

            bool group_has_data = false;
            for (size_t probe = seg_idx; probe < task.sparse_segments.size(); ++probe) {
                const auto& [seg_gpu_addr, seg_size] = task.sparse_segments[probe];
                const u64 seg_start = seg_gpu_addr - image.gpu_addr;
                const u64 seg_end = seg_start + seg_size;
                if (seg_start >= group_abs_end) break;
                if (seg_end > group_abs_start && seg_start < group_abs_end) {
                    group_has_data = true;
                    break;
                }
            }

            if (group_has_data) {
                for (u32 z = g_start; z < g_end; ++z) {
                    task.slice_has_data[z] = 1u;
                }
            }
        }
    } else {
        std::fill(task.slice_has_data.begin(), task.slice_has_data.end(), 1u);
    }

    if (async_unswizzle_slices_per_batch == 0) {
        u32 first_populated = total_slices_init;
        u32 last_populated_excl = 0;
        for (u32 z = 0; z < total_slices_init; ++z) {
            if (task.slice_has_data[z]) {
                first_populated = (std::min)(first_populated, z);
                last_populated_excl = z + 1;
            }
        }
        if (first_populated < last_populated_excl) {
            const u32 group_slices_align = (std::max)(1u << task.swizzle_block_depth, 1u);
            task.active_z_start = (first_populated / group_slices_align) * group_slices_align;
            task.active_z_end = (std::min)(
                Common::AlignUp(last_populated_excl, group_slices_align), total_slices_init);
        } else {
            task.active_z_start = 0;
            task.active_z_end = 0;
        }
    }
}

template <class P>
void TextureCache<P>::ReadSparseCoalesced(PendingUnswizzle& task, Image& image, u8* staging_base,
                                           size_t staging_base_abs_offset, size_t read_start,
                                           size_t read_end) {
    // This needs tuning as its just a number I pulled from out of nowhere
    const size_t max_gap_for_batch = 4096;

    const auto& sparse_segments = task.sparse_segments;
    size_t zero_cursor = read_start;
    while (task.segment_scan_cursor < sparse_segments.size()) {
        const auto& [seg_gpu_addr, seg_size] = sparse_segments[task.segment_scan_cursor];
        const size_t seg_abs_start = static_cast<size_t>(seg_gpu_addr - image.gpu_addr);
        const size_t seg_abs_end = seg_abs_start + seg_size;

        if (seg_abs_end <= read_start) { ++task.segment_scan_cursor; continue; }
        if (seg_abs_start >= read_end) { break; }

        const size_t ol_abs_start = (std::max)(seg_abs_start, read_start);

        size_t batch_abs_end = (std::min)(seg_abs_end, read_end);
        size_t batch_cursor = task.segment_scan_cursor + 1;

        while (batch_cursor < sparse_segments.size()) {
            const auto& [next_seg_addr, next_seg_size] = sparse_segments[batch_cursor];
            const size_t next_seg_abs_start = static_cast<size_t>(next_seg_addr - image.gpu_addr);
            const size_t next_seg_abs_end = next_seg_abs_start + next_seg_size;

            if (next_seg_abs_start >= read_end) break;
            if (next_seg_abs_end <= read_start) { ++batch_cursor; continue; }

            const size_t gap = next_seg_abs_start - batch_abs_end;
            if (gap > max_gap_for_batch) break;

            batch_abs_end = (std::min)(next_seg_abs_end, read_end);
            ++batch_cursor;
        }

        const size_t batch_size = batch_abs_end - ol_abs_start;
        const size_t buffer_ol_rel_start = ol_abs_start - staging_base_abs_offset;

        if (ol_abs_start > zero_cursor) {
            const size_t gap_rel_start = zero_cursor - staging_base_abs_offset;
            const size_t gap_size = ol_abs_start - zero_cursor;
            std::memset(staging_base + gap_rel_start, 0, gap_size);
        }
        zero_cursor = batch_abs_end;

        gpu_memory->ReadBlockUnsafe(image.gpu_addr + ol_abs_start, staging_base + buffer_ol_rel_start,
                                    batch_size);

        if (seg_abs_end > read_end) break;
        task.segment_scan_cursor = batch_cursor;
    }
    if (zero_cursor < read_end) {
        const size_t gap_rel_start = zero_cursor - staging_base_abs_offset;
        const size_t gap_size = read_end - zero_cursor;
        std::memset(staging_base + gap_rel_start, 0, gap_size);
    }
}

template <class P>
void TextureCache<P>::TickAsyncUnswizzleGpu(PendingUnswizzle& task, Image& image) {
    if (!task.initialized) {
        task.total_size = MapSizeBytes(image);
        task.bytes_per_block = BytesPerBlock(task.info.format);

        const auto& info = image.info;
        const u32 bytes_per_block = task.bytes_per_block;
        const u32 width_blocks = Common::DivCeil(info.size.width, 4u);
        const u32 height_blocks = Common::DivCeil(info.size.height, 4u);
        task.bytes_per_slice = static_cast<size_t>(width_blocks * bytes_per_block) * height_blocks;
        task.last_submitted_slice = 0;

        InitSparseUnswizzleTracking(task, image);

        size_t max_batch_size = task.total_size;
        {
            const u32 batch_slices = (async_unswizzle_slices_per_batch == 0)
                ? (task.active_z_end - task.active_z_start)
                : async_unswizzle_slices_per_batch;

            size_t calculated_max = 0;
            for (u32 z = task.active_z_start; z < task.active_z_end; z += (std::max)(batch_slices, 1u)) {
                const u32 z_want_init = (std::min)(z + (std::max)(batch_slices, 1u), task.active_z_end);
                const u32 z_count_init = z_want_init - z;

                const size_t group_off = (z < image.slice_offsets.size())
                    ? static_cast<size_t>(image.slice_offsets[z]) : 0;
                const size_t group_end = (z + z_count_init < image.slice_offsets.size())
                    ? static_cast<size_t>(image.slice_offsets[z + z_count_init]) : task.total_size;

                calculated_max = (std::max)(calculated_max, group_end - group_off);
            }
            if (calculated_max > 0 && calculated_max < max_batch_size) {
                max_batch_size = calculated_max;
            }
            if (task.active_z_start >= task.active_z_end) {
                max_batch_size = (std::min)(max_batch_size, task.bytes_per_slice);
            }

            if (task.active_z_start > 0 && task.active_z_start < image.slice_offsets.size()) {
                task.current_offset =
                    static_cast<size_t>(image.slice_offsets[task.active_z_start]);
                task.last_submitted_slice = task.active_z_start;
            }
            task.current_batch_start_byte = task.current_offset;
        }

        const size_t needed = (std::max)(max_batch_size, size_t{1});
        task.staging_buffer = runtime.UploadStagingBuffer(needed, true);

        const auto uploads_init = FullUploadSwizzles(task.info);
        task.upload_swizzles.assign(uploads_init.begin(), uploads_init.end());
        task.initialized = true;
    }

    const size_t active_end_byte = (task.active_z_end < image.slice_offsets.size())
                                       ? static_cast<size_t>(image.slice_offsets[task.active_z_end]) : task.total_size;

    const u32 total_slices = image.info.size.depth;
    const u32 effective_end = (async_unswizzle_slices_per_batch == 0 && task.is_sparse)
        ? task.active_z_end
        : total_slices;
    const u32 batch =
        (async_unswizzle_slices_per_batch == 0 ? effective_end : async_unswizzle_slices_per_batch);

    const u32 z_start = task.last_submitted_slice;
    const u32 z_want = (std::min)(z_start + batch, effective_end);

    const size_t current_group_end_byte = (z_want < image.slice_offsets.size())
                                            ? static_cast<size_t>(image.slice_offsets[z_want])
                                            : active_end_byte;

    if (task.current_offset < active_end_byte) {
        const size_t batch_capacity_end_byte = (std::min)(
            task.current_batch_start_byte + task.staging_buffer.mapped_span.size(),
            (std::min)(active_end_byte, current_group_end_byte)
        );

        if (task.current_offset < batch_capacity_end_byte) {
            const size_t remaining = batch_capacity_end_byte - task.current_offset;
            const size_t copy_amount = remaining;

            u8* const staging_base = task.staging_buffer.mapped_span.data();

            const size_t read_start = task.current_offset;
            const size_t read_end   = read_start + copy_amount;

            if (task.is_sparse) {
                ReadSparseCoalesced(task, image, staging_base, task.current_batch_start_byte,
                                    read_start, read_end);
            } else {
                gpu_memory->ReadBlockUnsafe(image.gpu_addr + read_start,
                                            staging_base + (read_start - task.current_batch_start_byte),
                                            copy_amount);
            }
            task.current_offset += copy_amount;
        }
    }

    const bool is_final_batch = task.current_offset >= active_end_byte;

    const std::span<const u8> sparse_hint =
        task.is_sparse ? std::span<const u8>(task.slice_has_data)
                       : std::span<const u8>{};

    if (task.current_offset >= current_group_end_byte && z_want > z_start) {
        const u32 z_count = z_want - z_start;
        const u32 z_src   = z_start;
        const u32 z_image = z_src; // + 0 for full tasks
        const bool image_already_uploaded = task.already_uploaded;

        runtime.AccelerateImageUpload(image, task.staging_buffer,
                                      task.upload_swizzles,
                                      z_src, z_image, z_count,
                                      sparse_hint,
                                      image_already_uploaded);
        task.already_uploaded = true;

        task.last_submitted_slice = z_want;
        task.current_batch_start_byte = current_group_end_byte;
    }

    // Check if complete
    const bool all_submitted = task.last_submitted_slice >= effective_end ||
                               (is_final_batch && task.last_submitted_slice >= task.active_z_end);

    if (is_final_batch && all_submitted) {
        runtime.FreeDeferredStagingBuffer(task.staging_buffer);
        runtime.ReleaseSparseUnswizzleBuffer(image);
        image.flags &= ~ImageFlagBits::IsDecoding;

        if (!unswizzle_queue.empty() && unswizzle_queue.front().image_id == task.image_id) {
            unswizzle_queue.erase(unswizzle_queue.begin());
        }
    }
}

template <class P>
void TextureCache<P>::TickAsyncUnswizzleCpu(PendingUnswizzle& task, Image& image) {
    if (!task.initialized) {
        task.total_size = MapSizeBytes(image);

        const auto uploads = FullUploadSwizzles(task.info);
        task.cpu_num_tiles = uploads[0].num_tiles;
        task.cpu_block = uploads[0].block;
        task.cpu_stride_alignment = CalculateLevelStrideAlignment(task.info, 0);
        task.bytes_per_block = BytesPerBlock(task.info.format);

        size_t max_batch_size = task.total_size;

        const auto& info = image.info;
        const u32 bytes_per_block = task.bytes_per_block;
        const u32 width_blocks = Common::DivCeil(info.size.width, 4u);
        const u32 height_blocks = Common::DivCeil(info.size.height, 4u);
        task.bytes_per_slice = static_cast<size_t>(width_blocks * bytes_per_block) * height_blocks;
        task.last_submitted_slice = 0;

        const u32 total_slices_init = info.size.depth;
        const auto sp = VideoCommon::Accelerated::MakeBlockLinearSwizzle3DParams(
            uploads[0], task.info);
        task.swizzled_slice_size = sp.slice_size;
        task.swizzle_block_depth = sp.block_depth;

        InitSparseUnswizzleTracking(task, image);

        const u32 batch_slices = (async_unswizzle_slices_per_batch == 0)
            ? (task.active_z_end - task.active_z_start)
            : async_unswizzle_slices_per_batch;

        size_t calculated_max = 0;
        const u32 group_slices = (std::min)(1u << task.swizzle_block_depth, total_slices_init);

        // Complicated but I'd rather size the staging buffer to the slices we found rather than the whole texture
        // Not every machine has 32GB of ram
        for (u32 z = task.active_z_start; z < task.active_z_end; z += (std::max)(batch_slices, 1u)) {
            const u32 z_want_init = (std::min)(z + (std::max)(batch_slices, 1u), task.active_z_end);
            const u32 z_count_init = z_want_init - z;

            const u32 z_group_start = (z / group_slices) * group_slices;
            const u32 z_group_end = Common::AlignUp(z + z_count_init, group_slices);
            const u32 z_group_count = (std::min)(z_group_end, total_slices_init) - z_group_start;

            const size_t group_off = (z_group_start < image.slice_offsets.size())
                ? static_cast<size_t>(image.slice_offsets[z_group_start]) : 0;
            const size_t group_end = (z_group_start + z_group_count < image.slice_offsets.size())
                ? static_cast<size_t>(image.slice_offsets[z_group_start + z_group_count]) : task.total_size;

            calculated_max = (std::max)(calculated_max, group_end - group_off);
        }
        if (calculated_max > 0 && calculated_max < max_batch_size) {
            max_batch_size = calculated_max;
        }
        if (task.active_z_start >= task.active_z_end) {
            max_batch_size = (std::min)(max_batch_size, task.bytes_per_slice);
        }

        if (task.active_z_start > 0 && task.active_z_start < image.slice_offsets.size()) {
            const size_t start_byte = static_cast<size_t>(image.slice_offsets[task.active_z_start]);
            task.current_offset = start_byte;
            task.last_submitted_slice = task.active_z_start;
        }

        const size_t needed = (std::max)(max_batch_size, size_t{1});
        task.staging_buffer = runtime.UploadStagingBuffer(needed, true);

        task.initialized = true;
    }

    const u32 total_slices = image.info.size.depth;
    const u32 effective_end = (async_unswizzle_slices_per_batch == 0 && task.is_sparse)
        ? task.active_z_end
        : total_slices;
    const u32 batch = (async_unswizzle_slices_per_batch == 0)
        ? effective_end
        : async_unswizzle_slices_per_batch;

    const u32 z_src = task.last_submitted_slice;
    const u32 z_want = (std::min)(z_src + batch, effective_end);
    const u32 z_count_current = (z_want > z_src) ? (z_want - z_src) : 0;

    size_t current_group_off = 0;
    size_t current_group_end = task.total_size;

    if (z_count_current > 0) {
        const u32 group_slices = (std::min)(1u << task.swizzle_block_depth, total_slices);
        const u32 z_group_start = (z_src / group_slices) * group_slices;
        const u32 z_group_end_excl = Common::AlignUp(z_src + z_count_current, group_slices);
        const u32 z_group_count = (std::min)(z_group_end_excl, total_slices) - z_group_start;

        current_group_off = (z_group_start < image.slice_offsets.size())
            ? static_cast<size_t>(image.slice_offsets[z_group_start]) : 0;
        current_group_end = (z_group_start + z_group_count < image.slice_offsets.size())
            ? static_cast<size_t>(image.slice_offsets[z_group_start + z_group_count]) : task.total_size;
    } else {
        current_group_off = task.total_size;
        current_group_end = task.total_size;
    }

    if (task.current_offset < current_group_end) {
        u8* const staging_base = task.staging_buffer.mapped_span.data();

        const size_t remaining = current_group_end - task.current_offset;
        const size_t copy_amount = remaining;

        const size_t read_start = task.current_offset;
        const size_t read_end = read_start + copy_amount;

        const size_t buffer_offset_start = read_start - current_group_off;

        if (task.is_sparse) {
            ReadSparseCoalesced(task, image, staging_base, current_group_off, read_start, read_end);
        } else {
            gpu_memory->ReadBlockUnsafe(image.gpu_addr + read_start, staging_base + buffer_offset_start,
                                        copy_amount);
        }
        task.current_offset += copy_amount;
    }

    const size_t active_end_byte = (task.active_z_end < image.slice_offsets.size())
        ? static_cast<size_t>(image.slice_offsets[task.active_z_end]) : task.total_size;
    const bool is_final_batch = task.current_offset >= active_end_byte;
    const bool batch_ready = task.current_offset >= current_group_end;

    if (!task.cpu_job_in_flight && z_src < effective_end && (batch_ready || is_final_batch) &&
        z_count_current > 0) {
        const u32 z_image = z_src;

        bool any_data = true;
        if (task.is_sparse && !task.slice_has_data.empty()) {
            any_data = false;
            for (u32 z = z_src; z < z_src + z_count_current; ++z) {
                if (z < task.slice_has_data.size() && task.slice_has_data[z]) {
                    any_data = true;
                    break;
                }
            }
        }

        const u32 aligned_width = Common::AlignUp(image.info.size.width, 4u);
        const u32 aligned_height = Common::AlignUp(image.info.size.height, 4u);

        if (!any_data) {
            // I had no choice but to do this otherwise the area around the data within the texture will be filled with uninitialized junk from VRAM
            const size_t upload_size = static_cast<size_t>(z_count_current) * task.bytes_per_slice;
            auto upload_staging = runtime.UploadStagingBuffer(upload_size);
            std::memset(upload_staging.mapped_span.data(), 0, upload_size);

            const BufferImageCopy copy{
                .buffer_offset = 0,
                .buffer_size = upload_size,
                .buffer_row_length = aligned_width,
                .buffer_image_height = aligned_height,
                .image_subresource =
                    {
                        .base_level = 0,
                        .base_layer = 0,
                        .num_layers = 1,
                    },
                .image_offset = {0, 0, static_cast<s32>(z_image)},
                .image_extent = {image.info.size.width, image.info.size.height, z_count_current},
            };
            const std::array copies{copy};
            image.UploadMemory(upload_staging, copies);
            runtime.InsertUploadMemoryBarrier();

            task.last_submitted_slice += z_count_current;
        } else {
            const u32 group_slices = (std::min)(1u << task.swizzle_block_depth, total_slices);
            const u32 z_group_start = (z_src / group_slices) * group_slices;
            const u32 z_group_end_excl = Common::AlignUp(z_src + z_count_current, group_slices);
            const u32 z_group_count = (std::min)(z_group_end_excl, total_slices) - z_group_start;

            const size_t group_guest_off = (z_group_start < image.slice_offsets.size())
                ? static_cast<size_t>(image.slice_offsets[z_group_start]) : 0;
            const size_t group_guest_end = (z_group_start + z_group_count < image.slice_offsets.size())
                ? static_cast<size_t>(image.slice_offsets[z_group_start + z_group_count]) : task.total_size;
            const size_t group_guest_size = group_guest_end - group_guest_off;

            const u8* const staging_base = task.staging_buffer.mapped_span.data();
            std::span<const u8> swizzled_batch(staging_base, group_guest_size);

            const size_t group_linear_size = static_cast<size_t>(z_group_count) * task.bytes_per_slice;

            task.cpu_job_in_flight = true;
            AsyncCpuUnswizzleChunk* const chunk = &AcquireCpuChunk(task);
            chunk->z_src = z_src;
            chunk->z_image = z_image;
            chunk->z_count = z_count_current;
            chunk->group_z_start = z_group_start;
            if (chunk->linear_staging_capacity < group_linear_size) {
                if (chunk->linear_staging_capacity > 0) {
                    runtime.FreeDeferredStagingBuffer(chunk->linear_staging);
                }
                chunk->linear_staging = runtime.UploadStagingBuffer(group_linear_size, true);
                chunk->linear_staging_capacity = group_linear_size;
            }

            boost::container::small_vector<std::pair<u32, u32>, 32> subranges;
            for (u32 sub_start = 0; sub_start < z_group_count; sub_start += group_slices) {
                const u32 sub_count = (std::min)(group_slices, z_group_count - sub_start);
                subranges.emplace_back(sub_start, sub_count);
            }

            const size_t worker_count = texture_unswizzle_worker.NumWorkers();
            const size_t groups_per_job = subranges.empty()
                ? size_t{1}
                : (std::max)(size_t{1}, Common::DivCeil(subranges.size(), worker_count));

            boost::container::small_vector<std::pair<u32, u32>, 32> jobs;
            for (size_t i = 0; i < subranges.size(); i += groups_per_job) {
                const size_t end = (std::min)(i + groups_per_job, subranges.size());
                const u32 job_start = subranges[i].first;
                const u32 job_end = subranges[end - 1].first + subranges[end - 1].second;
                jobs.emplace_back(job_start, job_end - job_start);
            }

            chunk->jobs_pending.store(jobs.size(), std::memory_order_relaxed);

            for (const auto& [sub_start, sub_count] : jobs) {
                const size_t sub_guest_off = (z_group_start + sub_start < image.slice_offsets.size())
                    ? static_cast<size_t>(image.slice_offsets[z_group_start + sub_start]) - group_guest_off
                    : (static_cast<size_t>(sub_start) * task.bytes_per_slice);
                const size_t sub_guest_end =
                    (z_group_start + sub_start + sub_count < image.slice_offsets.size())
                        ? static_cast<size_t>(image.slice_offsets[z_group_start + sub_start + sub_count]) -
                              group_guest_off
                        : group_guest_size;
                const size_t sub_guest_size = sub_guest_end - sub_guest_off;

                const size_t sub_linear_off = static_cast<size_t>(sub_start) * task.bytes_per_slice;
                const size_t sub_linear_size = static_cast<size_t>(sub_count) * task.bytes_per_slice;

                const u8* const swizzled_base = swizzled_batch.data();
                u8* const linear_base = chunk->linear_staging.mapped_span.data();

                texture_unswizzle_worker.QueueWork(
                    [chunk, swizzled_base, sub_guest_off, sub_guest_size, linear_base, sub_linear_off,
                     sub_linear_size, bytes_per_block = task.bytes_per_block,
                     num_tiles = task.cpu_num_tiles, sub_count, block = task.cpu_block,
                     stride_alignment = task.cpu_stride_alignment]() mutable {
                        std::span<const u8> job_swizzled(swizzled_base + sub_guest_off, sub_guest_size);
                        std::span<u8> job_linear(linear_base + sub_linear_off, sub_linear_size);
                        UnswizzleTexture(job_linear, job_swizzled, bytes_per_block, num_tiles.width,
                                         num_tiles.height, sub_count, block.height, block.depth,
                                         stride_alignment);
                        chunk->jobs_pending.fetch_sub(1, std::memory_order_acq_rel);
                    });
            }
        }
    }

    if (task.cpu_job_in_flight) {
        AsyncCpuUnswizzleChunk& chunk = AcquireCpuChunk(task);
        if (chunk.jobs_pending.load(std::memory_order_acquire) == 0) {
            const size_t offset_slices = chunk.z_src - chunk.group_z_start;
            const size_t upload_offset = offset_slices * task.bytes_per_slice;
            const size_t upload_size = static_cast<size_t>(chunk.z_count) * task.bytes_per_slice;
            const u32 aligned_width = Common::AlignUp(image.info.size.width, 4u);
            const u32 aligned_height = Common::AlignUp(image.info.size.height, 4u);

            const BufferImageCopy copy{
                .buffer_offset = upload_offset,
                .buffer_size = upload_size,
                .buffer_row_length = aligned_width,
                .buffer_image_height = aligned_height,
                .image_subresource =
                    {
                        .base_level = 0,
                        .base_layer = 0,
                        .num_layers = 1,
                    },
                .image_offset = {0, 0, static_cast<s32>(chunk.z_image)},
                .image_extent = {image.info.size.width, image.info.size.height, chunk.z_count},
            };
            const std::array copies{copy};
            image.UploadMemory(chunk.linear_staging, copies);
            runtime.InsertUploadMemoryBarrier();

            task.last_submitted_slice += chunk.z_count;
            task.cpu_job_in_flight = false;
        }
    }

    // Check if complete
    const u32 slices_submitted = task.last_submitted_slice;
    const bool all_submitted = slices_submitted >= effective_end;

    if (is_final_batch && all_submitted && !task.cpu_job_in_flight) {
        runtime.FreeDeferredStagingBuffer(task.staging_buffer);
        ReleaseCpuChunk(task);
        image.flags &= ~ImageFlagBits::IsDecoding;
        unswizzle_queue.erase(unswizzle_queue.begin());
    }
}

template <class P>
TextureCache<P>::AsyncCpuUnswizzleChunk& TextureCache<P>::AcquireCpuChunk(
    PendingUnswizzle& task) {
    if (!task.cpu_chunk_slot) {
        const auto it = std::ranges::find(cpu_chunk_slot_used, false);
        ASSERT_MSG(it != cpu_chunk_slot_used.end(),
                   "Ran out of CPU unswizzle chunk pool slots; pool and queue capacity have "
                   "gone out of sync");
        *it = true;
        task.cpu_chunk_slot = static_cast<size_t>(std::distance(cpu_chunk_slot_used.begin(), it));
    }
    return cpu_chunk_pool[*task.cpu_chunk_slot];
}

template <class P>
void TextureCache<P>::ReleaseCpuChunk(PendingUnswizzle& task) {
    if (!task.cpu_chunk_slot) {
        return;
    }
    const size_t slot = *task.cpu_chunk_slot;
    AsyncCpuUnswizzleChunk& chunk = cpu_chunk_pool[slot];
    if (chunk.linear_staging_capacity > 0) {
        runtime.FreeDeferredStagingBuffer(chunk.linear_staging);
        chunk.linear_staging_capacity = 0;
    }
    chunk.jobs_pending.store(0, std::memory_order_relaxed);
    cpu_chunk_slot_used[slot] = false;
    task.cpu_chunk_slot.reset();
}

template <class P>
bool TextureCache<P>::ScaleUp(Image& image) {
    const bool has_copy = image.HasScaled();
    const bool rescaled = image.ScaleUp();
    if (!has_copy && image.HasScaled()) {
        total_used_memory += GetScaledImageSizeBytes(image);
    }
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
bool TextureCache<P>::ScaleDown(Image& image) {
    const bool rescaled = image.ScaleDown();
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
ImageId TextureCache<P>::InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                     RelaxedOptions options) {
    std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        const auto size = CalculateGuestSizeInBytes(info);
        cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, size);
        if (!cpu_addr) {
            const DAddr fake_addr = ~(1ULL << 40ULL) + virtual_invalid_space;
            virtual_invalid_space += Common::AlignUp(size, 32);
            cpu_addr = std::optional<DAddr>(fake_addr);
        }
    }
    ASSERT_MSG(cpu_addr, "Tried to insert an image to an invalid gpu_addr={:#x}", gpu_addr);
    const ImageId image_id = JoinImages(info, gpu_addr, *cpu_addr);
    const Image& image = slot_images[image_id];
    // Using "image.gpu_addr" instead of "gpu_addr" is important because it might be different
    const auto [it, is_new] = image_allocs_table.try_emplace(image.gpu_addr);
    if (is_new) {
        it->second = slot_image_allocs.insert();
    }
    slot_image_allocs[it->second].images.push_back(image_id);
    return image_id;
}

template <class P>
ImageId TextureCache<P>::JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
    ImageInfo new_info = info;
    const size_t size_bytes = CalculateGuestSizeInBytes(new_info);
    const bool broken_views = runtime.HasBrokenTextureViewFormats();
    const bool native_bgr = runtime.HasNativeBgr();
    join_overlap_ids.clear();
    join_overlaps_found.clear();
    join_left_aliased_ids.clear();
    join_right_aliased_ids.clear();
    join_ignore_textures.clear();
    join_bad_overlap_ids.clear();
    join_copies_to_do.clear();
    join_alias_indices.clear();
    const bool this_is_linear = info.type == ImageType::Linear;
    const auto region_check = [&](ImageId overlap_id, ImageBase& overlap) {
        if (True(overlap.flags & ImageFlagBits::Remapped)) {
            join_ignore_textures.insert(overlap_id);
            return;
        }
        const bool overlap_is_linear = overlap.info.type == ImageType::Linear;
        if (this_is_linear != overlap_is_linear) {
            return;
        }
        if (this_is_linear && overlap_is_linear) {
            if (info.pitch == overlap.info.pitch && gpu_addr == overlap.gpu_addr) {
                // Alias linear images with the same pitch
                join_left_aliased_ids.push_back(overlap_id);
            }
            return;
        }
        join_overlaps_found.insert(overlap_id);
        constexpr bool strict_size = true;
        const std::optional<OverlapResult> solution = ResolveOverlap(
            new_info, gpu_addr, cpu_addr, overlap, strict_size, broken_views, native_bgr);
        if (solution) {
            gpu_addr = solution->gpu_addr;
            cpu_addr = solution->cpu_addr;
            new_info.resources = solution->resources;
            join_overlap_ids.push_back(overlap_id);
            join_copies_to_do.emplace_back(JoinCopy{false, overlap_id});
            return;
        }
        constexpr auto options = RelaxedOptions::Size | RelaxedOptions::Format;
        const ImageBase new_image_base(new_info, gpu_addr, cpu_addr);
        if (IsSubresource(new_info, overlap, gpu_addr, options, broken_views, native_bgr)) {
            join_left_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else if (IsSubresource(overlap.info, new_image_base, overlap.gpu_addr, options,
                                 broken_views, native_bgr)) {
            join_right_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else {
            join_bad_overlap_ids.push_back(overlap_id);
        }
    };
    ForEachImageInRegion(cpu_addr, size_bytes, region_check);
    const auto region_check_gpu = [&](ImageId overlap_id, ImageBase& overlap) {
        if (!join_overlaps_found.contains(overlap_id)) {
            if (True(overlap.flags & ImageFlagBits::Remapped)) {
                join_ignore_textures.insert(overlap_id);
            }
            if (overlap.gpu_addr == gpu_addr && overlap.guest_size_bytes == size_bytes) {
                join_ignore_textures.insert(overlap_id);
            }
        }
    };
    ForEachSparseImageInRegion(channel_state->gpu_memory.GetID(), gpu_addr, size_bytes,
                               region_check_gpu);

    bool can_rescale = info.rescaleable;
    bool any_rescaled = false;
    for (const auto& copy : join_copies_to_do) {
        if (!can_rescale) {
            break;
        }
        Image& sibling = slot_images[copy.id];
        can_rescale &= ImageCanRescale(sibling);
        any_rescaled |= True(sibling.flags & ImageFlagBits::Rescaled);
    }

    can_rescale &= any_rescaled;

    if (can_rescale) {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleUp(sibling);
        }
    } else {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleDown(sibling);
        }
    }

    const ImageId new_image_id = slot_images.insert(runtime, new_info, gpu_addr, cpu_addr);
    Image& new_image = slot_images[new_image_id];
    Common::Kledd::GetFrameStats().images_created.fetch_add(1, std::memory_order_relaxed);

    new_image.allocation_tick = frame_tick;

    if (!gpu_memory->IsContinuousRange(new_image.gpu_addr, new_image.guest_size_bytes) &&
        new_info.is_sparse) {
        new_image.flags |= ImageFlagBits::Sparse;
    }

    for (const ImageId overlap_id : join_ignore_textures) {
        Image& overlap = slot_images[overlap_id];
        if (True(overlap.flags & ImageFlagBits::GpuModified)) {
            Common::Kledd::GetFrameStats().join_dropped_gpu.fetch_add(1,
                                                                     std::memory_order_relaxed);
            UNIMPLEMENTED();
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, overlap_id);
        }
        UnregisterImage(overlap_id);
        DeleteImage(overlap_id);
    }

    // TODO: Only upload what we need
    RefreshContents(new_image, new_image_id);

    if (can_rescale) {
        ScaleUp(new_image);
    } else {
        ScaleDown(new_image);
    }

    std::ranges::sort(join_copies_to_do, [this](const JoinCopy& lhs, const JoinCopy& rhs) {
        const ImageBase& lhs_image = slot_images[lhs.id];
        const ImageBase& rhs_image = slot_images[rhs.id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });

    ImageBase& new_image_base = new_image;
    for (const ImageId aliased_id : join_right_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(new_image_base, aliased, new_image_id, aliased_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_left_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(aliased, new_image_base, aliased_id, new_image_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_bad_overlap_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        aliased.overlapping_images.push_back(new_image_id);
        new_image.overlapping_images.push_back(aliased_id);
        if (aliased.info.resources.levels == 1 && aliased.info.block.depth == 0 &&
            aliased.overlapping_images.size() > 1) {
            aliased.flags |= ImageFlagBits::BadOverlap;
        }
        if (new_image.info.resources.levels == 1 && new_image.info.block.depth == 0 &&
            new_image.overlapping_images.size() > 1) {
            new_image.flags |= ImageFlagBits::BadOverlap;
        }
    }

    for (const auto& copy_object : join_copies_to_do) {
        Image& overlap = slot_images[copy_object.id];
        if (copy_object.is_alias) {
            if (!overlap.IsSafeGpuCopy()) {
                continue;
            }
            if (overlap.info.num_samples != new_image.info.num_samples) {
                continue;
            }
            const auto alias_pointer = join_alias_indices.find(copy_object.id);
            if (alias_pointer == join_alias_indices.end()) {
                continue;
            }
            const AliasedImage& aliased = new_image.aliased_images[alias_pointer->second];
            CopyImage(new_image_id, aliased.id, aliased.copies);
            new_image.modification_tick = overlap.modification_tick;
            continue;
        }
        if (True(overlap.flags & ImageFlagBits::GpuModified)) {
            new_image.flags |= ImageFlagBits::GpuModified;
            const auto& resolution = Settings::values.resolution_info;
            const SubresourceBase base = new_image.TryFindBase(overlap.gpu_addr).value();
            const u32 up_scale = can_rescale ? resolution.up_scale : 1;
            const u32 down_shift = can_rescale ? resolution.down_shift : 0;
            auto copies = MakeShrinkImageCopies(new_info, overlap.info, base, up_scale, down_shift);
            if (overlap.info.num_samples != new_image.info.num_samples) {
                runtime.CopyImageMSAA(new_image, overlap, copies);
            } else {
                runtime.CopyImage(new_image, overlap, copies);
            }
            new_image.modification_tick = overlap.modification_tick;
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, copy_object.id);
        }
        UnregisterImage(copy_object.id);
        DeleteImage(copy_object.id);
    }

    RegisterImage(new_image_id);
    return new_image_id;
}

template <class P>
std::optional<typename TextureCache<P>::BlitImages> TextureCache<P>::GetBlitImages(
    const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
    const Tegra::Engines::Fermi2D::Config& copy) {

    constexpr auto FIND_OPTIONS = RelaxedOptions::Samples;
    const GPUVAddr dst_addr = dst.Address();
    const GPUVAddr src_addr = src.Address();
    ImageInfo dst_info(dst);
    ImageInfo src_info(src);
    const bool can_be_depth_blit =
        dst_info.format == src_info.format && copy.filter == Tegra::Engines::Fermi2D::Filter::Point;
    ImageId dst_id;
    ImageId src_id;
    RelaxedOptions try_options = FIND_OPTIONS;
    if (can_be_depth_blit) {
        try_options |= RelaxedOptions::Format;
    }
    do {
        has_deleted_images = false;
        src_id = FindImage(src_info, src_addr, try_options);
        dst_id = FindImage(dst_info, dst_addr, try_options);
        if (!copy.must_accelerate) {
            do {
                if (!src_id && !dst_id) {
                    return std::nullopt;
                }
                if (src_id && True(slot_images[src_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                if (dst_id && True(slot_images[dst_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        const ImageBase* const src_image = src_id ? &slot_images[src_id] : nullptr;
        if (src_image && src_image->info.num_samples > 1) {
            RelaxedOptions find_options{FIND_OPTIONS | RelaxedOptions::ForceBrokenViews};
            src_id = FindOrInsertImage(src_info, src_addr, find_options);
            dst_id = FindOrInsertImage(dst_info, dst_addr, find_options);
            if (has_deleted_images) {
                continue;
            }
            break;
        }
        if (can_be_depth_blit) {
            const ImageBase* const dst_image = dst_id ? &slot_images[dst_id] : nullptr;
            DeduceBlitImages(dst_info, src_info, dst_image, src_image);
            if (GetFormatType(dst_info.format) != GetFormatType(src_info.format)) {
                continue;
            }
        }
        if (!src_id) {
            src_id = InsertImage(src_info, src_addr, RelaxedOptions{});
        }
        if (!dst_id) {
            dst_id = InsertImage(dst_info, dst_addr, RelaxedOptions{});
        }
    } while (has_deleted_images);
    const ImageBase& src_image = slot_images[src_id];
    const ImageBase& dst_image = slot_images[dst_id];
    const bool native_bgr = runtime.HasNativeBgr();
    if (GetFormatType(dst_info.format) != GetFormatType(dst_image.info.format) ||
        GetFormatType(src_info.format) != GetFormatType(src_image.info.format) ||
        !VideoCore::Surface::IsViewCompatible(dst_info.format, dst_image.info.format, false,
                                              native_bgr) ||
        !VideoCore::Surface::IsViewCompatible(src_info.format, src_image.info.format, false,
                                              native_bgr)) {
        // Make sure the images match the expected format.
        do {
            has_deleted_images = false;
            src_id = FindOrInsertImage(src_info, src_addr, RelaxedOptions{});
            dst_id = FindOrInsertImage(dst_info, dst_addr, RelaxedOptions{});
        } while (has_deleted_images);
    }
    return {BlitImages{
        .dst_id = dst_id,
        .src_id = src_id,
        .dst_format = dst_info.format,
        .src_format = src_info.format,
    }};
}

template <class P>
ImageId TextureCache<P>::FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr) {
    std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
        if (!cpu_addr) {
            return ImageId{};
        }
    }
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, false, true)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return true;
            }
        } else if (IsSubCopy(info, existing_image, gpu_addr)) {
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return true;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
std::pair<u32, u32> TextureCache<P>::PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                     bool mark_as_modified) {
    const auto& image = slot_images[dst_id];
    const auto base = image.TryFindBase(base_addr);
    PrepareImage(dst_id, mark_as_modified, false);
    const auto& new_image = slot_images[dst_id];
    lru_cache.Touch(new_image.lru_index, frame_tick);
    return std::make_pair(base->level, base->layer);
}

template <class P>
SamplerId TextureCache<P>::FindSampler(const TSCEntry& config, bool compute) {
    if (std::ranges::all_of(config.raw, [](u64 value) { return value == 0; })) {
        return NULL_SAMPLER_ID;
    }
    const auto [pair, is_new] = channel_state->samplers.try_emplace(config);
    if (is_new) {
        pair->second = slot_samplers.insert(runtime, config);
    }
    return pair->second;
}

template <class P>
ImageViewId TextureCache<P>::FindColorBuffer(size_t index) {
    const auto& regs = maxwell3d->regs;
    if (index >= regs.rt_control.count) {
        return ImageViewId{};
    }
    const auto& rt = regs.rt[index];
    const GPUVAddr gpu_addr = rt.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    if (rt.format == Tegra::RenderTargetFormat::NONE) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.rt[index], regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr);
}

template <class P>
ImageViewId TextureCache<P>::FindDepthBuffer() {
    const auto& regs = maxwell3d->regs;
    if (!regs.zeta_enable) {
        return ImageViewId{};
    }
    const GPUVAddr gpu_addr = regs.zeta.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.zeta, regs.zeta_size, regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr);
}

template <class P>
ImageViewId TextureCache<P>::FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr) {
    ImageId image_id{};
    bool delete_state = has_deleted_images;
    do {
        has_deleted_images = false;
        image_id = FindOrInsertImage(info, gpu_addr);
        delete_state |= has_deleted_images;
    } while (has_deleted_images);
    has_deleted_images = delete_state;
    if (!image_id) {
        return NULL_IMAGE_VIEW_ID;
    }
    Image& image = slot_images[image_id];
    const ImageViewType view_type = RenderTargetImageViewType(info);
    SubresourceBase base;
    if (image.info.type == ImageType::Linear) {
        base = SubresourceBase{.level = 0, .layer = 0};
    } else {
        base = image.TryFindBase(gpu_addr).value();
    }
    const s32 layers = image.info.type == ImageType::e3D ? info.size.depth : info.resources.layers;
    const SubresourceRange range{
        .base = base,
        .extent = {.levels = 1, .layers = layers},
    };
    return FindOrEmplaceImageView(image_id, ImageViewInfo(view_type, info.format, range));
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 32> images;
    boost::container::small_vector<ImageMapId, 32> maps;
    ForEachCPUPage(cpu_addr, size, [this, &images, &maps, cpu_addr, size, func](u64 page) {
        const auto it = page_table.find(page);
        if (it == page_table.end()) {
            if constexpr (BOOL_BREAK) {
                return false;
            } else {
                return;
            }
        }
        for (const ImageMapId map_id : it->second) {
            ImageMapView& map = slot_map_views[map_id];
            if (map.picked) {
                continue;
            }
            if (!map.Overlaps(cpu_addr, size)) {
                continue;
            }
            map.picked = true;
            maps.push_back(map_id);
            Image& image = slot_images[map.image_id];
            if (True(image.flags & ImageFlagBits::Picked)) {
                continue;
            }
            image.flags |= ImageFlagBits::Picked;
            images.push_back(map.image_id);
            if constexpr (BOOL_BREAK) {
                if (func(map.image_id, image)) {
                    return true;
                }
            } else {
                func(map.image_id, image);
            }
        }
        if constexpr (BOOL_BREAK) {
            return false;
        }
    });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
    for (const ImageMapId map_id : maps) {
        slot_map_views[map_id].picked = false;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                              Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& gpu_page_table = gpu_page_table_storage[*storage_id * 2];
    ForEachGPUPage(gpu_addr, size,
                   [this, &gpu_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = gpu_page_table.find(page);
                       if (it == gpu_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                                 Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& sparse_page_table = gpu_page_table_storage[*storage_id * 2 + 1];
    ForEachGPUPage(gpu_addr, size,
                   [this, &sparse_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = sparse_page_table.find(page);
                       if (it == sparse_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseSegment(ImageBase& image, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, GPUVAddr, DAddr, size_t>::type;
    constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
    const auto segments = gpu_memory->GetSubmappedRange(image.gpu_addr, image.guest_size_bytes);
    for (const auto& [gpu_addr, size] : segments) {
        std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        ASSERT(cpu_addr);
        if constexpr (RETURNS_BOOL) {
            if (func(gpu_addr, *cpu_addr, size)) {
                break;
            }
        } else {
            func(gpu_addr, *cpu_addr, size);
        }
    }
}

template <class P>
ImageViewId TextureCache<P>::FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info) {
    Image& image = slot_images[image_id];
    if (const ImageViewId image_view_id = image.FindView(info); image_view_id) {
        return image_view_id;
    }
    const ImageViewId image_view_id = slot_image_views.insert(runtime, info, image_id, image, slot_images);
    image.InsertView(info, image_view_id);
    return image_view_id;
}

template <class P>
void TextureCache<P>::RegisterImage(ImageId image_id) {
    ImageBase& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    u64 tentative_size = (std::max)(image.guest_size_bytes, image.unswizzled_size_bytes);
    if ((IsPixelFormatASTC(image.info.format) &&
         True(image.flags & ImageFlagBits::AcceleratedUpload)) ||
        True(image.flags & ImageFlagBits::Converted)) {
        tentative_size = TranscodedAstcSize(tentative_size, image.info.format);
    }
    total_used_memory += Common::AlignUp(tentative_size, 1024);
    image.lru_index = lru_cache.Insert(image_id, frame_tick);

    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->gpu_page_table)[page].push_back(image_id);
    });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        auto map_id =
            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes,
                       [this, map_id](u64 page) { page_table[page].push_back(map_id); });
        image.map_view_id = map_id;
        return;
    }
    boost::container::small_vector<ImageViewId, 16> sparse_maps;
    ForEachSparseSegment(
        image, [this, image_id, &sparse_maps](GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);
            ForEachCPUPage(cpu_addr, size,
                           [this, map_id](u64 page) { page_table[page].push_back(map_id); });
            sparse_maps.push_back(map_id);
        });
    sparse_views.emplace(image_id, std::move(sparse_maps));
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->sparse_page_table)[page].push_back(image_id);
    });
}

template <class P>
void TextureCache<P>::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already registered image");
    image.flags &= ~ImageFlagBits::Registered;
    image.flags &= ~ImageFlagBits::BadOverlap;
    lru_cache.Free(image.lru_index);
    const auto& clear_page_table =
        [image_id](u64 page, ::Common::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>& selected_page_table) {
            const auto page_it = selected_page_table.find(page);
            if (page_it == selected_page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageId>& image_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_ids, image_id);
            if (vector_it == image_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page={:#x}",
                           page << YUZU_PAGEBITS);
                return;
            }
            image_ids.erase(vector_it);
        };
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, &clear_page_table](u64 page) {
        clear_page_table(page, (*channel_state->gpu_page_table));
    });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        const auto map_id = image.map_view_id;
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes, [this, map_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_map_ids, map_id);
            if (vector_it == image_map_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page={:#x}",
                           page << YUZU_PAGEBITS);
                return;
            }
            image_map_ids.erase(vector_it);
        });
        slot_map_views.erase(map_id);
        return;
    }
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, &clear_page_table](u64 page) {
        clear_page_table(page, (*channel_state->sparse_page_table));
    });
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map_range = slot_map_views[map_view_id];
        const DAddr cpu_addr = map_range.cpu_addr;
        const std::size_t size = map_range.size;
        ForEachCPUPage(cpu_addr, size, [this, image_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            auto vector_it = image_map_ids.begin();
            while (vector_it != image_map_ids.end()) {
                ImageMapView& map = slot_map_views[*vector_it];
                if (map.image_id != image_id) {
                    vector_it++;
                    continue;
                }
                if (!map.picked) {
                    map.picked = true;
                }
                vector_it = image_map_ids.erase(vector_it);
            }
        });
        slot_map_views.erase(map_view_id);
    }
    sparse_views.erase(it);
}

template <class P>
void TextureCache<P>::TrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(False(image.flags & ImageFlagBits::Tracked));
    image.flags |= ImageFlagBits::Tracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, 1);
        }
        return;
    }
    if (True(image.flags & ImageFlagBits::Registered)) {
        auto it = sparse_views.find(image_id);
        ASSERT(it != sparse_views.end());
        auto& sparse_maps = it->second;
        for (auto& map_view_id : sparse_maps) {
            const auto& map = slot_map_views[map_view_id];
            const DAddr cpu_addr = map.cpu_addr;
            const std::size_t size = map.size;
            device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
        }
        return;
    }
    ForEachSparseSegment(image,
                         [this]([[maybe_unused]] GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
                             device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
                         });
}

template <class P>
void TextureCache<P>::UntrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(True(image.flags & ImageFlagBits::Tracked));
    image.flags &= ~ImageFlagBits::Tracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, -1);
        }
        return;
    }
    ASSERT(True(image.flags & ImageFlagBits::Registered));
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map = slot_map_views[map_view_id];
        const DAddr cpu_addr = map.cpu_addr;
        const std::size_t size = map.size;
        device_memory.UpdatePagesCachedCount(cpu_addr, size, -1);
    }
}

template <class P>
void TextureCache<P>::CancelPendingUnswizzle(ImageId image_id) {
    ImageBase& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::IsDecoding)) {
        return;
    }
    const bool has_cpu_job_in_flight =
        std::ranges::any_of(unswizzle_queue, [image_id](const PendingUnswizzle& task) {
            return task.image_id == image_id && task.cpu_job_in_flight;
        });
    if (has_cpu_job_in_flight) {
        texture_unswizzle_worker.WaitForRequests();
    }
    for (auto& task : unswizzle_queue) {
        if (task.image_id == image_id && task.initialized) {
            runtime.FreeDeferredStagingBuffer(task.staging_buffer);
            ReleaseCpuChunk(task);
        }
    }
    unswizzle_queue.erase(
        std::remove_if(unswizzle_queue.begin(), unswizzle_queue.end(),
                        [image_id](const PendingUnswizzle& task) {
                            return task.image_id == image_id;
                        }),
        unswizzle_queue.end());
}

template <class P>
void TextureCache<P>::DeleteImage(ImageId image_id, bool immediate_delete) {
    Common::Kledd::GetFrameStats().images_deleted.fetch_add(1, std::memory_order_relaxed);
    ImageBase& image = slot_images[image_id];
    CancelPendingUnswizzle(image_id);
    std::erase_if(async_decodes, [image_id](const std::unique_ptr<AsyncDecodeContext>& ctx) {
        return ctx->image_id == image_id;
    });
    image.flags &= ~ImageFlagBits::IsDecoding;
    if (image.HasScaled()) {
        total_used_memory -= GetScaledImageSizeBytes(image);
    }
    u64 tentative_size = (std::max)(image.guest_size_bytes, image.unswizzled_size_bytes);
    if ((IsPixelFormatASTC(image.info.format) &&
         True(image.flags & ImageFlagBits::AcceleratedUpload)) ||
        True(image.flags & ImageFlagBits::Converted)) {
        tentative_size = TranscodedAstcSize(tentative_size, image.info.format);
    }
    total_used_memory -= Common::AlignUp(tentative_size, 1024);
    const GPUVAddr gpu_addr = image.gpu_addr;
    const auto alloc_it = image_allocs_table.find(gpu_addr);
    if (alloc_it == image_allocs_table.end()) {
        ASSERT_MSG(false, "Trying to delete an image alloc that does not exist in address {:#x}",
                   gpu_addr);
        return;
    }
    const ImageAllocId alloc_id = alloc_it->second;
    std::vector<ImageId>& alloc_images = slot_image_allocs[alloc_id].images;
    const auto alloc_image_it = std::ranges::find(alloc_images, image_id);
    if (alloc_image_it == alloc_images.end()) {
        ASSERT_MSG(false, "Trying to delete an image that does not exist");
        return;
    }
    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Mark render targets as dirty
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);

    for (const AliasedImage& alias : image.aliased_images) {
        ImageBase& other_image = slot_images[alias.id];
        [[maybe_unused]] const size_t num_removed_aliases =
            std::erase_if(other_image.aliased_images, [image_id](const AliasedImage& other_alias) {
                return other_alias.id == image_id;
            });
        other_image.CheckAliasState();
        ASSERT_MSG(num_removed_aliases == 1, "Invalid number of removed aliases: {}",
                   num_removed_aliases);
    }
    for (const ImageId overlap_id : image.overlapping_images) {
        ImageBase& other_image = slot_images[overlap_id];
        [[maybe_unused]] const size_t num_removed_overlaps = std::erase_if(
            other_image.overlapping_images,
            [image_id](const ImageId other_overlap_id) { return other_overlap_id == image_id; });
        other_image.CheckBadOverlapState();
        ASSERT_MSG(num_removed_overlaps == 1, "Invalid number of removed overlapps: {}",
                   num_removed_overlaps);
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        if (!immediate_delete) {
            sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        }
        slot_image_views.erase(image_view_id);
    }
    if (!immediate_delete) {
        sentenced_images.Push(std::move(slot_images[image_id]));
    }
    slot_images.erase(image_id);

    alloc_images.erase(alloc_image_it);
    if (alloc_images.empty()) {
        image_allocs_table.erase(alloc_it);
    }
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
        if constexpr (ENABLE_VALIDATION)
            for (auto& e : channel_info.image_view_ids)
                e.second = CORRUPT_ID;
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    has_deleted_images = true;
}

template <class P>
void TextureCache<P>::RemoveImageViewReferences(std::span<const ImageViewId> removed_views) {
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
        auto it = channel_info.image_views.begin();
        while (it != channel_info.image_views.end()) {
            const auto found = std::ranges::find(removed_views, it->second);
            if (found != removed_views.end()) {
                it = channel_info.image_views.erase(it);
            } else {
                ++it;
            }
        }
    }
}

template <class P>
void TextureCache<P>::RemoveFramebuffers(std::span<const ImageViewId> removed_views) {
    runtime.FlushDeferredClear();
    auto it = framebuffers.begin();
    while (it != framebuffers.end()) {
        if (it->first.Contains(removed_views)) {
            auto framebuffer_id = it->second;
            ASSERT(framebuffer_id);
            if (framebuffer_id == last_framebuffer_id) {
                last_framebuffer_id = {};
                last_framebuffer_serial = 0;
            }
            sentenced_framebuffers.Push(std::move(slot_framebuffers[framebuffer_id]));
            it = framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

template <class P>
void TextureCache<P>::MarkModification(ImageBase& image) noexcept {
    image.flags |= ImageFlagBits::GpuModified;
    image.modification_tick = ++modification_tick;
}

template <class P>
void TextureCache<P>::SynchronizeAliases(ImageId image_id) {
    boost::container::small_vector<const AliasedImage*, 8> aliased_images;
    Image& image = slot_images[image_id];
    bool any_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    bool any_modified = True(image.flags & ImageFlagBits::GpuModified);
    u64 most_recent_tick = image.modification_tick;
    for (const AliasedImage& aliased : image.aliased_images) {
        ImageBase& aliased_image = slot_images[aliased.id];
        if (image.modification_tick < aliased_image.modification_tick) {
            most_recent_tick = (std::max)(most_recent_tick, aliased_image.modification_tick);
            aliased_images.push_back(&aliased);
            any_rescaled |= True(aliased_image.flags & ImageFlagBits::Rescaled);
            any_modified |= True(aliased_image.flags & ImageFlagBits::GpuModified);
        }
    }
    if (aliased_images.empty()) {
        return;
    }
    const bool can_rescale = ImageCanRescale(image);
    if (any_rescaled) {
        if (can_rescale) {
            ScaleUp(image);
        } else {
            ScaleDown(image);
        }
    }
    image.modification_tick = most_recent_tick;
    if (any_modified) {
        image.flags |= ImageFlagBits::GpuModified;
    }
    std::ranges::sort(aliased_images, [this](const AliasedImage* lhs, const AliasedImage* rhs) {
        const ImageBase& lhs_image = slot_images[lhs->id];
        const ImageBase& rhs_image = slot_images[rhs->id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });
    const auto& resolution = Settings::values.resolution_info;
    for (const AliasedImage* const aliased : aliased_images) {
        if (!resolution.active || !any_rescaled) {
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        Image& aliased_image = slot_images[aliased->id];
        if (!can_rescale) {
            ScaleDown(aliased_image);
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        ScaleUp(aliased_image);
        CopyImage(image_id, aliased->id, aliased->copies);
    }
}

template <class P>
void TextureCache<P>::PrepareImage(ImageId image_id, bool is_modification, bool invalidate) {
    Image& image = slot_images[image_id];
    runtime.SynchronizePendingGpuWrite(image);
    if (invalidate) {
        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);
        if (False(image.flags & ImageFlagBits::Tracked)) {
            TrackImage(image, image_id);
        }
    } else {
        RefreshContents(image, image_id);
        if (!image.aliased_images.empty()) {
            SynchronizeAliases(image_id);
        }
    }
    if (is_modification) {
        MarkModification(image);
    }
    lru_cache.Touch(image.lru_index, frame_tick);
}

template <class P>
void TextureCache<P>::PrepareImageView(ImageViewId image_view_id, bool is_modification,
                                       bool invalidate) {
    if (!image_view_id) {
        return;
    }
    const ImageViewBase& image_view = slot_image_views[image_view_id];
    if (image_view.IsBuffer()) {
        return;
    }
    PrepareImage(image_view.image_id, is_modification, invalidate);
}

template <class P>
void TextureCache<P>::CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies) {
    Image& dst = slot_images[dst_id];
    Image& src = slot_images[src_id];
    const bool is_rescaled = True(src.flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ASSERT(True(dst.flags & ImageFlagBits::Rescaled));
        const bool both_2d{src.info.type == ImageType::e2D && dst.info.type == ImageType::e2D};
        const auto& resolution = Settings::values.resolution_info;
        for (auto& copy : copies) {
            copy.src_offset.x = resolution.ScaleUp(copy.src_offset.x);
            copy.dst_offset.x = resolution.ScaleUp(copy.dst_offset.x);
            copy.extent.width = resolution.ScaleUp(copy.extent.width);
            if (both_2d) {
                copy.src_offset.y = resolution.ScaleUp(copy.src_offset.y);
                copy.dst_offset.y = resolution.ScaleUp(copy.dst_offset.y);
                copy.extent.height = resolution.ScaleUp(copy.extent.height);
            }
        }
    }
    const auto dst_format_type = GetFormatType(dst.info.format);
    const auto src_format_type = GetFormatType(src.info.format);
    if (src_format_type == dst_format_type) {
        if constexpr (HAS_EMULATED_COPIES) {
            if (!runtime.CanImageBeCopied(dst, src)) {
                return runtime.EmulateCopyImage(dst, src, copies);
            }
        }
        return runtime.CopyImage(dst, src, copies);
    }
    UNIMPLEMENTED_IF(dst.info.type != ImageType::e2D);
    UNIMPLEMENTED_IF(src.info.type != ImageType::e2D);
    if (runtime.ShouldReinterpret(dst, src)) {
        return runtime.ReinterpretImage(dst, src, copies);
    }
    for (const ImageCopy& copy : copies) {
        UNIMPLEMENTED_IF(copy.dst_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_offset != Offset3D{});
        UNIMPLEMENTED_IF(copy.dst_offset != Offset3D{});

        const SubresourceBase dst_base{
            .level = copy.dst_subresource.base_level,
            .layer = copy.dst_subresource.base_layer,
        };
        const SubresourceBase src_base{
            .level = copy.src_subresource.base_level,
            .layer = copy.src_subresource.base_layer,
        };
        const SubresourceExtent dst_extent{.levels = 1, .layers = 1};
        const SubresourceExtent src_extent{.levels = 1, .layers = 1};
        const SubresourceRange dst_range{.base = dst_base, .extent = dst_extent};
        const SubresourceRange src_range{.base = src_base, .extent = src_extent};
        PixelFormat dst_format = dst.info.format;
        if (GetFormatType(src.info.format) == SurfaceType::DepthStencil &&
            GetFormatType(dst_format) == SurfaceType::ColorTexture &&
            BytesPerBlock(dst_format) == 4) {
            dst_format = PixelFormat::A8B8G8R8_UNORM;
        }
        const ImageViewInfo dst_view_info(ImageViewType::e2D, dst_format, dst_range);
        const ImageViewInfo src_view_info(ImageViewType::e2D, src.info.format, src_range);
        const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
        Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
        const ImageViewId src_view_id = FindOrEmplaceImageView(src_id, src_view_info);
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        [[maybe_unused]] const Extent3D expected_size{
            .width = (std::min)(dst_view.size.width, src_view.size.width),
            .height = (std::min)(dst_view.size.height, src_view.size.height),
            .depth = (std::min)(dst_view.size.depth, src_view.size.depth),
        };
        const Extent3D scaled_extent = [is_rescaled, expected_size]() {
            if (!is_rescaled) {
                return expected_size;
            }
            const auto& resolution = Settings::values.resolution_info;
            return Extent3D{
                .width = resolution.ScaleUp(expected_size.width),
                .height = resolution.ScaleUp(expected_size.height),
                .depth = expected_size.depth,
            };
        }();
        UNIMPLEMENTED_IF(copy.extent != scaled_extent);

        runtime.ConvertImage(dst_framebuffer, dst_view, src_view);
    }
}

template <class P>
void TextureCache<P>::BindRenderTarget(ImageViewId* old_id, ImageViewId new_id) {
    if (*old_id == new_id) {
        return;
    }
    if (new_id) {
        const ImageViewBase& old_view = slot_image_views[new_id];
        if (True(old_view.flags & ImageViewFlagBits::PreemtiveDownload)) {
            const PendingDownload new_download{true, 0, old_view.image_id};
            uncommitted_downloads.emplace_back(new_download);
        }
    }
    *old_id = new_id;
}

template <class P>
std::pair<FramebufferId, ImageViewId> TextureCache<P>::RenderTargetFromImage(
    ImageId image_id, const ImageViewInfo& view_info) {
    const ImageViewId view_id = FindOrEmplaceImageView(image_id, view_info);
    const ImageBase& image = slot_images[image_id];
    const bool is_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    const bool is_color = GetFormatType(image.info.format) == SurfaceType::ColorTexture;
    const ImageViewId color_view_id = is_color ? view_id : ImageViewId{};
    const ImageViewId depth_view_id = is_color ? ImageViewId{} : view_id;
    Extent3D extent = MipSize(image.info.size, view_info.range.base.level);
    if (is_rescaled) {
        const auto& resolution = Settings::values.resolution_info;
        extent.width = resolution.ScaleUp(extent.width);
        if (image.info.type == ImageType::e2D) {
            extent.height = resolution.ScaleUp(extent.height);
        }
    }
    const u32 num_samples = image.info.num_samples;
    const auto [samples_x, samples_y] = SamplesLog2(num_samples);
    const FramebufferId framebuffer_id = GetFramebufferId(RenderTargets{
        .color_buffer_ids = {color_view_id},
        .depth_buffer_id = depth_view_id,
        .size = {extent.width >> samples_x, extent.height >> samples_y},
        .is_rescaled = is_rescaled,
    });
    return {framebuffer_id, view_id};
}

template <class P>
bool TextureCache<P>::IsFullClear(ImageViewId id) {
    if (!id) {
        return true;
    }
    const ImageViewBase& image_view = slot_image_views[id];
    const ImageBase& image = slot_images[image_view.image_id];
    const Extent3D size = image_view.size;
    const auto& regs = maxwell3d->regs;
    const auto& scissor = regs.scissor_test[0];
    if (image.info.resources.levels > 1 || image.info.resources.layers > 1) {
        // Images with multiple resources can't be cleared in a single call
        return false;
    }
    if (regs.clear_control.use_scissor == 0) {
        // If scissor testing is disabled, the clear is always full
        return true;
    }
    // Make sure the clear covers all texels in the subresource
    return scissor.min_x == 0 && scissor.min_y == 0 && scissor.max_x >= size.width &&
           scissor.max_y >= size.height;
}

template <class P>
void TextureCache<P>::CreateChannel(struct Tegra::Control::ChannelState& channel) {
    VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo>::CreateChannel(channel);
    const auto it = channel_map.find(channel.bind_id);
    auto* this_state = &channel_storage[it->second];
    const auto& this_as_ref = address_spaces[channel.memory_manager->GetID()];
    this_state->gpu_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2];
    this_state->sparse_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2 + 1];
}

/// Bind a channel for execution.
template <class P>
void TextureCache<P>::OnGPUASRegister([[maybe_unused]] size_t map_id) {
    gpu_page_table_storage.emplace_back();
    gpu_page_table_storage.emplace_back();
}

} // namespace VideoCommon
