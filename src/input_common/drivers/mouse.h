// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>

#include <thread>
#include <chrono>

#include "common/steady_clock.h"
#include "common/polyfill_thread.h"
#include "common/vector_math.h"
#include "input_common/input_engine.h"

namespace InputCommon {

enum class MouseButton {
    Left,
    Right,
    Wheel,
    Backward,
    Forward,
    Task,
    Extra,
    Undefined,
};

/**
 * A button device factory representing a keyboard. It receives keyboard events and forward them
 * to all button devices it created.
 */
class Mouse final : public InputEngine {
public:
    explicit Mouse(std::string input_engine_);

    /**
     * Signals that mouse has moved.
     * @param x the x-coordinate of the cursor
     * @param y the y-coordinate of the cursor
     * @param center_x the x-coordinate of the middle of the screen
     * @param center_y the y-coordinate of the middle of the screen
     */
    void Move(int x, int y, int center_x, int center_y);

    /**
     * Signals that real mouse has moved.
     * @param x the absolute position on the touchscreen of the cursor
     * @param y the absolute position on the touchscreen of the cursor
     */
    void MouseMove(f32 touch_x, f32 touch_y);

    /**
     * Signals that touch finger has moved.
     * @param x the absolute position on the touchscreen of the cursor
     * @param y the absolute position on the touchscreen of the cursor
     */
    void TouchMove(f32 touch_x, f32 touch_y);

    /**
     * Sets the status of a button to pressed
     * @param x the x-coordinate of the cursor
     * @param y the y-coordinate of the cursor
     * @param button the id of the button to press
     */
    void PressButton(int x, int y, MouseButton button);

    /**
     * Sets the status of a mouse button to pressed
     * @param button the id of the button to press
     */
    void PressMouseButton(MouseButton button);

    /**
     * Sets the status of touch finger to pressed
     * @param x the absolute position on the touchscreen of the cursor
     * @param y the absolute position on the touchscreen of the cursor
     * @param button the id of the button to press
     */
    void PressTouchButton(f32 touch_x, f32 touch_y, MouseButton button);

    /**
     * Sets the status of all buttons bound with the key to released
     * @param key_code the code of the key to release
     */
    void ReleaseButton(MouseButton button);

    /**
     * Sets the status of the mouse wheel
     * @param x delta movement in the x direction
     * @param y delta movement in the y direction
     */
    void MouseWheelChange(int x, int y);

    void ReleaseAllButtons();

    /// eden-kledd: level the mouse gyro's imaginary controller and re-sync Eden's orientation
    void KleddRequestGravityCheck();
    /// eden-kledd: absolute mode wants the OS cursor put back in the window center (once)
    bool KleddTakeCenterRequest();
    /// eden-kledd: cursor position inside the game picture (0..1 each axis, black bars excluded)
    void KleddSetPicturePos(float x, float y);
    /// eden-kledd: toggle "mouse drives the right stick" (e.g. for menus/maps that ignore motion)
    /// @return the new state
    bool KleddToggleStick();
    /// eden-kledd: next step of the absolute-mode edge calibration. The settings only change
    /// when both edges are measured.
    /// @return -1 = absolute mode is off, -2 = edge angles come from the in-game sensitivity,
    ///         1 = started (reticle to the right edge next),
    ///         2 = side measured (reticle to the top edge next), 3 = done and saved to settings,
    ///         4 = the reticle has barely moved: not at the edge yet, same step again
    int KleddCalibrateStep();
    /// eden-kledd: cancel a running calibration (old values stay). @return true if one was running
    bool KleddCalibrateCancel();
    /// eden-kledd: the side value measured in step 1 of the calibration (degrees)
    int KleddCalibratePendingX() const;

    /// @brief Notifies we changed something about the mouse state and we must update
    void NotifyChanged();

    std::vector<Common::ParamPackage> GetInputDevices() const override;
    AnalogMapping GetAnalogMappingForDevice(const Common::ParamPackage& params) override;
    Common::Input::ButtonNames GetUIName(const Common::ParamPackage& params) const override;

private:
    void UpdateStickInput(Common::SteadyClock::time_point timestamp);
    void UpdateMotionInput(Common::SteadyClock::time_point timestamp);
    bool IsMousePanningEnabled();
    /// eden-kledd: camera-mode mouse gyro currently hands the mouse to the right stick
    bool KleddStickMode();

    Common::Input::ButtonNames GetUIButtonName(const Common::ParamPackage& params) const;

    Common::Vec<int, 2> mouse_origin;
    Common::Vec<int, 2> last_mouse_position;
    Common::Vec<float, 2> last_mouse_change;
    Common::Vec<float, 3> last_motion_change;
    Common::Vec<int, 2> wheel_position;
    Common::SteadyClock::time_point last_notify_timestamp{};
    // eden-kledd mouse gyro: raw pixel movement since the last update, plus a short history
    // so the rate is averaged over ~20 ms (the game samples motion at its own pace)
    Common::Vec<float, 2> kledd_gyro_pixels{};
    // 256 updates of ~5 ms = ~1.3 s, enough for the hitch window (frames up to 1 s)
    std::array<Common::Vec<float, 2>, 256> kledd_gyro_hist_px{};
    std::array<float, 256> kledd_gyro_hist_dt{};
    std::size_t kledd_gyro_hist_pos{};
    // eden-kledd: total up/down tilt sent so far (degrees), kept within a limit so the virtual
    // controller can't tip past vertical and turn the up/down aim upside down
    float kledd_gyro_pitch_deg{};
    std::atomic<bool> kledd_gravity_check_pending{false};
    Common::SteadyClock::time_point kledd_last_auto_level{};
    bool kledd_at_wall = false;
    bool kledd_last_panning = false;
    Common::Vec<float, 2> kledd_abs_pos{};
    float kledd_gyro_yaw_deg{};
    std::atomic<bool> kledd_center_request{false};
    int kledd_skip_moves = 0;
    Common::Vec<float, 2> kledd_picture_pos{0.5f, 0.5f};
    std::atomic<bool> kledd_stick_toggle{false};
    bool kledd_stick_active = false;
    // 0 = not calibrating, 1 = waiting for the right edge, 2 = waiting for the top edge
    std::atomic<int> kledd_calib_step{0};
    // temporary ranges used while calibrating, and the side value measured in step 1
    std::atomic<float> kledd_calib_range_x{60.0f};
    std::atomic<float> kledd_calib_range_y{40.0f};
    int kledd_calib_pending_x = 0;
    // left/right angle where step 1 ended; held there during step 2 (degrees)
    std::atomic<float> kledd_calib_locked_yaw{0.0f};
    // up/down tilt at the last re-sync, for the automatic tilt re-sync
    float kledd_last_resync_pitch{};
    Common::SteadyClock::time_point kledd_last_tilt_resync{};
    bool button_pressed = false;
};

} // namespace InputCommon
