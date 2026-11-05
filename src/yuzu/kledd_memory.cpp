// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/kledd_memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "common/fs/path_util.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/memory.h"
#include "qt_common/qt_common.h"

namespace {

using ValueType = KleddMemory::ValueType;
using Hit = KleddMemory::Hit;

constexpr std::uint64_t kBlock = 0x10000;
constexpr std::uint64_t kPage = 0x1000;
constexpr std::size_t kMaxHits = 8'000'000;
constexpr int kMaxHitRows = 1000;
constexpr std::uint64_t kMaxUnknownRegion = 256ull * 1024 * 1024;
constexpr std::uint64_t kMaxSnapshot = 3ull * 1024 * 1024 * 1024;

enum FirstMode : int { FirstExact = 0, FirstUnknown = 1 };
enum NextMode : int { NextExact = 0, NextChanged, NextUnchanged, NextIncreased, NextDecreased };

std::size_t TypeSize(ValueType type) {
    switch (type) {
    case ValueType::U8:
        return 1;
    case ValueType::U16:
        return 2;
    case ValueType::U32:
    case ValueType::S32:
    case ValueType::F32:
        return 4;
    case ValueType::U64:
    case ValueType::F64:
        return 8;
    case ValueType::Bytes:
        return 8;
    }
    return 4;
}

std::uint64_t LoadBits(const std::uint8_t* p, std::size_t size) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, size);
    return v;
}

double AsDouble(ValueType type, std::uint64_t bits) {
    switch (type) {
    case ValueType::F32: {
        float f;
        const auto b32 = static_cast<std::uint32_t>(bits);
        std::memcpy(&f, &b32, 4);
        return static_cast<double>(f);
    }
    case ValueType::F64: {
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    case ValueType::S32:
        return static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)));
    default:
        return static_cast<double>(bits);
    }
}

bool IsFloat(ValueType type) {
    return type == ValueType::F32 || type == ValueType::F64;
}

QString FormatValue(ValueType type, std::uint64_t bits) {
    switch (type) {
    case ValueType::F32:
    case ValueType::F64:
        return QString::number(AsDouble(type, bits), 'g', 7);
    case ValueType::S32:
        return QString::number(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)));
    case ValueType::Bytes: {
        QString s;
        for (int i = 0; i < 8; ++i) {
            s += QStringLiteral("%1 ").arg((bits >> (8 * i)) & 0xFF, 2, 16, QLatin1Char('0'));
        }
        return s.trimmed().toUpper();
    }
    default:
        return QStringLiteral("%1 (0x%2)")
            .arg(bits)
            .arg(bits, 0, 16)
            .toUpper()
            .replace(QStringLiteral("0X"), QStringLiteral("0x"));
    }
}

bool ParseValue(ValueType type, const QString& text, std::uint64_t& bits) {
    bool ok = false;
    const QString t = text.trimmed();
    switch (type) {
    case ValueType::F32: {
        const float f = t.toFloat(&ok);
        std::uint32_t b32;
        std::memcpy(&b32, &f, 4);
        bits = b32;
        return ok;
    }
    case ValueType::F64: {
        const double d = t.toDouble(&ok);
        std::memcpy(&bits, &d, 8);
        return ok;
    }
    case ValueType::S32: {
        const qlonglong v = t.toLongLong(&ok, 0);
        bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
        return ok;
    }
    default: {
        bits = t.toULongLong(&ok, 0);
        const std::size_t size = TypeSize(type);
        if (size < 8) {
            bits &= (1ull << (size * 8)) - 1;
        }
        return ok;
    }
    }
}

// Pattern bytes; -1 = wildcard.
bool ParsePattern(const QString& text, std::vector<int>& out) {
    out.clear();
    const QStringList parts =
        text.simplified().remove(QStringLiteral(",")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList tokens;
    for (const auto& p : parts) {
        if (p.size() > 2 && p != QStringLiteral("??")) {
            for (qsizetype i = 0; i + 1 < p.size(); i += 2) {
                tokens.push_back(p.mid(i, 2));
            }
        } else {
            tokens.push_back(p);
        }
    }
    for (const auto& tok : tokens) {
        if (tok == QStringLiteral("??") || tok == QStringLiteral("?")) {
            out.push_back(-1);
            continue;
        }
        bool ok = false;
        const int v = tok.toInt(&ok, 16);
        if (!ok || v < 0 || v > 255) {
            return false;
        }
        out.push_back(v);
    }
    return !out.empty();
}

struct ScanParams {
    Kernel::KProcess* process{};
    bool first = true;
    int mode = 0;
    ValueType type = ValueType::U32;
    bool aligned = true;
    std::uint64_t target = 0;
    double target_d = 0.0;
    double tolerance = 0.0;
    std::vector<int> pattern;
    std::uint64_t start = 0;
    std::uint64_t size = 0;
    std::vector<Hit> hits;
    const std::vector<KleddMemory::Chunk>* snapshot = nullptr;
};

bool MatchesExact(const ScanParams& p, std::uint64_t bits) {
    if (IsFloat(p.type)) {
        const double v = AsDouble(p.type, bits);
        return std::isfinite(v) && std::fabs(v - p.target_d) <= p.tolerance;
    }
    return bits == p.target;
}

bool MatchesNext(const ScanParams& p, std::uint64_t now, std::uint64_t prev) {
    switch (p.mode) {
    case NextExact:
        return MatchesExact(p, now);
    case NextChanged:
        return now != prev;
    case NextUnchanged:
        return now == prev;
    case NextIncreased:
        return AsDouble(p.type, now) > AsDouble(p.type, prev);
    case NextDecreased:
        return AsDouble(p.type, now) < AsDouble(p.type, prev);
    default:
        return false;
    }
}

bool MatchesPattern(const std::vector<int>& pattern, const std::uint8_t* p) {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] >= 0 && p[i] != static_cast<std::uint8_t>(pattern[i])) {
            return false;
        }
    }
    return true;
}

