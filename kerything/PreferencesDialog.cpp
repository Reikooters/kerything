// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "PreferencesDialog.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>
#include <QVBoxLayout>

#include "PreferencesDialogPage.h"
#include "BlockDeviceDisplayUtils.h"
#include "HoverRowHighlight.h"

namespace {
    constexpr int DeviceIdRole = Qt::UserRole + 1;
    constexpr int InitialEnabledRole = Qt::UserRole + 2;
    constexpr int ScanWhenUnmountedRole = Qt::UserRole + 3;
    constexpr int ShowOfflineResultsRole = Qt::UserRole + 4;

    constexpr int FilterIdRole = Qt::UserRole + 20;
}

PreferencesDialog::PreferencesDialog(
    Preferences& preferences,
    const std::vector<BlockDevice>& knownDevices,
    QWidget* parent
)
    : QDialog(parent),
      preferences_(preferences),
      knownDevices_(knownDevices)
{
    setWindowTitle(QStringLiteral("Configure Kerything"));
    resize(980, 620);

    for (const BlockDevice& blockDevice : knownDevices_) {
        if (!blockDevice.deviceId.isEmpty()) {
            knownDeviceById_.insert(blockDevice.deviceId, blockDevice);
        }
    }

    for (const IndexedDevicePreference& preference : preferences_.indexedDevicePreferences()) {
        if (!preference.deviceId.isEmpty()) {
            originalPreferencesByDeviceId_.insert(preference.deviceId, preference);
        }
    }

    originalSearchFilters_ = preferences_.searchFilters();

    auto* rootLayout = new QVBoxLayout(this);
    auto* contentLayout = new QHBoxLayout();

    navigation_ = new QListWidget(this);
    navigation_->setFixedWidth(180);
    navigation_->setSelectionMode(QAbstractItemView::SingleSelection);

    pages_ = new QStackedWidget(this);

    populateNavigation();

    contentLayout->addWidget(navigation_);
    contentLayout->addWidget(pages_, 1);

    rootLayout->addLayout(contentLayout, 1);

    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
        this
    );

    applyButton_ = buttonBox_->button(QDialogButtonBox::Apply);
    applyButton_->setEnabled(false);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, [this]() {
        applyChanges();
        accept();
    });

    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        applyChanges();
    });

    rootLayout->addWidget(buttonBox_);

    if (navigation_->count() > 0) {
        navigation_->setCurrentRow(0);
    }

    updateApplyButtonEnabled();
}

void PreferencesDialog::setCurrentPage(PreferencesDialogPage page)
{
    if (!navigation_ || !pages_) {
        return;
    }

    int row = 0;

    switch (page) {
        case PreferencesDialogPage::Devices:
            row = 0;
            break;
        case PreferencesDialogPage::Filters:
            row = 1;
            break;
        case PreferencesDialogPage::Advanced:
            row = 2;
            break;
    }

    if (row >= 0 && row < navigation_->count()) {
        navigation_->setCurrentRow(row);
    }
}

