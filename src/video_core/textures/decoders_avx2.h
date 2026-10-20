// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Tegra::Texture {

void UnswizzleGobPermuteAVX2(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                             u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                             u32 stride);

} // namespace Tegra::Texture
