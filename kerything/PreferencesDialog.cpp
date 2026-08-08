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
#include <QKeyEvent>
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
    constexpr int LiveUpdatesEnabledRole = Qt::UserRole + 5;

    constexpr int FilterIdRole = Qt::UserRole + 20;

    bool moveTableSelectionToEdge(QTableWidget* table, QKeyEvent* keyEvent)
    {
        if (!table || table->rowCount() <= 0 || keyEvent->modifiers() != Qt::NoModifier) {
            return false;
        }

        if (keyEvent->key() != Qt::Key_Home && keyEvent->key() != Qt::Key_End) {
            return false;
        }

        const int row = keyEvent->key() == Qt::Key_Home
            ? 0
            : table->rowCount() - 1;

        const int column = table->currentColumn() >= 0
            ? table->currentColumn()
            : 0;

        table->setCurrentCell(row, column);
        table->scrollToItem(table->item(row, column), QAbstractItemView::PositionAtCenter);
        return true;
    }
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

bool PreferencesDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = dynamic_cast<QKeyEvent*>(event);

        if (watched == deviceTable_) {
            if (keyEvent->key() == Qt::Key_Space && keyEvent->modifiers() == Qt::NoModifier) {
                toggleDeviceRowChecked(deviceTable_->currentRow());
                return true;
            }

            if (moveTableSelectionToEdge(deviceTable_, keyEvent)) {
                return true;
            }
        }
        else if (watched == filterTable_) {
            if (moveTableSelectionToEdge(filterTable_, keyEvent)) {
                return true;
            }
        }
    }

    return QDialog::eventFilter(watched, event);
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

void PreferencesDialog::setAutoRefreshResultsForLiveUpdates(bool enabled)
{
    if (!autoRefreshLiveUpdatesCheckBox_) {
        return;
    }

    const QSignalBlocker blocker(autoRefreshLiveUpdatesCheckBox_);
    autoRefreshLiveUpdatesCheckBox_->setChecked(enabled);
    updateApplyButtonEnabled();
}