void PreferencesDialog::setKnownDevices(const std::vector<BlockDevice>& knownDevices)
{
    QString selectedDeviceId;

    struct CurrentDeviceState {
        bool enabled = false;
        bool scanWhenUnmounted = true;
        bool showOfflineResults = true;
    };

    QHash<QString, CurrentDeviceState> currentStateByDeviceId;

    if (deviceTable_) {
        const int currentRow = deviceTable_->currentRow();
        if (currentRow >= 0 && currentRow < deviceTable_->rowCount()) {
            if (const auto* item = deviceTable_->item(currentRow, DeviceEnabledColumn)) {
                selectedDeviceId = item->data(DeviceIdRole).toString();
            }
        }

        for (int row = 0; row < deviceTable_->rowCount(); ++row) {
            const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
            if (!item) {
                continue;
            }

            const QString deviceId = item->data(DeviceIdRole).toString();
            if (deviceId.isEmpty()) {
                continue;
            }

            currentStateByDeviceId.insert(deviceId, CurrentDeviceState{
                .enabled = item->checkState() == Qt::Checked,
                .scanWhenUnmounted = item->data(ScanWhenUnmountedRole).toBool(),
                .showOfflineResults = item->data(ShowOfflineResultsRole).toBool(),
            });
        }
    }

    knownDevices_ = knownDevices;

    knownDeviceById_.clear();
    for (const BlockDevice& blockDevice : knownDevices_) {
        if (!blockDevice.deviceId.isEmpty()) {
            knownDeviceById_.insert(blockDevice.deviceId, blockDevice);
        }
    }

    if (!deviceTable_) {
        updateApplyButtonEnabled();
        return;
    }

    populateDeviceTable();

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        const auto it = currentStateByDeviceId.constFind(deviceId);
        if (it == currentStateByDeviceId.constEnd()) {
            continue;
        }

        item->setCheckState(it->enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(ScanWhenUnmountedRole, it->scanWhenUnmounted);
        item->setData(ShowOfflineResultsRole, it->showOfflineResults);
    }

    int rowToSelect = -1;
    if (!selectedDeviceId.isEmpty()) {
        for (int row = 0; row < deviceTable_->rowCount(); ++row) {
            const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
            if (item && item->data(DeviceIdRole).toString() == selectedDeviceId) {
                rowToSelect = row;
                break;
            }
        }
    }

    if (rowToSelect < 0 && deviceTable_->rowCount() > 0) {
        rowToSelect = 0;
    }

    if (rowToSelect >= 0) {
        deviceTable_->setCurrentCell(rowToSelect, DeviceNameColumn);
    }

    updateApplyButtonEnabled();
}

void PreferencesDialog::populateNavigation()
{
    pages_->addWidget(createDevicesPage());
    navigation_->addItem(QStringLiteral("Devices"));

    pages_->addWidget(createFiltersPage());
    navigation_->addItem(QStringLiteral("Filters"));

    // pages_->addWidget(createIndexingPage());
    // navigation_->addItem(QStringLiteral("Indexing"));

    pages_->addWidget(createAdvancedPage());
    navigation_->addItem(QStringLiteral("Advanced"));

    connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
}

