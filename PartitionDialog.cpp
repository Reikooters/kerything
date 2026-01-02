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
    if (m_isScanning) {
        m_cancelRequested = true;
        return;
    }

    // Start the scan
    auto selected = getSelected();
    if (selected.devicePath.isEmpty()) {
        return;
    }

    setScanning(true);

    // We run the helper logic here. Since we are inside a slot,
    // the dialog stays visible and the sudo prompt has a valid parent.
    m_scannedDb = scanViaHelper(selected.devicePath);

    if (m_scannedDb) {
        // SUCCESS: Now we can close the dialog
        this->accept();
    } else {
        // FAILED or CANCELLED: Just reset the UI and stay open
        setScanning(false);
    }
}

std::optional<ScannerEngine::SearchDatabase> PartitionDialog::takeDatabase() {
    return std::move(m_scannedDb);
}

void PartitionDialog::setScanning(bool scanning) {
    m_isScanning = scanning;
    m_cancelRequested = false;
    listWidget->setEnabled(!scanning);
    refreshBtn->setEnabled(!scanning);

    if (scanning) {
        startBtn->setText("Cancel Scanning");
        startBtn->setEnabled(true); // Button stays enabled to allow cancelling
        statusLabel->setText("<b>Scanning MFT... Please check the password prompt.</b>");
    } else {
        startBtn->setText("Start Indexing");
        statusLabel->setText("Select a partition to begin.");
    }

    QCoreApplication::processEvents();
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

std::optional<ScannerEngine::SearchDatabase> PartitionDialog::scanViaHelper(const QString& devicePath) {
    setScanning(true);

    // Using pkexec triggers the system authentication dialog (Native KDE feel)
    // We assume 'kerything-scanner-helper' is in the same directory as our app

    // Use unique_ptr to prevent leaks on early returns
    auto helper = std::make_unique<QProcess>();
    QString helperPath = QCoreApplication::applicationDirPath() + "/kerything-scanner-helper";

    // We want to handle the data as a stream
    qDebug() << "Launching helper:" << helperPath << "on" << devicePath;
    helper->start("pkexec", {helperPath, devicePath});

    // Loop until finished, keeping the UI responsive
    while (!helper->waitForFinished(100)) {
        QCoreApplication::processEvents();

        // Check if user clicked the "Cancel" button or closed the dialog
        if (cancelRequested() || !isVisible()) {
            qDebug() << "Cancellation requested. Abandoning process...";
            helper->kill(); // Send SIGKILL

            // Do not call waitForFinished here, otherwise the GUI will freeze.
            // We tell the process to delete itself whenever it finally manages to exit.

            // Ownership transfer: We release from unique_ptr because the process
            // needs to live long enough to die properly in the background.
            QProcess* helperPtr = helper.release();
            connect(helperPtr, &QProcess::finished, helperPtr, &QObject::deleteLater);

            setScanning(false);
            return std::nullopt;
        }
    }

    if (helper->exitCode() != 0) {
        setScanning(false);

        qDebug() << "Helper failed with exit code" << helper->exitCode();

        QMessageBox::critical(this, "Scanner Helper failed",
            "The Scanner Helper did not run successfully.\n\n"
            "Exit code: " + QString::number(helper->exitCode()));

        return std::nullopt;
    }

    // Helper process has finished, disable the button while we consume the data
    setButtonEnabled(false);
    QCoreApplication::processEvents();

    qDebug() << "Helper finished with exit code" << helper->exitCode();

    // Read the binary data from the finished process buffer
    QByteArray rawData = helper->readAllStandardOutput();
    if (rawData.isEmpty()) {
        setScanning(false);
        setButtonEnabled(true);

        return std::nullopt;
    }

    // Use a QDataStream directly on the process's stdout
    QDataStream stream(rawData);
    stream.setByteOrder(QDataStream::LittleEndian);

    ScannerEngine::SearchDatabase db;

    auto readVal = [&](auto& val) {
        return stream.readRawData(reinterpret_cast<char*>(&val), sizeof(val)) == sizeof(val);
    };

    // 1. Read record count
    quint64 recordCount = 0;
    if (!readVal(recordCount)) return std::nullopt;
    qDebug() << "Helper reporting" << recordCount << "records. Allocating memory...";

    // 2. Read records
    try {
        db.records.resize(recordCount);
    } catch (const std::exception& e) {
        setScanning(false);
        setButtonEnabled(true);

        qDebug() << "CRASH prevented! Attempted to resize to" << recordCount << "records. Error:" << e.what();
        return std::nullopt;
    }

    stream.readRawData(reinterpret_cast<char*>(db.records.data()), static_cast<qint64>(recordCount * sizeof(ScannerEngine::FileRecord)));
    qDebug() << "Records transfer complete.";

    // 3. Read string pool size
    quint64 poolSize = 0;
    if (!readVal(poolSize)) {
        setScanning(false);
        setButtonEnabled(true);

        return std::nullopt;
    }
    qDebug() << "String pool size:" << poolSize;

    // 4. Read string pool
    db.stringPool.resize(poolSize);
    stream.readRawData(db.stringPool.data(), static_cast<qint64>(poolSize));

    // 5. Read directory paths
    uint64_t dirCount = 0;
    if (!readVal(dirCount)) {
        setScanning(false);
        setButtonEnabled(true);

        return std::nullopt;
    }

    qDebug() << "Reading" << dirCount << "directory paths...";
    for (uint64_t i = 0; i < dirCount; ++i) {
        quint32 idx, len;

        if (!readVal(idx) || !readVal(len)) {
            break;
        }

        std::string path(len, '\0');
        stream.readRawData(path.data(), len);
        db.directoryPaths[idx] = std::move(path);
    }

    // 6. Build the trigram index
    qDebug() << "Data transfer complete. Building trigrams...";
    db.buildTrigramIndexParallel();

    qDebug() << "Building trigrams complete.";

    return db;
}