void PreferencesDialog::setKnownDevices(const std::vector<BlockDevice>& knownDevices)
{
    QString selectedDeviceId;

    struct CurrentDeviceState {
        bool enabled = false;
        bool scanWhenUnmounted = true;
        bool showOfflineResults = true;
        bool liveUpdatesEnabled = true;
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
                .liveUpdatesEnabled = item->data(LiveUpdatesEnabledRole).toBool(),
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
        item->setData(LiveUpdatesEnabledRole, it->liveUpdatesEnabled);
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
    deviceTable_->installEventFilter(this);

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

    liveUpdatesEnabledCheckBox_ = new QCheckBox(
        QStringLiteral("Watch this device for live updates when mounted"),
        optionsGroup
    );

    optionsLayout->addWidget(selectedDeviceLabel_);
    optionsLayout->addWidget(scanWhenUnmountedCheckBox_);
    optionsLayout->addWidget(showOfflineResultsCheckBox_);
    optionsLayout->addWidget(liveUpdatesEnabledCheckBox_);

    layout->addWidget(optionsGroup);

    auto updateSelectedDeviceOptions = [this]() {
        if (!deviceTable_ ||
            !scanWhenUnmountedCheckBox_ ||
            !showOfflineResultsCheckBox_ ||
            !liveUpdatesEnabledCheckBox_) {
            return;
        }

        const int row = deviceTable_->currentRow();
        const bool validRow = row >= 0 && row < deviceTable_->rowCount();
        auto* enabledItem = validRow ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        const QString deviceId = enabledItem ? enabledItem->data(DeviceIdRole).toString() : QString();

        const QSignalBlocker scanBlocker(scanWhenUnmountedCheckBox_);
        const QSignalBlocker offlineBlocker(showOfflineResultsCheckBox_);
        const QSignalBlocker liveUpdatesBlocker(liveUpdatesEnabledCheckBox_);

        if (deviceId.isEmpty()) {
            selectedDeviceLabel_->setText(QStringLiteral("No device selected."));
            scanWhenUnmountedCheckBox_->setEnabled(false);
            showOfflineResultsCheckBox_->setEnabled(false);
            liveUpdatesEnabledCheckBox_->setEnabled(false);
            scanWhenUnmountedCheckBox_->setChecked(false);
            showOfflineResultsCheckBox_->setChecked(false);
            liveUpdatesEnabledCheckBox_->setChecked(false);
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

        const bool liveUpdatesSupported = liveUpdatesSupportedForDevice(deviceId);
        liveUpdatesEnabledCheckBox_->setEnabled(liveUpdatesSupported);

        scanWhenUnmountedCheckBox_->setChecked(scanWhenUnmountedForDevice(deviceId));
        showOfflineResultsCheckBox_->setChecked(showOfflineResultsForDevice(deviceId));
        liveUpdatesEnabledCheckBox_->setChecked(
            liveUpdatesSupported && liveUpdatesEnabledForDevice(deviceId)
        );

        if (liveUpdatesSupported) {
            liveUpdatesEnabledCheckBox_->setText(
                QStringLiteral("Watch this device for live updates when mounted")
            );
            liveUpdatesEnabledCheckBox_->setToolTip(
                QStringLiteral("Kerything will keep this EXT4 device up to date using fanotify while it is mounted.")
            );
        } else {
            liveUpdatesEnabledCheckBox_->setText(
                QStringLiteral("Watch this device for live updates when mounted (EXT4 only)")
            );
            liveUpdatesEnabledCheckBox_->setToolTip(
                QStringLiteral("Live updates currently require EXT4. This device uses a different filesystem.")
            );
        }
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

    connect(liveUpdatesEnabledCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        if (item) {
            item->setData(LiveUpdatesEnabledRole, checked);
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
            "<p>Create and manage the filter presets shown in the Filter menu. "
            "Filters are saved search shortcuts that are added to your current search text. "
            "Double-click a filter name or query to change it.</p>"
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
    filterTable_->installEventFilter(this);

    populateFilterTable();

    layout->addWidget(filterTable_, 1);

    auto* buttonLayout = new QHBoxLayout();

    addFilterButton_ = new QPushButton(QStringLiteral("Add"), page);
    duplicateFilterButton_ = new QPushButton(QStringLiteral("Duplicate"), page);
    removeFilterButton_ = new QPushButton(QStringLiteral("Remove"), page);
    moveFilterUpButton_ = new QPushButton(QStringLiteral("Move Up"), page);
    moveFilterDownButton_ = new QPushButton(QStringLiteral("Move Down"), page);
    restoreDefaultFiltersButton_ = new QPushButton(QStringLiteral("Restore Defaults"), page);

    duplicateFilterButton_->setEnabled(false);
    removeFilterButton_->setEnabled(false);
    moveFilterUpButton_->setEnabled(false);
    moveFilterDownButton_->setEnabled(false);

    buttonLayout->addWidget(addFilterButton_);
    buttonLayout->addWidget(duplicateFilterButton_);
    buttonLayout->addWidget(removeFilterButton_);
    buttonLayout->addWidget(moveFilterUpButton_);
    buttonLayout->addWidget(moveFilterDownButton_);
    buttonLayout->addStretch();
    buttonLayout->addWidget(restoreDefaultFiltersButton_);

    layout->addLayout(buttonLayout);

    connect(filterTable_, &QTableWidget::itemChanged, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(filterTable_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        updateFilterButtonStates();
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

        updateFilterButtonStates();
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

        updateFilterButtonStates();
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

        updateFilterButtonStates();
        updateApplyButtonEnabled();
    });

    connect(moveFilterUpButton_, &QPushButton::clicked, this, [this]() {
        moveSelectedFilters(-1);
    });

    connect(moveFilterDownButton_, &QPushButton::clicked, this, [this]() {
        moveSelectedFilters(1);
    });

    connect(restoreDefaultFiltersButton_, &QPushButton::clicked, this, [this]() {
        QMessageBox messageBox(this);
        messageBox.setIcon(QMessageBox::Question);
        messageBox.setWindowTitle(QStringLiteral("Restore Default Filters?"));
        messageBox.setText(QStringLiteral(
            "This will restore the default filters and keep any custom filters you created.\n\n"
            "Existing default filters will be reset to their original names and queries.\n\n"
            "The change will not be saved until you click Apply or OK."
        ));

        QPushButton* restoreButton = messageBox.addButton(
            QStringLiteral("Restore Defaults"),
            QMessageBox::AcceptRole
        );
        messageBox.addButton(QMessageBox::Cancel);
        messageBox.setDefaultButton(QMessageBox::Cancel);
        messageBox.setEscapeButton(QMessageBox::Cancel);

        messageBox.exec();

        if (messageBox.clickedButton() != restoreButton) {
            return;
        }

        std::vector<SearchFilterPreference> filters = filtersFromTable();
        const std::vector<SearchFilterPreference> defaults = Preferences::defaultSearchFilters();

        for (const SearchFilterPreference& defaultFilter : defaults) {
            auto existing = std::ranges::find_if(
                filters,
                [&](const SearchFilterPreference& filter) {
                    return filter.id == defaultFilter.id;
                }
            );

            if (existing == filters.end()) {
                filters.push_back(defaultFilter);
            } else {
                *existing = defaultFilter;
            }
        }

        populateFilterTable(filters);
        updateApplyButtonEnabled();
        updateFilterButtonStates();
    });

    updateFilterButtonStates();

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
            "<p>Advanced maintenance actions and application-wide preferences for Kerything.</p>"
        ),
        page
    );
    label->setWordWrap(true);

    layout->addWidget(label);

    auto* liveUpdatesGroup = new QGroupBox(QStringLiteral("Live updates"), page);
    auto* liveUpdatesLayout = new QVBoxLayout(liveUpdatesGroup);

    autoRefreshLiveUpdatesCheckBox_ = new QCheckBox(
        QStringLiteral("Automatically refresh and re-sort visible search results as live updates arrive"),
        liveUpdatesGroup
    );
    autoRefreshLiveUpdatesCheckBox_->setChecked(preferences_.autoRefreshResultsForLiveUpdates());
    autoRefreshLiveUpdatesCheckBox_->setToolTip(
        QStringLiteral(
            "When disabled, Kerything still keeps the internal index up to date, "
            "but visible search results are not automatically refreshed or re-sorted until you search again or re-enable this option."
        )
    );

    auto* liveUpdatesDescription = new QLabel(
        QStringLiteral(
            "Disable this if many filesystem changes are occurring and you want the visible results list to stay still while you browse it. "
            "The internal index will continue receiving live updates."
        ),
        liveUpdatesGroup
    );
    liveUpdatesDescription->setWordWrap(true);

    liveUpdatesLayout->addWidget(autoRefreshLiveUpdatesCheckBox_);
    liveUpdatesLayout->addWidget(liveUpdatesDescription);

    layout->addWidget(liveUpdatesGroup);

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

    connect(autoRefreshLiveUpdatesCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
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
        enabledItem->setData(LiveUpdatesEnabledRole, preference.liveUpdatesEnabled);
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
    populateFilterTable(preferences_.searchFilters());
}

void PreferencesDialog::populateFilterTable(const std::vector<SearchFilterPreference>& filters)
{
    if (!filterTable_) {
        return;
    }

    const QSignalBlocker blocker(filterTable_);

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

    updateFilterButtonStates();
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

bool PreferencesDialog::liveUpdatesEnabledForDevice(const QString& deviceId) const
{
    if (!deviceTable_) {
        return true;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (item && item->data(DeviceIdRole).toString() == deviceId) {
            return item->data(LiveUpdatesEnabledRole).toBool();
        }
    }

    return true;
}

bool PreferencesDialog::liveUpdatesSupportedForDevice(const QString& deviceId) const
{
    const auto knownDeviceIt = knownDeviceById_.constFind(deviceId);
    if (knownDeviceIt != knownDeviceById_.constEnd()) {
        return knownDeviceIt->fsType == QStringLiteral("ext4");
    }

    const IndexedDevicePreference preference =
        originalPreferencesByDeviceId_.value(
            deviceId,
            IndexedDevicePreference{ .deviceId = deviceId }
        );

    return preference.fsType == QStringLiteral("ext4");
}

void PreferencesDialog::toggleDeviceRowChecked(int row)
{
    if (!deviceTable_ || row < 0 || row >= deviceTable_->rowCount()) {
        return;
    }

    auto* item = deviceTable_->item(row, DeviceEnabledColumn);
    if (!item) {
        return;
    }

    item->setCheckState(
        item->checkState() == Qt::Checked
            ? Qt::Unchecked
            : Qt::Checked
    );

    updateApplyButtonEnabled();
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

QList<int> PreferencesDialog::selectedFilterRows() const
{
    QList<int> rows;

    if (!filterTable_ || !filterTable_->selectionModel()) {
        return rows;
    }

    const QModelIndexList selectedRows = filterTable_->selectionModel()->selectedRows();
    rows.reserve(selectedRows.size());

    for (const QModelIndex& index : selectedRows) {
        rows << index.row();
    }

    std::ranges::sort(rows);
    return rows;
}

void PreferencesDialog::updateFilterButtonStates()
{
    const QList<int> rows = selectedFilterRows();
    const bool hasSelection = !rows.isEmpty();
    const int rowCount = filterTable_ ? filterTable_->rowCount() : 0;
    const bool canReorder = hasSelection && rowCount > 1;

    if (duplicateFilterButton_) {
        duplicateFilterButton_->setEnabled(hasSelection);
    }

    if (removeFilterButton_) {
        removeFilterButton_->setEnabled(hasSelection);
    }

    if (moveFilterUpButton_) {
        moveFilterUpButton_->setEnabled(canReorder && rows.first() > 0);
    }

    if (moveFilterDownButton_) {
        moveFilterDownButton_->setEnabled(canReorder && rows.last() < rowCount - 1);
    }
}

void PreferencesDialog::moveSelectedFilters(int direction)
{
    if (!filterTable_ || direction == 0) {
        return;
    }

    QList<int> rows = selectedFilterRows();
    if (rows.isEmpty()) {
        return;
    }

    const int rowCount = filterTable_->rowCount();

    if (direction < 0 && rows.first() <= 0) {
        return;
    }

    if (direction > 0 && rows.last() >= rowCount - 1) {
        return;
    }

    filterTable_->clearSelection();

    if (direction > 0) {
        std::ranges::reverse(rows);
    }

    QList<int> movedRows;
    movedRows.reserve(rows.size());

    for (const int row : rows) {
        const int destinationRow = row + direction;

        QList<QTableWidgetItem*> movedItems;
        movedItems.reserve(FilterColumnCount);

        for (int column = 0; column < FilterColumnCount; ++column) {
            movedItems << filterTable_->takeItem(row, column);
        }

        filterTable_->removeRow(row);
        filterTable_->insertRow(destinationRow);

        for (int column = 0; column < FilterColumnCount; ++column) {
            filterTable_->setItem(destinationRow, column, movedItems.at(column));
        }

        movedRows << destinationRow;
    }

    std::ranges::sort(movedRows);

    if (auto* selectionModel = filterTable_->selectionModel()) {
        const int currentRow = direction < 0 ? movedRows.first() : movedRows.last();
        const QModelIndex currentIndex = filterTable_->model()->index(currentRow, FilterNameColumn);

        selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);

        for (const int row : movedRows) {
            const QModelIndex left = filterTable_->model()->index(row, 0);
            const QModelIndex right = filterTable_->model()->index(row, FilterColumnCount - 1);

            selectionModel->select(
                QItemSelection(left, right),
                QItemSelectionModel::Select | QItemSelectionModel::Rows
            );
        }

        filterTable_->scrollTo(currentIndex, QAbstractItemView::EnsureVisible);
    }

    updateFilterButtonStates();
    updateApplyButtonEnabled();
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
    return hasDeviceChanges() || hasFilterChanges() || hasGeneralChanges();
}

bool PreferencesDialog::hasGeneralChanges() const
{
    if (!autoRefreshLiveUpdatesCheckBox_) {
        return false;
    }

    return autoRefreshLiveUpdatesCheckBox_->isChecked() !=
           preferences_.autoRefreshResultsForLiveUpdates();
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

        const bool liveUpdatesSupported = liveUpdatesSupportedForDevice(deviceId);
        const bool liveUpdatesEnabled =
            liveUpdatesSupported && item->data(LiveUpdatesEnabledRole).toBool();

        if (liveUpdatesEnabled != original.liveUpdatesEnabled) {
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

    const bool autoRefreshChanged = hasGeneralChanges();
    const bool autoRefreshEnabled =
        autoRefreshLiveUpdatesCheckBox_
            ? autoRefreshLiveUpdatesCheckBox_->isChecked()
            : preferences_.autoRefreshResultsForLiveUpdates();

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

        if (autoRefreshChanged) {
            Q_EMIT autoRefreshResultsForLiveUpdatesApplied(autoRefreshEnabled);
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
        const bool liveUpdatesSupported = liveUpdatesSupportedForDevice(deviceId);
        const bool liveUpdatesEnabled =
            liveUpdatesSupported && item->data(LiveUpdatesEnabledRole).toBool();

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
        preference.liveUpdatesEnabled = liveUpdatesEnabled;

        preferences_.saveIndexedDevicePreference(preference);

        if (enabled != original.enabled ||
            scanWhenUnmounted != original.scanWhenUnmounted ||
            showOfflineResults != original.showOfflineResults ||
            liveUpdatesEnabled != original.liveUpdatesEnabled) {
            changes.append(DevicePreferenceChange{
                .deviceId = deviceId,
                .wasEnabled = originallyEnabled,
                .enabled = enabled,
                .wasScanWhenUnmounted = original.scanWhenUnmounted,
                .scanWhenUnmounted = scanWhenUnmounted,
                .wasShowOfflineResults = original.showOfflineResults,
                .showOfflineResults = showOfflineResults,
                .wasLiveUpdatesEnabled = original.liveUpdatesEnabled,
                .liveUpdatesEnabled = liveUpdatesEnabled,
            });
            }

        item->setData(InitialEnabledRole, enabled);
        item->setData(LiveUpdatesEnabledRole, liveUpdatesEnabled);
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

    if (autoRefreshChanged) {
        Q_EMIT autoRefreshResultsForLiveUpdatesApplied(autoRefreshEnabled);
    }

    if (!changes.isEmpty()) {
        Q_EMIT preferencesApplied(changes);
    }
}