QWidget* PreferencesDialog::createDevicesPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(
        QStringLiteral(
            "<h2>Devices</h2>"
            "<p>Choose which devices Kerything indexes. Enabling a mounted device starts indexing immediately. Unmounted devices are indexed immediately only if “Scan this device even when it is not mounted” is enabled.</p>"
        ),
        page
    );
    title->setWordWrap(true);
    layout->addWidget(title);

    deviceTable_ = new QTableWidget(page);
    deviceTable_->setColumnCount(DeviceColumnCount);
    deviceTable_->setHorizontalHeaderLabels({
        QStringLiteral("Index"),
        QStringLiteral("Name"),
        QStringLiteral("Status"),
        QStringLiteral("Filesystem"),
        QStringLiteral("Mount point"),
        QStringLiteral("Device"),
        QStringLiteral("Model"),
    });

    deviceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    deviceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    deviceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    deviceTable_->setAlternatingRowColors(true);
    deviceTable_->setShowGrid(false);
    deviceTable_->setWordWrap(false);
    deviceTable_->verticalHeader()->setVisible(false);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceEnabledColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceNameColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceStatusColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceFsTypeColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceMountPointColumn, QHeaderView::Stretch);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceNodeColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceModelColumn, QHeaderView::ResizeToContents);
    installHoverRowHighlight(deviceTable_);

    populateDeviceTable();

    layout->addWidget(deviceTable_, 1);

    auto* optionsGroup = new QGroupBox(QStringLiteral("Selected device options"), page);
    auto* optionsLayout = new QVBoxLayout(optionsGroup);

    selectedDeviceLabel_ = new QLabel(QStringLiteral("No device selected."), optionsGroup);
    selectedDeviceLabel_->setWordWrap(true);

    scanWhenUnmountedCheckBox_ = new QCheckBox(
        QStringLiteral("Scan this device even when it is not mounted"),
        optionsGroup
    );

    showOfflineResultsCheckBox_ = new QCheckBox(
        QStringLiteral("Keep this device searchable when it is not mounted"),
        optionsGroup
    );

    optionsLayout->addWidget(selectedDeviceLabel_);
    optionsLayout->addWidget(scanWhenUnmountedCheckBox_);
    optionsLayout->addWidget(showOfflineResultsCheckBox_);

    layout->addWidget(optionsGroup);

    auto updateSelectedDeviceOptions = [this]() {
        if (!deviceTable_ || !scanWhenUnmountedCheckBox_ || !showOfflineResultsCheckBox_) {
            return;
        }

        const int row = deviceTable_->currentRow();
        const bool validRow = row >= 0 && row < deviceTable_->rowCount();
        auto* enabledItem = validRow ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        const QString deviceId = enabledItem ? enabledItem->data(DeviceIdRole).toString() : QString();

        const QSignalBlocker scanBlocker(scanWhenUnmountedCheckBox_);
        const QSignalBlocker offlineBlocker(showOfflineResultsCheckBox_);

        if (deviceId.isEmpty()) {
            selectedDeviceLabel_->setText(QStringLiteral("No device selected."));
            scanWhenUnmountedCheckBox_->setEnabled(false);
            showOfflineResultsCheckBox_->setEnabled(false);
            scanWhenUnmountedCheckBox_->setChecked(false);
            showOfflineResultsCheckBox_->setChecked(false);
            return;
        }

        QString deviceName = deviceTable_->item(row, DeviceNameColumn)->text().toHtmlEscaped();
        QString deviceMountPoint = deviceTable_->item(row, DeviceMountPointColumn)->text().toHtmlEscaped();

        if (deviceMountPoint != QStringLiteral("—")) {
            selectedDeviceLabel_->setText(QStringLiteral("Options for <b>%1</b><br>%2 | %3").arg(
                deviceName,
                deviceId.toHtmlEscaped(),
                deviceMountPoint
            ));
        }
        else {
            selectedDeviceLabel_->setText(QStringLiteral("Options for <b>%1</b><br>%2").arg(
                deviceName,
                deviceId.toHtmlEscaped()
            ));
        }

        scanWhenUnmountedCheckBox_->setEnabled(true);
        showOfflineResultsCheckBox_->setEnabled(true);
        scanWhenUnmountedCheckBox_->setChecked(scanWhenUnmountedForDevice(deviceId));
        showOfflineResultsCheckBox_->setChecked(showOfflineResultsForDevice(deviceId));
    };

    connect(deviceTable_, &QTableWidget::currentCellChanged, this, updateSelectedDeviceOptions);

    connect(deviceTable_, &QTableWidget::itemChanged, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(scanWhenUnmountedCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        if (item) {
            item->setData(ScanWhenUnmountedRole, checked);
        }

        updateApplyButtonEnabled();
    });

    connect(showOfflineResultsCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        if (item) {
            item->setData(ShowOfflineResultsRole, checked);
        }

        updateApplyButtonEnabled();
    });

    if (deviceTable_->rowCount() > 0) {
        deviceTable_->setCurrentCell(0, DeviceNameColumn);
    }

    updateSelectedDeviceOptions();

    return page;
}

