// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DaemonClient.h"

#include <QDataStream>
#include <iostream>

#include "AppController.h"
#include "FileRecord.h"

DaemonClient::DaemonClient(AppController* controller, QObject* parent)
    : QObject(parent),
      controller_(controller)
{
    connect(&socket_, &QLocalSocket::connected, this, &DaemonClient::onConnected);
    connect(&socket_, &QLocalSocket::disconnected, this, &DaemonClient::onDisconnected);
    connect(&socket_, &QLocalSocket::readyRead, this, &DaemonClient::onReadyRead);
    connect(&socket_, &QLocalSocket::errorOccurred, this, &DaemonClient::onError);

    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout,
            this, &DaemonClient::tryConnect);

    tryConnect();
}

DaemonClient::~DaemonClient() {
    shuttingDown_ = true;
    reconnectTimer_.stop();
    connected_ = false;
    ready_ = false;

    if (socket_.state() != QLocalSocket::UnconnectedState) {
        socket_.disconnectFromServer();
        socket_.abort();
    }
}

[[nodiscard]] bool DaemonClient::isConnected() const noexcept
{
    if (shuttingDown_) {
        return false;
    }

    return connected_;
}

[[nodiscard]] bool DaemonClient::isReady() const noexcept
{
    if (shuttingDown_) {
        return false;
    }

    return ready_;
}

bool DaemonClient::sendRequest(Protocol::MessageType type, const QByteArray& payload, quint32* requestIdOut)
{
    if (shuttingDown_) {
        return false;
    }

    const quint32 requestId = nextRequestId_++;

    if (requestIdOut) {
        *requestIdOut = requestId;
    }

    PendingRequest req;
    req.requestId = requestId;
    req.bytes = Protocol::packMessage(type, requestId, payload);;

    pendingRequests_.insert(requestId, QStringLiteral("pending"));

    if (!connected_ || !ready_) {
        outgoingQueue_.enqueue(std::move(req));
        return true;
    }

    const qint64 written = socket_.write(req.bytes);
    if (written != req.bytes.size()) {
        outgoingQueue_.enqueue(std::move(req));
        return false;
    }

    return true;
}

bool DaemonClient::cancelRequest(quint32 requestId)
{
    if (shuttingDown_) {
        return false;
    }

    Protocol::MessageHeader header{};
    header.type = static_cast<quint16>(Protocol::MessageType::CancelRequest);
    header.requestId = requestId;
    header.payloadSize = 0;

    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << header.magic
        << header.version
        << header.type
        << header.requestId
        << header.payloadSize;

    if (!connected_ || !ready_) {
        PendingRequest req;
        req.requestId = requestId;
        req.bytes = bytes;
        outgoingQueue_.enqueue(std::move(req));
        return true;
    }

    return socket_.write(bytes) == bytes.size();
}

void DaemonClient::tryConnect()
{
    if (shuttingDown_) {
        return;
    }

    if (connected_) {
        return;
    }

    if (socket_.state() != QLocalSocket::UnconnectedState) {
        socket_.abort();
    }

    socket_.connectToServer(Protocol::ServerName);
}

void DaemonClient::onConnected()
{
    if (shuttingDown_) {
        return;
    }

    setConnectedState(true);

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Connected to daemon transport\n";
#endif
}

void DaemonClient::onDisconnected()
{
    if (shuttingDown_) {
        return;
    }

    resetConnectionState();
    scheduleReconnect();
}

void DaemonClient::onReadyRead()
{
    if (shuttingDown_) {
        return;
    }

    framer_.append(socket_.readAll());

    Protocol::MessageFrame frame;
    while (framer_.tryTake(frame)) {
        handleMessage(frame.header, frame.payload);
    }
}

void DaemonClient::onError(QLocalSocket::LocalSocketError)
{
    if (shuttingDown_) {
        return;
    }

    // Connection failures and runtime disconnects both funnel through here.
    // Don't spam the log if the daemon simply isn't up yet; just retry.
    if (!connected_) {
        scheduleReconnect();
        return;
    }

    std::cerr << "Daemon socket error: "
              << socket_.errorString().toStdString()
              << "\n";

    resetConnectionState();
    scheduleReconnect();
}

void DaemonClient::flushOutgoingQueue()
{
    if (shuttingDown_ || !connected_ || !ready_) {
        return;
    }

    while (!outgoingQueue_.isEmpty()) {
        const PendingRequest req = outgoingQueue_.head();

        const qint64 written = socket_.write(req.bytes);
        if (written != req.bytes.size()) {
            socket_.flush();
            return;
        }

        outgoingQueue_.dequeue();
    }

    socket_.flush();
}

