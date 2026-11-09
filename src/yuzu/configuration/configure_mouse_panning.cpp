// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initializer_list>

#include <QCloseEvent>
#include <QMessageBox>

#include "common/settings.h"
#include "ui_configure_mouse_panning.h"
#include "yuzu/configuration/configure_mouse_panning.h"

ConfigureMousePanning::ConfigureMousePanning(QWidget* parent,
                                             InputCommon::InputSubsystem* input_subsystem_,
                                             float right_stick_deadzone, float right_stick_range)
    : QDialog(parent), input_subsystem{input_subsystem_},
      ui(std::make_unique<Ui::ConfigureMousePanning>()) {
    ui->setupUi(this);
    SetConfiguration(right_stick_deadzone, right_stick_range);
    ConnectEvents();
}

ConfigureMousePanning::~ConfigureMousePanning() = default;

void ConfigureMousePanning::closeEvent(QCloseEvent* event) {
    event->accept();
}

void ConfigureMousePanning::SetConfiguration(float right_stick_deadzone, float right_stick_range) {
    ui->enable->setChecked(Settings::values.mouse_panning.GetValue());
    ui->kledd_mouse_gyro->setChecked(Settings::values.kledd_mouse_gyro.GetValue());
    ui->kledd_mouse_gyro_yaw_z->setChecked(Settings::values.kledd_mouse_gyro_yaw_z.GetValue());
    ui->kledd_mouse_gyro_gravity->setChecked(Settings::values.kledd_mouse_gyro_gravity.GetValue());
    ui->kledd_mouse_gyro_tilt_flip->setChecked(Settings::values.kledd_mouse_gyro_tilt_flip.GetValue());
    ui->kledd_mouse_gyro_auto_level->setChecked(Settings::values.kledd_mouse_gyro_auto_level.GetValue());
    ui->kledd_mouse_gyro_no_fusion->setChecked(Settings::values.kledd_mouse_gyro_no_fusion.GetValue());
    ui->kledd_mouse_gyro_probe->setChecked(Settings::values.kledd_mouse_gyro_probe.GetValue());
    ui->kledd_mouse_gyro_frame_avg->setChecked(Settings::values.kledd_mouse_gyro_frame_avg.GetValue());
    ui->kledd_mouse_gyro_absolute->setChecked(Settings::values.kledd_mouse_gyro_absolute.GetValue());
    ui->kledd_mouse_gyro_abs_range_x->setValue(Settings::values.kledd_mouse_gyro_abs_range_x.GetValue());
    ui->kledd_mouse_gyro_abs_range_y->setValue(Settings::values.kledd_mouse_gyro_abs_range_y.GetValue());
    ui->kledd_mouse_gyro_abs_from_game->setChecked(
        Settings::values.kledd_mouse_gyro_abs_from_game.GetValue());
    ui->kledd_mouse_gyro_game_sens_x->setValue(
        Settings::values.kledd_mouse_gyro_game_sens_x.GetValue());
    ui->kledd_mouse_gyro_game_sens_y->setValue(
        Settings::values.kledd_mouse_gyro_game_sens_y.GetValue());
    ui->kledd_mouse_gyro_abs_show_cursor->setChecked(Settings::values.kledd_mouse_gyro_abs_show_cursor.GetValue());
    ui->kledd_mouse_gyro_smoothing->setValue(Settings::values.kledd_mouse_gyro_smoothing.GetValue());
    ui->kledd_mouse_gyro_tilt_resync->setChecked(
        Settings::values.kledd_mouse_gyro_tilt_resync.GetValue());
    ui->kledd_mouse_gyro_r_recenter->setChecked(
        Settings::values.kledd_mouse_gyro_r_recenter.GetValue());
    ui->kledd_mouse_gyro_zl_stick->setChecked(Settings::values.kledd_mouse_gyro_zl_stick.GetValue());
    ui->kledd_mouse_stick_sensitivity->setValue(
        Settings::values.kledd_mouse_stick_sensitivity.GetValue());
    ui->x_sensitivity->setValue(Settings::values.mouse_panning_x_sensitivity.GetValue());
    ui->y_sensitivity->setValue(Settings::values.mouse_panning_y_sensitivity.GetValue());
    ui->deadzone_counterweight->setValue(
        Settings::values.mouse_panning_deadzone_counterweight.GetValue());
    ui->decay_strength->setValue(Settings::values.mouse_panning_decay_strength.GetValue());
    ui->min_decay->setValue(Settings::values.mouse_panning_min_decay.GetValue());

    if (right_stick_deadzone > 0.0f || right_stick_range != 1.0f) {
        const QString right_stick_deadzone_str =
            QString::fromStdString(std::to_string(static_cast<int>(right_stick_deadzone * 100.0f)));
        const QString right_stick_range_str =
            QString::fromStdString(std::to_string(static_cast<int>(right_stick_range * 100.0f)));

        ui->warning_label->setText(
            tr("Mouse panning works better with a deadzone of 0% and a range of 100%.\nCurrent "
               "values are %1% and %2% respectively.")
                .arg(right_stick_deadzone_str, right_stick_range_str));
    }

    if (Settings::values.mouse_enabled) {
        ui->warning_label->setText(
            tr("Emulated mouse is enabled. This is incompatible with mouse panning."));
    }
}

