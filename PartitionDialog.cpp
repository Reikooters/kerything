// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <Solid/Device>
#include <Solid/StorageAccess>
#include <Solid/StorageVolume>
#include <Solid/Block>
#include <QCoreApplication>
#include <QVBoxLayout>
#include <QMessageBox>
#include "PartitionDialog.h"
#include "ScannerManager.h"

#include <QProcess>

PartitionDialog::PartitionDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Select NTFS Partition");
    auto *layout = new QVBoxLayout(this);

    auto *topLayout = new QHBoxLayout();
    layout->addLayout(topLayout);

    topLayout->addWidget(new QLabel("Select an NTFS partition:", this));
    refreshBtn = new QPushButton(QIcon::fromTheme("view-refresh"), "Refresh", this);
    refreshBtn->setToolTip("Refresh list");
    topLayout->addWidget(refreshBtn);
    connect(refreshBtn, &QPushButton::clicked, this, &PartitionDialog::refreshPartitions);

    listWidget = new QListWidget(this);
    layout->addWidget(listWidget);

    statusLabel = new QLabel("Select a partition to begin.", this);
    layout->addWidget(statusLabel);

    startBtn = new QPushButton("Start Indexing", this);
    startBtn->setEnabled(false); // Disable the button by default
    layout->addWidget(startBtn);

    // Connect the button to our start logic
    connect(startBtn, &QPushButton::clicked, this, &PartitionDialog::onStartClicked);

    // Enable button only when an item is selected
    connect(listWidget, &QListWidget::currentRowChanged, this, [this](int row) {
        startBtn->setEnabled(row >= 0);
    });

    // Handle Return key (and double-click) on the list
    connect(listWidget, &QListWidget::itemActivated, this, [this]() {
        if (startBtn->isEnabled()) {
            onStartClicked();
        }
    });

    m_manager = std::make_unique<ScannerManager>(this);

    // Connect manager signals to Dialog UI
    connect(m_manager.get(), &ScannerManager::scannerStarted, this, [this]() { setScanning(true); });
    connect(m_manager.get(), &ScannerManager::scannerFinished, this, [this]() { setScanning(false); });
    connect(m_manager.get(), &ScannerManager::progressMessage, statusLabel, &QLabel::setText);

    // Connect error messages to show a critical message box
    connect(m_manager.get(), &ScannerManager::errorMessage, this, [](const QString &title, const QString &msg) {
        QMessageBox::critical(nullptr, title, msg);
    });

    resize(500, 300);
    refreshPartitions(); // Initial load
}

void PartitionDialog::refreshPartitions() {
    listWidget->clear();
    partitions.clear();

    // Find NTFS partitions using Solid
    auto devices = Solid::Device::listFromType(Solid::DeviceInterface::StorageVolume);
    for (const auto &device : devices) {
        auto *volume = device.as<Solid::StorageVolume>();
        auto *block = device.as<Solid::Block>();

        if (!volume || !block) {
            continue;
        }

        // Only care about NTFS
        if (volume->fsType().contains("ntfs", Qt::CaseInsensitive)) {
            auto *access = device.as<Solid::StorageAccess>();
            QString mp = access ? access->filePath() : "";

            PartitionInfo info = {
                QString("%1 (%2) - %3")
                    .arg(device.product(), block->device(), mp.isEmpty() ? "Not Mounted" : mp),
                block->device(),
                mp
            };

            listWidget->addItem(new QListWidgetItem(info.name, listWidget));
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

    m_scannedDb = m_manager->scanDevice(selected.devicePath);

    if (m_scannedDb) {
        this->accept();
    }

    m_isHandlingClick = false;
}

std::optional<ScannerEngine::SearchDatabase> PartitionDialog::takeDatabase() {
    return std::move(m_scannedDb);
}

void PartitionDialog::setScanning(bool scanning) {
    listWidget->setEnabled(!scanning);
    refreshBtn->setEnabled(!scanning);

    if (scanning) {
        startBtn->setText("Cancel Scanning");
        startBtn->setEnabled(true); // Button stays enabled to allow cancelling
    } else {
        startBtn->setText("Start Indexing");
        statusLabel->setText("Select a partition to begin.");

        // Ensure button state is correct based on whether something is selected
        startBtn->setEnabled(listWidget->currentItem() != nullptr);
    }
}

void PartitionDialog::setButtonEnabled(bool enabled) const {
    startBtn->setEnabled(enabled);
}

PartitionInfo PartitionDialog::getSelected() {
    int row = listWidget->currentRow();

    if (row < 0 || row >= static_cast<int>(partitions.size())) {
        return {};
    }

    return partitions[row];
}