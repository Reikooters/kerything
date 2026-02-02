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
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusServiceWatcher>
#include "PartitionDialog.h"
#include "GuiUtils.h"
#include "ScannerManager.h"
#include "DbusIndexerClient.h"

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

    // Connect to daemon job signals (if daemon is present)
    connectDaemonSignals();

    // Watch daemon availability: if it vanishes mid-scan, reset UI and explain what happened.
    constexpr const char* kService = "net.reikooters.Kerything1";
    m_daemonWatcher = new QDBusServiceWatcher(QString::fromLatin1(kService),
                                             QDBusConnection::systemBus(),
                                             QDBusServiceWatcher::WatchForUnregistration,
                                             this);
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &PartitionDialog::onDaemonVanished);

    resize(715, 360);
    refreshPartitions(); // Initial load
}

void PartitionDialog::connectDaemonSignals() {
    if (m_daemonSignalsConnected) {
        return;
    }

    constexpr const char* kService = "net.reikooters.Kerything1";
    constexpr const char* kPath    = "/net/reikooters/Kerything1";
    constexpr const char* kIface   = "net.reikooters.Kerything1.Indexer";

    auto conn = QDBusConnection::systemBus();
    if (!conn.isConnected()) {
        return;
    }

    // Use functor-based connects (type-safe-ish) via QDBusConnection::connect overload that takes a SLOT string
    // isn't great; instead, connect using the Qt meta-object signature strings.

    const bool c1 = conn.connect(
        QString::fromLatin1(kService),
        QString::fromLatin1(kPath),
        QString::fromLatin1(kIface),
        QStringLiteral("JobProgress"),
        this,
        SLOT(onDaemonJobProgress(quint64,quint32,QVariantMap))
    );

    const bool c2 = conn.connect(
        QString::fromLatin1(kService),
        QString::fromLatin1(kPath),
        QString::fromLatin1(kIface),
        QStringLiteral("JobFinished"),
        this,
        SLOT(onDaemonJobFinished(quint64,QString,QString,QVariantMap))
    );

    m_daemonSignalsConnected = (c1 && c2);
}

void PartitionDialog::onDaemonVanished(const QString& serviceName) {
    Q_UNUSED(serviceName);

    // Only relevant if we are currently tracking a daemon job.
    if (!m_isDaemonScanActive) {
        return;
    }

    // Capture state for messaging, then reset internal flags.
    const quint64 lostJobId = m_activeJobId;

    m_isDaemonScanActive = false;
    m_activeJobId = 0;

    // Reset UI first so the dialog doesn't look stuck behind a modal popup.
    setScanning(false);

    QMessageBox::warning(
       this,
       tr("Scan aborted"),
       tr("The indexing daemon stopped while scanning.\n\n"
          "The scan was aborted. Please start the daemon and try again.\n\n"
          "Last job id: %1")
           .arg(lostJobId)
   );
}

void PartitionDialog::onDaemonJobProgress(quint64 jobId, quint32 percent, const QVariantMap& props) {
    Q_UNUSED(props);

    if (!m_isDaemonScanActive || jobId != m_activeJobId) {
        return;
    }

    progressBar->setVisible(true);
    progressBar->setValue(static_cast<int>(percent));
    statusLabel->setText(QStringLiteral("Scanning device... %1%").arg(percent));
}

void PartitionDialog::onDaemonJobFinished(quint64 jobId, const QString& status, const QString& message, const QVariantMap& props) {
    Q_UNUSED(props);

    if (!m_isDaemonScanActive || jobId != m_activeJobId) {
        return;
    }

    m_isDaemonScanActive = false;
    m_activeJobId = 0;

    if (status == QStringLiteral("ok")) {
        statusLabel->setText(QStringLiteral("Scan finished."));
        setScanning(false);

        // close dialog automatically on success
        this->accept();
        return;
    } else if (status == QStringLiteral("cancelled")) {
        statusLabel->setText(QStringLiteral("Scan cancelled."));
    } else {
        statusLabel->setText(QStringLiteral("Scan failed: %1").arg(message));
    }

    setScanning(false);
}

