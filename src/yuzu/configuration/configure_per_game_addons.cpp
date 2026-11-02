// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <utility>

#include <fmt/format.h>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QTextStream>
#include <QUrl>
#include <QHeaderView>
#include <QMenu>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QTreeView>

#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "configuration/addon/mod_select_dialog.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "frontend_common/mod_manager.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/mod.h"
#include "ui_configure_per_game_addons.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_per_game_addons.h"

ConfigurePerGameAddons::ConfigurePerGameAddons(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigurePerGameAddons>()}, system{system_} {
    ui->setupUi(this);

    layout = new QVBoxLayout;
    tree_view = new QTreeView;
    item_model = new QStandardItemModel(tree_view);
    tree_view->setModel(item_model);
    tree_view->setAlternatingRowColors(true);
    tree_view->setSelectionMode(QHeaderView::ExtendedSelection);
    tree_view->setSelectionBehavior(QHeaderView::SelectRows);
    tree_view->setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    // eden-kledd: rows are kept sorted inside the Active / Inactive groups instead
    tree_view->setSortingEnabled(false);
    tree_view->setEditTriggers(QHeaderView::NoEditTriggers);
    tree_view->setUniformRowHeights(true);
    tree_view->setContextMenuPolicy(Qt::CustomContextMenu);

    item_model->insertColumns(0, 2);
    item_model->setHeaderData(0, Qt::Horizontal, tr("Patch Name"));
    item_model->setHeaderData(1, Qt::Horizontal, tr("Version"));

    tree_view->header()->setStretchLastSection(false);
    tree_view->header()->setSectionResizeMode(0, QHeaderView::ResizeMode::Stretch);
    tree_view->header()->setMinimumSectionSize(150);

    // We must register all custom types with the Qt Automoc system so that we are able to use it
    // with signals/slots. In this case, QList falls under the umbrella of custom types.
    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tree_view);

    ui->scrollArea->setLayout(layout);

    ui->scrollArea->setEnabled(!system.IsPoweredOn());

    connect(item_model, &QStandardItemModel::itemChanged, this,
            &ConfigurePerGameAddons::OnItemChanged);
    connect(item_model, &QStandardItemModel::itemChanged,
            [] { UISettings::values.is_game_list_reload_pending.exchange(true); });

    connect(ui->folder, &QAbstractButton::clicked, this, &ConfigurePerGameAddons::InstallModFolder);
    connect(ui->zip, &QAbstractButton::clicked, this, &ConfigurePerGameAddons::InstallModZip);

    connect(tree_view, &QTreeView::customContextMenuRequested, this,
            &ConfigurePerGameAddons::showContextMenu);
    connect(tree_view, &QTreeView::doubleClicked, this,
            &ConfigurePerGameAddons::OnItemDoubleClicked);
    connect(tree_view, &QTreeView::clicked, this, &ConfigurePerGameAddons::OnItemClicked);
}

ConfigurePerGameAddons::~ConfigurePerGameAddons() = default;

void ConfigurePerGameAddons::OnItemChanged(QStandardItem* item) {
    if (update_items.size() > 1 && item->checkState() == Qt::Checked) {
        auto it = std::find(update_items.begin(), update_items.end(), item);
        if (it != update_items.end()) {
            for (auto* update_item : update_items) {
                if (update_item != item && update_item->checkState() == Qt::Checked) {
                    disconnect(item_model, &QStandardItemModel::itemChanged, this,
                               &ConfigurePerGameAddons::OnItemChanged);
                    update_item->setCheckState(Qt::Unchecked);
                    connect(item_model, &QStandardItemModel::itemChanged, this,
                            &ConfigurePerGameAddons::OnItemChanged);
                }
            }
        }
    }

    // eden-kledd: a ticked / unticked add-on moves to the Active / Inactive group. Done after
    // the signal returns so the model is not changed while it is still notifying.
    if (item->isCheckable() && !regroup_pending) {
        regroup_pending = true;
        QTimer::singleShot(0, this, [this] { RegroupItems(); });
    }
}

QStandardItem* ConfigurePerGameAddons::MakeGroupItem(const QString& title) const {
    auto* const group = new QStandardItem(title);
    group->setEditable(false);
    group->setSelectable(false);
    group->setCheckable(false);
    QFont font = group->font();
    font.setBold(true);
    group->setFont(font);
    return group;
}

void ConfigurePerGameAddons::UpdateGroupTitles() {
    if (active_group == nullptr || inactive_group == nullptr) {
        return;
    }
    const auto title = [](const QString& name, int count) {
        return QStringLiteral("%1 (%2)").arg(name).arg(count);
    };
    if (const auto t = title(tr("Active"), active_group->rowCount()); active_group->text() != t) {
        active_group->setText(t);
    }
    if (const auto t = title(tr("Inactive"), inactive_group->rowCount());
        inactive_group->text() != t) {
        inactive_group->setText(t);
    }
}

