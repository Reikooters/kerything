// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_APPCONTROLLER_H
#define KERYTHING_APPCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QList>
#include <QHash>
#include <QSet>

#include "DaemonClient.h"
#include "IndexController.h"
#include "Preferences.h"

class QApplication;
class MainWindow;
class SingleInstanceServer;

class AppController final : public QObject {
    Q_OBJECT

public:
    explicit AppController(QApplication& app, QObject* parent = nullptr);

    bool start();
    void openNewWindow();
    void requestRefreshAllWindows();
    void requestWindowStatusMessage(const QString& message, int timeoutMs);
    [[nodiscard]] bool isDaemonConnected() const noexcept;
    [[nodiscard]] bool isDaemonReady() const noexcept;
    IndexController* indexController() const noexcept;

private Q_SLOTS:
    void onPrimaryRequestedOpenWindow();
    void onPrimaryRequestedCommand(const QString& command);

private:
    void cleanupWindows();
    void requestKnownDevices();
    void scanEnabledKnownDevices(const std::vector<BlockDevice>& blockDevices);
    void maybeShowFirstRunDevicePicker(const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] bool validateScanDeviceId(quint32 requestId, const QString& actualDeviceId, const char* eventName) const;
    QString takeTrackedScanDeviceId(quint32 requestId, const QString& fallbackDeviceId = {});

    QApplication& app_;
    SingleInstanceServer* instanceServer_ = nullptr;
    DaemonClient* daemonClient_ = nullptr;
    IndexController* indexController_ = nullptr;
    Preferences preferences_;

    // requestId -> stable deviceId
    QHash<quint32, QString> scanRequestDeviceIds_;

    // stable deviceIds currently queued/scanning
    QSet<QString> activeScanDeviceIds_;

    QList<QPointer<MainWindow>> windows_;
};

#endif // KERYTHING_APPCONTROLLER_H