// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <math.h>

#include "common/kledd_mouse_gyro.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/steady_clock.h"
#include "input_common/drivers/mouse.h"

namespace InputCommon {
constexpr float default_panning_sensitivity = 0.0010f;
constexpr float default_stick_sensitivity = 0.0006f;
constexpr float default_deadzone_counterweight = 0.01f;
constexpr float default_motion_panning_sensitivity = 2.5f;
constexpr float default_motion_sensitivity = 0.416f;
constexpr float maximum_rotation_speed = 2.0f;
constexpr float maximum_stick_range = 1.5f;
constexpr int mouse_axis_x = 0;
constexpr int mouse_axis_y = 1;
constexpr int wheel_axis_x = 2;
constexpr int wheel_axis_y = 3;
constexpr PadIdentifier identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 0,
};

constexpr PadIdentifier motion_identifier = {
    .guid = Common::UUID{},
    .port = 0,
    .pad = 1,
};

constexpr PadIdentifier real_mouse_identifier = {
    .guid = Common::UUID{},
    .port = 1,
    .pad = 0,
};

constexpr PadIdentifier touch_identifier = {
    .guid = Common::UUID{},
    .port = 2,
    .pad = 0,
};

Mouse::Mouse(std::string input_engine_) : InputEngine(std::move(input_engine_)) {
    PreSetController(identifier);
    PreSetController(real_mouse_identifier);
    PreSetController(touch_identifier);
    PreSetController(motion_identifier);

    // Initialize all mouse axis
    PreSetAxis(identifier, mouse_axis_x);
    PreSetAxis(identifier, mouse_axis_y);
    PreSetAxis(identifier, wheel_axis_x);
    PreSetAxis(identifier, wheel_axis_y);
    PreSetAxis(real_mouse_identifier, mouse_axis_x);
    PreSetAxis(real_mouse_identifier, mouse_axis_y);
    PreSetAxis(touch_identifier, mouse_axis_x);
    PreSetAxis(touch_identifier, mouse_axis_y);

    // Initialize variables
    mouse_origin = {};
    last_mouse_position = {};
    wheel_position = {};
    last_mouse_change = {};
    last_motion_change = {};
}

void Mouse::UpdateStickInput(Common::SteadyClock::time_point timestamp) {
    if (IsMousePanningEnabled()) {
        const float length = last_mouse_change.Length();

        // Prevent input from exceeding the max range (1.0f) too much,
        // but allow some room to make it easier to sustain
        if (length > maximum_stick_range) {
            last_mouse_change /= length;
            last_mouse_change *= maximum_stick_range;
        }

        SetAxis(identifier, mouse_axis_x, last_mouse_change[0]);
        SetAxis(identifier, mouse_axis_y, -last_mouse_change[1]);

        // Decay input over time
        const float clamped_length = (std::min)(1.0f, length);
        const float decay_strength = Settings::values.mouse_panning_decay_strength.GetValue();
        const float decay = 1 - clamped_length * clamped_length * decay_strength * 0.01f;
        const float min_decay = Settings::values.mouse_panning_min_decay.GetValue();
        const float clamped_decay = (std::min)(1 - min_decay / 100.0f, decay);
        last_mouse_change *= clamped_decay;
    }
}

// eden-kledd: degrees of controller rotation per pixel of mouse movement at 100% sensitivity
constexpr float kledd_gyro_degrees_per_pixel = 0.1f;
// eden-kledd: real gyros top out around 2000 deg/s; clamp to that (in rotations per second)
constexpr float kledd_gyro_max_rps = 4.9f;
// eden-kledd: maximum up/down tilt of the virtual controller (degrees from where it started)
constexpr float kledd_gyro_pitch_limit = 75.0f;

