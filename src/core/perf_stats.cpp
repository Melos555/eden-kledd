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
#include "common/kledd_mouse_gyro.h"
#include "common/logging.h"
#include "common/scm_rev.h"
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

    const s64 now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    std::scoped_lock lock{kledd_mutex};
    if (kledd_last_frame_ns != 0) {
        // eden-kledd mouse gyro: tell the mouse driver how long the game's frames are
        Common::KleddMouseGyro::game_frame_ns.store(now_ns - kledd_last_frame_ns);
    }
    kledd_last_frame_ns = now_ns;
    // eden-kledd frame recorder: only while the recorder is switched on
    if (kledd_segment_active) {
        kledd_frames.push_back(now_ns);
    }
}


namespace {

/// eden-kledd: the settings that matter for frame pacing, as label=value pairs
std::vector<std::string> KleddSettingsPairs() {
    const auto& v = Settings::values;
    const auto on = [](bool b) { return std::string(b ? "on" : "off"); };
    return {
        fmt::format("GPU mode={}", v.gpu_accuracy.ToString()),
        fmt::format("ASTC decode={}", v.accelerate_astc.ToString()),
        fmt::format("ASTC recompression={}", v.astc_recompression.ToString()),
        fmt::format("VRAM mode={}", v.vram_usage_mode.ToString()),
        fmt::format("Resolution={}", v.resolution_setup.ToString()),
        fmt::format("Async shaders={}", on(v.use_asynchronous_shaders.GetValue())),
        "DMA fix=on", // the scan readback fix is always on
        "Gentle cleanup=no effect (hidden)",
        "Keep seconds=5 (locked)",
        "Descriptor fix=on (locked)",
        "Max descriptors=256 (locked)",
        fmt::format("GPU BC7={}", on(v.astc_recompression.GetValue() ==
                                     Settings::AstcRecompression::Bc7)),
        fmt::format("Long frame log={}", on(v.kledd_frame_log.GetValue())),
        "Audio hold=on (locked)",
        "Resumable cleanup=off (locked)",
        "Cheat write fix=on (locked)",
        "Delta upload=off (locked)",
        "Early buffer release=off (locked)",
        "Async unswizzle=GPU / chunk Normal (locked)",
        fmt::format("Mouse gyro={}",
                    v.kledd_mouse_gyro.GetValue()
                        ? fmt::format("on (yaw {}, smoothing {}, gravity {})",
                                      v.kledd_mouse_gyro_yaw_z.GetValue() ? "Z" : "Y",
                                      v.kledd_mouse_gyro_smoothing.GetValue(),
                                      on(v.kledd_mouse_gyro_gravity.GetValue()))
                        : std::string("off")),
    };
}

struct KleddStretchStats {
    double seconds{};
    std::size_t frames{};
    double avg{}, median{}, p5{}, low1{}, low01{}, min{}, worst_ms{};
    std::ptrdiff_t over50{}, over100{}, over250{}, over1000{};
};

/// Frame statistics for stamps[begin..end] (inclusive), at least 3 stamps
KleddStretchStats KleddStats(const std::vector<s64>& stamps, std::size_t begin, std::size_t end) {
    std::vector<double> ft;
    ft.reserve(end - begin);
    for (std::size_t i = begin + 1; i <= end; ++i) {
        ft.push_back(static_cast<double>(stamps[i] - stamps[i - 1]) / 1e6);
    }
    KleddStretchStats r;
    r.frames = ft.size();
    const double total_ms = std::accumulate(ft.begin(), ft.end(), 0.0);
    r.seconds = total_ms / 1000.0;
    std::vector<double> sorted = ft;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t count = sorted.size();
    const auto pct = [&](double p) {
        return sorted[(std::min)(count - 1,
                                 static_cast<std::size_t>(p * static_cast<double>(count - 1)))];
    };
    const auto low_avg_fps = [&](double fraction) {
        const std::size_t k = (std::max)(
            std::size_t{1}, static_cast<std::size_t>(fraction * static_cast<double>(count)));
        const double sum = std::accumulate(sorted.end() - static_cast<std::ptrdiff_t>(k),
                                           sorted.end(), 0.0);
        return 1000.0 / (sum / static_cast<double>(k));
    };
    const auto over = [&](double ms) {
        return std::count_if(ft.begin(), ft.end(), [ms](double v) { return v > ms; });
    };
    r.avg = 1000.0 * static_cast<double>(count) / total_ms;
    r.median = 1000.0 / pct(0.5);
    r.p5 = 1000.0 / pct(0.95);
    r.low1 = low_avg_fps(0.01);
    r.low01 = low_avg_fps(0.001);
    r.worst_ms = sorted.back();
    r.min = 1000.0 / r.worst_ms;
    r.over50 = over(50.0);
    r.over100 = over(100.0);
    r.over250 = over(250.0);
    r.over1000 = over(1000.0);
    return r;
}

std::string KleddClock(std::time_t t, const char* format) {
    std::ostringstream out;
    out << std::put_time(std::localtime(&t), format);
    return out.str();
}

} // namespace

