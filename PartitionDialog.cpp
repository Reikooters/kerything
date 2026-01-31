// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <Solid/Device>
#include <Solid/StorageAccess>
#include <Solid/StorageVolume>
#include <Solid/Block>
#include <QCoreApplication>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressBar>
#include "PartitionDialog.h"
#include "GuiUtils.h"
#include "ScannerManager.h"

PartitionDialog::PartitionDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Select Partition");
    auto *layout = new QVBoxLayout(this);

    auto *topLayout = new QHBoxLayout();
    layout->addLayout(topLayout);

    topLayout->addWidget(new QLabel("Select a partition:", this));
    refreshBtn = new QPushButton(QIcon::fromTheme("view-refresh"), "Refresh", this);
    refreshBtn->setToolTip("Refresh list");
    topLayout->addWidget(refreshBtn);
    connect(refreshBtn, &QPushButton::clicked, this, &PartitionDialog::refreshPartitions);

    treeWidget = new QTreeWidget(this);
    treeWidget->setColumnCount(4);
    treeWidget->setHeaderLabels({"FS", "Name", "Device", "Mount Path"});
    treeWidget->setSortingEnabled(true);
    treeWidget->sortByColumn(2, Qt::AscendingOrder); // Sort by device path by default
    treeWidget->setRootIsDecorated(false); // No expansion arrows needed for a list
    treeWidget->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    treeWidget->header()->setStretchLastSection(true);
    // treeWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(treeWidget);

    statusLabel = new QLabel("Select a partition to begin.", this);
    layout->addWidget(statusLabel);

    progressBar = new QProgressBar(this);
    progressBar->setTextVisible(false);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setVisible(false); // Hidden until scanning
    layout->addWidget(progressBar);

    startBtn = new QPushButton("Start Indexing", this);
    startBtn->setEnabled(false); // Disable the button by default
    layout->addWidget(startBtn);

    // Connect the button to our start logic
    connect(startBtn, &QPushButton::clicked, this, &PartitionDialog::onStartClicked);

    // Enable button only when an item is selected
    connect(treeWidget, &QTreeWidget::itemSelectionChanged, this, [this]() {
        startBtn->setEnabled(!treeWidget->selectedItems().isEmpty());
    });

    // Handle Return key (and double-click) on the list
    connect(treeWidget, &QTreeWidget::itemActivated, this, [this]() {
        if (startBtn->isEnabled()) {
            onStartClicked();
        }
    });

    m_manager = std::make_unique<ScannerManager>(this);

    // Connect manager signals to Dialog UI
    connect(m_manager.get(), &ScannerManager::scannerStarted, this, [this]() { setScanning(true); });
    connect(m_manager.get(), &ScannerManager::scannerFinished, this, [this]() { setScanning(false); });
    connect(m_manager.get(), &ScannerManager::progressMessage, statusLabel, &QLabel::setText);
    connect(m_manager.get(), &ScannerManager::progressValue, progressBar, &QProgressBar::setValue);

    // Connect error messages to show a critical message box
    connect(m_manager.get(), &ScannerManager::errorMessage, this, [](const QString &title, const QString &msg) {
        QMessageBox::critical(nullptr, title, msg);
    });

    resize(625, 360);
    refreshPartitions(); // Initial load
}

