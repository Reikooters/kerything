// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QDateTime>
#include <QSettings>
#include <QtDBus/QDBusConnection>

#include "DbusIndexerClient.h"
#include "GuiUtils.h"

namespace {
    constexpr const char* kService = "net.reikooters.Kerything1";
    constexpr const char* kPath    = "/net/reikooters/Kerything1";
    constexpr const char* kIface   = "net.reikooters.Kerything1.Indexer";

    enum Columns {
        ColIndexed = 0,
        ColWatch,
        ColFs,
        ColLabel,
        ColMount,
        ColEntries,
        ColLastIndexed,
        ColDeviceId,
        ColDevNode,
        ColCount
    };

    static QString boolToYesNo(bool b) { return b ? QStringLiteral("Yes") : QStringLiteral("No"); }
}

SettingsDialog::SettingsDialog(DbusIndexerClient* dbusClient, QWidget* parent)
    : QDialog(parent), m_client(dbusClient)
{
    setWindowTitle(QStringLiteral("Kerything Settings"));
    resize(1100, 520);

    auto* root = new QVBoxLayout(this);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({
        "Indexed", "Watch", "FS", "Label", "Mount", "Entries", "Last Indexed", "Device ID", "Dev Node"
    });
    m_tree->setRootIsDecorated(false);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(ColLabel, Qt::AscendingOrder);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    root->addWidget(m_tree);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &SettingsDialog::onSelectionChanged);

    // Handle checkbox toggles
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (!item) return;
        if (column != ColWatch) return;
        if (m_updatingTree) return;
        if (m_jobActive) return; // keep it simple: don't toggle watch while indexing

        const QString deviceId = item->data(ColDeviceId, Qt::UserRole).toString();
        if (deviceId.isEmpty()) return;

        const bool indexed = (item->text(ColIndexed).compare(QStringLiteral("Yes"), Qt::CaseInsensitive) == 0);
        if (!indexed) return;

        const bool enabled = (item->checkState(ColWatch) == Qt::Checked);

        if (!m_client || !m_client->isAvailable()) {
            m_updatingTree = true;
            item->setCheckState(ColWatch, enabled ? Qt::Unchecked : Qt::Checked);
            m_updatingTree = false;
            return;
        }

        QString err;
        if (!m_client->setWatchEnabled(deviceId, enabled, &err)) {
            QMessageBox::warning(this,
                                 QStringLiteral("Failed to update watch setting"),
                                 err.isEmpty() ? QStringLiteral("SetWatchEnabled failed.") : err);

            // Revert checkbox
            m_updatingTree = true;
            item->setCheckState(ColWatch, enabled ? Qt::Unchecked : Qt::Checked);
            m_updatingTree = false;
            return;
        }

        // IMPORTANT: Don't call refresh() synchronously from itemChanged.
        // Clearing the tree while Qt is processing the signal can invalidate 'item' and crash.
        QTimer::singleShot(0, this, [this]() { refresh(); });
    });

    m_status = new QLabel(this);
    m_status->setText(QStringLiteral("Ready."));
    root->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    auto* btnRow = new QHBoxLayout();
    root->addLayout(btnRow);

    m_refreshBtn = new QPushButton(QIcon::fromTheme("view-refresh"), QStringLiteral("Refresh"), this);
    connect(m_refreshBtn, &QPushButton::clicked, this, &SettingsDialog::refresh);
    btnRow->addWidget(m_refreshBtn);

    btnRow->addStretch(1);

    // --- UI preference: remember last query ---
    {
        m_rememberQueryCheck = new QCheckBox(QStringLiteral("Remember last search query"), this);

        QSettings s;
        s.beginGroup(QStringLiteral("ui"));
        m_rememberQueryCheck->setChecked(s.value(QStringLiteral("persistLastQuery"), false).toBool());
        s.endGroup();

        connect(m_rememberQueryCheck, &QCheckBox::toggled, this, [](bool checked) {
            QSettings s;
            s.beginGroup(QStringLiteral("ui"));
            s.setValue(QStringLiteral("persistLastQuery"), checked);
            if (!checked) {
                s.remove(QStringLiteral("lastQueryText"));
            }
            s.endGroup();
        });

        btnRow->addWidget(m_rememberQueryCheck);
    }
    // --- end remember last query ---

    m_startBtn = new QPushButton(QIcon::fromTheme("system-run"), QStringLiteral("Index / Rescan"), this);
    connect(m_startBtn, &QPushButton::clicked, this, &SettingsDialog::onStartOrRescanClicked);
    btnRow->addWidget(m_startBtn);

    m_forgetBtn = new QPushButton(QIcon::fromTheme("edit-delete"), QStringLiteral("Forget Index"), this);
    connect(m_forgetBtn, &QPushButton::clicked, this, &SettingsDialog::onForgetClicked);
    m_forgetBtn->setEnabled(false);
    btnRow->addWidget(m_forgetBtn);

    m_cancelBtn = new QPushButton(QIcon::fromTheme("process-stop"), QStringLiteral("Cancel"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SettingsDialog::onCancelClicked);
    m_cancelBtn->setEnabled(false);
    btnRow->addWidget(m_cancelBtn);

    // Watch daemon availability: if it vanishes mid-job, reset UI
    m_daemonWatcher = new QDBusServiceWatcher(QString::fromLatin1(kService),
                                             QDBusConnection::systemBus(),
                                             QDBusServiceWatcher::WatchForUnregistration,
                                             this);
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &SettingsDialog::onDaemonVanished);

    connectDaemonSignals();
    refresh();
}