void Mouse::UpdateMotionInput(Common::SteadyClock::time_point timestamp) {
    // eden-kledd: mouse gyro while the mouse is freed (Ctrl+F9): hold the imaginary controller
    // perfectly still where it is, instead of falling back to stock drag-to-tilt motion
    if (!IsMousePanningEnabled() && Settings::values.kledd_mouse_gyro.GetValue()) {
        kledd_gyro_pixels = {};
        kledd_gyro_hist_px = {};
        last_motion_change = {};
        const float tilt = kledd_gyro_pitch_deg * 3.14159265f / 180.0f;
        const bool yaw_on_z = Settings::values.kledd_mouse_gyro_yaw_z.GetValue();
        const float side_sign =
            Settings::values.kledd_mouse_gyro_tilt_flip.GetValue() ? -1.0f : 1.0f;
        const bool gravity = Settings::values.kledd_mouse_gyro_gravity.GetValue();
        SetMotion(motion_identifier, 0, BasicMotion{
            .gyro_x = 0,
            .gyro_y = 0,
            .gyro_z = 0,
            .accel_x = 0,
            .accel_y = gravity && yaw_on_z ? -std::sin(tilt) * side_sign : 0.0f,
            .accel_z = gravity ? (yaw_on_z ? -std::cos(tilt) : -1.0f) : 0.0f,
            .delta_timestamp = u64(std::chrono::duration_cast<std::chrono::microseconds>(timestamp - last_notify_timestamp).count()),
        });
        return;
    }
    if (IsMousePanningEnabled() && Settings::values.kledd_mouse_gyro.GetValue()) {
        // Mouse movement since the last update becomes a rotation speed: the cursor is
        // recentered every move, so only real movement counts, and stopping stops the aim.
        float dt = std::chrono::duration<float>(timestamp - last_notify_timestamp).count();
        if (!(dt > 0.0f) || dt > 0.1f) {
            dt = 0.005f; // first update or a long pause: assume the 5 ms timer tick
        }
        kledd_gyro_hist_px[kledd_gyro_hist_pos] = kledd_gyro_pixels;
        kledd_gyro_hist_dt[kledd_gyro_hist_pos] = dt;
        kledd_gyro_hist_pos = (kledd_gyro_hist_pos + 1) % kledd_gyro_hist_px.size();
        kledd_gyro_pixels = {};

        const std::size_t size = kledd_gyro_hist_px.size();
        Common::Vec<float, 2> sum_px{};
        float sum_dt = 0.0f;
        if (Settings::values.kledd_mouse_gyro_frame_avg.GetValue()) {
            // Average over the game's last frame: the game reads the gyro once per frame and
            // multiplies it by its frame time, so the speed must cover that same window. Then a
            // long frame (hitch) gets the hand's whole movement, not a 5 ms snapshot of it.
            const float frame_s = std::clamp(
                static_cast<float>(Common::KleddMouseGyro::game_frame_ns.load()) * 1e-9f, 0.005f,
                0.1f);
            for (std::size_t i = 1; i <= size && sum_dt < frame_s; ++i) {
                const std::size_t idx = (kledd_gyro_hist_pos + size - i) % size;
                const float tick_dt = kledd_gyro_hist_dt[idx];
                if (!(tick_dt > 0.0f)) {
                    break;
                }
                // Last tick may stick out past the window: take only the part inside it
                const float part = std::min(1.0f, (frame_s - sum_dt) / tick_dt);
                sum_px += kledd_gyro_hist_px[idx] * part;
                sum_dt += tick_dt * part;
            }
        } else {
            // Average over the last N updates (N = smoothing setting, 1 = no smoothing)
            const std::size_t count = std::clamp<std::size_t>(
                Settings::values.kledd_mouse_gyro_smoothing.GetValue(), 1, size);
            for (std::size_t i = 1; i <= count; ++i) {
                const std::size_t idx = (kledd_gyro_hist_pos + size - i) % size;
                sum_px += kledd_gyro_hist_px[idx];
                sum_dt += kledd_gyro_hist_dt[idx];
            }
        }
        if (!(sum_dt > 0.0f)) {
            sum_dt = 0.005f;
        }
        const float x_scale = Settings::values.mouse_panning_x_sensitivity.GetValue() / 100.0f *
                              kledd_gyro_degrees_per_pixel / 360.0f;
        const float y_scale = Settings::values.mouse_panning_y_sensitivity.GetValue() / 100.0f *
                              kledd_gyro_degrees_per_pixel / 360.0f;
        // rotations per second, same unit the SDL driver reports
        float yaw = -sum_px[0] / sum_dt * x_scale;
        float pitch = -sum_px[1] / sum_dt * y_scale;
        // Limit the total rotation speed, keeping its direction. Eden clamps each gyro axis
        // at 5 rotations/s on its own, which on a wild flick would bend the direction.
        {
            const float speed = std::sqrt(yaw * yaw + pitch * pitch);
            if (speed > kledd_gyro_max_rps) {
                yaw *= kledd_gyro_max_rps / speed;
                pitch *= kledd_gyro_max_rps / speed;
            }
        }
        const bool probe = Settings::values.kledd_mouse_gyro_probe.GetValue();
        bool resync = false;

        // Gravity Check (hotkey): level the imaginary controller and snap Eden's orientation to
        // it. Only the up/down tilt is reset; left/right stays where it is.
        if (kledd_gravity_check_pending.exchange(false)) {
            if (probe) {
                Common::KleddMouseGyro::ProbeLog(fmt::format(
                    "GRAVITY CHECK (hotkey)  tracked {:+.2f} -> 0", kledd_gyro_pitch_deg));
            }
            kledd_gyro_pitch_deg = 0.0f;
            pitch = 0.0f;
            resync = true;
        }

        // Keep the virtual controller's up/down tilt within +-kledd_gyro_pitch_limit degrees:
        // only send the part of this update's rotation that stays inside the limit
        {
            const float step_deg = pitch * 360.0f * dt;
            const float wanted = kledd_gyro_pitch_deg + step_deg;
            const float next =
                std::clamp(wanted, -kledd_gyro_pitch_limit, kledd_gyro_pitch_limit);
            pitch = (next - kledd_gyro_pitch_deg) / (360.0f * dt);
            kledd_gyro_pitch_deg = next;

            // Pushing into the limit: re-sync Eden to the imaginary controller (auto-level), on
            // entering the wall and then at most twice a second while pushing into it
            const bool pushing_wall = wanted != next;
            if (pushing_wall && Settings::values.kledd_mouse_gyro_auto_level.GetValue() &&
                (!kledd_at_wall ||
                 timestamp - kledd_last_auto_level > std::chrono::milliseconds(500))) {
                kledd_last_auto_level = timestamp;
                resync = true;
                if (probe) {
                    Common::KleddMouseGyro::ProbeLog(fmt::format(
                        "WALL {}  tracked {:+.2f} (wanted {:+.2f})  -> auto-level",
                        next < 0.0f ? "DOWN" : "UP", next, wanted));
                }
            }
            kledd_at_wall = pushing_wall;
        }
        Common::KleddMouseGyro::tracked_pitch_deg.store(kledd_gyro_pitch_deg);
        const bool yaw_on_z = Settings::values.kledd_mouse_gyro_yaw_z.GetValue();

        // Turn left/right around the WORLD's vertical axis, like a person turning a real
        // controller, not around the controller's own (tilted) axis. When the virtual controller
        // is tilted up or down, the controller's own axis leans with it, and turning around it
        // makes the pointer move diagonally and the tilt drift.
        float yaw_main = yaw;
        float yaw_side = 0.0f;
        const float tilt = kledd_gyro_pitch_deg * 3.14159265f / 180.0f;
        const float side_sign =
            Settings::values.kledd_mouse_gyro_tilt_flip.GetValue() ? -1.0f : 1.0f;
        if (yaw_on_z) {
            yaw_main = yaw * std::cos(tilt);
            yaw_side = yaw * std::sin(tilt) * side_sign;
        }
        // Gravity that matches the imaginary controller's tilt (world "down" seen from the
        // controller). A level controller sees (0, 0, -1). Matching it to the tilt keeps the
        // motion fusion from pulling the aim back to level, and the game always gets a real
        // gravity reference instead of free fall.
        // A re-sync sends gravity for this one sample even when gravity is switched off
        const bool gravity = resync || Settings::values.kledd_mouse_gyro_gravity.GetValue();
        if (resync) {
            Common::KleddMouseGyro::resync_request.store(true);
        }
        const float grav_y = gravity && yaw_on_z ? -std::sin(tilt) * side_sign : 0.0f;
        const float grav_z = gravity ? (yaw_on_z ? -std::cos(tilt) : -1.0f) : 0.0f;

        last_motion_change = {};
        SetMotion(motion_identifier, 0, BasicMotion{
            .gyro_x = pitch,
            .gyro_y = yaw_on_z ? yaw_side : yaw,
            .gyro_z = yaw_on_z ? yaw_main : 0.0f,
            .accel_x = 0,
            .accel_y = grav_y,
            .accel_z = grav_z,
            .delta_timestamp = u64(std::chrono::duration_cast<std::chrono::microseconds>(timestamp - last_notify_timestamp).count()),
        });
        return;
    }

    const float sensitivity =
        IsMousePanningEnabled() ? default_motion_panning_sensitivity : default_motion_sensitivity;

    const float rotation_velocity = std::sqrt(last_motion_change[0] * last_motion_change[0] +
                                              last_motion_change[1] * last_motion_change[1]);

    // Clamp rotation speed
    if (rotation_velocity > maximum_rotation_speed / sensitivity) {
        const float multiplier = maximum_rotation_speed / rotation_velocity / sensitivity;
        last_motion_change[0] = last_motion_change[0] * multiplier;
        last_motion_change[1] = last_motion_change[1] * multiplier;
    }

    if (IsMousePanningEnabled()) {
        last_motion_change[0] = 0;
        last_motion_change[1] = 0;
    }
    last_motion_change[2] = 0;

    SetMotion(motion_identifier, 0, BasicMotion{
        .gyro_x = last_motion_change[0] * sensitivity,
        .gyro_y = last_motion_change[1] * sensitivity,
        .gyro_z = last_motion_change[2] * sensitivity,
        .accel_x = 0,
        .accel_y = 0,
        .accel_z = 0,
        .delta_timestamp = u64(std::chrono::duration_cast<std::chrono::microseconds>(timestamp - last_notify_timestamp).count()),
    });
}

