// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DAEMONCLIENT_H
#define KERYTHING_DAEMONCLIENT_H

#include "SocketFramer.h"

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QTimer>

#include "BlockDevice.h"
#include "FileRecord.h"
#include "LiveUpdateEvent.h"

class AppController;

class DaemonClient final : public QObject {
    Q_OBJECT

public:
    explicit DaemonClient(AppController* controller, QObject* parent = nullptr);
    ~DaemonClient() override;

    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] bool isReady() const noexcept;

    bool sendRequest(Protocol::MessageType type, const QByteArray& payload, quint32* requestIdOut = nullptr);
    bool cancelRequest(quint32 requestId);
    bool setLiveUpdateDevices(const QStringList& deviceIds);

Q_SIGNALS:
    void connectedChanged(bool connected);
    void readyChanged(bool ready);
    void daemonAvailable();
    void daemonUnavailable();
    void daemonReady();

    void scanStarted(
        quint32 requestId,
        const QString& deviceId,
        const QString& devNode,
        const QString& fsType,
        const QString& label,
        const QStringList& mountPoints,
        const QString& primaryMountPoint
    );
    void scanProgress(quint32 requestId, quint64 filesProcessed, quint64 filesTotal);
    void scanFileRecordChunkReceived(quint32 requestId, const std::vector<FileRecord>& chunk);
    void scanStringPoolChunkReceived(quint32 requestId, QByteArrayView chunk);
    void scanCompleted(quint32 requestId, const QString& deviceId, const QString& devNode, const QString& fsType);
    void scanCancelled(quint32 requestId, const QString& deviceId);
    void scanFailed(quint32 requestId, const QString& errorText);
    void knownDevices(quint32 requestId, const std::vector<BlockDevice>& blockDevices);
    void liveUpdateBatchReceived(
        const QString& deviceId,
        const QString& mountPoint,
        const std::vector<LiveUpdateEvent>& events
    );
    void liveUpdateStatusChanged(
        const QString& deviceId,
        LiveUpdateStatus status,
        const QString& reason
    );
    void liveUpdateOperationBatchReceived(
        const QString& deviceId,
        const QString& mountPoint,
        const std::vector<LiveUpdateOperation>& operations
    );

private Q_SLOTS:
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
    void handleScanIndexResultFileRecordChunk(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanIndexResultStringPoolChunk(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanCompleted(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanCancelled(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleKnownDevices(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleLiveUpdateBatch(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleLiveUpdateStatusChanged(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleLiveUpdateOperationBatch(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleErrorMessage(const Protocol::MessageHeader& header, const QByteArray& payload);

    AppController* controller_ = nullptr;
    QLocalSocket socket_;
    SocketFramer framer_;
    QTimer reconnectTimer_;
    bool connected_ = false;
    bool ready_ = false;
    bool shuttingDown_ = false;

    quint32 nextRequestId_ = 1;
    QHash<quint32, QString> pendingRequests_;
    QQueue<PendingRequest> outgoingQueue_;
};

#endif //KERYTHING_DAEMONCLIENT_H