KleddMemory::ScanResult RunFirstScan(const ScanParams& p, std::atomic<bool>* cancel,
                                     std::atomic<std::uint64_t>* done) {
    KleddMemory::ScanResult result;
    auto& memory = p.process->GetMemory();
    const bool bytes = p.type == ValueType::Bytes;
    const std::size_t vsize = bytes ? p.pattern.size() : TypeSize(p.type);
    const std::size_t step = (p.aligned && !bytes) ? vsize : 1;
    std::vector<std::uint8_t> buf(kBlock + 16);
    const std::uint64_t end = p.start + p.size;

    const auto scan_chunk = [&](std::uint64_t base, std::size_t len, std::size_t readable) {
        // readable >= len; bytes past len only complete values that start inside the chunk
        for (std::size_t off = 0; off < len; off += step) {
            if (off + vsize > readable) {
                break;
            }
            const std::uint8_t* ptr = buf.data() + off;
            bool match;
            std::uint64_t bits;
            if (bytes) {
                match = MatchesPattern(p.pattern, ptr);
                bits = LoadBits(ptr, std::min<std::size_t>(8, readable - off));
            } else {
                bits = LoadBits(ptr, vsize);
                match = p.mode == FirstUnknown || MatchesExact(p, bits);
            }
            if (match) {
                if (result.hits.size() >= kMaxHits) {
                    result.truncated = true;
                    return false;
                }
                result.hits.push_back({base + off, bits});
            }
        }
        return true;
    };

    for (std::uint64_t b = p.start; b < end; b += kBlock) {
        if (cancel->load()) {
            result.cancelled = true;
            break;
        }
        const std::uint64_t len = std::min<std::uint64_t>(kBlock, end - b);
        bool keep_going = true;
        if (memory.IsValidVirtualAddressRange(b, len)) {
            std::size_t readable = static_cast<std::size_t>(len);
            const std::size_t extra = vsize > 1 ? vsize - 1 : 0;
            if (extra && b + len < end && memory.IsValidVirtualAddressRange(b + len, extra)) {
                readable += extra;
            }
            memory.ReadBlockUnsafe(b, buf.data(), readable);
            keep_going = scan_chunk(b, static_cast<std::size_t>(len), readable);
        } else {
            for (std::uint64_t pg = b; pg < b + len && keep_going; pg += kPage) {
                const std::uint64_t plen = std::min<std::uint64_t>(kPage, b + len - pg);
                if (!memory.IsValidVirtualAddress(pg)) {
                    continue;
                }
                memory.ReadBlockUnsafe(pg, buf.data(), static_cast<std::size_t>(plen));
                keep_going = scan_chunk(pg, static_cast<std::size_t>(plen),
                                        static_cast<std::size_t>(plen));
            }
        }
        done->store(b + len - p.start);
        if (!keep_going) {
            break;
        }
    }
    return result;
}

KleddMemory::ScanResult RunNextScan(const ScanParams& p, std::atomic<bool>* cancel,
                                    std::atomic<std::uint64_t>* done) {
    KleddMemory::ScanResult result;
    auto& memory = p.process->GetMemory();
    const bool bytes = p.type == ValueType::Bytes;
    const std::size_t vsize = bytes ? p.pattern.size() : TypeSize(p.type);
    const std::size_t lsize = bytes ? std::min<std::size_t>(8, vsize) : vsize;
    std::vector<std::uint8_t> buf(kBlock + 16);
    std::uint64_t block_base = ~0ull;
    bool block_ok = false;

    for (std::size_t i = 0; i < p.hits.size(); ++i) {
        if ((i & 0xFFF) == 0) {
            if (cancel->load()) {
                result.cancelled = true;
                break;
            }
            done->store(i);
        }
        const Hit& h = p.hits[i];
        const std::uint64_t base = h.addr & ~(kBlock - 1);
        if (base != block_base) {
            block_base = base;
            block_ok = memory.IsValidVirtualAddressRange(base, kBlock + 16);
            if (block_ok) {
                memory.ReadBlockUnsafe(base, buf.data(), kBlock + 16);
            }
        }
        std::uint8_t tmp[64];
        const std::uint8_t* ptr;
        if (block_ok) {
            ptr = buf.data() + (h.addr - base);
        } else {
            if (vsize > sizeof(tmp) || !memory.IsValidVirtualAddressRange(h.addr, vsize)) {
                continue;
            }
            memory.ReadBlockUnsafe(h.addr, tmp, vsize);
            ptr = tmp;
        }
        const std::uint64_t now = LoadBits(ptr, lsize);
        bool match;
        if (bytes && p.mode == NextExact) {
            match = MatchesPattern(p.pattern, ptr);
        } else {
            match = MatchesNext(p, now, h.prev);
        }
        if (match) {
            result.hits.push_back({h.addr, now});
        }
    }
    return result;
}

// Unknown starting value on a big region: copy every readable block instead of storing a
// result per value, then compare the copy against live memory on the next scan.
KleddMemory::ScanResult RunSnapshot(const ScanParams& p, std::atomic<bool>* cancel,
                                    std::atomic<std::uint64_t>* done) {
    KleddMemory::ScanResult result;
    auto& memory = p.process->GetMemory();
    const std::uint64_t end = p.start + p.size;
    std::uint64_t total = 0;
    for (std::uint64_t b = p.start; b < end; b += kBlock) {
        if (cancel->load()) {
            result.cancelled = true;
            break;
        }
        const std::uint64_t len = std::min<std::uint64_t>(kBlock, end - b);
        if (memory.IsValidVirtualAddressRange(b, len)) {
            KleddMemory::Chunk chunk{b, std::vector<std::uint8_t>(static_cast<std::size_t>(len))};
            memory.ReadBlockUnsafe(b, chunk.data.data(), chunk.data.size());
            result.snapshot.push_back(std::move(chunk));
            total += len;
            if (total >= kMaxSnapshot) {
                result.truncated = true;
                break;
            }
        }
        done->store(b + len - p.start);
    }
    return result;
}