void Mouse::Move(int x, int y, int center_x, int center_y) {
    // eden-kledd: mouse gyro + Ctrl+F9. While the mouse is freed, the mouse must not move the
    // gyro at all (stock Eden turns clicks into drag-tilts, and you click to scan). When it is
    // captured again, the first moves still measure from wherever the free cursor was to the
    // window center, which would be one giant flick: skip them.
    if (Settings::values.kledd_mouse_gyro.GetValue()) {
        const bool panning = IsMousePanningEnabled();
        if (panning && !kledd_last_panning) {
            kledd_skip_moves = 2;
        }
        kledd_last_panning = panning;
        if (!panning) {
            return;
        }
        if (kledd_skip_moves > 0) {
            --kledd_skip_moves;
            return;
        }
    }
    if (IsMousePanningEnabled()) {
        auto const mouse_change_int = Common::Vec<int, 2>(x, y) - Common::Vec<int, 2>(center_x, center_y);
        auto const mouse_change = Common::Vec<float, 2>(float(mouse_change_int[0]), float(mouse_change_int[1]));
        auto const x_sensitivity = Settings::values.mouse_panning_x_sensitivity.GetValue() * default_panning_sensitivity;
        auto const y_sensitivity = Settings::values.mouse_panning_y_sensitivity.GetValue() * default_panning_sensitivity;
        auto const deadzone_cw = Settings::values.mouse_panning_deadzone_counterweight.GetValue() * default_deadzone_counterweight;
        if (Settings::values.kledd_mouse_gyro.GetValue()) {
            // eden-kledd: movement goes to the gyro only; the stick stays centered
            kledd_gyro_pixels += mouse_change;
            return;
        }
        last_motion_change += {-mouse_change[1] * x_sensitivity, -mouse_change[0] * y_sensitivity, 0};
        last_mouse_change[0] += mouse_change[0] * x_sensitivity;
        last_mouse_change[1] += mouse_change[1] * y_sensitivity;
        // Bind the mouse change to [0 <= deadzone_cw <= 1.0]
        const float length = last_mouse_change.Length();
        if (length < deadzone_cw && length != 0.0f) {
            last_mouse_change /= length;
            last_mouse_change *= deadzone_cw;
        }
    } else {
        if (button_pressed) {
            const auto mouse_move = Common::Vec<int, 2>(x, y) - mouse_origin;
            const float x_sensitivity = Settings::values.mouse_panning_x_sensitivity.GetValue() * default_stick_sensitivity;
            const float y_sensitivity = Settings::values.mouse_panning_y_sensitivity.GetValue() * default_stick_sensitivity;
            SetAxis(identifier, mouse_axis_x, float(mouse_move[0]) * x_sensitivity);
            SetAxis(identifier, mouse_axis_y, float(-mouse_move[1]) * y_sensitivity);
            last_motion_change = {
                float(-mouse_move[1]) * x_sensitivity,
                float(-mouse_move[0]) * y_sensitivity,
                last_motion_change[2],
            };
        }
    }
}

