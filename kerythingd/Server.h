// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_SERVER_H
#define KERYTHINGD_SERVER_H

#include <QLocalServer>
#include <QObject>

#include "ClientConnection.h"

class Server : public QObject {
    Q_OBJECT

public:
    explicit Server(QObject* parent = nullptr);

private:
    static std::optional<gid_t> resolveSocketGroupId();
    static bool prepareSocketDirectory(gid_t socketGroupId);
    static bool applySocketPermissions(gid_t socketGroupId);

private Q_SLOTS:
    void onNewConnection();
    void onClientDisconnected(ClientConnection* connection);

private:
    QLocalServer server_;
    std::vector<ClientConnection*> clients_;
};

#endif //KERYTHINGD_SERVER_H