bool SaneFloat(ValueType type, std::uint64_t bits) {
    if (!IsFloat(type)) {
        return true;
    }
    const double v = AsDouble(type, bits);
    return std::isfinite(v) && std::fabs(v) < 1e9 && (v == 0.0 || std::fabs(v) > 1e-20);
}

KleddMemory::ScanResult RunSnapshotCompare(const ScanParams& p, std::atomic<bool>* cancel,
                                           std::atomic<std::uint64_t>* done) {
    KleddMemory::ScanResult result;
    auto& memory = p.process->GetMemory();
    const std::size_t vsize = TypeSize(p.type);
    const std::size_t step = p.aligned ? vsize : 1;
    std::vector<std::uint8_t> buf(kBlock);
    const auto& chunks = *p.snapshot;
    for (std::size_t c = 0; c < chunks.size(); ++c) {
        if ((c & 0x3F) == 0) {
            if (cancel->load()) {
                result.cancelled = true;
                break;
            }
            done->store(c);
        }
        const auto& chunk = chunks[c];
        const std::size_t len = chunk.data.size();
        if (!memory.IsValidVirtualAddressRange(chunk.base, len)) {
            continue;
        }
        memory.ReadBlockUnsafe(chunk.base, buf.data(), len);
        for (std::size_t off = 0; off + vsize <= len; off += step) {
            const std::uint64_t prev = LoadBits(chunk.data.data() + off, vsize);
            const std::uint64_t now = LoadBits(buf.data() + off, vsize);
            if (!SaneFloat(p.type, now) || !SaneFloat(p.type, prev)) {
                continue;
            }
            if (MatchesNext(p, now, prev)) {
                if (result.hits.size() >= kMaxHits) {
                    result.truncated = true;
                    return result;
                }
                result.hits.push_back({chunk.base + off, now});
            }
        }
    }
    return result;
}

} // namespace

KleddMemory::KleddMemory(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle(tr("Kledd Memory"));
    resize(900, 640);
    BuildUi();

    scan_watcher = new QFutureWatcher<ScanResult>(this);
    connect(scan_watcher, &QFutureWatcher<ScanResult>::finished, this,
            &KleddMemory::OnScanFinished);

    tick_timer = new QTimer(this);
    connect(tick_timer, &QTimer::timeout, this, &KleddMemory::OnTick);
    tick_timer->start(50);
    UpdateStatus();
}

KleddMemory::~KleddMemory() {
    cancel_flag = true;
    if (scan_watcher && scan_watcher->isRunning()) {
        scan_watcher->waitForFinished();
    }
}

