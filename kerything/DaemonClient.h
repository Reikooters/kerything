// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DAEMONCLIENT_H
#define KERYTHING_DAEMONCLIENT_H

#include "../shared/SocketFramer.h"

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

class DaemonClient final : public QObject {
    Q_OBJECT

public:
    explicit DaemonClient(QObject* parent = nullptr);
    ~DaemonClient();

    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] bool isReady() const noexcept;

signals:
    void connectedChanged(bool connected);
    void readyChanged(bool ready);
    void daemonAvailable();
    void daemonUnavailable();
    void daemonReady();

private slots:
    void tryConnect();
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QLocalSocket::LocalSocketError);

private:
    void resetConnectionState();
    void setConnectedState(bool connected);
    void setReadyState(bool ready);
    void scheduleReconnect();

    void handleMessage(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleReadyMessage(const Protocol::MessageHeader& header, const QByteArray& payload);

    QLocalSocket socket_;
    SocketFramer framer_;
    QTimer reconnectTimer_;
    bool connected_ = false;
    bool ready_ = false;
    bool shuttingDown_ = false;

    quint32 nextRequestId_ = 0;
    QHash<quint32, QString> pendingRequests_;
};

#endif //KERYTHING_DAEMONCLIENT_H