QWidget* PreferencesDialog::createFiltersPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(
        QStringLiteral(
            "<h2>Filters</h2>"
            "<p>Manage the filter presets shown in the Filter menu. "
            "Filters are saved as query fragments and are combined with the current search text.</p>"
        ),
        page
    );
    title->setWordWrap(true);
    layout->addWidget(title);

    filterTable_ = new QTableWidget(page);
    filterTable_->setColumnCount(FilterColumnCount);
    filterTable_->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Query"),
    });

    filterTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    filterTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    filterTable_->setEditTriggers(
        QAbstractItemView::DoubleClicked |
        QAbstractItemView::EditKeyPressed |
        QAbstractItemView::SelectedClicked
    );
    filterTable_->setAlternatingRowColors(true);
    filterTable_->setShowGrid(false);
    filterTable_->setWordWrap(false);
    filterTable_->verticalHeader()->setVisible(false);
    filterTable_->horizontalHeader()->setSectionResizeMode(FilterNameColumn, QHeaderView::ResizeToContents);
    filterTable_->horizontalHeader()->setSectionResizeMode(FilterQueryColumn, QHeaderView::Stretch);
    installHoverRowHighlight(filterTable_);

    populateFilterTable();

    layout->addWidget(filterTable_, 1);

    auto* buttonLayout = new QHBoxLayout();

    addFilterButton_ = new QPushButton(QStringLiteral("Add"), page);
    duplicateFilterButton_ = new QPushButton(QStringLiteral("Duplicate"), page);
    removeFilterButton_ = new QPushButton(QStringLiteral("Remove"), page);
    restoreDefaultFiltersButton_ = new QPushButton(QStringLiteral("Restore Defaults"), page);

    buttonLayout->addWidget(addFilterButton_);
    buttonLayout->addWidget(duplicateFilterButton_);
    buttonLayout->addWidget(removeFilterButton_);
    buttonLayout->addStretch();
    buttonLayout->addWidget(restoreDefaultFiltersButton_);

    layout->addLayout(buttonLayout);

    connect(filterTable_, &QTableWidget::itemChanged, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(filterTable_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        const bool hasSelection = filterTable_ && !filterTable_->selectionModel()->selectedRows().isEmpty();

        if (duplicateFilterButton_) {
            duplicateFilterButton_->setEnabled(hasSelection);
        }

        if (removeFilterButton_) {
            removeFilterButton_->setEnabled(hasSelection);
        }
    });

    connect(addFilterButton_, &QPushButton::clicked, this, [this]() {
        const int row = filterTable_->rowCount();
        filterTable_->insertRow(row);

        auto* nameItem = new QTableWidgetItem(uniqueFilterName(QStringLiteral("New Filter")));
        nameItem->setData(FilterIdRole, newCustomFilterId());

        auto* queryItem = new QTableWidgetItem(QStringLiteral("ext:"));

        filterTable_->setItem(row, FilterNameColumn, nameItem);
        filterTable_->setItem(row, FilterQueryColumn, queryItem);
        filterTable_->setCurrentCell(row, FilterNameColumn);
        filterTable_->editItem(nameItem);

        updateApplyButtonEnabled();
    });

    connect(duplicateFilterButton_, &QPushButton::clicked, this, [this]() {
        const QModelIndexList selectedRows = filterTable_->selectionModel()->selectedRows();
        if (selectedRows.isEmpty()) {
            return;
        }

        const int sourceRow = selectedRows.first().row();
        const auto* sourceNameItem = filterTable_->item(sourceRow, FilterNameColumn);
        const auto* sourceQueryItem = filterTable_->item(sourceRow, FilterQueryColumn);

        if (!sourceNameItem || !sourceQueryItem) {
            return;
        }

        const int row = filterTable_->rowCount();
        filterTable_->insertRow(row);

        auto* nameItem = new QTableWidgetItem(uniqueFilterName(sourceNameItem->text() + QStringLiteral(" Copy")));
        nameItem->setData(FilterIdRole, newCustomFilterId());

        auto* queryItem = new QTableWidgetItem(sourceQueryItem->text());

        filterTable_->setItem(row, FilterNameColumn, nameItem);
        filterTable_->setItem(row, FilterQueryColumn, queryItem);
        filterTable_->setCurrentCell(row, FilterNameColumn);
        filterTable_->editItem(nameItem);

        updateApplyButtonEnabled();
    });

    connect(removeFilterButton_, &QPushButton::clicked, this, [this]() {
        QModelIndexList selectedRows = filterTable_->selectionModel()->selectedRows();
        if (selectedRows.isEmpty()) {
            return;
        }

        std::ranges::sort(selectedRows, [](const QModelIndex& lhs, const QModelIndex& rhs) {
            return lhs.row() > rhs.row();
        });

        const QString message = selectedRows.size() == 1
            ? QStringLiteral("Remove the selected filter?")
            : QStringLiteral("Remove %1 selected filters?").arg(selectedRows.size());

        QMessageBox confirmBox(this);
        confirmBox.setIcon(QMessageBox::Question);
        confirmBox.setWindowTitle(QStringLiteral("Remove Filters?"));
        confirmBox.setText(message);
        confirmBox.setStandardButtons(QMessageBox::Discard | QMessageBox::Cancel);
        confirmBox.setDefaultButton(QMessageBox::Cancel);
        confirmBox.button(QMessageBox::Discard)->setText(QStringLiteral("Remove"));

        if (confirmBox.exec() != QMessageBox::Discard) {
            return;
        }

        for (const QModelIndex& index : selectedRows) {
            filterTable_->removeRow(index.row());
        }

        updateApplyButtonEnabled();
    });

    connect(restoreDefaultFiltersButton_, &QPushButton::clicked, this, [this]() {
        const QMessageBox::StandardButton result = QMessageBox::question(
            this,
            QStringLiteral("Restore Default Filters?"),
            QStringLiteral(
                "This will restore the default filters and keep any custom filters you created.\n\n"
                "Existing default filters will be reset to their original names and queries."
            ),
            QMessageBox::RestoreDefaults | QMessageBox::Cancel,
            QMessageBox::Cancel
        );

        if (result != QMessageBox::RestoreDefaults) {
            return;
        }

        preferences_.restoreDefaultSearchFilters();
        originalSearchFilters_ = preferences_.searchFilters();
        populateFilterTable();
        updateApplyButtonEnabled();

        Q_EMIT searchFiltersApplied();
    });

    const bool hasSelection = !filterTable_->selectionModel()->selectedRows().isEmpty();
    duplicateFilterButton_->setEnabled(hasSelection);
    removeFilterButton_->setEnabled(hasSelection);

    return page;
}