void KleddMemory::BuildUi() {
    auto* root = new QVBoxLayout(this);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // --- Search controls
    auto* search_box = new QGroupBox(tr("Search"), this);
    auto* grid = new QGridLayout(search_box);

    region_box = new QComboBox(search_box);
    region_box->addItems({tr("Heap"), tr("Main (code + data)"), tr("Alias"), tr("Custom range")});
    custom_start = new QLineEdit(search_box);
    custom_start->setPlaceholderText(tr("start (hex or main+/heap+)"));
    custom_size = new QLineEdit(search_box);
    custom_size->setPlaceholderText(tr("size (hex)"));

    type_box = new QComboBox(search_box);
    type_box->addItems({tr("u8"), tr("u16"), tr("u32"), tr("u64"), tr("s32"), tr("float"),
                        tr("double"), tr("bytes (hex pattern, ?? = any)")});
    type_box->setCurrentIndex(static_cast<int>(ValueType::F32));

    first_mode_box = new QComboBox(search_box);
    first_mode_box->addItems({tr("Exact value"), tr("Unknown initial value (snapshot)")});
    next_mode_box = new QComboBox(search_box);
    next_mode_box->addItems(
        {tr("Exact value"), tr("Changed"), tr("Unchanged"), tr("Increased"), tr("Decreased")});

    value_edit = new QLineEdit(search_box);
    value_edit->setPlaceholderText(tr("value, e.g. 0.925, 42, 0x3F6CE402 or 3F 6C E4 02"));
    tolerance_edit = new QLineEdit(QStringLiteral("0.0001"), search_box);
    tolerance_edit->setMaximumWidth(90);
    aligned_check = new QCheckBox(tr("Aligned"), search_box);
    aligned_check->setChecked(true);

    first_button = new QPushButton(tr("First scan"), search_box);
    next_button = new QPushButton(tr("Next scan"), search_box);
    reset_button = new QPushButton(tr("Reset"), search_box);
    cancel_button = new QPushButton(tr("Cancel"), search_box);
    cancel_button->setEnabled(false);
    progress = new QProgressBar(search_box);
    progress->setRange(0, 1000);
    progress->setValue(0);

    grid->addWidget(new QLabel(tr("Region"), search_box), 0, 0);
    grid->addWidget(region_box, 0, 1);
    grid->addWidget(custom_start, 0, 2);
    grid->addWidget(custom_size, 0, 3);
    grid->addWidget(new QLabel(tr("Type"), search_box), 1, 0);
    grid->addWidget(type_box, 1, 1);
    grid->addWidget(aligned_check, 1, 2);
    auto* tol_row = new QHBoxLayout();
    tolerance_label = new QLabel(tr("Float tolerance"), search_box);
    tol_row->addWidget(tolerance_label);
    tol_row->addWidget(tolerance_edit);
    grid->addLayout(tol_row, 1, 3);
    grid->addWidget(new QLabel(tr("Value"), search_box), 2, 0);
    grid->addWidget(value_edit, 2, 1, 1, 3);
    grid->addWidget(new QLabel(tr("First scan"), search_box), 3, 0);
    grid->addWidget(first_mode_box, 3, 1);
    grid->addWidget(new QLabel(tr("Next scan"), search_box), 3, 2);
    grid->addWidget(next_mode_box, 3, 3);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(first_button);
    buttons->addWidget(next_button);
    buttons->addWidget(reset_button);
    buttons->addWidget(cancel_button);
    buttons->addWidget(progress, 1);
    grid->addLayout(buttons, 4, 0, 1, 4);
    status_label = new QLabel(search_box);
    grid->addWidget(status_label, 5, 0, 1, 4);

    // --- Two tabs: search + results, and watch list + viewer
    tabs = new QTabWidget(this);
    auto* search_page = new QWidget(tabs);
    auto* search_layout = new QVBoxLayout(search_page);
    search_layout->setContentsMargins(4, 4, 4, 4);
    search_box->setParent(search_page);
    search_layout->addWidget(search_box);

    hit_table = new QTableWidget(0, 4, search_page);
    hit_table->setHorizontalHeaderLabels({tr("Address"), tr("Where"), tr("Value"), tr("Previous")});
    hit_table->horizontalHeader()->setStretchLastSection(true);
    hit_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    hit_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    hit_table->setFont(mono);
    hit_table->setToolTip(tr("Double-click a result to add it to the watch list"));
    hit_table->verticalHeader()->setDefaultSectionSize(22);
    search_layout->addWidget(new QLabel(tr("Results: double-click one to watch it"), search_page));
    search_layout->addWidget(hit_table, 1);

    auto* splitter = new QSplitter(Qt::Vertical, tabs);
    auto* watch_box = new QWidget(splitter);
    auto* watch_layout = new QVBoxLayout(watch_box);
    watch_layout->setContentsMargins(0, 0, 0, 0);
    watch_table = new QTableWidget(0, 5, watch_box);
    watch_table->setHorizontalHeaderLabels(
        {tr("Freeze"), tr("Name"), tr("Address"), tr("Type"), tr("Value (edit to write)")});
    watch_table->horizontalHeader()->setStretchLastSection(true);
    watch_table->setFont(mono);
    auto* watch_buttons = new QHBoxLayout();
    manual_addr = new QLineEdit(watch_box);
    manual_addr->setPlaceholderText(tr("add address: 0x..., main+0x..., heap+0x..."));
    auto* add_button = new QPushButton(tr("Add"), watch_box);
    auto* remove_button = new QPushButton(tr("Remove"), watch_box);
    auto* cheat_button = new QPushButton(tr("Copy as cheat"), watch_box);
    auto* save_button = new QPushButton(tr("Save list"), watch_box);
    auto* load_button = new QPushButton(tr("Load list"), watch_box);
    watch_buttons->addWidget(manual_addr, 1);
    watch_buttons->addWidget(add_button);
    watch_buttons->addWidget(remove_button);
    watch_buttons->addWidget(cheat_button);
    watch_buttons->addWidget(save_button);
    watch_buttons->addWidget(load_button);
    watch_layout->addWidget(
        new QLabel(tr("Double-click a value to change it. Freeze keeps it from changing back."),
                   watch_box));
    watch_layout->addWidget(watch_table);
    watch_layout->addLayout(watch_buttons);

    auto* viewer_box = new QWidget(splitter);
    auto* viewer_layout = new QVBoxLayout(viewer_box);
    viewer_layout->setContentsMargins(0, 0, 0, 0);
    auto* viewer_row = new QHBoxLayout();
    viewer_addr = new QLineEdit(viewer_box);
    viewer_addr->setPlaceholderText(tr("memory viewer address"));
    auto* go_button = new QPushButton(tr("View"), viewer_box);
    viewer_auto = new QCheckBox(tr("Live"), viewer_box);
    viewer_row->addWidget(new QLabel(tr("Viewer"), viewer_box));
    viewer_row->addWidget(viewer_addr, 1);
    viewer_row->addWidget(go_button);
    viewer_row->addWidget(viewer_auto);
    viewer = new QPlainTextEdit(viewer_box);
    viewer->setReadOnly(true);
    viewer->setFont(mono);
    viewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    viewer_layout->addLayout(viewer_row);
    viewer_layout->addWidget(viewer);

    splitter->addWidget(watch_box);
    splitter->addWidget(viewer_box);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    tabs->addTab(search_page, tr("1. Search"));
    tabs->addTab(splitter, tr("2. Watch && edit"));
    root->addWidget(tabs, 1);

    // Only show the fields that matter for the current choices
    const auto update_fields = [this] {
        const bool custom = region_box->currentIndex() == 3;
        custom_start->setVisible(custom);
        custom_size->setVisible(custom);
        const auto t = CurrentType();
        const bool is_float = t == ValueType::F32 || t == ValueType::F64;
        tolerance_edit->setVisible(is_float);
        tolerance_label->setVisible(is_float);
        aligned_check->setVisible(t != ValueType::Bytes);
        first_mode_box->setEnabled(t != ValueType::Bytes);
    };
    connect(region_box, &QComboBox::currentIndexChanged, this, update_fields);
    connect(type_box, &QComboBox::currentIndexChanged, this, update_fields);
    update_fields();

    connect(first_button, &QPushButton::clicked, this, &KleddMemory::OnFirstScan);
    connect(next_button, &QPushButton::clicked, this, &KleddMemory::OnNextScan);
    connect(reset_button, &QPushButton::clicked, this, &KleddMemory::OnReset);
    connect(cancel_button, &QPushButton::clicked, this, [this] { cancel_flag = true; });
    connect(hit_table, &QTableWidget::cellDoubleClicked, this, &KleddMemory::OnHitDoubleClicked);
    connect(watch_table, &QTableWidget::cellChanged, this, &KleddMemory::OnWatchCellChanged);
    connect(add_button, &QPushButton::clicked, this, &KleddMemory::OnAddManual);
    connect(remove_button, &QPushButton::clicked, this, &KleddMemory::OnRemoveWatch);
    connect(cheat_button, &QPushButton::clicked, this, &KleddMemory::OnCopyCheat);
    connect(save_button, &QPushButton::clicked, this, &KleddMemory::OnSaveWatch);
    connect(load_button, &QPushButton::clicked, this, &KleddMemory::OnLoadWatch);
    connect(go_button, &QPushButton::clicked, this, &KleddMemory::OnViewerGo);
    connect(viewer_addr, &QLineEdit::returnPressed, this, &KleddMemory::OnViewerGo);
    connect(manual_addr, &QLineEdit::returnPressed, this, &KleddMemory::OnAddManual);
}