void SettingsDialog::connectDaemonSignals() {
    if (m_daemonSignalsConnected) return;

    auto conn = QDBusConnection::systemBus();
    if (!conn.isConnected()) return;

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

    const bool c3 = conn.connect(
        QString::fromLatin1(kService),
        QString::fromLatin1(kPath),
        QString::fromLatin1(kIface),
        QStringLiteral("DeviceIndexUpdated"),
        this,
        SLOT(onDeviceIndexUpdated(QString,quint64,quint64))
    );

    const bool c4 = conn.connect(
        QString::fromLatin1(kService),
        QString::fromLatin1(kPath),
        QString::fromLatin1(kIface),
        QStringLiteral("DeviceIndexRemoved"),
        this,
        SLOT(onDeviceIndexRemoved(QString))
    );

    m_daemonSignalsConnected = (c1 && c2 && c3 && c4);
}

void SettingsDialog::setBusy(bool busy, const QString& text) {
    if (!text.isEmpty()) m_status->setText(text);

    m_jobActive = busy;

    m_progress->setVisible(busy);
    if (!busy) m_progress->setValue(0);

    m_refreshBtn->setEnabled(!busy);
    m_tree->setEnabled(!busy);

    m_cancelBtn->setEnabled(busy);
    onSelectionChanged();
}

QString SettingsDialog::formatLastIndexedTime(qint64 unixSeconds) {
    if (unixSeconds <= 0) return QStringLiteral("Unknown");
    // const QDateTime dt = QDateTime::fromSecsSinceEpoch(unixSeconds);
    // return QLocale().toString(dt, QLocale::ShortFormat);
    return QString::fromStdString(GuiUtils::uint64ToFormattedTime(unixSeconds));
}

QString SettingsDialog::selectedDeviceId() const {
    auto* item = m_tree->currentItem();
    if (!item) return {};
    return item->data(ColDeviceId, Qt::UserRole).toString();
}

