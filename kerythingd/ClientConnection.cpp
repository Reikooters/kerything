// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ClientConnection.h"

#include "ScannerWorker.h"
#include "FileRecord.h"
#include "BlockDeviceHelper.h"
#include "BlockDevice.h"

#include <iostream>
#include <QDataStream>
#include <QMetaObject>

ClientConnection::ClientConnection(QLocalSocket* socket, QObject* parent)
    : QObject(parent),
      socket_(socket)
{
    Q_ASSERT(socket_);

    connect(socket_, &QLocalSocket::readyRead,
            this, &ClientConnection::onReadyRead);

    connect(socket_, &QLocalSocket::disconnected,
            this, &ClientConnection::onDisconnected);

    scannerWorker_ = new ScannerWorker();
    scannerWorker_->moveToThread(&scanThread_);

    connect(&scanThread_, &QThread::finished,
            scannerWorker_, &QObject::deleteLater);

    connect(scannerWorker_, &ScannerWorker::scanStarted,
            this, [this](
                quint32 requestId,
                const QString& deviceId,
                const QString& devNode,
                const QString& fsType,
                const QString& label,
                const QStringList& mountPoints,
                const QString& primaryMountPoint
            ) {
                sendScanStarted(
                    requestId,
                    deviceId,
                    devNode,
                    fsType,
                    label,
                    mountPoints,
                    primaryMountPoint
                );
            });

    connect(scannerWorker_, &ScannerWorker::scanProgress,
            this, [this](quint32 requestId, const Protocol::ScanProgress& progress) {
                sendScanProgress(requestId, progress);
            });

    connect(scannerWorker_, &ScannerWorker::scanFileRecordChunkReady,
            this, [this](quint32 requestId, const std::vector<FileRecord>& fileRecordChunk) {
                sendScanFileRecordChunk(requestId, fileRecordChunk);
            });

    connect(scannerWorker_, &ScannerWorker::scanStringPoolChunkReady,
            this, [this](quint32 requestId, const std::vector<char>& stringPoolChunk) {
                sendScanStringPoolChunk(requestId, stringPoolChunk);
            });

    connect(scannerWorker_, &ScannerWorker::scanCompleted,
            this, [this](quint32 requestId, const QString& deviceId, const QString& devNode, const QString& fsType) {
                activeJobs_.remove(requestId);
                sendScanCompleted(requestId, deviceId, devNode, fsType);
            });

    connect(scannerWorker_, &ScannerWorker::scanCancelled,
            this, [this](quint32 requestId, const QString& deviceId) {
                activeJobs_.remove(requestId);
                sendScanCancelled(requestId, deviceId);
            });

    connect(scannerWorker_, &ScannerWorker::scanError,
            this, [this](quint32 requestId, const QString& errorText) {
                activeJobs_.remove(requestId);
                sendError(requestId, errorText);
            });

    scanThread_.start();
}

