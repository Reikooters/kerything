// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DevicePickerDialog.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "BlockDeviceDisplayUtils.h"
#include "HoverRowHighlight.h"

namespace {
    constexpr int DefaultCheckStateRole = Qt::UserRole + 1;

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

DevicePickerDialog::DevicePickerDialog(
    const std::vector<BlockDevice>& blockDevices,
    const Preferences& preferences,
    QWidget* parent
)
    : QDialog(parent),
      preferences_(preferences)
{
    setWindowTitle(QStringLiteral("Choose Devices to Index"));
    resize(900, 520);

    auto* layout = new QVBoxLayout(this);

    auto* titleLabel = new QLabel(
        QStringLiteral(
            "<b>Choose devices to index</b><br>"
            "Kerything will build a fast filename index for the selected devices.<br><br>"
            "It indexes file and folder names, paths, sizes, and last modified timestamps.<br>"
            "It does not read file contents.<br><br>"
            "Choose which devices to index. You can change this later in settings."
        ),
        this
    );
    titleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);

    table_ = new QTableWidget(this);
    table_->setColumnCount(ColumnCount);
    table_->setHorizontalHeaderLabels({
        QStringLiteral("Include"),
        QStringLiteral("Name"),
        QStringLiteral("Filesystem"),
        QStringLiteral("Mount point"),
        QStringLiteral("Device"),
        QStringLiteral("Model"),
    });

    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setSortingEnabled(false);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsClickable(false);
    table_->horizontalHeader()->setSectionResizeMode(EnabledColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(FsTypeColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(MountPointColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(DevNodeColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(DiskModelColumn, QHeaderView::ResizeToContents);
    installHoverRowHighlight(table_);
    table_->viewport()->installEventFilter(this);
    table_->installEventFilter(this);

    populateTable(blockDevices);

    layout->addWidget(table_);

    auto* selectionButtonLayout = new QHBoxLayout();

    auto* selectAllButton = new QPushButton(QStringLiteral("Select All"), this);
    auto* selectNoneButton = new QPushButton(QStringLiteral("Select None"), this);
    auto* selectMountedButton = new QPushButton(QStringLiteral("Select Mounted"), this);

    selectionButtonLayout->addWidget(selectAllButton);
    selectionButtonLayout->addWidget(selectNoneButton);
    selectionButtonLayout->addWidget(selectMountedButton);
    selectionButtonLayout->addStretch();

    layout->addLayout(selectionButtonLayout);

    auto* buttonBox = new QDialogButtonBox(this);

    auto* cancelButton = buttonBox->addButton(QDialogButtonBox::Cancel);
    cancelButton->setText(QStringLiteral("Skip for Now"));

    startButton_ = buttonBox->addButton(QStringLiteral("Start Indexing"), QDialogButtonBox::AcceptRole);
    startButton_->setDefault(true);

    connect(selectAllButton, &QPushButton::clicked, this, [this]() {
        setAllChecked(Qt::Checked);
    });

    connect(selectNoneButton, &QPushButton::clicked, this, [this]() {
        setAllChecked(Qt::Unchecked);
    });

    connect(selectMountedButton, &QPushButton::clicked, this, [this]() {
        restoreDefaultSelection();
    });

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(table_, &QTableWidget::itemChanged, this, [this]() {
        updateStartButtonEnabled();
    });
    updateStartButtonEnabled();

    layout->addWidget(buttonBox);
}

QStringList DevicePickerDialog::selectedDeviceIds() const
{
    QStringList out;

    if (!table_) {
        return out;
    }

    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* item = table_->item(row, EnabledColumn);
        if (!item || item->checkState() != Qt::Checked) {
            continue;
        }

        const QString deviceId = item->data(Qt::UserRole).toString();
        if (!deviceId.isEmpty()) {
            out << deviceId;
        }
    }

    return out;
}

bool DevicePickerDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (!table_) {
        return QDialog::eventFilter(watched, event);
    }

    if (watched == table_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = dynamic_cast<QKeyEvent*>(event);

        if (moveTableSelectionToEdge(table_, keyEvent)) {
            return true;
        }

        if (keyEvent->key() == Qt::Key_Space) {
            toggleRowChecked(table_->currentRow());
            return true;
        }
    }

    if (watched == table_->viewport() && event->type() == QEvent::MouseButtonRelease) {
        const auto* mouseEvent = dynamic_cast<QMouseEvent*>(event);

        if (mouseEvent->button() == Qt::LeftButton) {
            const QModelIndex index = table_->indexAt(mouseEvent->pos());
            if (index.isValid()) {
                table_->setCurrentCell(index.row(), index.column());
                toggleRowChecked(index.row());
                return true;
            }
        }
    }

    return QDialog::eventFilter(watched, event);
}

bool DevicePickerDialog::shouldSelectByDefault(const BlockDevice& blockDevice)
{
    if (!blockDevice.mounted) {
        return false;
    }

    const QString mountPoint = blockDevice.primaryMountPoint.trimmed();

    if (mountPoint == QStringLiteral("/boot") ||
        mountPoint == QStringLiteral("/boot/efi") ||
        mountPoint.startsWith(QStringLiteral("/var/lib/")) ||
        mountPoint.startsWith(QStringLiteral("/snap/"))) {
        return false;
    }

    return true;
}

Qt::CheckState DevicePickerDialog::initialCheckStateForBlockDevice(const BlockDevice& blockDevice) const
{
    if (const std::optional<IndexedDevicePreference> preference =
            preferences_.indexedDevicePreference(blockDevice.deviceId)) {
        return preference->enabled ? Qt::Checked : Qt::Unchecked;
    }

    return shouldSelectByDefault(blockDevice) ? Qt::Checked : Qt::Unchecked;
}


void DevicePickerDialog::populateTable(const std::vector<BlockDevice>& blockDevices)
{
    const QSignalBlocker blocker(table_);
    const bool sortingWasEnabled = table_->isSortingEnabled();

    table_->setSortingEnabled(false);
    table_->clearContents();

    std::vector<BlockDevice> sortedDevices = blockDevices;
    std::sort(
        sortedDevices.begin(),
        sortedDevices.end(),
        BlockDeviceDisplayUtils::deviceLessThan
    );

    table_->setRowCount(static_cast<int>(sortedDevices.size()));

    const QPalette palette = table_->palette();
    const QBrush unmountedForeground = palette.brush(QPalette::PlaceholderText);

    int row = 0;
    for (const BlockDevice& blockDevice : sortedDevices) {
        const Qt::CheckState defaultCheckState = initialCheckStateForBlockDevice(blockDevice);

        auto* enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enabledItem->setCheckState(defaultCheckState);
        enabledItem->setData(Qt::UserRole, blockDevice.deviceId);
        enabledItem->setData(DefaultCheckStateRole, static_cast<int>(defaultCheckState));
        enabledItem->setToolTip(QStringLiteral("Click the row or press Space to include/exclude this device."));
        table_->setItem(row, EnabledColumn, enabledItem);

        auto* nameItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayNameForBlockDevice(blockDevice));
        nameItem->setToolTip(blockDevice.deviceId);
        table_->setItem(row, NameColumn, nameItem);

        auto* fsTypeItem = new QTableWidgetItem(
            BlockDeviceDisplayUtils::filesystemDisplayTextForBlockDevice(blockDevice)
        );
        fsTypeItem->setToolTip(
            BlockDeviceDisplayUtils::filesystemToolTipForBlockDevice(blockDevice)
        );
        table_->setItem(row, FsTypeColumn, fsTypeItem);

        const QString mountPointText =
            BlockDeviceDisplayUtils::mountPointSummaryForBlockDevice(blockDevice);

        auto* mountPointItem = new QTableWidgetItem(mountPointText);
        mountPointItem->setToolTip(
            BlockDeviceDisplayUtils::mountPointToolTipForBlockDevice(blockDevice)
        );
        table_->setItem(row, MountPointColumn, mountPointItem);

        auto* devNodeItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.devNode));
        devNodeItem->setToolTip(blockDevice.deviceId);
        table_->setItem(row, DevNodeColumn, devNodeItem);

