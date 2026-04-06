// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DAEMONCLIENT_H
#define KERYTHING_DAEMONCLIENT_H

#include "SocketFramer.h"

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QQueue>
#include <QTimer>

#include "FileRecord.h"

class DaemonClient final : public QObject {
    Q_OBJECT

public:
    explicit DaemonClient(QObject* parent = nullptr);
    ~DaemonClient() override;

    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] bool isReady() const noexcept;

    bool sendRequest(Protocol::MessageType type, const QByteArray& payload, quint32* requestIdOut = nullptr);
    bool cancelRequest(quint32 requestId);

signals:
    void connectedChanged(bool connected);
    void readyChanged(bool ready);
    void daemonAvailable();
    void daemonUnavailable();
    void daemonReady();

    void scanStarted(quint32 requestId);
    void scanProgress(quint32 requestId, quint64 filesSeen, quint64 filesEmitted);
    void scanChunkReceived(quint32 requestId, const std::vector<FileRecord>& chunk);
    void scanCompleted(quint32 requestId);
    void scanCancelled(quint32 requestId);
    void scanFailed(quint32 requestId, const QString& errorText);

private slots:
    void tryConnect();
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QLocalSocket::LocalSocketError);

private:
    struct PendingRequest {
        QByteArray bytes;
        quint32 requestId = 0;
    };

    void flushOutgoingQueue();
    void resetConnectionState();
    void setConnectedState(bool connected);
    void setReadyState(bool ready);
    void scheduleReconnect();

    void handleMessage(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleReadyMessage(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanStarted(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanProgress(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanIndexResultChunk(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanCompleted(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanCancelled(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleErrorMessage(const Protocol::MessageHeader& header, const QByteArray& payload);

    QLocalSocket socket_;
    SocketFramer framer_;
    QTimer reconnectTimer_;
    bool connected_ = false;
    bool ready_ = false;
    bool shuttingDown_ = false;

    quint32 nextRequestId_ = 0;
    QHash<quint32, QString> pendingRequests_;
    QQueue<PendingRequest> outgoingQueue_;
};

#endif //KERYTHING_DAEMONCLIENT_H