QWidget* PreferencesDialog::createIndexingPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* label = new QLabel(
        QStringLiteral(
            "<h2>Indexing</h2>"
            "<p>Indexing-wide preferences can go here later, such as refresh intervals, ignored paths, or filesystem filters.</p>"
        ),
        page
    );
    label->setWordWrap(true);

    layout->addWidget(label);
    layout->addStretch();

    return page;
}

QWidget* PreferencesDialog::createAdvancedPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* label = new QLabel(
        QStringLiteral(
            "<h2>Advanced</h2>"
            "<p>Advanced maintenance actions for Kerything.</p>"
        ),
        page
    );
    label->setWordWrap(true);

    layout->addWidget(label);

    auto* firstRunGroup = new QGroupBox(QStringLiteral("First-run setup"), page);
    auto* firstRunLayout = new QVBoxLayout(firstRunGroup);

    auto* firstRunDescription = new QLabel(
        QStringLiteral(
            "Resetting first-run setup will make Kerything show the initial device selection "
            "setup again the next time it starts. Your saved device preferences are not deleted."
        ),
        firstRunGroup
    );
    firstRunDescription->setWordWrap(true);

    auto* resetFirstRunButton = new QPushButton(QStringLiteral("Reset first-run setup"), firstRunGroup);

    firstRunLayout->addWidget(firstRunDescription);
    firstRunLayout->addWidget(resetFirstRunButton, 0, Qt::AlignLeft);

    layout->addWidget(firstRunGroup);
    layout->addStretch();

    connect(resetFirstRunButton, &QPushButton::clicked, this, [this]() {
        const QMessageBox::StandardButton result = QMessageBox::question(
            this,
            QStringLiteral("Reset first-run setup?"),
            QStringLiteral(
                "Kerything will show the initial device selection setup again the next time it starts.\n\n"
                "This will not delete your saved device preferences.\n\n"
                "Do you want to continue?"
            ),
            QMessageBox::Reset | QMessageBox::Cancel,
            QMessageBox::Cancel
        );

        if (result != QMessageBox::Reset) {
            return;
        }

        preferences_.setInitialDeviceSelectionCompleted(false);

        QMessageBox::information(
            this,
            QStringLiteral("First-run setup reset"),
            QStringLiteral("First-run setup has been reset and will appear again the next time Kerything starts.")
        );
    });

    return page;
}

