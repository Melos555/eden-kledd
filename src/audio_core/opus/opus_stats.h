// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace AudioCore::OpusDecoder {

/// eden-kledd: counters for hwopus decode calls, read by the audio renderer lateness diagnostic
struct Stats {
    static inline std::atomic<u64> decode_calls{};
    static inline std::atomic<u64> decode_microseconds{};
};

} // namespace AudioCore::OpusDecoder