Kernel::KProcess* KleddMemory::Process() const {
    if (!QtCommon::system || !QtCommon::system->IsPoweredOn()) {
        return nullptr;
    }
    return QtCommon::system->ApplicationProcess();
}

KleddMemory::ValueType KleddMemory::CurrentType() const {
    return static_cast<ValueType>(type_box->currentIndex());
}

bool KleddMemory::GetRegion(std::uint64_t& start, std::uint64_t& size) const {
    auto* process = Process();
    if (!process) {
        return false;
    }
    const auto& pt = process->GetPageTable();
    switch (region_box->currentIndex()) {
    case 0:
        start = GetInteger(pt.GetHeapRegionStart());
        size = pt.GetHeapRegionSize();
        return true;
    case 1:
        start = QtCommon::system->GetMainModuleBegin();
        size = QtCommon::system->GetMainModuleSize();
        return size != 0;
    case 2:
        start = GetInteger(pt.GetAliasRegionStart());
        size = pt.GetAliasRegionSize();
        return true;
    default: {
        if (!ParseAddress(custom_start->text(), start)) {
            return false;
        }
        bool ok = false;
        size = custom_size->text().trimmed().toULongLong(&ok, 16);
        return ok && size != 0;
    }
    }
}

bool KleddMemory::ParseAddress(const QString& text, std::uint64_t& out) const {
    QString t = text.trimmed().toLower();
    std::uint64_t base = 0;
    auto* process = Process();
    const auto take_prefix = [&](const QString& prefix, std::uint64_t value) {
        if (t.startsWith(prefix)) {
            t = t.mid(prefix.size()).trimmed();
            base = value;
            return true;
        }
        return false;
    };
    if (process) {
        const auto& pt = process->GetPageTable();
        if (!take_prefix(QStringLiteral("main+"), QtCommon::system->GetMainModuleBegin()) &&
            !take_prefix(QStringLiteral("heap+"), GetInteger(pt.GetHeapRegionStart()))) {
            take_prefix(QStringLiteral("alias+"), GetInteger(pt.GetAliasRegionStart()));
        }
    }
    if (t.startsWith(QStringLiteral("0x"))) {
        t = t.mid(2);
    }
    bool ok = false;
    const std::uint64_t v = t.toULongLong(&ok, 16);
    if (!ok) {
        return false;
    }
    out = base + v;
    return true;
}

QString KleddMemory::AddressLabel(std::uint64_t addr) const {
    auto* process = Process();
    if (!process) {
        return {};
    }
    const std::uint64_t mb = QtCommon::system->GetMainModuleBegin();
    const std::uint64_t ms = QtCommon::system->GetMainModuleSize();
    if (ms && addr >= mb && addr < mb + ms) {
        return QStringLiteral("main+0x%1").arg(addr - mb, 0, 16);
    }
    const auto& pt = process->GetPageTable();
    const std::uint64_t hb = GetInteger(pt.GetHeapRegionStart());
    if (addr >= hb && addr < hb + pt.GetHeapRegionSize()) {
        return QStringLiteral("heap+0x%1").arg(addr - hb, 0, 16);
    }
    const std::uint64_t ab = GetInteger(pt.GetAliasRegionStart());
    if (addr >= ab && addr < ab + pt.GetAliasRegionSize()) {
        return QStringLiteral("alias+0x%1").arg(addr - ab, 0, 16);
    }
    return {};
}

bool KleddMemory::ReadBits(std::uint64_t addr, ValueType type, std::uint64_t& out) const {
    auto* process = Process();
    const std::size_t size = TypeSize(type);
    if (!process || !process->GetMemory().IsValidVirtualAddressRange(addr, size)) {
        return false;
    }
    std::uint8_t tmp[8]{};
    process->GetMemory().ReadBlockUnsafe(addr, tmp, size);
    out = LoadBits(tmp, size);
    return true;
}

bool KleddMemory::WriteBits(std::uint64_t addr, ValueType type, std::uint64_t bits) {
    auto* process = Process();
    const std::size_t size = type == ValueType::Bytes ? 8 : TypeSize(type);
    if (!process || !process->GetMemory().IsValidVirtualAddressRange(addr, size)) {
        return false;
    }
    if (!process->GetMemory().WriteBlock(addr, &bits, size)) {
        return false;
    }
    const std::uint64_t mb = QtCommon::system->GetMainModuleBegin();
    const std::uint64_t ms = QtCommon::system->GetMainModuleSize();
    if (ms && addr >= mb && addr < mb + ms) {
        Core::InvalidateInstructionCacheRange(process, addr, size);
    }
    return true;
}

void KleddMemory::SetBusy(bool busy) {
    first_button->setEnabled(!busy);
    next_button->setEnabled(!busy && (!hits.empty() || !snapshot.empty()));
    reset_button->setEnabled(!busy);
    cancel_button->setEnabled(busy);
}

void KleddMemory::UpdateStatus() {
    if (!Process()) {
        status_label->setText(tr("No game running."));
        first_button->setEnabled(false);
        next_button->setEnabled(false);
        return;
    }
    if (scan_watcher && scan_watcher->isRunning()) {
        return;
    }
    first_button->setEnabled(true);
    next_button->setEnabled(!hits.empty() || !snapshot.empty());
    if (hits.empty() && !snapshot.empty()) {
        std::uint64_t bytes = 0;
        for (const auto& chunk : snapshot) {
            bytes += chunk.data.size();
        }
        status_label->setText(tr("Snapshot taken (%1 MB). Change the value in game, then Next "
                                 "scan with Changed / Increased / Decreased.")
                                  .arg(bytes >> 20));
    } else if (hits.empty()) {
        status_label->setText(tr("Ready. Main module: %1 (size 0x%2)")
                                  .arg(QStringLiteral("0x%1").arg(
                                      QtCommon::system->GetMainModuleBegin(), 0, 16))
                                  .arg(QtCommon::system->GetMainModuleSize(), 0, 16));
    } else {
        status_label->setText(tr("%1 results%2")
                                  .arg(hits.size())
                                  .arg(hits.size() > static_cast<std::size_t>(kMaxHitRows)
                                           ? tr(" (showing the first %1)").arg(kMaxHitRows)
                                           : QString()));
    }
}