void PreferencesDialog::populateDeviceTable()
{
    const QSignalBlocker blocker(deviceTable_);

    std::vector<BlockDevice> sortedDevices = knownDevices_;
    std::sort(
        sortedDevices.begin(),
        sortedDevices.end(),
        BlockDeviceDisplayUtils::deviceLessThan
    );

    deviceTable_->clearContents();
    deviceTable_->setRowCount(static_cast<int>(sortedDevices.size()));

    const QPalette palette = deviceTable_->palette();
    const QBrush unmountedForeground = palette.brush(QPalette::PlaceholderText);

    int row = 0;
    for (const BlockDevice& blockDevice : sortedDevices) {
        const IndexedDevicePreference preference =
            preferences_.indexedDevicePreference(blockDevice.deviceId).value_or(IndexedDevicePreference{
                .deviceId = blockDevice.deviceId
            });

        auto* enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enabledItem->setCheckState(preference.enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setData(DeviceIdRole, blockDevice.deviceId);
        enabledItem->setData(InitialEnabledRole, preference.enabled);
        enabledItem->setData(ScanWhenUnmountedRole, preference.scanWhenUnmounted);
        enabledItem->setData(ShowOfflineResultsRole, preference.showOfflineResults);
        deviceTable_->setItem(row, DeviceEnabledColumn, enabledItem);

        auto* nameItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayNameForBlockDevice(blockDevice));
        nameItem->setToolTip(blockDevice.deviceId);
        deviceTable_->setItem(row, DeviceNameColumn, nameItem);

        auto* statusItem = new QTableWidgetItem(blockDevice.mounted ? QStringLiteral("Mounted") : QStringLiteral("Not mounted"));
        deviceTable_->setItem(row, DeviceStatusColumn, statusItem);

        auto* fsTypeItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.fsType));
        deviceTable_->setItem(row, DeviceFsTypeColumn, fsTypeItem);

        const QString mountPointText = blockDevice.primaryMountPoint.trimmed().isEmpty()
            ? QStringLiteral("—")
            : blockDevice.primaryMountPoint.trimmed();

        auto* mountPointItem = new QTableWidgetItem(mountPointText);
        mountPointItem->setToolTip(
            blockDevice.mountPoints.isEmpty()
                ? QStringLiteral("This device is not currently mounted.")
                : blockDevice.mountPoints.join(QStringLiteral("\n"))
        );
        deviceTable_->setItem(row, DeviceMountPointColumn, mountPointItem);

        auto* devNodeItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.devNode));
        devNodeItem->setToolTip(blockDevice.deviceId);
        deviceTable_->setItem(row, DeviceNodeColumn, devNodeItem);

        auto* modelItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.diskModel));
        deviceTable_->setItem(row, DeviceModelColumn, modelItem);

        if (!blockDevice.mounted) {
            for (int column = 0; column < DeviceColumnCount; ++column) {
                if (auto* item = deviceTable_->item(row, column)) {
                    item->setForeground(unmountedForeground);
                }
            }
        }

        ++row;
    }
}

void PreferencesDialog::populateFilterTable()
{
    if (!filterTable_) {
        return;
    }

    const QSignalBlocker blocker(filterTable_);
    const std::vector<SearchFilterPreference> filters = preferences_.searchFilters();

    filterTable_->clearContents();
    filterTable_->setRowCount(static_cast<int>(filters.size()));

    int row = 0;
    for (const SearchFilterPreference& filter : filters) {
        auto* nameItem = new QTableWidgetItem(filter.name);
        nameItem->setData(FilterIdRole, filter.id);

        auto* queryItem = new QTableWidgetItem(filter.query);
        queryItem->setToolTip(filter.query);

        filterTable_->setItem(row, FilterNameColumn, nameItem);
        filterTable_->setItem(row, FilterQueryColumn, queryItem);

        ++row;
    }

    if (filterTable_->rowCount() > 0) {
        filterTable_->setCurrentCell(0, FilterNameColumn);
    }
}

QStringList PreferencesDialog::enabledDeviceIdsFromTable() const
{
    QStringList out;

    if (!deviceTable_) {
        return out;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item || item->checkState() != Qt::Checked) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        if (!deviceId.isEmpty()) {
            out << deviceId;
        }
    }

    return out;
}

