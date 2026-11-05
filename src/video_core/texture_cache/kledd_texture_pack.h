// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// eden-kledd: texture dumping and replacement packs.
//
// Textures are identified by a hash of their swizzled guest bytes. A dump writes the unswizzled
// (linear) guest-format data plus a text file describing the layout. A replacement is a file with
// the same linear layout and size, placed in the replace folder as <hash>.bin; it is copied over
// the unswizzled data before the normal decode/convert/upload, so every host path stays the same.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon::KleddPack {

/// True when dumping is on or the replace folder has files.
[[nodiscard]] bool Active();

void SetDumping(bool enabled);
[[nodiscard]] bool IsDumping();

/// Rescan the replace folder on the next lookup.
void RequestReload();

[[nodiscard]] u64 Hash(std::span<const u8> guest);

/// Replacement for this hash, or nullptr. Ignored (and logged once) when the size differs.
[[nodiscard]] const std::vector<u8>* Find(u64 hash, std::size_t expected_size);

/// Dumping is on and this hash has not been written yet.
[[nodiscard]] bool NeedsDump(u64 hash);

/// Writes <hash>.bin and <hash>.txt once per hash.
void Dump(u64 hash, const ImageInfo& info, std::span<const u8> linear,
          std::span<const BufferImageCopy> copies);

} // namespace VideoCommon::KleddPack
