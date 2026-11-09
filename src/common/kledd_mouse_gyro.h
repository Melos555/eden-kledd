// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// eden-kledd: state shared between the mouse driver (input_common) and the motion engine
// (hid_core) for the mouse gyro, plus a small probe log that is flushed on every line.

#pragma once

#include <atomic>
#include <string>

namespace Common::KleddMouseGyro {

/// Up/down tilt of the imaginary controller as tracked by the mouse driver (degrees)
inline std::atomic<float> tracked_pitch_deg{0.0f};
/// Left/right turn of the imaginary controller as tracked by the mouse driver (degrees)
inline std::atomic<float> tracked_yaw_deg{0.0f};
/// Set by hid_core: ZL (lock-on in Metroid Prime) is held on the connected controller
inline std::atomic<bool> zl_held{false};
/// Set by the mouse driver: the mouse drives the right stick (ZL held / toggle) with this value
/// (-1..1 each axis, up = +y); hid_core puts it on the right stick whatever it is bound to
inline std::atomic<bool> stick_override_active{false};
inline std::atomic<float> stick_override_x{0.0f};
inline std::atomic<float> stick_override_y{0.0f};
/// Set by the mouse driver: on the next motion sample, snap Eden's orientation to the gravity
/// sent with that sample (re-sync Eden's idea of the tilt with the mouse driver's)
inline std::atomic<bool> resync_request{false};

/// Length of the game's last frame (time between the last two presented frames), in ns
inline std::atomic<long long> game_frame_ns{0};

/// Appends a line to log/kledd_mouse_gyro.txt (file is restarted on the first line of a run)
void ProbeLog(const std::string& line);

} // namespace Common::KleddMouseGyro
