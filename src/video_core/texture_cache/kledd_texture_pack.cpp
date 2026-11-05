// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/texture_cache/kledd_texture_pack.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace VideoCommon::KleddPack {

namespace {

namespace fs = std::filesystem;

struct State {
    std::mutex mutex;
    std::atomic<bool> dumping{false};
    std::atomic<bool> has_replacements{false};
    std::atomic<bool> scanned{false};
    std::unordered_map<u64, fs::path> replace_files;
    std::unordered_map<u64, std::vector<u8>> replace_cache;
    std::unordered_set<u64> bad_size_logged;
    std::unordered_set<u64> dumped;
};

State& GetState() {
    static State state;
    return state;
}

fs::path BaseDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "kledd_textures";
}

void ScanLocked(State& s) {
    s.replace_files.clear();
    s.replace_cache.clear();
    s.bad_size_logged.clear();
    const fs::path dir = BaseDir() / "replace";
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".bin") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        if (stem.size() != 16) {
            continue;
        }
        try {
            const u64 hash = std::stoull(stem, nullptr, 16);
            s.replace_files.emplace(hash, entry.path());
        } catch (...) {
        }
    }
    s.has_replacements = !s.replace_files.empty();
    s.scanned = true;
    LOG_INFO(HW_GPU, "[Kledd] texture pack: {} replacement(s) in {}", s.replace_files.size(),
             dir.string());
}

void EnsureScanned() {
    State& s = GetState();
    if (s.scanned.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{s.mutex};
    if (!s.scanned) {
        ScanLocked(s);
    }
}

} // namespace

bool Active() {
    EnsureScanned();
    const State& s = GetState();
    return s.dumping.load(std::memory_order_relaxed) ||
           s.has_replacements.load(std::memory_order_relaxed);
}

void SetDumping(bool enabled) {
    GetState().dumping = enabled;
    LOG_INFO(HW_GPU, "[Kledd] texture dumping {}", enabled ? "on" : "off");
}

bool IsDumping() {
    return GetState().dumping.load(std::memory_order_relaxed);
}

void RequestReload() {
    GetState().scanned = false;
}

bool NeedsDump(u64 hash) {
    State& s = GetState();
    if (!s.dumping.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{s.mutex};
    return !s.dumped.contains(hash);
}

u64 Hash(std::span<const u8> guest) {
    return Common::CityHash64(reinterpret_cast<const char*>(guest.data()), guest.size());
}

const std::vector<u8>* Find(u64 hash, std::size_t expected_size) {
    EnsureScanned();
    State& s = GetState();
    if (!s.has_replacements.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    std::scoped_lock lock{s.mutex};
    const auto file = s.replace_files.find(hash);
    if (file == s.replace_files.end()) {
        return nullptr;
    }
    auto cached = s.replace_cache.find(hash);
    if (cached == s.replace_cache.end()) {
        std::ifstream in(file->second, std::ios::binary);
        std::vector<u8> data((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        cached = s.replace_cache.emplace(hash, std::move(data)).first;
    }
    if (cached->second.size() != expected_size) {
        if (s.bad_size_logged.insert(hash).second) {
            LOG_WARNING(HW_GPU, "[Kledd] texture pack: {:016X}.bin is {} bytes, expected {}",
                        hash, cached->second.size(), expected_size);
        }
        return nullptr;
    }
    return &cached->second;
}

void Dump(u64 hash, const ImageInfo& info, std::span<const u8> linear,
          std::span<const BufferImageCopy> copies) {
    State& s = GetState();
    {
        std::scoped_lock lock{s.mutex};
        if (!s.dumped.insert(hash).second) {
            return;
        }
    }
    const fs::path dir = BaseDir() / "dump";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string name = fmt::format("{:016X}", hash);
    {
        std::ofstream out(dir / (name + ".bin"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(linear.data()),
                  static_cast<std::streamsize>(linear.size()));
    }
    std::string text = fmt::format(
        "hash {:016X}\nformat {}\ntype {}\nsize {} {} {}\nlevels {}\nlayers {}\nbytes {}\n", hash,
        static_cast<u32>(info.format), static_cast<u32>(info.type), info.size.width,
        info.size.height, info.size.depth, info.resources.levels, info.resources.layers,
        linear.size());
    for (const BufferImageCopy& copy : copies) {
        text += fmt::format("copy offset {} size {} row {} height {} level {} layer {} layers {} "
                            "extent {} {} {}\n",
                            copy.buffer_offset, copy.buffer_size, copy.buffer_row_length,
                            copy.buffer_image_height, copy.image_subresource.base_level,
                            copy.image_subresource.base_layer, copy.image_subresource.num_layers,
                            copy.image_extent.width, copy.image_extent.height,
                            copy.image_extent.depth);
    }
    std::ofstream(dir / (name + ".txt")) << text;
}

} // namespace VideoCommon::KleddPack