void ConfigureMousePanning::SetDefaultConfiguration() {
    ui->x_sensitivity->setValue(Settings::values.mouse_panning_x_sensitivity.GetDefault());
    ui->y_sensitivity->setValue(Settings::values.mouse_panning_y_sensitivity.GetDefault());
    ui->deadzone_counterweight->setValue(
        Settings::values.mouse_panning_deadzone_counterweight.GetDefault());
    ui->decay_strength->setValue(Settings::values.mouse_panning_decay_strength.GetDefault());
    ui->min_decay->setValue(Settings::values.mouse_panning_min_decay.GetDefault());
}

void ConfigureMousePanning::ConnectEvents() {
    connect(ui->default_button, &QPushButton::clicked, this,
            &ConfigureMousePanning::SetDefaultConfiguration);
    connect(ui->button_box, &QDialogButtonBox::accepted, this,
            &ConfigureMousePanning::ApplyConfiguration);
    connect(ui->button_box, &QDialogButtonBox::rejected, this, [this] { reject(); });

    // eden-kledd: show the edge angles the in-game sensitivity gives (same formula as the mouse
    // driver), and grey out whichever way of setting them isn't in use
    const auto update_game_sens = [this] {
        const auto degrees = [](int sensitivity) { return 692.0 / (sensitivity + 11.5); };
        const bool from_game = ui->kledd_mouse_gyro_abs_from_game->isChecked();
        ui->kledd_game_sens_result->setText(
            tr("= %1° / %2°")
                .arg(degrees(ui->kledd_mouse_gyro_game_sens_x->value()), 0, 'f', 1)
                .arg(degrees(ui->kledd_mouse_gyro_game_sens_y->value()), 0, 'f', 1));
        ui->kledd_mouse_gyro_game_sens_x->setEnabled(from_game);
        ui->kledd_mouse_gyro_game_sens_y->setEnabled(from_game);
        ui->kledd_game_sens_result->setEnabled(from_game);
        ui->kledd_mouse_gyro_abs_range_x->setEnabled(!from_game);
        ui->kledd_mouse_gyro_abs_range_y->setEnabled(!from_game);
    };
    connect(ui->kledd_mouse_gyro_abs_from_game, &QCheckBox::toggled, this, update_game_sens);
    connect(ui->kledd_mouse_gyro_game_sens_x, qOverload<int>(&QSpinBox::valueChanged), this,
            update_game_sens);
    connect(ui->kledd_mouse_gyro_game_sens_y, qOverload<int>(&QSpinBox::valueChanged), this,
            update_game_sens);
    update_game_sens();

    // eden-kledd: one dropdown instead of a wall of checkboxes. The checkboxes still hold the
    // settings, but only the boxes the chosen mode uses are shown; the rest are set to Melos's
    // tested values when a gyro mode is saved (see ApplyConfiguration).
    const int mode = !ui->enable->isChecked()          ? 0
                     : !ui->kledd_mouse_gyro->isChecked() ? 1
                     : ui->kledd_mouse_gyro_absolute->isChecked() ? 3
                                                                  : 2;
    ui->kledd_mode->setCurrentIndex(mode);

    // Never shown: fixed in gyro modes, or only for debugging
    for (QWidget* widget : std::initializer_list<QWidget*>{
             ui->enable, ui->kledd_mouse_gyro, ui->kledd_mouse_gyro_absolute,
             ui->kledd_mouse_gyro_yaw_z, ui->kledd_mouse_gyro_gravity,
             ui->kledd_mouse_gyro_tilt_flip, ui->kledd_mouse_gyro_auto_level,
             ui->kledd_mouse_gyro_no_fusion, ui->kledd_mouse_gyro_frame_avg,
             ui->kledd_mouse_gyro_probe, ui->kledd_mouse_gyro_tilt_resync,
             ui->kledd_mouse_gyro_r_recenter, ui->kledd_mouse_gyro_zl_stick,
             ui->kledd_mouse_stick_sensitivity_label, ui->kledd_mouse_stick_sensitivity,
             ui->kledd_mouse_gyro_smoothing_label, ui->kledd_mouse_gyro_smoothing}) {
        widget->hide();
    }

    const auto update_mode = [this] {
        const int index = ui->kledd_mode->currentIndex();
        const bool stick = index == 1;
        const bool camera = index == 2;
        const bool cursor = index == 3;
        // Sensitivity: the stick and camera modes; cursor mode aims by the mouse's position
        ui->sensitivity_box->setVisible(stick || camera);
        ui->deadzone_counterweight_box->setVisible(stick);
        ui->decay_box->setVisible(stick);
        for (QWidget* widget : std::initializer_list<QWidget*>{
                 ui->kledd_abs_range_label, ui->kledd_mouse_gyro_abs_range_x,
                 ui->kledd_mouse_gyro_abs_range_y, ui->kledd_mouse_gyro_abs_from_game,
                 ui->kledd_mouse_gyro_game_sens_x, ui->kledd_mouse_gyro_game_sens_y,
                 ui->kledd_game_sens_result, ui->kledd_mouse_gyro_abs_show_cursor}) {
            widget->setVisible(cursor);
        }
        adjustSize();
    };
    connect(ui->kledd_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, update_mode);
    update_mode();
}

