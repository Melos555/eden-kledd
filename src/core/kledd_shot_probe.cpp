// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/kledd_shot_probe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Core::KleddShotProbe {
namespace {

// Metroid Prime 4: Beyond, v1.1.0 (NSO build id 8275FBE1EC5191C2...)
constexpr u64 kTitleId = 0x010019A01E2F2000ULL;
// vtable of the gun FSM component (its constructor at main+0xA90E94 stores this; the script
// natives ShouldRapidFire, GunShooting, ... get this object as "this")
constexpr u64 kGunVtableOffset = 0x180CE08;

constexpr std::size_t kSnapshotSize = 0x300; // bytes of the component that are sampled
constexpr u64 kScanChunk = 16ULL * 1024 * 1024; // heap bytes scanned per frame while searching
constexpr u64 kScanBlock = 0x10000;
constexpr std::size_t kMaxGuns = 4;

// Fields named in the log (offset, label, is_float). Everything else that changes is listed raw.
struct Field {
    u32 offset;
    const char* name;
    bool is_float;
    u32 size;
};
constexpr std::array<Field, 19> kFields{{
    {0x198, "hold", true, 4},        // hold-to-charge timer (vs tweak+0x504)
    {0x1A0, "t1a0", true, 4},
    {0x1A4, "b1a4", false, 1},
    {0x1A8, "shots", false, 4},      // shots this press (GunShooting enter: ++)
    {0x1AC, "i1ac", false, 4},
    {0x1BC, "noShoot", false, 4},    // AllowShooting: >0 = blocked
    {0x1C4, "i1c4", false, 4},
    {0x1C8, "mslCd", true, 4},       // missile cooldown
    {0x1CC, "msl2", true, 4},        // missile second window
    {0x1D0, "t1d0", true, 4},
    {0x1D4, "t1d4", true, 4},        // ShouldRapidFire needs >= 0.158
    {0x1F4, "mslType", false, 4},
    {0x220, "fire", false, 1},       // set to 1 when GunShooting is entered
    {0x22C, "b22c", false, 1},
    {0x22D, "b22d", false, 1},
    {0x232, "b232", false, 1},
    {0x238, "b238", false, 1},
    {0x282, "needPress", false, 1},  // RequireAnotherPressForShooting
    {0x283, "fastInto", false, 1},
}};

struct State {
    u64 main_begin = 0;
    std::vector<u64> guns;
    std::vector<std::array<u8, kSnapshotSize>> prev;
    std::vector<bool> have_prev;
    // heap scan
    bool scanning = false;
    u64 scan_pos = 0;
    u64 scan_end = 0;
    std::vector<u64> found;
    u64 frames_since_scan = 0;
    // output
    std::unique_ptr<Common::FS::IOFile> file;
    u64 frame = 0;
    std::chrono::steady_clock::time_point t0{};
};

State& GetState() {
    static State s;
    return s;
}

void Write(State& s, const std::string& line) {
    if (s.file && s.file->IsOpen()) {
        void(s.file->WriteString(line));
        void(s.file->WriteString("\n"));
    }
}

void Close(State& s) {
    if (s.file) {
        void(s.file->Flush());
        s.file.reset();
    }
    s.guns.clear();
    s.prev.clear();
    s.have_prev.clear();
    s.scanning = false;
    s.found.clear();
    s.main_begin = 0;
}

bool Open(State& s) {
    const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    const auto path = dir / "kledd_shot_probe.txt";
    if (!Common::FS::CreateParentDir(path)) {
        return false;
    }
    s.file = std::make_unique<Common::FS::IOFile>(path, Common::FS::FileAccessMode::Write,
                                                  Common::FS::FileType::TextFile);
    if (!s.file->IsOpen()) {
        s.file.reset();
        return false;
    }
    s.frame = 0;
    s.t0 = std::chrono::steady_clock::now();
    Write(s, "# eden-kledd shot probe (MP4 Beyond 1.1.0). One line per game frame per gun object.");
    Write(s, "# f=frame t=ms since probe on; named fields of the gun FSM component; "
             "chg: other dwords of the first 0x300 bytes that changed (+off=old->new).");
    return true;
}

void StartScan(State& s, Kernel::KProcess& process) {
    const auto& pt = process.GetPageTable();
    s.scan_pos = GetInteger(pt.GetHeapRegionStart());
    s.scan_end = s.scan_pos + pt.GetHeapRegionSize();
    s.found.clear();
    s.scanning = true;
    Write(s, fmt::format("# scanning heap 0x{:X}-0x{:X} for gun vtable 0x{:X}", s.scan_pos,
                         s.scan_end, s.main_begin + kGunVtableOffset));
}

void ContinueScan(State& s, Core::Memory::Memory& memory) {
    const u64 vt = s.main_begin + kGunVtableOffset;
    std::vector<u8> buf(kScanBlock);
    const u64 stop = std::min(s.scan_end, s.scan_pos + kScanChunk);
    for (; s.scan_pos < stop; s.scan_pos += kScanBlock) {
        if (!memory.IsValidVirtualAddressRange(s.scan_pos, kScanBlock)) {
            continue;
        }
        memory.ReadBlock(s.scan_pos, buf.data(), kScanBlock);
        for (std::size_t i = 0; i + 8 <= kScanBlock; i += 8) {
            u64 v;
            std::memcpy(&v, buf.data() + i, 8);
            if (v == vt && s.found.size() < 64) {
                s.found.push_back(s.scan_pos + i);
            }
        }
    }
    if (s.scan_pos < s.scan_end) {
        return;
    }
    s.scanning = false;
    s.frames_since_scan = 1; // next rescan (if nothing was found) only after ~300 frames
    s.guns.clear();
    for (const u64 a : s.found) {
        if (s.guns.size() >= kMaxGuns) {
            break;
        }
        s.guns.push_back(a);
    }
    s.prev.assign(s.guns.size(), {});
    s.have_prev.assign(s.guns.size(), false);
    std::string line = fmt::format("# scan done: {} candidate(s)", s.found.size());
    for (std::size_t i = 0; i < s.guns.size(); ++i) {
        line += fmt::format(" g{}=0x{:X}", i, s.guns[i]);
    }
    Write(s, line);
}

u64 Load(const u8* p, u32 size) {
    u64 v = 0;
    std::memcpy(&v, p, size);
    return v;
}

float AsFloat(u64 bits) {
    const u32 b = static_cast<u32>(bits);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

bool IsNamed(u32 off) {
    for (const auto& f : kFields) {
        if (off + 4 > f.offset && off < f.offset + f.size) {
            return true;
        }
    }
    return false;
}

void Sample(State& s, Core::Memory::Memory& memory) {
    const u64 vt = s.main_begin + kGunVtableOffset;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                s.t0)
                          .count();
    bool lost = false;
    for (std::size_t g = 0; g < s.guns.size(); ++g) {
        std::array<u8, kSnapshotSize> cur{};
        if (!memory.IsValidVirtualAddressRange(s.guns[g], kSnapshotSize)) {
            lost = true;
            continue;
        }
        memory.ReadBlock(s.guns[g], cur.data(), kSnapshotSize);
        if (Load(cur.data(), 8) != vt) {
            lost = true; // object was freed or moved
            continue;
        }
        std::string line = fmt::format("f={} t={:.1f} g{}", s.frame, ms, g);
        for (const auto& f : kFields) {
            const u64 v = Load(cur.data() + f.offset, f.size);
            if (f.is_float) {
                line += fmt::format(" {}={:.4f}", f.name, AsFloat(v));
            } else {
                line += fmt::format(" {}={}", f.name, static_cast<s64>(static_cast<s32>(v)));
            }
        }
        if (s.have_prev[g]) {
            std::string chg;
            for (u32 off = 0x40; off + 4 <= kSnapshotSize; off += 4) {
                if (IsNamed(off)) {
                    continue;
                }
                const u64 a = Load(s.prev[g].data() + off, 4);
                const u64 b = Load(cur.data() + off, 4);
                if (a != b) {
                    chg += fmt::format(" +{:X}={:08X}->{:08X}({:.4g})", off, a, b, AsFloat(b));
                }
            }
            if (!chg.empty()) {
                line += " chg:" + chg;
            }
        }
        Write(s, line);
        s.prev[g] = cur;
        s.have_prev[g] = true;
    }
    if (lost || s.guns.empty()) {
        s.guns.clear();
    }
}

} // namespace

void OnGameFrame(Core::System& system) {
    State& s = GetState();
    if (!Settings::values.kledd_shot_probe.GetValue()) {
        if (s.file) {
            Close(s);
        }
        return;
    }
    if (system.GetApplicationProcessProgramID() != kTitleId) {
        return;
    }
    auto* process = system.ApplicationProcess();
    const u64 main_begin = system.GetMainModuleBegin();
    if (!process || main_begin == 0) {
        return;
    }
    if (!s.file && !Open(s)) {
        return;
    }
    if (s.main_begin != main_begin) {
        s.main_begin = main_begin;
        s.guns.clear();
        s.scanning = false;
        Write(s, fmt::format("# main module at 0x{:X}", main_begin));
    }
    auto& memory = process->GetMemory();
    ++s.frame;
    if (s.scanning) {
        ContinueScan(s, memory);
        return;
    }
    if (s.guns.empty()) {
        // (re)scan, but not more often than every ~5 s at 60 fps
        if (s.frames_since_scan++ % 300 == 0) {
            StartScan(s, *process);
        }
        return;
    }
    Sample(s, memory);
}

} // namespace Core::KleddShotProbe
