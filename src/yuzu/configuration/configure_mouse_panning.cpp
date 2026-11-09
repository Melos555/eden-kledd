// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initializer_list>

#include <QCloseEvent>
#include <QKeySequenceEdit>
#include <QMessageBox>
#include <QSignalBlocker>

#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_mouse_panning.h"
#include "yuzu/configuration/configure_hotkeys.h"
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

    // eden-kledd: one dropdown (Off / Mouse to camera / Mouse to cursor) instead of a wall of
    // checkboxes. The checkboxes still hold the settings; only the chosen mode's boxes are
    // shown, and the hidden ones are saved the way Melos runs it (see ApplyConfiguration).
    const int mode = !ui->enable->isChecked()                     ? 0
                     : ui->kledd_mouse_gyro_absolute->isChecked() ? 2
                                                                  : 1;
    ui->kledd_mode->setCurrentIndex(mode);

    // Never shown: fixed to Melos's setup, or replaced by the big X / Y boxes below (which read
    // and write these hidden ones)
    for (QWidget* widget : std::initializer_list<QWidget*>{
             ui->enable, ui->kledd_mouse_gyro, ui->kledd_mouse_gyro_absolute,
             ui->kledd_mouse_gyro_yaw_z, ui->kledd_mouse_gyro_gravity,
             ui->kledd_mouse_gyro_tilt_flip, ui->kledd_mouse_gyro_auto_level,
             ui->kledd_mouse_gyro_no_fusion, ui->kledd_mouse_gyro_frame_avg,
             ui->kledd_mouse_gyro_tilt_resync, ui->kledd_mouse_gyro_r_recenter,
             ui->kledd_mouse_gyro_zl_stick, ui->kledd_mouse_stick_sensitivity_label,
             ui->kledd_mouse_stick_sensitivity, ui->kledd_mouse_gyro_smoothing_label,
             ui->kledd_mouse_gyro_smoothing, ui->deadzone_counterweight_box, ui->decay_box,
             ui->sensitivity_box, ui->kledd_abs_range_label, ui->kledd_mouse_gyro_abs_range_x,
             ui->kledd_mouse_gyro_abs_range_y, ui->kledd_mouse_gyro_abs_from_game,
             ui->kledd_mouse_gyro_game_sens_x, ui->kledd_mouse_gyro_game_sens_y,
             ui->kledd_game_sens_result}) {
        widget->hide();
    }

    // Big question, big X / Y boxes, small grey hint
    {
        const auto scaled = [](QFont font, double factor) {
            if (font.pointSizeF() > 0) {
                font.setPointSizeF(font.pointSizeF() * factor);
            } else if (font.pixelSize() > 0) {
                font.setPixelSize(static_cast<int>(font.pixelSize() * factor));
            }
            return font;
        };
        ui->kledd_question->setFont(scaled(ui->kledd_question->font(), 1.4));
        const QFont box_font = scaled(ui->kledd_box_x->font(), 1.6);
        const QFont box_label_font = scaled(ui->kledd_box_x_label->font(), 1.6);
        for (QSpinBox* box : {ui->kledd_box_x, ui->kledd_box_y}) {
            box->setFont(box_font);
            box->setMinimumSize(110, 48);
        }
        for (QLabel* label : {ui->kledd_box_x_label, ui->kledd_box_y_label}) {
            label->setFont(box_label_font);
        }
        ui->kledd_box_hint->setEnabled(false); // drawn greyed out
    }

    // Gravity Check key: the same hotkey as in Settings > Hotkeys
    {
        QKeySequence key;
        for (const auto& shortcut : UISettings::values.shortcuts) {
            if (shortcut.name == "[Kledd] Gravity Check") {
                key = QKeySequence::fromString(QString::fromStdString(shortcut.shortcut.keyseq),
                                               QKeySequence::NativeText);
            }
        }
        ui->kledd_gravity_key->setKeySequence(key);
        ui->kledd_gravity_key->setMaximumSequenceLength(1);
        ui->kledd_gravity_key->setProperty("kledd_initial", key);
    }

    // The big boxes edit the hidden boxes of the chosen mode: Mouse to camera = Sensitivity
    // (Horizontal / Vertical, %), Mouse to cursor = Metroid Prime 4's in-game Aiming Sensitivity
    const auto update_hint = [this] {
        if (ui->kledd_mode->currentIndex() != 2) {
            ui->kledd_box_hint->clear();
            return;
        }
        const auto degrees = [](int sensitivity) { return 692.0 / (sensitivity + 11.5); };
        const int x = ui->kledd_box_x->value();
        const int y = ui->kledd_box_y->value();
        ui->kledd_box_hint->setText(
            x == y ? tr("(%1°)").arg(degrees(x), 0, 'f', 1)
                   : tr("(X %1° · Y %2°)")
                         .arg(degrees(x), 0, 'f', 1)
                         .arg(degrees(y), 0, 'f', 1));
    };
    const auto load_boxes = [this, update_hint] {
        const bool cursor = ui->kledd_mode->currentIndex() == 2;
        const QSignalBlocker block_x(ui->kledd_box_x);
        const QSignalBlocker block_y(ui->kledd_box_y);
        if (cursor) {
            ui->kledd_question->setText(tr("What is your in-game cursor sensitivity?"));
            ui->kledd_box_x_label->setText(tr("X"));
            ui->kledd_box_y_label->setText(tr("Y"));
            for (QSpinBox* box : {ui->kledd_box_x, ui->kledd_box_y}) {
                box->setRange(0, 100);
                box->setSuffix(QString());
            }
            ui->kledd_box_x->setValue(ui->kledd_mouse_gyro_game_sens_x->value());
            ui->kledd_box_y->setValue(ui->kledd_mouse_gyro_game_sens_y->value());
        } else {
            ui->kledd_question->setText(tr("How fast should the camera turn?"));
            ui->kledd_box_x_label->setText(tr("Horizontal"));
            ui->kledd_box_y_label->setText(tr("Vertical"));
            for (QSpinBox* box : {ui->kledd_box_x, ui->kledd_box_y}) {
                box->setRange(ui->x_sensitivity->minimum(), ui->x_sensitivity->maximum());
                box->setSuffix(QStringLiteral("%"));
            }
            ui->kledd_box_x->setValue(ui->x_sensitivity->value());
            ui->kledd_box_y->setValue(ui->y_sensitivity->value());
        }
        update_hint();
    };
    const auto store_boxes = [this, update_hint] {
        if (ui->kledd_mode->currentIndex() == 2) {
            ui->kledd_mouse_gyro_game_sens_x->setValue(ui->kledd_box_x->value());
            ui->kledd_mouse_gyro_game_sens_y->setValue(ui->kledd_box_y->value());
        } else if (ui->kledd_mode->currentIndex() == 1) {
            ui->x_sensitivity->setValue(ui->kledd_box_x->value());
            ui->y_sensitivity->setValue(ui->kledd_box_y->value());
        }
        update_hint();
    };
    connect(ui->kledd_box_x, qOverload<int>(&QSpinBox::valueChanged), this, store_boxes);
    connect(ui->kledd_box_y, qOverload<int>(&QSpinBox::valueChanged), this, store_boxes);

    const auto update_mode = [this, load_boxes] {
        const int index = ui->kledd_mode->currentIndex();
        const bool gyro = index != 0;
        const bool cursor = index == 2;
        for (QWidget* widget : std::initializer_list<QWidget*>{
                 ui->kledd_question, ui->kledd_box_x_label, ui->kledd_box_x,
                 ui->kledd_box_y_label, ui->kledd_box_y, ui->kledd_box_hint,
                 ui->kledd_gravity_key_label, ui->kledd_gravity_key,
                 ui->kledd_mouse_gyro_probe}) {
            widget->setVisible(gyro);
        }
        ui->kledd_mouse_gyro_abs_show_cursor->setVisible(cursor);
        if (gyro) {
            load_boxes();
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
    if (mode != 0) {
        ui->kledd_mouse_gyro->setChecked(true);
        ui->kledd_mouse_gyro_absolute->setChecked(mode == 2);
        // Everything else the way Melos runs Metroid Prime 4 (his screenshot of the old window)
        ui->kledd_mouse_gyro_yaw_z->setChecked(true);
        ui->kledd_mouse_gyro_gravity->setChecked(false);
        ui->kledd_mouse_gyro_tilt_flip->setChecked(false);
        ui->kledd_mouse_gyro_auto_level->setChecked(true);
        ui->kledd_mouse_gyro_no_fusion->setChecked(true);
        ui->kledd_mouse_gyro_frame_avg->setChecked(true);
        ui->kledd_mouse_gyro_tilt_resync->setChecked(false);
        ui->kledd_mouse_gyro_r_recenter->setChecked(false);
        ui->kledd_mouse_gyro_zl_stick->setChecked(false);
        ui->kledd_mouse_gyro_smoothing->setValue(1);
        ui->deadzone_counterweight->setValue(0);
        ui->decay_strength->setValue(0);
        ui->min_decay->setValue(0);
        // Mouse to cursor always works from the in-game sensitivity (the formula)
        ui->kledd_mouse_gyro_abs_from_game->setChecked(true);
    }
    // A changed Gravity Check key goes to the Hotkeys tab, which saves all keys on OK
    const QKeySequence gravity_key = ui->kledd_gravity_key->keySequence();
    if (gravity_key != ui->kledd_gravity_key->property("kledd_initial").value<QKeySequence>()) {
        ConfigureHotkeys::KleddSetPendingGravityKey(gravity_key);
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
