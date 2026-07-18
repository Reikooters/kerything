// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_SERVER_H
#define KERYTHINGD_SERVER_H

#include <QLocalServer>
#include <QObject>
#include <QTimer>

#include "BlockDevice.h"
#include "ClientConnection.h"
#include "DeviceChangeMonitor.h"

class Server : public QObject {
    Q_OBJECT

public:
    explicit Server(QObject* parent = nullptr);

private:
    static std::optional<gid_t> resolveSocketGroupId();
    static bool prepareSocketDirectory(gid_t socketGroupId);
    static bool applySocketPermissions(gid_t socketGroupId);
    static bool blockDeviceListsEqual(
        const std::vector<BlockDevice>& lhs,
        const std::vector<BlockDevice>& rhs
    );
    bool listen();
    bool listenFromSystemd();
    bool listenStandalone();
    void startIdleShutdownTimerIfIdle();
    void stopIdleShutdownTimer();

private Q_SLOTS:
    void onNewConnection();
    void onClientDisconnected(ClientConnection* connection);
    void scheduleKnownDevicesRefresh();
    void refreshKnownDevices();
    void maybeShutdownAfterIdle();

private:
    void broadcastKnownDevices(const std::vector<BlockDevice>& devices);

    QLocalServer server_;
    std::vector<ClientConnection*> clients_;

    DeviceChangeMonitor* deviceChangeMonitor_ = nullptr;
    QTimer knownDevicesRefreshTimer_;
    QTimer idleShutdownTimer_;
    std::vector<BlockDevice> lastKnownDevices_;
};

#endif //KERYTHINGD_SERVER_H