void PartitionDialog::refreshPartitions() {
    treeWidget->clear();
    partitions.clear();

    // Find NTFS and EXT4 partitions using Solid
    auto devices = Solid::Device::listFromType(Solid::DeviceInterface::StorageVolume);
    for (const auto &device : devices) {
        auto *volume = device.as<Solid::StorageVolume>();
        auto *block = device.as<Solid::Block>();

        if (!volume || !block) {
            continue;
        }

        // Support both NTFS and EXT4
        if (volume->fsType().contains("ntfs", Qt::CaseInsensitive)
            || volume->fsType().contains("ext4", Qt::CaseInsensitive)) {
            auto *access = device.as<Solid::StorageAccess>();
            QString mp = access ? access->filePath() : "";

            PartitionInfo info = {
                volume->fsType(),
                device.product(),
                block->device(),
                mp.isEmpty() ? "Not Mounted" : mp
            };

            auto *item = new QTreeWidgetItem(treeWidget);
            // item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            // item->setCheckState(0, Qt::Unchecked);
            item->setText(0, info.fsType);
            item->setText(1, info.name);
            item->setText(2, info.devicePath);
            item->setText(3, info.mountPoint);

            if (mp.isEmpty()) {
                item->setForeground(0, QBrush(Qt::gray));
                item->setForeground(1, QBrush(Qt::gray));
                item->setForeground(2, QBrush(Qt::gray));
                item->setForeground(3, QBrush(Qt::gray));
            }

            partitions.push_back(info);
        }
    }
}

void PartitionDialog::onStartClicked() {
    // If currently running, request cancel and return
    if (m_manager->isRunning()) {
        m_manager->requestCancel();
        return;
    }

    // Prevent re-entry if the user double-clicks faster than
    // the state flags can update
    if (m_isHandlingClick) {
        return;
    }
    m_isHandlingClick = true;

    // Perform the scan
    auto selected = getSelected();

    if (selected.devicePath.isEmpty()) {
        m_isHandlingClick = false;
        return;
    }

    const QString fsTypeNormalized = GuiUtils::normalizeFsTypeForHelper(selected.fsType);
    if (fsTypeNormalized.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Unsupported filesystem"),
            tr("This filesystem type is not supported for raw scanning.\n\nDetected: %1")
                .arg(selected.fsType)
        );
        m_isHandlingClick = false;
        return;
    }

    m_scannedDb = m_manager->scanDevice(selected.devicePath, fsTypeNormalized);

    if (m_scannedDb) {
        this->accept();
    }

    m_isHandlingClick = false;
}

std::optional<ScannerEngine::SearchDatabase> PartitionDialog::takeDatabase() {
    return std::move(m_scannedDb);
}

void PartitionDialog::setScanning(bool scanning) {
    treeWidget->setEnabled(!scanning);
    refreshBtn->setEnabled(!scanning);

    if (scanning) {
        startBtn->setText("Cancel Scanning");
        startBtn->setEnabled(true); // Button stays enabled to allow cancelling

        // Show the progress bar while scanning
        progressBar->setVisible(true);
    } else {
        startBtn->setText("Start Indexing");
        statusLabel->setText("Select a partition to begin.");

        // Hide the progress bar after scanning completes or cancellation
        progressBar->setVisible(false);
        progressBar->setValue(0);

        // Ensure button state is correct based on whether something is selected
        startBtn->setEnabled(!treeWidget->selectedItems().isEmpty());
    }
}

void PartitionDialog::setButtonEnabled(bool enabled) const {
    startBtn->setEnabled(enabled);
}

PartitionInfo PartitionDialog::getSelected() {
    auto *item = treeWidget->currentItem();
    if (!item) {
        return {};
    }

    int index = treeWidget->indexOfTopLevelItem(item);
    if (index < 0 || index >= static_cast<int>(partitions.size())) {
        // Fallback: search by device path if sorting changed the visual order
        QString dev = item->text(2);

        for (const auto& p : partitions) {
            if (p.devicePath == dev) {
                return p;
            }
        }

        return {};
    }

    // Since QTreeWidget items aren't mapped 1:1 with the vector when sorted,
    // we should reconstruct from the item text or use data roles.
    return { item->text(0), item->text(1), item->text(2), item->text(3) };
}

// QList<PartitionInfo> PartitionDialog::getSelectedPartitions() {
//     QList<PartitionInfo> selectedList;
//
//     for (QTreeWidgetItem* item : treeWidget->selectedItems()) {
//         selectedList.append({
//             item->text(0), // FS
//             item->text(1), // Name
//             item->text(2), // Device
//             item->text(3)  // Mount
//         });
//     }
//
//     return selectedList;
// }