void KleddMemory::OnFirstScan() {
    auto* process = Process();
    if (!process) {
        return;
    }
    ScanParams p;
    p.process = process;
    p.first = true;
    p.mode = first_mode_box->currentIndex();
    p.type = CurrentType();
    p.aligned = aligned_check->isChecked();
    if (!GetRegion(p.start, p.size)) {
        QMessageBox::warning(this, tr("Kledd Memory"), tr("Invalid region."));
        return;
    }
    if (p.type == ValueType::Bytes) {
        if (!ParsePattern(value_edit->text(), p.pattern) || p.pattern.size() > 48) {
            QMessageBox::warning(this, tr("Kledd Memory"),
                                 tr("Enter a hex byte pattern (1-48 bytes), e.g. 3F 6C ?? 02"));
            return;
        }
        p.mode = FirstExact;
    } else if (p.mode == FirstExact) {
        if (!ParseValue(p.type, value_edit->text(), p.target)) {
            QMessageBox::warning(this, tr("Kledd Memory"), tr("Could not read the value."));
            return;
        }
        p.target_d = AsDouble(p.type, p.target);
        p.tolerance = std::fabs(tolerance_edit->text().toDouble());
    }
    const bool use_snapshot = p.type != ValueType::Bytes && p.mode == FirstUnknown &&
                              p.size > kMaxUnknownRegion;
    snapshot.clear();
    hits.clear();
    hit_table->setRowCount(0);
    hits_type = p.type;
    hits_pattern_size = p.pattern.size();
    scan_total = std::max<std::uint64_t>(1, p.size);
    scan_done = 0;
    cancel_flag = false;
    SetBusy(true);
    status_label->setText(use_snapshot ? tr("Taking a snapshot...") : tr("Scanning..."));
    if (use_snapshot) {
        scan_watcher->setFuture(QtConcurrent::run(
            [p, this]() { return RunSnapshot(p, &cancel_flag, &scan_done); }));
    } else {
        scan_watcher->setFuture(QtConcurrent::run(
            [p, this]() { return RunFirstScan(p, &cancel_flag, &scan_done); }));
    }
}

void KleddMemory::OnNextScan() {
    auto* process = Process();
    if (!process || (hits.empty() && snapshot.empty())) {
        return;
    }
    ScanParams p;
    p.process = process;
    p.first = false;
    p.mode = next_mode_box->currentIndex();
    p.type = hits_type;
    if (p.type == ValueType::Bytes) {
        ParsePattern(value_edit->text(), p.pattern);
        if (p.pattern.empty()) {
            p.pattern.assign(8, -1);
        }
    } else if (p.mode == NextExact) {
        if (!ParseValue(p.type, value_edit->text(), p.target)) {
            QMessageBox::warning(this, tr("Kledd Memory"), tr("Could not read the value."));
            return;
        }
        p.target_d = AsDouble(p.type, p.target);
        p.tolerance = std::fabs(tolerance_edit->text().toDouble());
    }
    p.aligned = aligned_check->isChecked();
    scan_done = 0;
    cancel_flag = false;
    SetBusy(true);
    status_label->setText(tr("Narrowing..."));
    if (hits.empty()) {
        p.snapshot = &snapshot;
        scan_total = std::max<std::uint64_t>(1, snapshot.size());
        scan_watcher->setFuture(QtConcurrent::run(
            [p, this]() { return RunSnapshotCompare(p, &cancel_flag, &scan_done); }));
        return;
    }
    p.hits = hits;
    scan_total = std::max<std::uint64_t>(1, hits.size());
    scan_watcher->setFuture(QtConcurrent::run(
        [p, this]() { return RunNextScan(p, &cancel_flag, &scan_done); }));
}

void KleddMemory::OnScanFinished() {
    ScanResult r = scan_watcher->result();
    SetBusy(false);
    progress->setValue(1000);
    if (r.cancelled) {
        status_label->setText(tr("Cancelled. Previous results kept."));
        return;
    }
    if (!r.snapshot.empty()) {
        snapshot = std::move(r.snapshot);
    } else {
        snapshot.clear();
    }
    hits = std::move(r.hits);
    RefreshHitTable();
    UpdateStatus();
    if (r.truncated) {
        status_label->setText(status_label->text() +
                              tr("  (stopped at %1 results: refine with a next scan)").arg(kMaxHits));
    }
}

void KleddMemory::OnReset() {
    hits.clear();
    snapshot.clear();
    hit_table->setRowCount(0);
    progress->setValue(0);
    UpdateStatus();
}

void KleddMemory::RefreshHitTable() {
    const int rows = static_cast<int>(std::min<std::size_t>(hits.size(), kMaxHitRows));
    hit_table->setRowCount(rows);
    for (int r = 0; r < rows; ++r) {
        const Hit& h = hits[static_cast<std::size_t>(r)];
        std::uint64_t now = h.prev;
        ReadBits(h.addr, hits_type, now);
        const QString cells[4] = {QStringLiteral("0x%1").arg(h.addr, 0, 16), AddressLabel(h.addr),
                                  FormatValue(hits_type, now), FormatValue(hits_type, h.prev)};
        for (int c = 0; c < 4; ++c) {
            auto* item = hit_table->item(r, c);
            if (!item) {
                item = new QTableWidgetItem();
                hit_table->setItem(r, c, item);
            }
            item->setText(cells[c]);
        }
    }
}

