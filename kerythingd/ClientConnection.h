// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_CLIENTCONNECTION_H
#define KERYTHINGD_CLIENTCONNECTION_H

#include "ScanJob.h"
#include "SocketFramer.h"
#include "Protocol.h"
#include "ScannerWorker.h"
#include "FileRecord.h"

#include <QObject>
#include <QPointer>
#include <QLocalSocket>
#include <QHash>
#include <QThread>
#include <memory>

class ClientConnection final : public QObject {
    Q_OBJECT

public:
    explicit ClientConnection(QLocalSocket* socket, QObject* parent = nullptr);
    ~ClientConnection() override;

    void sendReady();
    void sendScanStarted(quint32 requestId, const QString& devicePath, const QString& fsType);
    void sendScanProgress(quint32 requestId, quint64 filesSeen, quint64 filesEmitted);
    bool sendScanFileRecordChunk(quint32 requestId, const std::vector<FileRecord>& fileRecordChunk);
    bool sendScanStringPoolChunk(quint32 requestId, const std::vector<char>& stringPoolChunk);
    void sendScanCompleted(quint32 requestId);
    void sendScanCancelled(quint32 requestId);
    void sendError(quint32 requestId, const QString& errorText);

Q_SIGNALS:
    void disconnected(ClientConnection* connection);

private Q_SLOTS:
    void onReadyRead();
    void onDisconnected();

private:
    void handleFrame(const Protocol::MessageHeader& header, const QByteArray& payload);
    void handleScanDevice(quint32 requestId, const QByteArray& payload);
    void handleCancelRequest(quint32 requestId);

    bool sendFrame(Protocol::MessageType type, quint32 requestId, const QByteArray& payload) const;
    QByteArray encodeFrame(Protocol::MessageType type, quint32 requestId, const QByteArray& payload) const;
    void cancelAllJobs();

    QPointer<QLocalSocket> socket_;
    SocketFramer framer_;

    QThread scanThread_;
    ScannerWorker* scannerWorker_ = nullptr;

    QHash<quint32, std::shared_ptr<ScanJob>> activeJobs_;
    bool shuttingDown_ = false;
};

#endif //KERYTHINGD_CLIENTCONNECTION_H