void SettingsDialog::refresh() {
    if (!m_client || !m_client->isAvailable()) {
        m_tree->clear();
        m_status->setText(QStringLiteral("Daemon is not available."));
        return;
    }

    QString errKnown;
    auto knownOpt = m_client->listKnownDevices(&errKnown);
    if (!knownOpt) {
        m_tree->clear();
        m_status->setText(QStringLiteral("Failed to list devices: %1").arg(errKnown));
        return;
    }

    QString errIdx;
    auto indexedOpt = m_client->listIndexedDevices(&errIdx);
    if (!indexedOpt) {
        m_tree->clear();
        m_status->setText(QStringLiteral("Failed to list indexed devices: %1").arg(errIdx));
        return;
    }

    // Build indexed lookup: deviceId -> metadata
    QHash<QString, RowState> idxMap;

    // Also keep watch retry metadata
    struct WatchRetryMeta {
        quint32 failCount = 0;
        quint32 retryInSec = 0;
        qint64 retryAtMs = 0;
        QString retryMode; // "backoff" | "onRemount" | "none"
    };
    QHash<QString, WatchRetryMeta> retryMap;

    for (const QVariant& v : *indexedOpt) {
        const QVariantMap m = v.toMap();
        RowState st;
        st.deviceId = m.value(QStringLiteral("deviceId")).toString();
        st.fsType = m.value(QStringLiteral("fsType")).toString();
        st.labelLastKnown = m.value(QStringLiteral("label")).toString();
        st.uuidLastKnown  = m.value(QStringLiteral("uuid")).toString();
        st.indexed = true;
        st.entryCount = m.value(QStringLiteral("entryCount")).toULongLong();
        st.lastIndexedTime = m.value(QStringLiteral("lastIndexedTime")).toLongLong();
        st.watchEnabled = m.value(QStringLiteral("watchEnabled"), true).toBool();

        // Watch health/status from daemon
        st.watchState = m.value(QStringLiteral("watchState")).toString();
        st.watchError = m.value(QStringLiteral("watchError")).toString();

        idxMap.insert(st.deviceId, st);

        WatchRetryMeta rm;
        rm.failCount = m.value(QStringLiteral("watchFailCount"), 0u).toUInt();
        rm.retryInSec = m.value(QStringLiteral("watchRetryInSec"), 0u).toUInt();
        rm.retryAtMs = m.value(QStringLiteral("watchRetryAtMs"), 0).toLongLong();
        rm.retryMode = m.value(QStringLiteral("watchRetryMode"), QStringLiteral("none")).toString();
        retryMap.insert(st.deviceId, rm);
    }

    // Track which deviceIds are present in the "known" list
    QSet<QString> knownIds;
    knownIds.reserve(knownOpt->size());

    m_updatingTree = true;

    // Block signals hard while rebuilding the tree to avoid itemChanged re-entrancy.
    const bool oldBlocked = m_tree->blockSignals(true);

    m_tree->clear();

    auto formatDurationCompact = [&](quint32 totalSec) -> QString {
        const quint32 minutes = totalSec / 60u;
        const quint32 seconds = totalSec % 60u;

        if (minutes == 0) {
            return QStringLiteral("%1s").arg(QLocale().toString(seconds));
        }
        return QStringLiteral("%1m %2s")
            .arg(QLocale().toString(minutes))
            .arg(QLocale().toString(seconds));
    };

    auto appendRetryInfoIfPresent = [&](const QString& deviceId, const QString& watchState, QString& tip) {
        if (watchState != QStringLiteral("error")) return;

        const auto it = retryMap.constFind(deviceId);
        if (it == retryMap.constEnd()) return;

        const WatchRetryMeta rm = it.value();

        tip = tip.trimmed();
        if (!tip.isEmpty()) tip += QStringLiteral("\n");

        if (rm.retryMode == QStringLiteral("onRemount")) {
            tip += QStringLiteral("Watch will be retried on remount");
        } else if (rm.retryInSec > 0) {
            tip += QStringLiteral("Next watch retry in %1").arg(formatDurationCompact(rm.retryInSec));
        } else {
            // Nothing to add
            return;
        }

        if (rm.failCount > 0) {
            tip += QStringLiteral(" (failures: %1)").arg(QLocale().toString(rm.failCount));
        }
    };

    // 1) Known devices (merge indexed metadata if present)
    for (const QVariant& v : *knownOpt) {
        const QVariantMap m = v.toMap();

        const QString deviceId  = m.value(QStringLiteral("deviceId")).toString();
        const QString devNode   = m.value(QStringLiteral("devNode")).toString();
        const QString fsType    = m.value(QStringLiteral("fsType")).toString();
        const QString liveLabel = m.value(QStringLiteral("label")).toString();
        const QString liveUuid  = m.value(QStringLiteral("uuid")).toString();
        const bool mounted      = m.value(QStringLiteral("mounted")).toBool();
        const QString mp        = m.value(QStringLiteral("primaryMountPoint")).toString();

        knownIds.insert(deviceId);

        RowState st = idxMap.value(deviceId, RowState{deviceId, QString(), QString(), QString(), false, 0, 0, true, QString(), QString()});

        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(ColIndexed, st.indexed ? QStringLiteral("Yes") : QStringLiteral("No"));

        // Watch column: only meaningful for indexed devices
        if (st.indexed) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(ColWatch, st.watchEnabled ? Qt::Checked : Qt::Unchecked);

            // Show health via tooltip/icon if present in indexedOpt row
            QString tip;
            if (!st.watchEnabled) {
                tip = QStringLiteral("Live watching is disabled.");
            } else if (st.watchState == QStringLiteral("watching")) {
                tip = QStringLiteral("Live watching is active.");
            } else if (st.watchState == QStringLiteral("notMounted")) {
                tip = QStringLiteral("Live watching is enabled, but this device is not mounted.");
            } else if (st.watchState == QStringLiteral("error")) {
                tip = st.watchError.trimmed().isEmpty()
                    ? QStringLiteral("Live watching is enabled, but watching failed for this device.")
                    : st.watchError.trimmed();
            } else {
                tip = QStringLiteral("Live watching status is unknown.");
            }

            appendRetryInfoIfPresent(deviceId, st.watchState, tip);

            item->setToolTip(ColWatch, tip);

            const bool warn = st.watchEnabled &&
                              (st.watchState == QStringLiteral("notMounted") || st.watchState == QStringLiteral("error"));
            if (warn) {
                item->setIcon(ColWatch, QIcon::fromTheme(QStringLiteral("dialog-warning")));
            }
        } else {
            item->setText(ColWatch, QStringLiteral("—"));
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        }

        item->setText(ColFs, fsType.isEmpty() ? QStringLiteral("—") : fsType);

        // Prefer live label when available
        const QString shownLabel = liveLabel.isEmpty() ? QStringLiteral("—") : liveLabel;
        item->setText(ColLabel, shownLabel);

        item->setText(ColMount, mounted ? (mp.isEmpty() ? QStringLiteral("(mounted)") : mp) : QStringLiteral("(not mounted)"));
        item->setText(ColEntries, st.indexed ? QLocale().toString(static_cast<qulonglong>(st.entryCount)) : QStringLiteral("—"));
        item->setText(ColLastIndexed, st.indexed ? formatLastIndexedTime(st.lastIndexedTime) : QStringLiteral("—"));
        item->setText(ColDeviceId, deviceId);
        item->setText(ColDevNode, devNode.isEmpty() ? QStringLiteral("—") : devNode);

        // UUID as tooltip (useful, but doesn’t clutter UI)
        if (!liveUuid.isEmpty()) {
            item->setToolTip(ColLabel, QStringLiteral("UUID: %1").arg(liveUuid));
            item->setToolTip(ColDeviceId, QStringLiteral("UUID: %1").arg(liveUuid));
        }

        // Store deviceId for selection logic (don’t rely on text)
        item->setData(ColDeviceId, Qt::UserRole, deviceId);

        if (!mounted) {
            for (int c = 0; c < ColCount; ++c) item->setForeground(c, QBrush(Qt::gray));
        }
    }

    // 2) Indexed-only devices (not currently known)
    for (auto it = idxMap.constBegin(); it != idxMap.constEnd(); ++it) {
        const QString deviceId = it.key();
        const RowState st = it.value();

        if (knownIds.contains(deviceId)) continue;

        auto* item = new QTreeWidgetItem(m_tree);
        item->setText(ColIndexed, QStringLiteral("Yes"));

        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(ColWatch, st.watchEnabled ? Qt::Checked : Qt::Unchecked);

        QString tip;
        if (!st.watchEnabled) {
            tip = QStringLiteral("Live watching is disabled.");
        } else if (st.watchState == QStringLiteral("watching")) {
            tip = QStringLiteral("Live watching is active.");
        } else if (st.watchState == QStringLiteral("notMounted")) {
            tip = QStringLiteral("Live watching is enabled, but this device is not mounted.");
        } else if (st.watchState == QStringLiteral("error")) {
            tip = st.watchError.trimmed().isEmpty()
                ? QStringLiteral("Live watching is enabled, but watching failed for this device.")
                : st.watchError.trimmed();
        } else {
            tip = QStringLiteral("Live watching status is unknown.");
        }

        appendRetryInfoIfPresent(deviceId, st.watchState, tip);

        item->setToolTip(ColWatch, tip);

        const bool warn = st.watchEnabled &&
                          (st.watchState == QStringLiteral("notMounted") || st.watchState == QStringLiteral("error"));
        if (warn) {
            item->setIcon(ColWatch, QIcon::fromTheme(QStringLiteral("dialog-warning")));
        }

        item->setText(ColFs, st.fsType.isEmpty() ? QStringLiteral("—") : st.fsType);

        const QString shownLabel = st.labelLastKnown.isEmpty() ? QStringLiteral("—") : st.labelLastKnown;
        item->setText(ColLabel, shownLabel);

        item->setText(ColMount, QStringLiteral("(not present)"));
        item->setText(ColEntries, QLocale().toString(static_cast<qulonglong>(st.entryCount)));
        item->setText(ColLastIndexed, formatLastIndexedTime(st.lastIndexedTime));
        item->setText(ColDeviceId, deviceId);
        item->setText(ColDevNode, QStringLiteral("—"));

        if (!st.uuidLastKnown.isEmpty()) {
            item->setToolTip(ColLabel, QStringLiteral("UUID: %1").arg(st.uuidLastKnown));
            item->setToolTip(ColDeviceId, QStringLiteral("UUID: %1").arg(st.uuidLastKnown));
        }

        item->setData(ColDeviceId, Qt::UserRole, deviceId);

        // Grey out to indicate it's not currently available as a block device
        for (int c = 0; c < ColCount; ++c) item->setForeground(c, QBrush(Qt::gray));
    }

    m_tree->blockSignals(oldBlocked);
    m_updatingTree = false;

    m_status->setText(QStringLiteral("Loaded %1 device(s).").arg(m_tree->topLevelItemCount()));
    onSelectionChanged();
}