void KleddMemory::OnHitDoubleClicked(int row, [[maybe_unused]] int column) {
    if (row < 0 || static_cast<std::size_t>(row) >= hits.size()) {
        return;
    }
    const Hit& h = hits[static_cast<std::size_t>(row)];
    if (hits_type == ValueType::Bytes) {
        // A byte pattern is usually a few floats in a row (a color, a position): watch each one.
        const std::size_t count = std::clamp<std::size_t>(hits_pattern_size / 4, 1, 4);
        static const char* const parts[4] = {"x / red", "y / green", "z / blue", "w / alpha"};
        for (std::size_t i = 0; i < count; ++i) {
            watches.push_back({tr("result %1: %2").arg(row + 1).arg(QString::fromLatin1(parts[i])),
                               h.addr + i * 4, ValueType::F32, false, 0});
        }
    } else {
        watches.push_back({tr("result %1").arg(row + 1), h.addr, hits_type, false, 0});
    }
    RefreshWatchTable(true);
    tabs->setCurrentIndex(1);
    viewer_addr->setText(QStringLiteral("0x%1").arg(h.addr & ~0xFull, 0, 16));
    RefreshViewer();
}

void KleddMemory::OnAddManual() {
    std::uint64_t addr = 0;
    if (!ParseAddress(manual_addr->text(), addr)) {
        return;
    }
    const ValueType t = CurrentType() == ValueType::Bytes ? ValueType::U32 : CurrentType();
    watches.push_back({tr("value %1").arg(watches.size() + 1), addr, t, false, 0});
    RefreshWatchTable(true);
}

void KleddMemory::RefreshWatchTable(bool full) {
    updating_watch_table = true;
    if (full) {
        tabs->setTabText(1, watches.empty() ? tr("2. Watch && edit")
                                            : tr("2. Watch && edit (%1)").arg(watches.size()));
        watch_table->setRowCount(static_cast<int>(watches.size()));
    }
    for (int r = 0; r < static_cast<int>(watches.size()); ++r) {
        const Watch& w = watches[static_cast<std::size_t>(r)];
        if (full) {
            auto* freeze = new QTableWidgetItem();
            freeze->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            freeze->setCheckState(w.frozen ? Qt::Checked : Qt::Unchecked);
            watch_table->setItem(r, 0, freeze);
            watch_table->setItem(r, 1, new QTableWidgetItem(w.name));
            auto* addr = new QTableWidgetItem(QStringLiteral("0x%1  %2")
                                                  .arg(w.addr, 0, 16)
                                                  .arg(AddressLabel(w.addr)));
            addr->setFlags(addr->flags() & ~Qt::ItemIsEditable);
            watch_table->setItem(r, 2, addr);
            auto* type = new QTableWidgetItem(type_box->itemText(static_cast<int>(w.type)));
            type->setFlags(type->flags() & ~Qt::ItemIsEditable);
            watch_table->setItem(r, 3, type);
            watch_table->setItem(r, 4, new QTableWidgetItem());
        }
        auto* value = watch_table->item(r, 4);
        if (value && !(watch_table->currentRow() == r && watch_table->currentColumn() == 4 &&
                       watch_table->viewport()->isAncestorOf(QApplication::focusWidget()))) {
            std::uint64_t bits = 0;
            value->setText(ReadBits(w.addr, w.type, bits) ? FormatValue(w.type, bits)
                                                          : tr("(unreadable)"));
        }
    }
    updating_watch_table = false;
}

void KleddMemory::OnWatchCellChanged(int row, int column) {
    if (updating_watch_table || row < 0 || static_cast<std::size_t>(row) >= watches.size()) {
        return;
    }
    Watch& w = watches[static_cast<std::size_t>(row)];
    auto* item = watch_table->item(row, column);
    if (!item) {
        return;
    }
    if (column == 0) {
        w.frozen = item->checkState() == Qt::Checked;
        if (w.frozen) {
            ReadBits(w.addr, w.type, w.frozen_bits);
        }
    } else if (column == 1) {
        w.name = item->text();
    } else if (column == 4) {
        QString text = item->text();
        const int paren = text.indexOf(QLatin1Char('('));
        if (paren > 0) {
            text = text.left(paren);
        }
        std::uint64_t bits = 0;
        if (ParseValue(w.type, text, bits)) {
            WriteBits(w.addr, w.type, bits);
            if (w.frozen) {
                w.frozen_bits = bits;
            }
        }
    }
}

void KleddMemory::OnRemoveWatch() {
    const int row = watch_table->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= watches.size()) {
        return;
    }
    watches.erase(watches.begin() + row);
    RefreshWatchTable(true);
}

void KleddMemory::OnCopyCheat() {
    const int row = watch_table->currentRow();
    auto* process = Process();
    if (!process || row < 0 || static_cast<std::size_t>(row) >= watches.size()) {
        return;
    }
    const Watch& w = watches[static_cast<std::size_t>(row)];
    const std::uint64_t mb = QtCommon::system->GetMainModuleBegin();
    const std::uint64_t ms = QtCommon::system->GetMainModuleSize();
    const auto& pt = process->GetPageTable();
    const std::uint64_t hb = GetInteger(pt.GetHeapRegionStart());
    std::uint32_t region;
    std::uint64_t off;
    if (ms && w.addr >= mb && w.addr < mb + ms) {
        region = 0;
        off = w.addr - mb;
    } else if (w.addr >= hb && w.addr < hb + pt.GetHeapRegionSize()) {
        region = 1;
        off = w.addr - hb;
    } else {
        QMessageBox::information(this, tr("Kledd Memory"),
                                 tr("Only main and heap addresses can become cheats."));
        return;
    }
    std::uint64_t bits = 0;
    ReadBits(w.addr, w.type, bits);
    const std::uint32_t width = static_cast<std::uint32_t>(TypeSize(w.type));
    const std::uint32_t head =
        (width << 24) | (region << 20) | static_cast<std::uint32_t>((off >> 32) & 0xFF);
    QString line = QStringLiteral("[%1]\n%2 %3 ")
                       .arg(w.name)
                       .arg(head, 8, 16, QLatin1Char('0'))
                       .arg(static_cast<std::uint32_t>(off), 8, 16, QLatin1Char('0'));
    if (width == 8) {
        line += QStringLiteral("%1 %2")
                    .arg(static_cast<std::uint32_t>(bits >> 32), 8, 16, QLatin1Char('0'))
                    .arg(static_cast<std::uint32_t>(bits), 8, 16, QLatin1Char('0'));
    } else {
        line += QStringLiteral("%1").arg(static_cast<std::uint32_t>(bits), 8, 16, QLatin1Char('0'));
    }
    QApplication::clipboard()->setText(line.toUpper().replace(QStringLiteral("[") + w.name.toUpper(),
                                                              QStringLiteral("[") + w.name));
    status_label->setText(tr("Cheat copied to the clipboard%1.")
                              .arg(region == 1 ? tr(" (heap: only valid while the heap layout stays the same)")
                                               : QString()));
}

