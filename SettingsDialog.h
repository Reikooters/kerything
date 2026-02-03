// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SETTINGSDIALOG_H
#define KERYTHING_SETTINGSDIALOG_H

#include <QDialog>
#include <QTreeWidget>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QtDBus/QDBusServiceWatcher>
#include <memory>

class DbusIndexerClient;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(DbusIndexerClient* dbusClient, QWidget* parent = nullptr);

private slots:
    void refresh();

    void onStartOrRescanClicked();
    void onForgetClicked();
    void onCancelClicked();

    void onDaemonJobProgress(quint64 jobId, quint32 percent, const QVariantMap& props);
    void onDaemonJobFinished(quint64 jobId, const QString& status, const QString& message, const QVariantMap& props);
    void onDeviceIndexUpdated(const QString& deviceId, quint64 generation, quint64 entryCount);
    void onDeviceIndexRemoved(const QString& deviceId);

    void onDaemonVanished(const QString& serviceName);
    void onSelectionChanged();

private:
    struct RowState {
        QString deviceId;
        QString fsType;
        QString labelLastKnown;
        QString uuidLastKnown;
        bool indexed = false;
        quint64 entryCount = 0;
        qint64 lastIndexedTime = 0;
    };

    void connectDaemonSignals();
    void setBusy(bool busy, const QString& text = QString());

    [[nodiscard]] QString selectedDeviceId() const;
    [[nodiscard]] static QString formatLastIndexedTime(qint64 unixSeconds);

    DbusIndexerClient* m_client = nullptr;

    QTreeWidget* m_tree = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;

    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_startBtn = nullptr;
    QPushButton* m_forgetBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    bool m_daemonSignalsConnected = false;
    QDBusServiceWatcher* m_daemonWatcher = nullptr;

    bool m_jobActive = false;
    quint64 m_jobId = 0;
};

#endif //KERYTHING_SETTINGSDIALOG_H