void SettingsDialog::onSelectionChanged() {
    const bool hasSelection = !selectedDeviceId().isEmpty();
    m_startBtn->setEnabled(hasSelection && !m_jobActive);

    // Enable forget only if selected row is indexed
    bool indexed = false;
    if (auto* item = m_tree->currentItem()) {
        indexed = (item->text(ColIndexed).compare(QStringLiteral("Yes"), Qt::CaseInsensitive) == 0);
    }
    m_forgetBtn->setEnabled(hasSelection && indexed && !m_jobActive);
}

void SettingsDialog::onStartOrRescanClicked() {
    if (!m_client || !m_client->isAvailable()) {
        QMessageBox::warning(this, QStringLiteral("Daemon unavailable"),
                             QStringLiteral("The daemon is not available."));
        return;
    }

    const QString deviceId = selectedDeviceId();
    if (deviceId.isEmpty()) return;

    QString err;
    auto jobIdOpt = m_client->startIndex(deviceId, &err);
    if (!jobIdOpt) {
        QMessageBox::warning(this, QStringLiteral("Indexing failed"),
                             err.isEmpty() ? QStringLiteral("Failed to start indexing.") : err);
        return;
    }

    m_jobId = *jobIdOpt;
    setBusy(true, QStringLiteral("Indexing started…"));
    m_progress->setValue(0);
}