void ConfigurePerGameAddons::RegroupItems() {
    regroup_pending = false;
    if (active_group == nullptr || inactive_group == nullptr) {
        return;
    }
    for (QStandardItem* group : {active_group, inactive_group}) {
        for (int row = group->rowCount() - 1; row >= 0; --row) {
            QStandardItem* const item = group->child(row, 0);
            if (item == nullptr || !item->isCheckable()) {
                continue;
            }
            QStandardItem* const target =
                item->checkState() == Qt::Checked ? active_group : inactive_group;
            if (target == group) {
                continue;
            }
            const bool was_expanded = tree_view->isExpanded(item->index());
            const QList<QStandardItem*> taken = group->takeRow(row);
            target->appendRow(taken);
            if (was_expanded) {
                tree_view->expand(taken.front()->index());
            }
        }
    }
    active_group->sortChildren(0);
    inactive_group->sortChildren(0);
    UpdateGroupTitles();
}

void ConfigurePerGameAddons::AddCheatRows(QStandardItem* mod_item, const QString& location) {
    // A mod folder keeps its cheats in a "cheats" subfolder; a loose cheat_* file is one file.
    QStringList files;
    const QFileInfo info(location);
    if (info.isFile()) {
        files << info.absoluteFilePath();
    } else if (info.isDir()) {
        const QDir mod_dir(location);
        for (const QFileInfo& sub : mod_dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (sub.fileName().compare(QStringLiteral("cheats"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            for (const QFileInfo& f :
                 QDir(sub.absoluteFilePath()).entryInfoList(QDir::Files, QDir::Name)) {
                files << f.absoluteFilePath();
            }
        }
    }

    QFont font = mod_item->font();
    font.setItalic(true);
    for (const QString& path : files) {
        QStringList names;
        QFile cheat_file(path);
        if (cheat_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&cheat_file);
            while (!in.atEnd()) {
                const QString line = in.readLine().trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    const auto end = line.indexOf(QLatin1Char(']'));
                    const QString name = line.mid(1, end > 0 ? end - 1 : -1).trimmed();
                    if (!name.isEmpty()) {
                        names << name;
                    }
                } else if (line.startsWith(QLatin1Char('{'))) {
                    names << tr("Master code");
                }
            }
        }
        if (names.isEmpty()) {
            names << QFileInfo(path).fileName();
        }
        for (const QString& name : names) {
            auto* const cheat = new QStandardItem(QStringLiteral("\u21B3 %1").arg(name));
            cheat->setEditable(false);
            cheat->setFont(font);
            cheat->setData(path, CHEAT_FILE);
            cheat->setToolTip(tr("Double-click to show this cheat file"));
            auto* const kind = new QStandardItem(tr("Cheat (%1)").arg(QFileInfo(path).fileName()));
            kind->setEditable(false);
            kind->setFont(font);
            kind->setData(path, CHEAT_FILE);
            mod_item->appendRow(QList<QStandardItem*>{cheat, kind});
        }
    }
}

void ConfigurePerGameAddons::OnItemClicked(const QModelIndex& index) {
    // Clicking the type cell ("IPSwitch, Cheats") of a mod with cheats shows / hides them.
    if (!index.isValid() || index.column() != 1) {
        return;
    }
    const QModelIndex first = index.siblingAtColumn(0);
    if (item_model->hasChildren(first) && first.parent().isValid()) {
        tree_view->setExpanded(first, !tree_view->isExpanded(first));
    }
}

void ConfigurePerGameAddons::OnItemDoubleClicked(const QModelIndex& index) {
    const QString path = index.siblingAtColumn(0).data(CHEAT_FILE).toString();
    if (path.isEmpty()) {
        return;
    }
#ifdef _WIN32
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void ConfigurePerGameAddons::ApplyConfiguration() {
    std::vector<std::string> disabled_addons;

    for (const auto& item : list_items) {
        const auto disabled = item.front()->checkState() == Qt::Unchecked;
        if (disabled) {
            QVariant userData = item.front()->data(Qt::UserRole);
            if (userData.isValid() && userData.canConvert<quint32>() &&
                item.front()->text() == QStringLiteral("Update")) {
                quint32 numeric_version = userData.toUInt();
                disabled_addons.push_back(fmt::format("Update@{}", numeric_version));
            } else {
                disabled_addons.push_back(item.front()->text().toStdString());
            }
        }
    }

    auto current = Settings::values.disabled_addons[title_id];
    std::sort(disabled_addons.begin(), disabled_addons.end());
    std::sort(current.begin(), current.end());
    if (disabled_addons != current) {
        Common::FS::RemoveFile(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                               "game_list" / fmt::format("{:016X}.pv.txt", title_id));
    }

    Settings::values.disabled_addons[title_id] = disabled_addons;
}

void ConfigurePerGameAddons::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGameAddons::SetTitleId(u64 id) {
    this->title_id = id;
}

void ConfigurePerGameAddons::InstallMods(const QStringList& mods) {
    QStringList failed;
    for (const auto& mod : mods) {
        if (FrontendCommon::InstallMod(mod.toStdString(), title_id, true) ==
            FrontendCommon::Failed) {
            failed << QFileInfo(mod).baseName();
        }
    }

    if (failed.empty()) {
        QtCommon::Frontend::Information(tr("Mod Install Succeeded"),
                                        tr("Successfully installed all mods."));

        item_model->removeRows(0, item_model->rowCount());
        list_items.clear();
        LoadConfiguration();

        UISettings::values.is_game_list_reload_pending.exchange(true);
    } else {
        QtCommon::Frontend::Critical(
            tr("Mod Install Failed"),
            tr("Failed to install the following mods:\n\t%1\nCheck the log for details.")
                .arg(failed.join(QStringLiteral("\n\t"))));
    }
}

void ConfigurePerGameAddons::InstallModPath(const QString& path, const QString& fallbackName) {
    const auto mods = QtCommon::Mod::GetModFolders(path, fallbackName);

    if (mods.size() > 1) {
        ModSelectDialog* dialog = new ModSelectDialog(mods, this);
        connect(dialog, &ModSelectDialog::modsSelected, this, &ConfigurePerGameAddons::InstallMods);
        dialog->show();
    } else if (!mods.empty()) {
        InstallMods(mods);
    }
}

void ConfigurePerGameAddons::InstallModFolder() {
    const auto path = QtCommon::Frontend::GetExistingDirectory(
        tr("Mod Folder"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (path.isEmpty()) {
        return;
    }

    InstallModPath(path);
}

void ConfigurePerGameAddons::InstallModZip() {
    // TODO(crueter): use GetOpenFileName to allow select multiple ZIPs
    const auto path = QtCommon::Frontend::GetOpenFileName(
        tr("Zipped Mod Location"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("Zipped Archives (*.zip)"));
    if (path.isEmpty()) {
        return;
    }

    const QString extracted = QtCommon::Mod::ExtractMod(path);
    if (!extracted.isEmpty())
        InstallModPath(extracted, QFileInfo(path).baseName());
}

void ConfigurePerGameAddons::AddonDeleteRequested(QList<QModelIndex> selected) {
    QList<QModelIndex> filtered;
    for (const QModelIndex& index : selected) {
        if (!index.data(PATCH_LOCATION).toString().isEmpty())
            filtered << index;
    }

    if (filtered.empty()) {
        QtCommon::Frontend::Critical(tr("Invalid Selection"),
                                     tr("Only mods, cheats, and patches can be deleted.\nTo delete "
                                        "NAND-installed updates, right-click the game in the game "
                                        "list and click Remove -> Remove Installed Update."));
        return;
    }

    const auto header = tr("You are about to delete the following installed mods:\n");
    QString selected_str;
    for (const QModelIndex& index : filtered) {
        selected_str = selected_str % index.data().toString() % QStringLiteral("\n");
    }

    const auto footer = tr("\nOnce deleted, these can NOT be recovered. Are you 100% sure "
                           "you want to delete them?");

    QString caption = header % selected_str % footer;

    auto choice = QtCommon::Frontend::Warning(tr("Delete add-on(s)?"), caption,
                                              QtCommon::Frontend::StandardButton::Yes |
                                                  QtCommon::Frontend::StandardButton::No);

    if (choice == QtCommon::Frontend::StandardButton::No)
        return;

    for (const QModelIndex& index : filtered) {
        std::filesystem::remove_all(index.data(PATCH_LOCATION).toString().toStdString());
    }

    QtCommon::Frontend::Information(tr("Successfully deleted"),
                                    tr("Successfully deleted all selected mods."));

    item_model->removeRows(0, item_model->rowCount());
    list_items.clear();
    LoadConfiguration();

    UISettings::values.is_game_list_reload_pending.exchange(true);
}

void ConfigurePerGameAddons::showContextMenu(const QPoint& pos) {
    const QModelIndex index = tree_view->indexAt(pos);
    auto selected = tree_view->selectionModel()->selectedRows();
    if (index.isValid() && selected.empty()) {
        QModelIndex idx = index.siblingAtColumn(0);
        if (idx.isValid())
            selected << idx;
    }

    if (selected.empty())
        return;

    QMenu menu(this);

    QAction* remove = menu.addAction(tr("&Delete"));
    connect(remove, &QAction::triggered, this,
            [this, selected]() { AddonDeleteRequested(selected); });

    if (selected.length() == 1) {
        auto loc = selected.at(0).data(PATCH_LOCATION).toString();
        if (loc.isEmpty()) {
            // eden-kledd: a cheat row opens the folder that holds its cheat file
            const auto cheat = selected.at(0).data(CHEAT_FILE).toString();
            if (!cheat.isEmpty()) {
                loc = QFileInfo(cheat).absolutePath();
            }
        }
        if (QFileInfo::exists(loc)) {
            QAction* open = menu.addAction(tr("&Open in File Manager"));
            connect(open, &QAction::triggered, this,
                    [selected, loc]() { QDesktopServices::openUrl(QUrl::fromLocalFile(loc)); });
        }
    }

    menu.exec(tree_view->viewport()->mapToGlobal(pos));
}

void ConfigurePerGameAddons::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigurePerGameAddons::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigurePerGameAddons::LoadConfiguration() {
    // eden-kledd: the old groups are gone once the rows are cleared
    active_group = nullptr;
    inactive_group = nullptr;
    if (file == nullptr) {
        return;
    }

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto loader = Loader::GetLoader(system, file);

    FileSys::VirtualFile update_raw;
    loader->ReadUpdateRaw(update_raw);

    const auto& disabled = Settings::values.disabled_addons[title_id];

    update_items.clear();
    list_items.clear();
    item_model->removeRows(0, item_model->rowCount());

    // eden-kledd: Active / Inactive groups. Titles are set before the groups enter the model so
    // opening the dialog does not count as a change.
    active_group = MakeGroupItem(tr("Active"));
    inactive_group = MakeGroupItem(tr("Inactive"));

    std::vector<FileSys::Patch> patches = pm.GetPatches(update_raw);

    bool has_enabled_update = false;

    for (const auto& patch : patches) {
        const auto name = QString::fromStdString(patch.name);

        auto* const first_item = new QStandardItem;
        first_item->setText(name);
        first_item->setCheckable(true);

        const bool is_external_update = patch.type == FileSys::PatchType::Update &&
                                        patch.source == FileSys::PatchSource::External &&
                                        patch.numeric_version != 0;

        const bool is_mod = patch.type == FileSys::PatchType::Mod;

        if (is_external_update) {
            first_item->setData(static_cast<quint32>(patch.numeric_version), NUMERIC_VERSION);
        } else if (is_mod) {
            first_item->setData(QString::fromStdString(patch.location), PATCH_LOCATION);
        }

        bool patch_disabled = false;
        if (is_external_update) {
            std::string disabled_key = fmt::format("Update@{}", patch.numeric_version);
            patch_disabled =
                std::find(disabled.begin(), disabled.end(), disabled_key) != disabled.end();
        } else {
            patch_disabled =
                std::find(disabled.begin(), disabled.end(), name.toStdString()) != disabled.end();
        }

        bool should_enable = !patch_disabled;

        if (patch.type == FileSys::PatchType::Update) {
            if (should_enable) {
                if (has_enabled_update) {
                    should_enable = false;
                } else {
                    has_enabled_update = true;
                }
            }
            update_items.push_back(first_item);
        }

        first_item->setCheckState(should_enable ? Qt::Checked : Qt::Unchecked);

        list_items.push_back(QList<QStandardItem*>{
            first_item, new QStandardItem{QString::fromStdString(patch.version)}});
        if (is_mod && patch.version.find("Cheats") != std::string::npos) {
            AddCheatRows(first_item, QString::fromStdString(patch.location));
            list_items.back().back()->setToolTip(tr("Click to show the cheats in this mod"));
        }
        (should_enable ? active_group : inactive_group)->appendRow(list_items.back());
    }

    active_group->sortChildren(0);
    inactive_group->sortChildren(0);
    active_group->setText(QStringLiteral("%1 (%2)").arg(tr("Active")).arg(active_group->rowCount()));
    inactive_group->setText(
        QStringLiteral("%1 (%2)").arg(tr("Inactive")).arg(inactive_group->rowCount()));
    item_model->appendRow(QList<QStandardItem*>{active_group, MakeGroupItem(QString{})});
    item_model->appendRow(QList<QStandardItem*>{inactive_group, MakeGroupItem(QString{})});
    tree_view->setFirstColumnSpanned(0, QModelIndex(), true);
    tree_view->setFirstColumnSpanned(1, QModelIndex(), true);
    tree_view->expand(active_group->index());
    tree_view->expand(inactive_group->index());

    tree_view->resizeColumnToContents(1);
}