bool PerfStats::KleddToggleRecorder(std::string& saved_path) {
    std::scoped_lock lock{kledd_mutex};
    if (!kledd_segment_active) {
        kledd_frames.clear();
        kledd_frames.reserve(65536);
        kledd_cuts.assign(1, 0);
        kledd_cut_times.assign(1, std::time(nullptr));
        kledd_session_files.clear();
        kledd_segment_active = true;
        return true;
    }
    // switching off closes the last stretch as a final checkpoint
    if (kledd_frames.size() >= kledd_cuts.back() + 3) {
        kledd_cuts.push_back(kledd_frames.size() - 1);
        kledd_cut_times.push_back(std::time(nullptr));
    }
    saved_path = KleddWriteSession();
    kledd_segment_active = false;
    kledd_frames.clear();
    return false;
}

std::string PerfStats::KleddSaveRecording() {
    std::scoped_lock lock{kledd_mutex};
    if (!kledd_segment_active || kledd_frames.size() < kledd_cuts.back() + 3) {
        return {};
    }
    kledd_cuts.push_back(kledd_frames.size() - 1);
    kledd_cut_times.push_back(std::time(nullptr));
    return KleddWriteSession();
}

bool PerfStats::KleddIsRecording() {
    std::scoped_lock lock{kledd_mutex};
    return kledd_segment_active;
}