void SettingsDialog::onForgetClicked() {
    if (!m_client || !m_client->isAvailable()) {
        QMessageBox::warning(this, QStringLiteral("Daemon unavailable"),
                             QStringLiteral("The daemon is not available."));
        return;
    }

    const QString deviceId = selectedDeviceId();
    if (deviceId.isEmpty()) return;

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("Forget index?"),
        QStringLiteral("Forget the index for:\n\n%1\n\nThis will remove it from memory and delete its snapshot.")
            .arg(deviceId),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) return;

    QString err;
    if (!m_client->forgetIndex(deviceId, &err)) {
        QMessageBox::warning(this,
                             QStringLiteral("Forget failed"),
                             err.isEmpty() ? QStringLiteral("Failed to forget index.") : err);
        return;
    }

    m_status->setText(QStringLiteral("Index forgotten."));

    // Refresh the list and clear selection so action buttons don't remain enabled
    // for a row that no longer exists / is no longer indexed.
    refresh();

    if (m_tree) {
        m_tree->clearSelection();
        m_tree->setCurrentItem(nullptr);
    }
    onSelectionChanged();
}

void SettingsDialog::onCancelClicked() {
    if (!m_client) return;
    if (!m_jobActive || m_jobId == 0) return;

    QString err;
    if (!m_client->cancelJob(m_jobId, &err)) {
        QMessageBox::warning(this, QStringLiteral("Cancel failed"),
                             err.isEmpty() ? QStringLiteral("Failed to cancel job.") : err);
        return;
    }

    m_status->setText(QStringLiteral("Cancelling…"));
}