void ConfigureMousePanning::ApplyConfiguration() {
    // eden-kledd: the dropdown decides the mode; the hidden checkboxes follow it
    const int mode = ui->kledd_mode->currentIndex();
    ui->enable->setChecked(mode != 0);
    if (mode == 1) {
        ui->kledd_mouse_gyro->setChecked(false);
    } else if (mode >= 2) {
        ui->kledd_mouse_gyro->setChecked(true);
        ui->kledd_mouse_gyro_absolute->setChecked(mode == 3);
        // The gyro modes' fixed setup (what Melos runs Metroid Prime 4 with)
        ui->kledd_mouse_gyro_yaw_z->setChecked(true);
        ui->kledd_mouse_gyro_gravity->setChecked(false);
        ui->kledd_mouse_gyro_tilt_flip->setChecked(false);
        ui->kledd_mouse_gyro_auto_level->setChecked(true);
        ui->kledd_mouse_gyro_no_fusion->setChecked(true);
        ui->kledd_mouse_gyro_frame_avg->setChecked(true);
        ui->kledd_mouse_gyro_smoothing->setValue(1);
    }

    Settings::values.mouse_panning = ui->enable->isChecked();
    Settings::values.kledd_mouse_gyro = ui->kledd_mouse_gyro->isChecked();
    Settings::values.kledd_mouse_gyro_yaw_z = ui->kledd_mouse_gyro_yaw_z->isChecked();
    Settings::values.kledd_mouse_gyro_gravity = ui->kledd_mouse_gyro_gravity->isChecked();
    Settings::values.kledd_mouse_gyro_tilt_flip = ui->kledd_mouse_gyro_tilt_flip->isChecked();
    Settings::values.kledd_mouse_gyro_auto_level = ui->kledd_mouse_gyro_auto_level->isChecked();
    Settings::values.kledd_mouse_gyro_no_fusion = ui->kledd_mouse_gyro_no_fusion->isChecked();
    Settings::values.kledd_mouse_gyro_probe = ui->kledd_mouse_gyro_probe->isChecked();
    Settings::values.kledd_mouse_gyro_frame_avg = ui->kledd_mouse_gyro_frame_avg->isChecked();
    Settings::values.kledd_mouse_gyro_absolute = ui->kledd_mouse_gyro_absolute->isChecked();
    Settings::values.kledd_mouse_gyro_abs_range_x =
        static_cast<u8>(ui->kledd_mouse_gyro_abs_range_x->value());
    Settings::values.kledd_mouse_gyro_abs_range_y =
        static_cast<u8>(ui->kledd_mouse_gyro_abs_range_y->value());
    Settings::values.kledd_mouse_gyro_abs_from_game =
        ui->kledd_mouse_gyro_abs_from_game->isChecked();
    Settings::values.kledd_mouse_gyro_game_sens_x =
        static_cast<u8>(ui->kledd_mouse_gyro_game_sens_x->value());
    Settings::values.kledd_mouse_gyro_game_sens_y =
        static_cast<u8>(ui->kledd_mouse_gyro_game_sens_y->value());
    Settings::values.kledd_mouse_gyro_abs_show_cursor =
        ui->kledd_mouse_gyro_abs_show_cursor->isChecked();
    Settings::values.kledd_mouse_gyro_smoothing =
        static_cast<u8>(ui->kledd_mouse_gyro_smoothing->value());
    Settings::values.kledd_mouse_gyro_tilt_resync = ui->kledd_mouse_gyro_tilt_resync->isChecked();
    Settings::values.kledd_mouse_gyro_r_recenter = ui->kledd_mouse_gyro_r_recenter->isChecked();
    Settings::values.kledd_mouse_gyro_zl_stick = ui->kledd_mouse_gyro_zl_stick->isChecked();
    Settings::values.kledd_mouse_stick_sensitivity =
        static_cast<u8>(ui->kledd_mouse_stick_sensitivity->value());
    Settings::values.mouse_panning_x_sensitivity = static_cast<float>(ui->x_sensitivity->value());
    Settings::values.mouse_panning_y_sensitivity = static_cast<float>(ui->y_sensitivity->value());
    Settings::values.mouse_panning_deadzone_counterweight =
        static_cast<float>(ui->deadzone_counterweight->value());
    Settings::values.mouse_panning_decay_strength = static_cast<float>(ui->decay_strength->value());
    Settings::values.mouse_panning_min_decay = static_cast<float>(ui->min_decay->value());

    if (Settings::values.mouse_enabled && Settings::values.mouse_panning) {
        Settings::values.mouse_panning = false;
        QMessageBox::critical(
            this, tr("Emulated mouse is enabled"),
            tr("Real mouse input and mouse panning are incompatible. Please disable the "
               "emulated mouse in input advanced settings to allow mouse panning."));
        return;
    }

    accept();
}