void DaemonClient::resetConnectionState()
{
    setReadyState(false);
    setConnectedState(false);

    framer_ = SocketFramer{};
    pendingRequests_.clear();
    outgoingQueue_.clear();
}

void DaemonClient::setConnectedState(bool connected)
{
    if (connected_ == connected) {
        return;
    }

    connected_ = connected;
    Q_EMIT connectedChanged(connected_);

    if (connected_) {
        Q_EMIT daemonAvailable();
    } else {
        Q_EMIT daemonUnavailable();
    }
}

void DaemonClient::setReadyState(bool ready)
{
    if (ready_ == ready) {
        return;
    }

    ready_ = ready;
    Q_EMIT readyChanged(ready_);

    if (ready_) {
        Q_EMIT daemonReady();
        flushOutgoingQueue();
    }
}

void DaemonClient::scheduleReconnect()
{
    if (shuttingDown_) {
        return;
    }

    if (connected_) {
        return;
    }

    if (!reconnectTimer_.isActive()) {
        reconnectTimer_.start(1000);
    }
}

void DaemonClient::handleMessage(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    const auto type = static_cast<Protocol::MessageType>(header.type);

    switch (type) {
        case Protocol::MessageType::Ready:
            handleReadyMessage(header, payload);
            break;

        case Protocol::MessageType::ScanStarted:
            handleScanStarted(header, payload);
            break;

        case Protocol::MessageType::ScanProgress:
            handleScanProgress(header, payload);
            break;

        case Protocol::MessageType::ScanIndexResultFileRecordChunk:
            handleScanIndexResultFileRecordChunk(header, payload);
            break;

        case Protocol::MessageType::ScanIndexResultStringPoolChunk:
            handleScanIndexResultStringPoolChunk(header, payload);
            break;

        case Protocol::MessageType::ScanCompleted:
            handleScanCompleted(header, payload);
            break;

        case Protocol::MessageType::ScanCancelled:
            handleScanCancelled(header, payload);
            break;

        case Protocol::MessageType::KnownDevices:
            handleKnownDevices(header, payload);
            break;

        case Protocol::MessageType::LiveUpdateBatch:
            handleLiveUpdateBatch(header, payload);
            break;

        case Protocol::MessageType::LiveUpdateStatusChanged:
            handleLiveUpdateStatusChanged(header, payload);
            break;

        case Protocol::MessageType::Error:
            handleErrorMessage(header, payload);
            break;

        default:
            std::cerr << "Unknown message type: " << header.type << "\n";
            break;
    }
}

void DaemonClient::handleReadyMessage(const Protocol::MessageHeader&, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    quint16 protocolVersion = 0;
    bool daemonReady = false;

    in >> protocolVersion >> daemonReady;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed READY payload\n";
        socket_.disconnectFromServer();
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Received READY response: ProtocolVersion="
              << protocolVersion
              << ", ReadyState="
              << daemonReady
              << "\n";
#endif

    if (protocolVersion != Protocol::Version) {
        std::cerr << "Protocol version mismatch: expected "
                  << Protocol::Version
                  << ", got "
                  << protocolVersion
                  << "\n";

        resetConnectionState();
        socket_.disconnectFromServer();
        socket_.abort();
        return;
    }

    if (daemonReady) {
        setReadyState(true);
#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "Daemon is ready\n";
#endif
    } else {
#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "Daemon is connected but not ready yet\n";
#endif
        setReadyState(false);
    }
}

void DaemonClient::handleScanStarted(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    QString devNode;
    QString fsType;
    QString label;
    QStringList mountPoints;
    QString primaryMountPoint;

    in >> deviceId
       >> devNode
       >> fsType
       >> label
       >> mountPoints
       >> primaryMountPoint;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed ScanStarted payload\n";
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Scan started for requestId=" << header.requestId
              << " deviceId=" << deviceId.toStdString()
              << " devNode=" << devNode.toStdString()
              << " fsType=" << fsType.toStdString()
              << " label=" << label.toStdString()
              << " primaryMountPoint=" << primaryMountPoint.toStdString()
              << "\n";
#endif

    Q_EMIT scanStarted(
        header.requestId,
        deviceId,
        devNode,
        fsType,
        label,
        mountPoints,
        primaryMountPoint
    );
}

void DaemonClient::handleScanProgress(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    quint64 filesProcessed = 0;
    quint64 filesTotal = 0;
    in >> filesProcessed >> filesTotal;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed ScanProgress payload\n";
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Scan progress requestId=" << header.requestId
              << " processed=" << filesProcessed
              << " total=" << filesTotal << "\n";
#endif

    Q_EMIT scanProgress(header.requestId, filesProcessed, filesTotal);
}