void SettingsDialog::onDaemonJobProgress(quint64 jobId, quint32 percent, const QVariantMap&) {
    if (!m_jobActive || jobId != m_jobId) return;

    m_progress->setVisible(true);
    m_progress->setValue(static_cast<int>(percent));
    m_status->setText(QStringLiteral("Indexing… %1%").arg(percent));
}

void SettingsDialog::onDaemonJobFinished(quint64 jobId, const QString& status, const QString& message, const QVariantMap&) {
    if (!m_jobActive || jobId != m_jobId) return;

    m_jobId = 0;

    if (status == QStringLiteral("ok")) {
        setBusy(false, QStringLiteral("Indexing complete."));
        refresh();
    } else if (status == QStringLiteral("cancelled")) {
        setBusy(false, QStringLiteral("Indexing cancelled."));
    } else {
        setBusy(false, QStringLiteral("Indexing failed: %1").arg(message));
    }
}

void SettingsDialog::onDeviceIndexUpdated(const QString&, quint64, quint64) {
    // Keep view in sync if fanotify or another client triggers updates.
    if (m_jobActive) return;
    refresh();
}

void SettingsDialog::onDeviceIndexRemoved(const QString&) {
    if (m_jobActive) return;
    refresh();
}

void SettingsDialog::onDaemonVanished(const QString&) {
    if (!m_jobActive) return;

    m_jobActive = false;
    m_jobId = 0;
    setBusy(false, QStringLiteral("Daemon stopped. Indexing aborted."));
    QMessageBox::warning(this, QStringLiteral("Daemon stopped"),
                         QStringLiteral("The indexing daemon stopped while an indexing job was running."));
}