// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// eden-kledd shot probe (log only, off by default).
// Metroid Prime 4: Beyond v1.1.0: finds the arm cannon ("gun FSM") component in guest memory and
// writes its shot-related fields once per game frame to log/kledd_shot_probe.txt, so we can see
// what limits the Power Beam fire rate. It never writes guest memory.

#pragma once

namespace Core {
class System;
}

namespace Core::KleddShotProbe {

/// Called once per game frame (from the GPU frame-end notification).
void OnGameFrame(Core::System& system);

} // namespace Core::KleddShotProbe