void Mouse::NotifyChanged() {
    auto const timestamp = Common::SteadyClock::Now();
    UpdateStickInput(timestamp);
    UpdateMotionInput(timestamp);
    last_notify_timestamp = timestamp;
}

void Mouse::MouseMove(f32 touch_x, f32 touch_y) {
    SetAxis(real_mouse_identifier, mouse_axis_x, touch_x);
    SetAxis(real_mouse_identifier, mouse_axis_y, touch_y);
}

void Mouse::TouchMove(f32 touch_x, f32 touch_y) {
    SetAxis(touch_identifier, mouse_axis_x, touch_x);
    SetAxis(touch_identifier, mouse_axis_y, touch_y);
}

void Mouse::PressButton(int x, int y, MouseButton button) {
    SetButton(identifier, static_cast<int>(button), true);

    // Set initial analog parameters
    mouse_origin = {x, y};
    last_mouse_position = {x, y};
    button_pressed = true;
}

void Mouse::PressMouseButton(MouseButton button) {
    SetButton(real_mouse_identifier, static_cast<int>(button), true);
}

void Mouse::PressTouchButton(f32 touch_x, f32 touch_y, MouseButton button) {
    SetAxis(touch_identifier, mouse_axis_x, touch_x);
    SetAxis(touch_identifier, mouse_axis_y, touch_y);
    SetButton(touch_identifier, static_cast<int>(button), true);
}