void PartitionDialog::refreshPartitions() {
    treeWidget->clear();
    partitions.clear();

    // Prefer daemon inventory (system-wide authoritative view).
    {
        DbusIndexerClient client;
        QString err;
        auto devicesOpt = client.listKnownDevices(&err);

        if (devicesOpt) {
            const QVariantList& devices = *devicesOpt;

            for (const QVariant& dv : devices) {
                const QVariantMap m = dv.toMap();

                const QString fsType = m.value(QStringLiteral("fsType")).toString();
                const QString fsTypeLower = fsType.toLower();

                // Only show file systems we can scan (for now).
                if (fsTypeLower != QStringLiteral("ntfs") && fsTypeLower != QStringLiteral("ext4")) {
                    continue;
                }

                const QString deviceId = m.value(QStringLiteral("deviceId")).toString();
                const QString devNode  = m.value(QStringLiteral("devNode")).toString();
                const QString label    = m.value(QStringLiteral("label")).toString();
                const bool mounted     = m.value(QStringLiteral("mounted")).toBool();
                const QString primary  = m.value(QStringLiteral("primaryMountPoint")).toString();

                const QString displayName = !label.isEmpty() ? label : deviceId;
                const QString displayMount = (mounted && !primary.isEmpty()) ? primary : QStringLiteral("Not Mounted");

                PartitionInfo info{
                    deviceId,
                    fsTypeLower,
                    displayName,
                    devNode,
                    displayMount
                };

                auto *item = new QTreeWidgetItem(treeWidget);
                item->setText(0, info.fsType);
                item->setText(1, info.name);
                item->setText(2, info.devicePath);
                item->setText(3, info.mountPoint);

                // Store deviceId on the row for robust selection even when sorted.
                item->setData(0, Qt::UserRole, info.deviceId);

                if (!mounted) {
                    item->setForeground(0, QBrush(Qt::gray));
                    item->setForeground(1, QBrush(Qt::gray));
                    item->setForeground(2, QBrush(Qt::gray));
                    item->setForeground(3, QBrush(Qt::gray));
                }

                partitions.push_back(info);
            }

            return;
        }

        // Daemon not available yet -> fall back to Solid for now.
        // (This keeps the app usable while system D-Bus activation is still being ironed out.)
        qWarning().noquote() << "ListKnownDevices() failed, falling back to Solid:" << err;
    }

    // --- Solid fallback ---

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
                QString(), // deviceId unknown in Solid fallback mode
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
    // If a daemon scan is active, request cancel.
    if (m_isDaemonScanActive) {
        if (m_activeJobId != 0) {
            DbusIndexerClient client;
            QString err;
            if (!client.cancelJob(m_activeJobId, &err)) {
                const QString msg = err.isEmpty()
                    ? QStringLiteral("Failed to cancel job because the daemon is not available.")
                    : QStringLiteral("Failed to cancel job: %1").arg(err);

                QMessageBox::warning(this, tr("Cancel failed"), msg);

                // If the daemon is gone, also reset the UI so the dialog doesn’t look “stuck”.
                if (!client.isAvailable()) {
                    m_isDaemonScanActive = false;
                    m_activeJobId = 0;
                    statusLabel->setText(QStringLiteral("Daemon not available. Scan aborted."));
                    setScanning(false);
                }
            }
        }
        return;
    }
    else {
        // Fallback: local helper via pkexec

        // If currently running, request cancel and return
        if (m_manager->isRunning()) {
            m_manager->requestCancel();
            return;
        }
    }

    // Prevent re-entry if the user double-clicks faster than the state flags can update
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

    // Prefer daemon indexing if we have a daemon deviceId.
    if (!selected.deviceId.isEmpty()) {
        connectDaemonSignals();

        DbusIndexerClient client;
        QString err;
        auto jobIdOpt = client.startIndex(selected.deviceId, &err);

        if (!jobIdOpt) {
            const QString msg = err.isEmpty()
                    ? QStringLiteral("Failed to start daemon indexing because the daemon is not available.")
                    : QStringLiteral("Failed to start daemon indexing: %1").arg(err);

            QMessageBox::warning(this, tr("Daemon indexing failed"), msg);

            m_isHandlingClick = false;
            return;
        }

        m_activeJobId = *jobIdOpt;
        m_isDaemonScanActive = true;

        setScanning(true);
        statusLabel->setText(QStringLiteral("Starting scan..."));
        progressBar->setVisible(true);
        progressBar->setValue(0);

        m_isHandlingClick = false;
        return;
    }

    // Fallback: local helper via pkexec
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

    const QString deviceId = item->data(0, Qt::UserRole).toString();
    const QString devNode  = item->text(2);

    // Prefer deviceId match (daemon mode), fall back to dev node match.
    if (!deviceId.isEmpty()) {
        for (const auto& p : partitions) {
            if (p.deviceId == deviceId) {
                return p;
            }
        }
    }

    for (const auto& p : partitions) {
        if (p.devicePath == devNode) {
            return p;
        }
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
