// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <QKeySequence>

#include <memory>
#include <QStandardItemModel>
#include <QWidget>

namespace Common {
class ParamPackage;
}

namespace Core::HID {
class HIDCore;
class EmulatedController;
enum class NpadButton : u64;
} // namespace Core::HID

namespace Ui {
class ConfigureHotkeys;
}

class HotkeyRegistry;
class QStandardItemModel;

class ConfigureHotkeys : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureHotkeys(Core::HID::HIDCore& hid_core_, QWidget* parent = nullptr);
    ~ConfigureHotkeys() override;

    void ApplyConfiguration(HotkeyRegistry& registry);

    /**
     * Populates the hotkey list widget using data from the provided registry.
     * Called every time the Configure dialog is opened.
     * @param registry The HotkeyRegistry whose data is used to populate the list.
     */
    void Populate(const HotkeyRegistry& registry);

    /// eden-kledd: the Gravity Check key set in Configure mouse panning. This tab keeps its own
    /// copy of every key while the Configure dialog is open and writes them all back on OK, so
    /// the mouse window hands its key over here, and it wins for that one row.
    static void KleddSetPendingGravityKey(const QKeySequence& key);

private:
    static inline std::optional<QKeySequence> kledd_pending_gravity_key{};

    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void Configure(QModelIndex index);
    void ConfigureController(QModelIndex index);
    std::pair<bool, QString> IsUsedKey(QKeySequence key_sequence) const;
    std::pair<bool, QString> IsUsedControllerKey(const QString& key_sequence) const;

    void RestoreDefaults();
    void ClearAll();
    void PopupContextMenu(const QPoint& menu_location);
    void RestoreControllerHotkey(QModelIndex index);
    void RestoreHotkey(QModelIndex index);

    void SetPollingResult(bool cancel);
    QString GetButtonCombinationName(Core::HID::NpadButton button, bool home, bool capture) const;

    std::unique_ptr<Ui::ConfigureHotkeys> ui;

    QStandardItemModel* model;

    bool pressed_home_button;
    bool pressed_capture_button;
    QModelIndex button_model_index;
    Core::HID::NpadButton pressed_buttons;

    Core::HID::EmulatedController* controller;
    std::unique_ptr<QTimer> timeout_timer;
    std::unique_ptr<QTimer> poll_timer;
    std::optional<std::function<void(bool)>> input_setter;
};