ClientConnection::~ClientConnection()
{
    shuttingDown_ = true;
    cancelAllJobs();

    scanThread_.quit();
    scanThread_.wait();

    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

void ClientConnection::onReadyRead()
{
    if (shuttingDown_ || !socket_) {
        return;
    }

    framer_.append(socket_->readAll());

    Protocol::MessageFrame frame;
    while (framer_.tryTake(frame)) {
        handleFrame(frame.header, frame.payload);
    }
}

void ClientConnection::onDisconnected()
{
    if (shuttingDown_) {
        return;
    }

    shuttingDown_ = true;
    cancelAllJobs();

    Q_EMIT disconnected(this);
    deleteLater();
}

void ClientConnection::handleFrame(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    const auto type = static_cast<Protocol::MessageType>(header.type);

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Received message: type: " << header.type << ", requestId: " << header.requestId << ", payloadSize: " << header.payloadSize << "\n";
#endif

    switch (type) {
        case Protocol::MessageType::ScanDevice:
            handleScanDevice(header.requestId, payload);
            break;

        case Protocol::MessageType::CancelRequest:
            handleCancelRequest(header.requestId);
            break;

        case Protocol::MessageType::ListKnownDevices:
            handleListKnownDevices(header.requestId);
            break;

        case Protocol::MessageType::SetLiveUpdateDevices:
            handleSetLiveUpdateDevices(payload);
            break;

        default:
            sendError(header.requestId, QStringLiteral("unknown request type"));
            break;
    }
}

void ClientConnection::handleScanDevice(quint32 requestId, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    in >> deviceId;

    if (in.status() != QDataStream::Ok) {
        sendError(requestId, QStringLiteral("malformed ScanDevice payload"));
        return;
    }

    const std::optional<BlockDevice> device = BlockDeviceHelper::findKnownDeviceById(deviceId);
    if (!device) {
        sendError(
            requestId,
            QStringLiteral("unknown or unsupported deviceId: %1").arg(deviceId)
        );
        return;
    }

    auto job = std::make_shared<ScanJob>();
    job->requestId = requestId;
    job->deviceId = device->deviceId;
    job->devNode = device->devNode;
    job->fsType = device->fsType;
    job->label = device->label;
    job->mountPoints = device->mountPoints;
    job->primaryMountPoint = device->primaryMountPoint;

    activeJobs_.insert(requestId, job);

    // Run the scan in the worker thread.
    QMetaObject::invokeMethod(scannerWorker_, [this, job]() {
        scannerWorker_->startScan(job);
    }, Qt::QueuedConnection);
}

void ClientConnection::handleCancelRequest(quint32 requestId)
{
    const auto it = activeJobs_.find(requestId);
    if (it == activeJobs_.end()) {
        sendError(requestId, QStringLiteral("no active job to cancel"));
        return;
    }

    it.value()->cancelled.store(true, std::memory_order_relaxed);
}

void ClientConnection::handleListKnownDevices(quint32 requestId)
{
    const auto devices = BlockDeviceHelper::listKnownDevices();
    sendKnownDevices(requestId, devices);
}

void ClientConnection::handleSetLiveUpdateDevices(const QByteArray& payload)
{
    const std::optional<QStringList> parsed =
        Protocol::parseSetLiveUpdateDevicesPayload(payload);

    if (!parsed) {
        sendError(0, QStringLiteral("malformed SetLiveUpdateDevices payload"));
        return;
    }

    liveUpdateDeviceIds_ = *parsed;
    liveUpdateDeviceIds_.removeDuplicates();
    liveUpdateDeviceIds_.removeAll(QString{});

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Client live update device subscription changed count="
              << liveUpdateDeviceIds_.size()
              << "\n";

    for (const QString& deviceId : liveUpdateDeviceIds_) {
        std::cout << "  liveUpdateDeviceId="
                  << deviceId.toStdString()
                  << "\n";
    }
#endif

    Q_EMIT liveUpdateDevicesChanged();
}

QStringList ClientConnection::liveUpdateDeviceIds() const
{
    return liveUpdateDeviceIds_;
}

void ClientConnection::sendReady()
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);
    out << Protocol::Version << true;

    sendFrame(Protocol::MessageType::Ready, 0, payload);
}

void ClientConnection::sendScanStarted(
        quint32 requestId,
        const QString& deviceId,
        const QString& devNode,
        const QString& fsType,
        const QString& label,
        const QStringList& mountPoints,
        const QString& primaryMountPoint)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceId
        << devNode
        << fsType
        << label
        << mountPoints
        << primaryMountPoint;

    sendFrame(Protocol::MessageType::ScanStarted, requestId, payload);
}

void ClientConnection::sendScanProgress(quint32 requestId,
                                        const Protocol::ScanProgress& progress)
{
    sendFrame(
        Protocol::MessageType::ScanProgress,
        requestId,
        Protocol::makeScanProgressPayload(progress)
    );
}