void Mouse::ReleaseButton(MouseButton button) {
    SetButton(identifier, static_cast<int>(button), false);
    SetButton(real_mouse_identifier, static_cast<int>(button), false);
    SetButton(touch_identifier, static_cast<int>(button), false);

    if (!IsMousePanningEnabled()) {
        SetAxis(identifier, mouse_axis_x, 0);
        SetAxis(identifier, mouse_axis_y, 0);
    }

    last_motion_change[0] = 0;
    last_motion_change[1] = 0;

    button_pressed = false;
}

void Mouse::MouseWheelChange(int x, int y) {
    wheel_position[0] += x;
    wheel_position[1] += y;
    last_motion_change[2] += static_cast<f32>(y);
    SetAxis(identifier, wheel_axis_x, f32(wheel_position[0]));
    SetAxis(identifier, wheel_axis_y, f32(wheel_position[1]));
}

void Mouse::KleddRequestGravityCheck() {
    kledd_gravity_check_pending.store(true);
}

void Mouse::ReleaseAllButtons() {
    ResetButtonState();
    button_pressed = false;
}

bool Mouse::IsMousePanningEnabled() {
    // Disable mouse panning when a real mouse is connected
    return Settings::values.mouse_panning && !Settings::values.mouse_enabled;
}

std::vector<Common::ParamPackage> Mouse::GetInputDevices() const {
    std::vector<Common::ParamPackage> devices;
    devices.emplace_back(Common::ParamPackage{
        {"engine", GetEngineName()},
        {"display", "Keyboard/Mouse"},
    });
    return devices;
}

AnalogMapping Mouse::GetAnalogMappingForDevice(
    [[maybe_unused]] const Common::ParamPackage& params) {
    // Only overwrite different buttons from default
    AnalogMapping mapping = {};
    Common::ParamPackage right_analog_params;
    right_analog_params.Set("engine", GetEngineName());
    right_analog_params.Set("axis_x", 0);
    right_analog_params.Set("axis_y", 1);
    right_analog_params.Set("threshold", 0.5f);
    right_analog_params.Set("range", 1.0f);
    right_analog_params.Set("deadzone", 0.0f);
    mapping.insert_or_assign(Settings::NativeAnalog::RStick, std::move(right_analog_params));
    return mapping;
}

Common::Input::ButtonNames Mouse::GetUIButtonName(const Common::ParamPackage& params) const {
    const auto button = static_cast<MouseButton>(params.Get("button", 0));
    switch (button) {
    case MouseButton::Left:
        return Common::Input::ButtonNames::ButtonLeft;
    case MouseButton::Right:
        return Common::Input::ButtonNames::ButtonRight;
    case MouseButton::Wheel:
        return Common::Input::ButtonNames::ButtonMouseWheel;
    case MouseButton::Backward:
        return Common::Input::ButtonNames::ButtonBackward;
    case MouseButton::Forward:
        return Common::Input::ButtonNames::ButtonForward;
    case MouseButton::Task:
        return Common::Input::ButtonNames::ButtonTask;
    case MouseButton::Extra:
        return Common::Input::ButtonNames::ButtonExtra;
    case MouseButton::Undefined:
    default:
        return Common::Input::ButtonNames::Undefined;
    }
}

Common::Input::ButtonNames Mouse::GetUIName(const Common::ParamPackage& params) const {
    if (params.Has("button")) {
        return GetUIButtonName(params);
    }
    if (params.Has("axis")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("axis_x") && params.Has("axis_y") && params.Has("axis_z")) {
        return Common::Input::ButtonNames::Engine;
    }
    if (params.Has("motion")) {
        return Common::Input::ButtonNames::Engine;
    }

    return Common::Input::ButtonNames::Invalid;
}

} // namespace InputCommon
