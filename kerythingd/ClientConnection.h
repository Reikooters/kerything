// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_CLIENTCONNECTION_H
#define KERYTHINGD_CLIENTCONNECTION_H

#include "../shared/Protocol.h"
#include "../shared/SocketFramer.h"

#include <iostream>

#include <QLocalSocket>
#include <QObject>
#include <QPointer>

class ClientConnection final : public QObject {
    Q_OBJECT
public:
    explicit ClientConnection(QLocalSocket* socket, QObject* parent = nullptr);

    [[nodiscard]] bool isAlive() const;
    void send(const QByteArray& message) const;

signals:
    void disconnected(ClientConnection* connection);

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    static void handleMessage(const Protocol::MessageHeader& header, const QByteArray& payload);

    QPointer<QLocalSocket> socket_;
    SocketFramer framer_;
};

#endif //KERYTHINGD_CLIENTCONNECTION_H
