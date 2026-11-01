// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

namespace Common::Kledd {

/// eden-kledd diagnostic: counts slow events and lets the caller log one summary line at most
/// every 2 seconds. Safe to use from any thread.
struct SlowEventLog {
    std::atomic<u32> count{};
    std::atomic<u32> max_ms{};
    std::atomic<s64> last_log_ns{};

    /// Records one slow event of `ms` milliseconds. Returns true when the caller should log now;
    /// `out_count` and `out_max` then hold the events since the previous log, and are reset.
    bool Note(u32 ms, u32& out_count, u32& out_max) {
        count.fetch_add(1, std::memory_order_relaxed);
        u32 prev = max_ms.load(std::memory_order_relaxed);
        while (prev < ms && !max_ms.compare_exchange_weak(prev, ms)) {
        }
        const s64 now = std::chrono::steady_clock::now().time_since_epoch().count();
        s64 last = last_log_ns.load(std::memory_order_relaxed);
        if (last != 0 && now - last < 2000000000) {
            return false;
        }
        if (!last_log_ns.compare_exchange_strong(last, now)) {
            return false;
        }
        out_count = count.exchange(0, std::memory_order_relaxed);
        out_max = max_ms.exchange(0, std::memory_order_relaxed);
        return true;
    }
};

} // namespace Common::Kledd