bool ClientConnection::sendScanFileRecordChunk(quint32 requestId, const std::vector<FileRecord>& fileRecordChunk)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << static_cast<quint32>(fileRecordChunk.size());

    for (const auto& rec : fileRecordChunk) {
        out << rec.fsIndex
            << rec.parentFsIndex
            << rec.parentRecordIdx
            << rec.size
            << rec.modificationTime
            << rec.nameOffset
            << rec.nameLen
            << rec.flags;
    }

    return sendFrame(Protocol::MessageType::ScanIndexResultFileRecordChunk, requestId, payload);
}

bool ClientConnection::sendScanStringPoolChunk(quint32 requestId, const std::vector<char>& stringPoolChunk)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out.writeRawData(stringPoolChunk.data(), stringPoolChunk.size());

    return sendFrame(Protocol::MessageType::ScanIndexResultStringPoolChunk, requestId, payload);
}

void ClientConnection::sendScanCompleted(
    quint32 requestId,
    const QString& deviceId,
    const QString& devNode,
    const QString& fsType)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceId
        << devNode
        << fsType;

    sendFrame(Protocol::MessageType::ScanCompleted, requestId, payload);
}

void ClientConnection::sendScanCancelled(quint32 requestId, const QString& deviceId)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceId;

    sendFrame(Protocol::MessageType::ScanCancelled, requestId, payload);
}

void ClientConnection::sendKnownDevices(
    quint32 requestId,
    const std::vector<BlockDevice>& devices)
{
    sendFrame(
        Protocol::MessageType::KnownDevices,
        requestId,
        Protocol::makeKnownDevicesPayload(devices)
    );
}

void ClientConnection::sendLiveUpdateBatch(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateEvent>& events)
{
    sendFrame(
        Protocol::MessageType::LiveUpdateBatch,
        0,
        Protocol::makeLiveUpdateBatchPayload(deviceId, mountPoint, events)
    );
}

void ClientConnection::sendLiveUpdateStatusChanged(
    const QString& deviceId,
    LiveUpdateStatus status,
    const QString& reason)
{
    sendFrame(
        Protocol::MessageType::LiveUpdateStatusChanged,
        0,
        Protocol::makeLiveUpdateStatusChangedPayload(deviceId, status, reason)
    );
}

void ClientConnection::sendLiveUpdateOperationBatch(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateOperation>& operations)
{
    sendFrame(
        Protocol::MessageType::LiveUpdateOperationBatch,
        0,
        Protocol::makeLiveUpdateOperationBatchPayload(deviceId, mountPoint, operations)
    );
}

void ClientConnection::sendError(quint32 requestId, const QString& errorText)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << errorText;

    sendFrame(Protocol::MessageType::Error, requestId, payload);
}

bool ClientConnection::sendFrame(const Protocol::MessageType type, const quint32 requestId, const QByteArray& payload) const
{
    if (shuttingDown_ || !socket_) {
        return false;
    }

    const QByteArray bytes = encodeFrame(type, requestId, payload);
    const qint64 written = socket_->write(bytes);

    if (written != bytes.size()) {
        return false;
    }

    socket_->flush();
    return true;
}

QByteArray ClientConnection::encodeFrame(Protocol::MessageType type,
                                        quint32 requestId,
                                        const QByteArray& payload) const
{
    Protocol::MessageHeader header{};
    header.type = static_cast<quint16>(type);
    header.requestId = requestId;
    header.payloadSize = static_cast<quint32>(payload.size());

    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << header.magic
        << header.version
        << header.type
        << header.requestId
        << header.payloadSize;

    bytes.append(payload);
    return bytes;
}

void ClientConnection::cancelAllJobs()
{
    for (auto& job : activeJobs_) {
        if (job) {
            job->cancelled.store(true, std::memory_order_relaxed);
        }
    }

    activeJobs_.clear();
}