bool PreferencesDialog::scanWhenUnmountedForDevice(const QString& deviceId) const
{
    if (!deviceTable_) {
        return true;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (item && item->data(DeviceIdRole).toString() == deviceId) {
            return item->data(ScanWhenUnmountedRole).toBool();
        }
    }

    return true;
}

bool PreferencesDialog::showOfflineResultsForDevice(const QString& deviceId) const
{
    if (!deviceTable_) {
        return true;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (item && item->data(DeviceIdRole).toString() == deviceId) {
            return item->data(ShowOfflineResultsRole).toBool();
        }
    }

    return true;
}

std::vector<SearchFilterPreference> PreferencesDialog::filtersFromTable() const
{
    std::vector<SearchFilterPreference> filters;

    if (!filterTable_) {
        return filters;
    }

    filters.reserve(static_cast<std::size_t>(filterTable_->rowCount()));

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        const auto* nameItem = filterTable_->item(row, FilterNameColumn);
        const auto* queryItem = filterTable_->item(row, FilterQueryColumn);

        if (!nameItem || !queryItem) {
            continue;
        }

        SearchFilterPreference filter;
        filter.id = nameItem->data(FilterIdRole).toString().trimmed();

        if (filter.id.isEmpty()) {
            filter.id = QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        filter.name = nameItem->text().trimmed();
        filter.query = queryItem->text().trimmed();

        filters.push_back(std::move(filter));
    }

    return filters;
}

QString PreferencesDialog::uniqueFilterName(const QString& baseName) const
{
    QStringList existingNames;

    if (filterTable_) {
        for (int row = 0; row < filterTable_->rowCount(); ++row) {
            if (const auto* item = filterTable_->item(row, FilterNameColumn)) {
                existingNames << item->text().trimmed().toCaseFolded();
            }
        }
    }

    QString candidate = baseName.trimmed().isEmpty()
        ? QStringLiteral("New Filter")
        : baseName.trimmed();

    if (!existingNames.contains(candidate.toCaseFolded())) {
        return candidate;
    }

    int suffix = 2;
    while (true) {
        const QString numbered = QStringLiteral("%1 %2").arg(candidate).arg(suffix);
        if (!existingNames.contains(numbered.toCaseFolded())) {
            return numbered;
        }

        ++suffix;
    }
}

QString PreferencesDialog::newCustomFilterId() const
{
    return QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool PreferencesDialog::validateFilters(QString* errorText) const
{
    if (!filterTable_) {
        return true;
    }

    QSet<QString> names;

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        const auto* nameItem = filterTable_->item(row, FilterNameColumn);
        const auto* queryItem = filterTable_->item(row, FilterQueryColumn);

        const QString name = nameItem ? nameItem->text().trimmed() : QString();
        const QString query = queryItem ? queryItem->text().trimmed() : QString();

        if (name.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("Filter names cannot be empty.");
            }

            return false;
        }

        if (query.isEmpty()) {
            if (errorText) {
                *errorText = QStringLiteral("Filter queries cannot be empty.");
            }

            return false;
        }

        const QString foldedName = name.toCaseFolded();
        if (names.contains(foldedName)) {
            if (errorText) {
                *errorText = QStringLiteral("Filter names must be unique.");
            }

            return false;
        }

        names.insert(foldedName);
    }

    return true;
}

bool PreferencesDialog::hasChanges() const
{
    return hasDeviceChanges() || hasFilterChanges();
}

bool PreferencesDialog::hasDeviceChanges() const
{
    if (!deviceTable_) {
        return false;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        const bool enabled = item->checkState() == Qt::Checked;
        const bool originalEnabled = item->data(InitialEnabledRole).toBool();

        if (enabled != originalEnabled) {
            return true;
        }

        const IndexedDevicePreference original =
            originalPreferencesByDeviceId_.value(deviceId, IndexedDevicePreference{ .deviceId = deviceId });

        if (item->data(ScanWhenUnmountedRole).toBool() != original.scanWhenUnmounted) {
            return true;
        }

        if (item->data(ShowOfflineResultsRole).toBool() != original.showOfflineResults) {
            return true;
        }
    }

    return false;
}