QString KleddMemory::WatchFilePath() const {
    const QString dir = QString::fromStdString(
                            Common::FS::GetEdenPathString(Common::FS::EdenPath::ConfigDir)) +
                        QStringLiteral("/kledd_memory");
    QDir().mkpath(dir);
    const std::uint64_t tid = QtCommon::system ? QtCommon::system->GetApplicationProcessProgramID() : 0;
    return dir + QStringLiteral("/%1.json").arg(tid, 16, 16, QLatin1Char('0')).toUpper();
}

void KleddMemory::OnSaveWatch() {
    QJsonArray arr;
    const std::uint64_t mb = QtCommon::system ? QtCommon::system->GetMainModuleBegin() : 0;
    for (const auto& w : watches) {
        QJsonObject o;
        o[QStringLiteral("name")] = w.name;
        // main addresses are saved relative to main so they survive ASLR
        const QString label = AddressLabel(w.addr);
        if (label.startsWith(QStringLiteral("main+"))) {
            o[QStringLiteral("main_offset")] = QStringLiteral("%1").arg(w.addr - mb, 0, 16);
        } else {
            o[QStringLiteral("address")] = QStringLiteral("%1").arg(w.addr, 0, 16);
        }
        o[QStringLiteral("type")] = static_cast<int>(w.type);
        arr.push_back(o);
    }
    QFile f(WatchFilePath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(arr).toJson());
        status_label->setText(tr("Saved %1 entries to %2").arg(watches.size()).arg(f.fileName()));
    }
}

void KleddMemory::OnLoadWatch() {
    QFile f(WatchFilePath());
    if (!f.open(QIODevice::ReadOnly)) {
        status_label->setText(tr("No saved list for this game yet."));
        return;
    }
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    const std::uint64_t mb = QtCommon::system ? QtCommon::system->GetMainModuleBegin() : 0;
    watches.clear();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        Watch w{};
        w.name = o[QStringLiteral("name")].toString();
        bool ok = false;
        if (o.contains(QStringLiteral("main_offset"))) {
            w.addr = mb + o[QStringLiteral("main_offset")].toString().toULongLong(&ok, 16);
        } else {
            w.addr = o[QStringLiteral("address")].toString().toULongLong(&ok, 16);
        }
        w.type = static_cast<ValueType>(std::clamp(o[QStringLiteral("type")].toInt(), 0, 6));
        if (ok) {
            watches.push_back(w);
        }
    }
    RefreshWatchTable(true);
}

void KleddMemory::OnViewerGo() {
    RefreshViewer();
}

void KleddMemory::RefreshViewer() {
    std::uint64_t addr = 0;
    auto* process = Process();
    if (!process || !ParseAddress(viewer_addr->text(), addr)) {
        return;
    }
    addr &= ~0xFull;
    QString out;
    for (int row = 0; row < 32; ++row) {
        const std::uint64_t a = addr + static_cast<std::uint64_t>(row) * 16;
        std::uint8_t b[16]{};
        const bool ok = process->GetMemory().IsValidVirtualAddressRange(a, 16);
        if (ok) {
            process->GetMemory().ReadBlockUnsafe(a, b, 16);
        }
        QString line = QStringLiteral("%1  ").arg(a, 12, 16, QLatin1Char('0'));
        for (int i = 0; i < 16; ++i) {
            line += ok ? QStringLiteral("%1 ").arg(static_cast<unsigned>(b[i]), 2, 16, QLatin1Char('0'))
                       : QStringLiteral("?? ");
            if (i == 7) {
                line += QLatin1Char(' ');
            }
        }
        line += QStringLiteral(" |");
        for (int i = 0; i < 4 && ok; ++i) {
            float f;
            std::memcpy(&f, b + i * 4, 4);
            line += QStringLiteral(" %1").arg(static_cast<double>(f), 11, 'g', 5);
        }
        out += line.toUpper().replace(QStringLiteral("E+"), QStringLiteral("e+"))
                   .replace(QStringLiteral("E-"), QStringLiteral("e-")) +
               QLatin1Char('\n');
    }
    viewer->setPlainText(out);
}

void KleddMemory::OnTick() {
    ++tick_count;
    if (scan_watcher->isRunning()) {
        progress->setValue(static_cast<int>(scan_done.load() * 1000 / std::max<std::uint64_t>(1, scan_total)));
        return;
    }
    if (!Process()) {
        if (tick_count % 20 == 0) {
            UpdateStatus();
        }
        return;
    }
    // freeze
    for (const auto& w : watches) {
        if (!w.frozen) {
            continue;
        }
        // Only write when the game has put a different value back: every write tells the
        // GPU side the memory changed, so blind writes 20 times a second tank the frame rate.
        std::uint64_t now = 0;
        if (ReadBits(w.addr, w.type, now) && now != w.frozen_bits) {
            WriteBits(w.addr, w.type, w.frozen_bits);
        }
    }
    if (!isVisible()) {
        return;
    }
    if (tick_count % 6 == 0) {
        RefreshWatchTable(false);
    }
    if (tick_count % 10 == 0) {
        if (!hits.empty() && isVisible()) {
            RefreshHitTable();
        }
        if (viewer_auto->isChecked()) {
            RefreshViewer();
        }
    }
    if (tick_count % 20 == 0) {
        UpdateStatus();
    }
}
