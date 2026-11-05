// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Kledd Memory: a small Cheat-Engine-style tool for the running game.
// Search guest memory, narrow results, watch/edit/freeze values, view memory as hex and floats.

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <QFutureWatcher>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;

namespace Kernel {
class KProcess;
}

class KleddMemory : public QWidget {
    Q_OBJECT

public:
    explicit KleddMemory(QWidget* parent = nullptr);
    ~KleddMemory() override;

    enum class ValueType : int { U8, U16, U32, U64, S32, F32, F64, Bytes };

    struct Hit {
        std::uint64_t addr;
        std::uint64_t prev;
    };

    struct Watch {
        QString name;
        std::uint64_t addr;
        ValueType type;
        bool frozen;
        std::uint64_t frozen_bits;
        std::uint64_t saved_bits = 0;
        bool has_saved = false;
    };

    struct Chunk {
        std::uint64_t base;
        std::vector<std::uint8_t> data;
    };

    struct ScanResult {
        std::vector<Hit> hits;
        std::vector<Chunk> snapshot;
        bool truncated = false;
        bool cancelled = false;
        QString error;
    };

private:
    void BuildUi();
    void OnFirstScan();
    void OnNextScan();
    void OnReset();
    void OnScanFinished();
    void OnHitDoubleClicked(int row, int column);
    void OnAddManual();
    void OnWatchCellChanged(int row, int column);
    void OnRemoveWatch();
    void OnAddSelectedHits();
    void OnSetSelected();
    void OnRestoreSelected();
    std::vector<int> SelectedRows(QTableWidget* table) const;
    void AddHit(int row);
    void OnCopyCheat();
    void OnSaveWatch();
    void OnLoadWatch();
    void OnViewerGo();
    void OnTick();

    void RefreshHitTable();
    void RefreshWatchTable(bool full);
    void RefreshViewer();
    void UpdateStatus();
    void SetBusy(bool busy);

    Kernel::KProcess* Process() const;
    bool GetRegion(std::uint64_t& start, std::uint64_t& size) const;
    bool ParseAddress(const QString& text, std::uint64_t& out) const;
    QString AddressLabel(std::uint64_t addr) const;
    bool ReadBits(std::uint64_t addr, ValueType type, std::uint64_t& out) const;
    bool WriteBits(std::uint64_t addr, ValueType type, std::uint64_t bits);
    QString WatchFilePath() const;

    ValueType CurrentType() const;

    // UI
    QComboBox* region_box{};
    QLineEdit* custom_start{};
    QLineEdit* custom_size{};
    QComboBox* type_box{};
    QComboBox* first_mode_box{};
    QComboBox* next_mode_box{};
    QLineEdit* value_edit{};
    QLineEdit* tolerance_edit{};
    QCheckBox* aligned_check{};
    QPushButton* first_button{};
    QPushButton* next_button{};
    QPushButton* reset_button{};
    QPushButton* cancel_button{};
    QProgressBar* progress{};
    QLabel* status_label{};
    QTableWidget* hit_table{};
    QTableWidget* watch_table{};
    QLineEdit* manual_addr{};
    QLineEdit* viewer_addr{};
    QCheckBox* viewer_auto{};
    QPlainTextEdit* viewer{};
    QTimer* tick_timer{};
    QTabWidget* tabs{};
    QComboBox* add_type_box{};
    QLineEdit* bulk_value{};
    QLabel* tolerance_label{};

    // State
    std::vector<Hit> hits;
    std::vector<Chunk> snapshot;
    ValueType hits_type = ValueType::U32;
    std::size_t hits_pattern_size = 0;
    std::vector<Watch> watches;
    bool updating_watch_table = false;
    int tick_count = 0;

    QFutureWatcher<ScanResult>* scan_watcher{};
    std::atomic<bool> cancel_flag{false};
    std::atomic<std::uint64_t> scan_done{0};
    std::uint64_t scan_total = 1;
};