bool PreferencesDialog::hasFilterChanges() const
{
    const std::vector<SearchFilterPreference> current = filtersFromTable();

    if (current.size() != originalSearchFilters_.size()) {
        return true;
    }

    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current[i].id != originalSearchFilters_[i].id ||
            current[i].name != originalSearchFilters_[i].name ||
            current[i].query != originalSearchFilters_[i].query) {
            return true;
        }
    }

    return false;
}

void PreferencesDialog::updateApplyButtonEnabled()
{
    if (applyButton_) {
        applyButton_->setEnabled(hasChanges());
    }
}

void PreferencesDialog::applyChanges()
{
    QString filterError;
    if (!validateFilters(&filterError)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Invalid Filters"),
            filterError
        );
        return;
    }

    const bool filtersChanged = hasFilterChanges();

    if (filtersChanged) {
        preferences_.saveSearchFilters(filtersFromTable());
        originalSearchFilters_ = preferences_.searchFilters();
        populateFilterTable();
    }

    if (!deviceTable_) {
        if (filtersChanged) {
            Q_EMIT searchFiltersApplied();
        }

        updateApplyButtonEnabled();
        return;
    }

    QList<DevicePreferenceChange> changes;

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        if (deviceId.isEmpty()) {
            continue;
        }

        const auto blockDeviceIt = knownDeviceById_.constFind(deviceId);
        if (blockDeviceIt == knownDeviceById_.constEnd()) {
            continue;
        }

        const bool enabled = item->checkState() == Qt::Checked;

        const IndexedDevicePreference original =
            originalPreferencesByDeviceId_.value(deviceId, IndexedDevicePreference{ .deviceId = deviceId });

        const bool originallyEnabled = original.enabled;
        const bool scanWhenUnmounted = item->data(ScanWhenUnmountedRole).toBool();
        const bool showOfflineResults = item->data(ShowOfflineResultsRole).toBool();

        const BlockDevice& blockDevice = blockDeviceIt.value();

        IndexedDevicePreference preference =
            preferences_.indexedDevicePreference(deviceId).value_or(IndexedDevicePreference{ .deviceId = deviceId });

        preference.deviceId = blockDevice.deviceId;
        preference.enabled = enabled;
        preference.displayName = BlockDeviceDisplayUtils::displayNameForBlockDevice(blockDevice);
        preference.fsType = blockDevice.fsType;
        preference.uuid = blockDevice.uuid;
        preference.partuuid = blockDevice.partuuid;
        preference.lastKnownDevNode = blockDevice.devNode;
        preference.lastKnownPrimaryMountPoint = blockDevice.primaryMountPoint;
        preference.lastKnownMountPoints = blockDevice.mountPoints;
        preference.lastSeenAt = QDateTime::currentDateTimeUtc();
        preference.scanWhenUnmounted = scanWhenUnmounted;
        preference.showOfflineResults = showOfflineResults;

        preferences_.saveIndexedDevicePreference(preference);

        if (enabled != original.enabled ||
            scanWhenUnmounted != original.scanWhenUnmounted ||
            showOfflineResults != original.showOfflineResults) {
            changes.append(DevicePreferenceChange{
                .deviceId = deviceId,
                .wasEnabled = originallyEnabled,
                .enabled = enabled,
                .wasScanWhenUnmounted = original.scanWhenUnmounted,
                .scanWhenUnmounted = scanWhenUnmounted,
                .wasShowOfflineResults = original.showOfflineResults,
                .showOfflineResults = showOfflineResults,
            });
        }

        item->setData(InitialEnabledRole, enabled);
    }

    originalPreferencesByDeviceId_.clear();
    for (const IndexedDevicePreference& preference : preferences_.indexedDevicePreferences()) {
        if (!preference.deviceId.isEmpty()) {
            originalPreferencesByDeviceId_.insert(preference.deviceId, preference);
        }
    }

    updateApplyButtonEnabled();

    if (filtersChanged) {
        Q_EMIT searchFiltersApplied();
    }

    if (!changes.isEmpty()) {
        Q_EMIT preferencesApplied(changes);
    }
}