std::string PerfStats::KleddWriteSession() {
    // called with kledd_mutex held; one file per recorder session, rewritten at every checkpoint
    const std::vector<s64>& stamps = kledd_frames;
    const std::size_t checkpoints = kledd_cuts.size() - 1;
    if (checkpoints == 0) {
        return {};
    }
    const std::size_t last = kledd_cuts.back();
    const std::time_t start_time = kledd_cut_times.front();
    const std::time_t end_time = kledd_cut_times.back();

    // eden-kledd: which build made this recording, e.g. "smooth fd8e7be"
    std::string build{Common::g_scm_branch};
    if (build.rfind("kledd-lab-", 0) == 0) {
        build.erase(0, 10);
    }
    for (char& c : build) {
        if (c == '/' || c == '\\' || c == ':' || c == '[' || c == ']') {
            c = '-';
        }
    }
    const std::string rev = std::string{Common::g_scm_rev}.substr(0, 7);
    const std::string build_tag = build.empty() ? rev : fmt::format("{} {}", build, rev);
    const std::string span = fmt::format("[{}] [{} == {}]", KleddClock(start_time, "%m-%d"),
                                         KleddClock(start_time, "%H.%M.%S"),
                                         KleddClock(end_time, "%H.%M.%S"));

    // "kledd_frames [smooth fd8e7be] [11-03] [10.59.39 == 11.02.28].txt"; the end time moves with
    // every checkpoint, so the previous version of this session's files is removed first
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const auto txt_path = dir / fmt::format("kledd_frames [{}] {}.txt", build_tag, span);
    const auto csv_path = dir / fmt::format("kledd_frames [{}] {}.csv", build_tag, span);
    if (!Common::FS::CreateParentDir(txt_path)) {
        return {};
    }
    for (const auto& old : kledd_session_files) {
        if (old != txt_path && old != csv_path) {
            void(Common::FS::RemoveFile(old));
        }
    }
    kledd_session_files = {txt_path, csv_path};

    std::string txt;
    txt += "Eden Kledd frame recording,\n";
    txt += span + fmt::format(" ({} checkpoint{})\n", checkpoints, checkpoints == 1 ? "" : "s");
    txt += fmt::format("[Build] {} (Eden {})\n\n", build_tag, Common::g_build_fullname);

    txt += "--:[Settings]:--\n\n";
    const auto pairs = KleddSettingsPairs();
    std::size_t width = 0;
    for (std::size_t i = 0; i < pairs.size(); i += 2) {
        width = (std::max)(width, pairs[i].size());
    }
    for (std::size_t i = 0; i < pairs.size(); i += 2) {
        if (i + 1 < pairs.size()) {
            txt += fmt::format("[{:<{}} | {}\n", pairs[i], width, pairs[i + 1]);
        } else {
            txt += fmt::format("[{}\n", pairs[i]);
        }
    }

    txt += "\n--:[Legend]:--\n";
    txt += "APS    = Average FPS\n";
    txt += "MPS    = Median FPS\n";
    txt += "P5     = P5 FPS (95% of frames are faster than this)\n";
    txt += "1%     = 1% low FPS\n";
    txt += "0.1    = 0.1% low FPS\n";
    txt += "MinFPS = Min FPS (the single slowest frame)\n";
    txt += "Hitch  = frames over 100 ms\n";

    const auto stats_block = [](const KleddStretchStats& st) {
        return fmt::format("APS:    {:.1f}\nMPS:    {:.1f}\nP5:     {:.1f}\n1%:     {:.1f}\n"
                           "0.1:    {:.1f}\nMinFPS: {:.1f}  (worst frame {:.0f} ms)\n"
                           "Hitch:  {}\n",
                           st.avg, st.median, st.p5, st.low1, st.low01, st.min, st.worst_ms,
                           st.over100);
    };
    for (std::size_t c = 0; c < checkpoints; ++c) {
        const auto st = KleddStats(stamps, kledd_cuts[c], kledd_cuts[c + 1]);
        txt += fmt::format("\n\n[{} == {}]     {:.1f} s ({} frames)\n",
                           KleddClock(kledd_cut_times[c], "%H.%M.%S"),
                           KleddClock(kledd_cut_times[c + 1], "%H.%M.%S"), st.seconds, st.frames);
        txt += stats_block(st);
    }

    const auto all = KleddStats(stamps, 0, last);
    txt += fmt::format("\n--:[Summary of all checkpoints]:--     {:.1f} s ({} frames)\n",
                       all.seconds, all.frames);
    txt += fmt::format("Average FPS:   {:.1f}\n", all.avg);
    txt += fmt::format("Median FPS:    {:.1f}\n", all.median);
    txt += fmt::format("P5 FPS:        {:.1f}\n", all.p5);
    txt += fmt::format("1% low FPS:    {:.1f}\n", all.low1);
    txt += fmt::format("0.1% low FPS:  {:.1f}\n", all.low01);
    txt += fmt::format("Min FPS:       {:.1f}  (worst frame {:.0f} ms)\n", all.min, all.worst_ms);
    txt += fmt::format("Frames over 50 ms: {} | over 100 ms: {} | over 250 ms: {} | over 1 s: {}\n\n",
                       all.over50, all.over100, all.over250, all.over1000);

    // Worst freezes with the clock time and the checkpoint they fell in
    std::vector<std::size_t> order;
    order.reserve(last);
    for (std::size_t i = 1; i <= last; ++i) {
        order.push_back(i);
    }
    const auto ft_of = [&](std::size_t i) {
        return static_cast<double>(stamps[i] - stamps[i - 1]) / 1e6;
    };
    const std::size_t top = (std::min)(std::size_t{10}, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(top), order.end(),
                      [&](std::size_t a, std::size_t b) { return ft_of(a) > ft_of(b); });
    txt += "Worst freezes (how long, when, and in which checkpoint):\n";
    for (std::size_t n = 0; n < top; ++n) {
        const std::size_t i = order[n];
        const auto at = start_time + static_cast<std::time_t>(
                                         static_cast<double>(stamps[i] - stamps[0]) / 1e9);
        std::size_t cp = 1;
        while (cp < checkpoints && i > kledd_cuts[cp]) {
            ++cp;
        }
        txt += fmt::format("  {:>7.0f} ms   at {}   (checkpoint {})\n", ft_of(i),
                           KleddClock(at, "%H.%M.%S"), cp);
    }

    std::string settings_line;
    for (const auto& pair : pairs) {
        settings_line += (settings_line.empty() ? "" : " | ") + pair;
    }
    txt += "\nPaste row for the test sheet (tab separated):\n";
    txt += fmt::format("{} {}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.1f}\t{:.0f}\t{}\t{}\n",
                       build_tag, span, all.seconds, all.avg, all.median, all.p5, all.low1,
                       all.min, all.worst_ms, all.over100, settings_line);

    std::string csv;
    csv.reserve(last * 32 + 64);
    csv += "frame,checkpoint,seconds_into_recording,frametime_ms\n";
    std::size_t cp = 1;
    for (std::size_t i = 1; i <= last; ++i) {
        while (cp < checkpoints && i > kledd_cuts[cp]) {
            ++cp;
        }
        csv += fmt::format("{},{},{:.3f},{:.3f}\n", i - 1, cp,
                           static_cast<double>(stamps[i] - stamps[0]) / 1e9, ft_of(i));
    }

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
