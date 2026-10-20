// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Common::Kledd {

/// eden-kledd: per-frame counters for the long-frame log (stutter investigation).
/// Written from the GPU and pipeline worker threads, read and reset once per frame.
struct FrameStats {
    std::atomic<u32> images_created{};
    std::atomic<u32> images_deleted{};
    std::atomic<u32> gc_deleted{};
    std::atomic<u32> gc_sync_downloads{};
    std::atomic<u32> join_dropped_gpu{};
    std::atomic<u32> uploads{};
    std::atomic<u64> upload_bytes{};
    std::atomic<u32> async_decodes{};
    std::atomic<u32> async_unswizzles{};
    std::atomic<u32> graphics_pipelines{};
    std::atomic<u32> compute_pipelines{};
};

inline FrameStats& GetFrameStats() {
    static FrameStats stats;
    return stats;
}

} // namespace Common::Kledd
