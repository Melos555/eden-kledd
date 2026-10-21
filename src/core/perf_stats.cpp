// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>
#include <ctime>
#include <iomanip>
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/perf_stats.h"

using namespace std::chrono_literals;
using DoubleSecs = std::chrono::duration<double, std::chrono::seconds::period>;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// Purposefully ignore the first five frames, as there's a significant amount of overhead in
// booting that we shouldn't account for
constexpr std::size_t IgnoreFrames = 5;

namespace Core {

PerfStats::PerfStats(u64 title_id_) : title_id(title_id_) {}

PerfStats::~PerfStats() {
    if (!Settings::values.record_frame_times || title_id == 0) {
        return;
    }

    const std::time_t t = std::time(nullptr);
    std::ostringstream stream;
    std::copy(perf_history.begin() + IgnoreFrames, perf_history.begin() + current_index,
              std::ostream_iterator<double>(stream, "\n"));

    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    // %F Date format expanded is "%Y-%m-%d"
    const auto filename = fmt::format("{}_{:016X}.csv",
        [&] {
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&t), "%F-%H-%M");
            return oss.str();
        }(),
        title_id);

    const auto filepath = path / filename;

    if (Common::FS::CreateParentDir(filepath)) {
        Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::TextFile);
        void(file.WriteString(stream.str()));
    }
}

void PerfStats::BeginSystemFrame() {
    std::scoped_lock lock{object_mutex};

    frame_begin = Clock::now();
}

void PerfStats::EndSystemFrame() {
    std::scoped_lock lock{object_mutex};

    auto frame_end = Clock::now();
    const auto frame_time = frame_end - frame_begin;
    if (current_index < perf_history.size()) {
        perf_history[current_index++] =
            std::chrono::duration<double, std::milli>(frame_time).count();
    }
    accumulated_frametime += frame_time;
    system_frames += 1;

    previous_frame_length = frame_end - previous_frame_end;
    previous_frame_end = frame_end;
}

void PerfStats::EndGameFrame() {
    game_frames.fetch_add(1, std::memory_order_relaxed);

    // eden-kledd frame recorder: always on, one timestamp per presented frame
    const s64 now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    std::scoped_lock lock{kledd_mutex};
    kledd_frames[kledd_count % KleddRecordFrames] = now_ns;
    ++kledd_count;
}

