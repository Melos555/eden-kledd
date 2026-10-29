// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <fstream>
#include <mutex>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/kledd_mouse_gyro.h"

namespace Common::KleddMouseGyro {

void ProbeLog(const std::string& line) {
    static std::mutex mutex;
    static std::ofstream file;
    static bool opened = false;
    static const auto start = std::chrono::steady_clock::now();

    std::scoped_lock lock{mutex};
    if (!opened) {
        opened = true;
        const auto path =
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "kledd_mouse_gyro.txt";
        file.open(path, std::ios::out | std::ios::trunc);
    }
    if (!file.is_open()) {
        return;
    }
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    file << fmt::format("{:9.3f} {}\n", t, line);
    file.flush();
}

} // namespace Common::KleddMouseGyro