void DaemonClient::handleScanIndexResultFileRecordChunk(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 count = 0;
    in >> count;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed ScanIndexResultChunk payload header\n";
        return;
    }

    std::vector<FileRecord> chunk;
    chunk.reserve(count);

    if (count == 0) {
        return;
    }

    if (count > (payload.size() - sizeof(quint32)) / sizeof(FileRecord)) {
        std::cerr << "Invalid ScanIndexResultChunk payload size\n";
        return;
    }

    for (quint32 i = 0; i < count; ++i) {
        FileRecord rec{};
        in >> rec.fsIndex
           >> rec.parentFsIndex
           >> rec.parentRecordIdx
           >> rec.size
           >> rec.modificationTime
           >> rec.nameOffset
           >> rec.nameLen
           >> rec.flags;

        chunk.push_back(rec);
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Received file record chunk requestId=" << header.requestId
              << " count=" << chunk.size() << "\n";
#endif

    Q_EMIT scanFileRecordChunkReceived(header.requestId, chunk);
}

void DaemonClient::handleScanIndexResultStringPoolChunk(const Protocol::MessageHeader& header, const QByteArray& payload)
{
#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Received string pool chunk requestId=" << header.requestId
              << " length=" << payload.size() << "\n";
#endif

    Q_EMIT scanStringPoolChunkReceived(header.requestId, QByteArrayView(payload));
}

void DaemonClient::handleScanCompleted(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    QString devNode;
    QString fsType;
    in >> deviceId >> devNode >> fsType;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed ScanCompleted payload\n";
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Scan completed for requestId=" << header.requestId
              << " deviceId=" << deviceId.toStdString()
              << " devNode=" << devNode.toStdString()
              << " fsType=" << fsType.toStdString()
              << "\n";
#endif

    pendingRequests_.remove(header.requestId);
    Q_EMIT scanCompleted(header.requestId, deviceId, devNode, fsType);
}

void DaemonClient::handleScanCancelled(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    in >> deviceId;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed ScanCompleted payload\n";
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Scan cancelled for requestId=" << header.requestId
              << " deviceId=" << deviceId.toStdString()
              << "\n";
#endif

    pendingRequests_.remove(header.requestId);
    Q_EMIT scanCancelled(header.requestId, deviceId);
}

void DaemonClient::handleKnownDevices(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    const std::optional<std::vector<BlockDevice>> blockDevices =
            Protocol::parseKnownDevicesPayload(payload);

    if (!blockDevices) {
        std::cerr << "Malformed KnownDevices payload\n";
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Received block devices requestId=" << header.requestId
              << " count=" << blockDevices->size() << "\n";
#endif

    Q_EMIT knownDevices(header.requestId, *blockDevices);
}

void DaemonClient::handleLiveUpdateBatch(const Protocol::MessageHeader&, const QByteArray& payload)
{
    const auto parsed = Protocol::parseLiveUpdateBatchPayload(payload);

    if (!parsed) {
        std::cerr << "Malformed LiveUpdateBatch payload\n";
        return;
    }

    const auto& [deviceId, mountPoint, events] = *parsed;

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "GUI: received live update batch"
              << " deviceId=" << deviceId.toStdString()
              << " mountPoint=" << mountPoint.toStdString()
              << " count=" << events.size()
              << "\n";
#endif

    Q_EMIT liveUpdateBatchReceived(deviceId, mountPoint, events);
}

void DaemonClient::handleLiveUpdateStatusChanged(const Protocol::MessageHeader&, const QByteArray& payload)
{
    const auto parsed = Protocol::parseLiveUpdateStatusChangedPayload(payload);

    if (!parsed) {
        std::cerr << "Malformed LiveUpdateStatusChanged payload\n";
        return;
    }

    const auto& [deviceId, status, reason] = *parsed;

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "GUI: received live update status"
              << " deviceId=" << deviceId.toStdString()
              << " status=" << liveUpdateStatusToString(status).toStdString()
              << " reason=" << reason.toStdString()
              << "\n";
#endif

    Q_EMIT liveUpdateStatusChanged(deviceId, status, reason);
}

void DaemonClient::handleErrorMessage(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString errorText;
    in >> errorText;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed Error payload\n";
        return;
    }

    std::cerr << "Server error requestId=" << header.requestId
              << ": " << errorText.toStdString() << "\n";

    Q_EMIT scanFailed(header.requestId, errorText);
}