std::string PerfStats::KleddSaveFrameRecording() {
    constexpr double KeepSeconds = 120.0;
    std::vector<s64> stamps;
    {
        std::scoped_lock lock{kledd_mutex};
        const std::size_t n = (std::min)(kledd_count, KleddRecordFrames);
        stamps.reserve(n);
        for (std::size_t i = kledd_count - n; i < kledd_count; ++i) {
            stamps.push_back(kledd_frames[i % KleddRecordFrames]);
        }
    }
    if (stamps.size() < 3) {
        return {};
    }
    const s64 last = stamps.back();
    const s64 keep_ns = static_cast<s64>(KeepSeconds * 1e9);
    std::size_t first = 0;
    while (first + 2 < stamps.size() && last - stamps[first] > keep_ns) {
        ++first;
    }

    // Frame times in ms, with seconds-before-save for each frame
    std::vector<double> ft;
    std::vector<double> ago;
    for (std::size_t i = first + 1; i < stamps.size(); ++i) {
        ft.push_back(static_cast<double>(stamps[i] - stamps[i - 1]) / 1e6);
        ago.push_back(static_cast<double>(last - stamps[i]) / 1e9);
    }
    const std::size_t count = ft.size();
    const double total_ms = std::accumulate(ft.begin(), ft.end(), 0.0);

    std::vector<double> sorted = ft;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&](double p) {
        const std::size_t idx =
            (std::min)(count - 1, static_cast<std::size_t>(p * static_cast<double>(count - 1)));
        return sorted[idx];
    };
    const auto low_avg_fps = [&](double fraction) {
        const std::size_t k = (std::max)(std::size_t{1},
                                     static_cast<std::size_t>(fraction * static_cast<double>(count)));
        const double sum = std::accumulate(sorted.end() - static_cast<std::ptrdiff_t>(k),
                                           sorted.end(), 0.0);
        return 1000.0 / (sum / static_cast<double>(k));
    };
    const auto over = [&](double ms) {
        return std::count_if(ft.begin(), ft.end(), [ms](double v) { return v > ms; });
    };

    const double avg_fps = 1000.0 * static_cast<double>(count) / total_ms;
    const double median_fps = 1000.0 / pct(0.5);
    const double p5_fps = 1000.0 / pct(0.95);
    const double low1 = low_avg_fps(0.01);
    const double low01 = low_avg_fps(0.001);
    const double min_fps = 1000.0 / sorted.back();

    // Worst freezes
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ft[a] > ft[b]; });

    const auto& v = Settings::values;
    const std::string settings_line = fmt::format(
        "GPU mode={} | ASTC decode={} | ASTC recompression={} | VRAM mode={} | Resolution={} | "
        "Async shaders={} | DMA fix={} | Gentle cleanup={} | Keep seconds={} | Descriptor fix={} | Max descriptors={} | GPU BC7={} | Long frame log={}",
        v.gpu_accuracy.ToString(), v.accelerate_astc.ToString(), v.astc_recompression.ToString(),
        v.vram_usage_mode.ToString(), v.resolution_setup.ToString(),
        v.use_asynchronous_shaders.GetValue() ? "on" : "off",
        v.kledd_dma_readback_fix.GetValue() ? "on" : "off",
        v.kledd_gentle_gc.GetValue() ? "on" : "off", v.kledd_gc_seconds.GetValue(),
        v.kledd_descriptor_stride_fix.GetValue() ? "on" : "off", v.kledd_descriptor_max.GetValue(),
        v.kledd_gpu_bc7.GetValue() ? "on" : "off",
        v.kledd_frame_log.GetValue() ? "on" : "off");

    const std::time_t t = std::time(nullptr);
    std::ostringstream stamp;
    stamp << std::put_time(std::localtime(&t), "%Y-%m-%d_%H-%M-%S");
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const auto csv_path = dir / fmt::format("kledd_frames_{}.csv", stamp.str());
    const auto txt_path = dir / fmt::format("kledd_frames_{}_summary.txt", stamp.str());
    if (!Common::FS::CreateParentDir(csv_path)) {
        return {};
    }

    std::string csv;
    csv.reserve(count * 24 + 256);
    csv += "frame,seconds_before_save,frametime_ms\n";
    for (std::size_t i = 0; i < count; ++i) {
        csv += fmt::format("{},{:.3f},{:.3f}\n", i, ago[i], ft[i]);
    }

    std::string txt;
    txt += fmt::format("Eden Kledd frame recording, saved {}\n", stamp.str());
    txt += settings_line + "\n\n";
    txt += fmt::format("Length:        {:.1f} s ({} frames)\n", total_ms / 1000.0, count);
    txt += fmt::format("Average FPS:   {:.1f}\n", avg_fps);
    txt += fmt::format("Median FPS:    {:.1f}\n", median_fps);
    txt += fmt::format("P5 FPS:        {:.1f}\n", p5_fps);
    txt += fmt::format("1% low FPS:    {:.1f}\n", low1);
    txt += fmt::format("0.1% low FPS:  {:.1f}\n", low01);
    txt += fmt::format("Min FPS:       {:.1f}  (worst frame {:.0f} ms)\n", min_fps, sorted.back());
    txt += fmt::format("Frames over 50 ms: {} | over 100 ms: {} | over 250 ms: {} | over 1 s: {}\n\n",
                       over(50.0), over(100.0), over(250.0), over(1000.0));
    txt += "Worst freezes (how long, and how many seconds before you pressed save):\n";
    for (std::size_t i = 0; i < (std::min)(std::size_t{10}, count); ++i) {
        const std::size_t f = order[i];
        txt += fmt::format("  {:>7.0f} ms   {:>6.1f} s before save\n", ft[f], ago[f]);
    }
    txt += "\nPaste row for the test sheet (tab separated):\n";
    txt += fmt::format("{}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.0f}\t{}\t{}\n",
                       stamp.str(), total_ms / 1000.0, avg_fps, median_fps, p5_fps, low1, min_fps,
                       sorted.back(), over(100.0), settings_line);

    Common::FS::IOFile csv_file(csv_path, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::TextFile);
    void(csv_file.WriteString(csv));
    Common::FS::IOFile txt_file(txt_path, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::TextFile);
    void(txt_file.WriteString(txt));
    LOG_INFO(Core, "[Kledd] Frame recording saved: {}", Common::FS::PathToUTF8String(txt_path));
    return Common::FS::PathToUTF8String(txt_path);
}