        auto* diskModelItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.diskModel));
        diskModelItem->setToolTip(BlockDeviceDisplayUtils::displayOrDash(blockDevice.diskModel));
        table_->setItem(row, DiskModelColumn, diskModelItem);

        if (!blockDevice.mounted) {
            for (int column = 0; column < ColumnCount; ++column) {
                if (auto* item = table_->item(row, column)) {
                    item->setForeground(unmountedForeground);
                }
            }
        }

        ++row;
    }

    table_->setSortingEnabled(sortingWasEnabled);

    if (table_->rowCount() > 0) {
        table_->setCurrentCell(0, NameColumn);
    }
}

void DevicePickerDialog::setAllChecked(Qt::CheckState checkState)
{
    if (!table_) {
        return;
    }

    const QSignalBlocker blocker(table_);

    for (int row = 0; row < table_->rowCount(); ++row) {
        if (auto* item = table_->item(row, EnabledColumn)) {
            item->setCheckState(checkState);
        }
    }

    updateStartButtonEnabled();
}

void DevicePickerDialog::restoreDefaultSelection()
{
    if (!table_) {
        return;
    }

    const QSignalBlocker blocker(table_);

    for (int row = 0; row < table_->rowCount(); ++row) {
        auto* item = table_->item(row, EnabledColumn);
        if (!item) {
            continue;
        }

        const Qt::CheckState defaultCheckState = static_cast<Qt::CheckState>(
            item->data(DefaultCheckStateRole).toInt()
        );

        item->setCheckState(defaultCheckState);
    }

    updateStartButtonEnabled();
}

void DevicePickerDialog::toggleRowChecked(int row)
{
    if (!table_ || row < 0 || row >= table_->rowCount()) {
        return;
    }

    auto* item = table_->item(row, EnabledColumn);
    if (!item) {
        return;
    }

    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
}

void DevicePickerDialog::updateStartButtonEnabled()
{
    if (!startButton_) {
        return;
    }

    startButton_->setEnabled(hasSelectedDevices());
}

bool DevicePickerDialog::hasSelectedDevices() const
{
    if (!table_) {
        return false;
    }

    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* item = table_->item(row, EnabledColumn);
        if (item && item->checkState() == Qt::Checked) {
            return true;
        }
    }

    return false;
}