// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/texture_cache/kledd_texture_pack.h"

#include <atomic>
#include <sstream>
#include <cmath>
#include <algorithm>
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

struct TintRule {
    u32 format;
    u32 width;
    u32 height;
    u32 layers; // 0 = any
    float mul[3];
    float keep; // share of the original color kept
};

struct State {
    std::mutex mutex;
    std::atomic<bool> dumping{false};
    std::atomic<bool> has_replacements{false};
    std::atomic<bool> scanned{false};
    std::unordered_map<u64, fs::path> replace_files;
    std::unordered_map<u64, std::vector<u8>> replace_cache;
    std::unordered_set<u64> bad_size_logged;
    std::unordered_set<u64> dumped;
    std::vector<TintRule> rules;
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
    s.rules.clear();
    std::ifstream rules_file(dir / "rules.txt");
    std::string line;
    while (std::getline(rules_file, line)) {
        std::istringstream in(line);
        std::string kind;
        TintRule rule{};
        if (!(in >> kind) || kind != "tint") {
            continue;
        }
        if (in >> rule.format >> rule.width >> rule.height >> rule.layers >> rule.mul[0] >>
            rule.mul[1] >> rule.mul[2] >> rule.keep) {
            s.rules.push_back(rule);
        }
    }
    s.has_replacements = !s.replace_files.empty() || !s.rules.empty();
    LOG_INFO(HW_GPU, "[Kledd] texture pack: {} tint rule(s)", s.rules.size());
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

u16 TintColor565(u16 c, const TintRule& r) {
    const float rgb[3] = {static_cast<float>((c >> 11) & 31) / 31.0f,
                          static_cast<float>((c >> 5) & 63) / 63.0f,
                          static_cast<float>(c & 31) / 31.0f};
    const float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    float out[3];
    for (int i = 0; i < 3; ++i) {
        out[i] = std::clamp(lum * r.mul[i], 0.0f, 1.0f) * (1.0f - r.keep) + rgb[i] * r.keep;
    }
    const u16 red = static_cast<u16>(std::lround(out[0] * 31.0f));
    const u16 green = static_cast<u16>(std::lround(out[1] * 63.0f));
    const u16 blue = static_cast<u16>(std::lround(out[2] * 31.0f));
    return static_cast<u16>((red << 11) | (green << 5) | blue);
}

// The tint is linear in RGB, so tinting the two BC1 endpoints tints every texel. The endpoint
// order picks the block mode, so the order is kept and the indices are remapped on a swap.
void TintBc1(std::span<u8> data, const TintRule& rule) {
    for (std::size_t off = 0; off + 8 <= data.size(); off += 8) {
        u8* b = data.data() + off;
        const u16 c0 = static_cast<u16>(b[0] | (b[1] << 8));
        const u16 c1 = static_cast<u16>(b[2] | (b[3] << 8));
        const bool four_color = c0 > c1;
        u16 n0 = TintColor565(c0, rule);
        u16 n1 = TintColor565(c1, rule);
        u32 idx = static_cast<u32>(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
        const bool swap = four_color ? (n0 < n1) : (n0 > n1);
        if (four_color && n0 == n1) {
            idx = 0; // one color left; any index is the same color
        } else if (swap) {
            std::swap(n0, n1);
            u32 remapped = 0;
            for (int t = 0; t < 16; ++t) {
                u32 v = (idx >> (2 * t)) & 3;
                if (four_color) {
                    v ^= 1; // 0<->1, 2<->3
                } else if (v < 2) {
                    v ^= 1; // 0<->1, midpoint and transparent stay
                }
                remapped |= v << (2 * t);
            }
            idx = remapped;
        }
        b[0] = static_cast<u8>(n0);
        b[1] = static_cast<u8>(n0 >> 8);
        b[2] = static_cast<u8>(n1);
        b[3] = static_cast<u8>(n1 >> 8);
        b[4] = static_cast<u8>(idx);
        b[5] = static_cast<u8>(idx >> 8);
        b[6] = static_cast<u8>(idx >> 16);
        b[7] = static_cast<u8>(idx >> 24);
    }
}

} // namespace

bool ApplyRules(const ImageInfo& info, std::span<u8> linear) {
    EnsureScanned();
    State& s = GetState();
    const auto format = static_cast<u32>(info.format);
    const bool bc1 = info.format == VideoCore::Surface::PixelFormat::BC1_RGBA_UNORM ||
                     info.format == VideoCore::Surface::PixelFormat::BC1_RGBA_SRGB;
    if (!bc1) {
        return false;
    }
    TintRule rule{};
    {
        std::scoped_lock lock{s.mutex};
        const auto it = std::find_if(s.rules.begin(), s.rules.end(), [&](const TintRule& r) {
            return r.format == format && r.width == info.size.width &&
                   r.height == info.size.height &&
                   (r.layers == 0 || r.layers == static_cast<u32>(info.resources.layers));
        });
        if (it == s.rules.end()) {
            return false;
        }
        rule = *it;
    }
    TintBc1(linear, rule);
    return true;
}

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