double PerfStats::GetMeanFrametime() const {
    std::scoped_lock lock{object_mutex};

    if (current_index <= IgnoreFrames) {
        return 0;
    }

    const double sum = std::accumulate(perf_history.begin() + IgnoreFrames,
                                       perf_history.begin() + current_index, 0.0);
    return sum / static_cast<double>(current_index - IgnoreFrames);
}

PerfStatsResults PerfStats::GetAndResetStats(microseconds current_system_time_us) {
    std::scoped_lock lock{object_mutex};

    const auto now = Clock::now();
    // Walltime elapsed since stats were reset
    const auto interval = duration_cast<DoubleSecs>(now - reset_point).count();

    const auto system_us_per_second = (current_system_time_us - reset_point_system_us) / interval;
    const auto current_frames = static_cast<double>(game_frames.load(std::memory_order_relaxed));
    const auto current_fps = current_frames / interval;
    const PerfStatsResults results{
        .system_fps = static_cast<double>(system_frames) / interval,
        .average_game_fps = (current_fps + previous_fps) / 2.0,
        .frametime = duration_cast<DoubleSecs>(accumulated_frametime).count() /
                     static_cast<double>(system_frames),
        .emulation_speed = system_us_per_second.count() / 1'000'000.0,
    };

    // Reset counters
    reset_point = now;
    reset_point_system_us = current_system_time_us;
    accumulated_frametime = Clock::duration::zero();
    system_frames = 0;
    game_frames.store(0, std::memory_order_relaxed);
    previous_fps = current_fps;

    return results;
}

double PerfStats::GetLastFrameTimeScale() const {
    std::scoped_lock lock{object_mutex};

    constexpr double FRAME_LENGTH = 1.0 / 60;
    return duration_cast<DoubleSecs>(previous_frame_length).count() / FRAME_LENGTH;
}

void SpeedLimiter::DoSpeedLimiting(microseconds current_system_time_us) {
    if (Settings::values.use_multi_core.GetValue() ||
        !Settings::values.use_speed_limit.GetValue()) {
        return;
    }

    auto now = Clock::now();

    const double sleep_scale = Settings::SpeedLimit() / 100.0;

    // Max lag caused by slow frames. Shouldn't be more than the length of a frame at the current
    // speed percent or it will clamp too much and prevent this from properly limiting to that
    // percent. High values means it'll take longer after a slow frame to recover and start
    // limiting
    const microseconds max_lag_time_us = duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(25ms / sleep_scale));
    speed_limiting_delta_err += duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(
            (current_system_time_us - previous_system_time_us) / sleep_scale));
    speed_limiting_delta_err -= duration_cast<microseconds>(now - previous_walltime);
    speed_limiting_delta_err =
        std::clamp(speed_limiting_delta_err, -max_lag_time_us, max_lag_time_us);

    if (speed_limiting_delta_err > microseconds::zero()) {
        std::this_thread::sleep_for(speed_limiting_delta_err);
        auto now_after_sleep = Clock::now();
        speed_limiting_delta_err -= duration_cast<microseconds>(now_after_sleep - now);
        now = now_after_sleep;
    }

    previous_system_time_us = current_system_time_us;
    previous_walltime = now;
}

} // namespace Core
