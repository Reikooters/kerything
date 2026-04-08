// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DaemonClient.h"

#include <QDataStream>
#include <iostream>

#include "FileRecord.h"

DaemonClient::DaemonClient(QObject* parent)
    : QObject(parent)
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
    std::cout << "Connected to daemon transport\n";
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
    emit connectedChanged(connected_);

    if (connected_) {
        emit daemonAvailable();
    } else {
        emit daemonUnavailable();
    }
}

void DaemonClient::setReadyState(bool ready)
{
    if (ready_ == ready) {
        return;
    }

    ready_ = ready;
    emit readyChanged(ready_);

    if (ready_) {
        emit daemonReady();
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

        case Protocol::MessageType::ScanIndexResultChunk:
            handleScanIndexResultChunk(header, payload);
            break;

        case Protocol::MessageType::ScanCompleted:
            handleScanCompleted(header, payload);
            break;

        case Protocol::MessageType::ScanCancelled:
            handleScanCancelled(header, payload);
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

    std::cout << "Received READY response: Version="
              << protocolVersion
              << ", ReadyState="
              << daemonReady
              << "\n";

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
        std::cout << "Daemon is ready\n";
    } else {
        std::cout << "Daemon is connected but not ready yet\n";
        setReadyState(false);
    }
}

void DaemonClient::handleScanStarted(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    Q_UNUSED(payload);

    std::cout << "Scan started for requestId=" << header.requestId << "\n";
    emit scanStarted(header.requestId);
}

void DaemonClient::handleScanProgress(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    quint64 filesSeen = 0;
    quint64 filesEmitted = 0;
    in >> filesSeen >> filesEmitted;

    if (in.status() != QDataStream::Ok) {
        std::cerr << "Malformed ScanProgress payload\n";
        return;
    }

    std::cout << "Scan progress requestId=" << header.requestId
              << " seen=" << filesSeen
              << " emitted=" << filesEmitted << "\n";

    emit scanProgress(header.requestId, filesSeen, filesEmitted);
}

void DaemonClient::handleScanIndexResultChunk(const Protocol::MessageHeader& header, const QByteArray& payload)
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

    for (quint32 i = 0; i < count; ++i) {
        // FileRecord rec;
        // in >> rec.path >> rec.size >> rec.mtime;
        //
        // if (in.status() != QDataStream::Ok) {
        //     std::cerr << "Malformed ScanIndexResultChunk payload body\n";
        //     return;
        // }
        //
        // chunk.push_back(std::move(rec));
    }

    std::cout << "Received chunk requestId=" << header.requestId
              << " count=" << chunk.size() << "\n";

    // TODO: Update the GUI-side index here.
    // For now this is just a signal.
    emit scanChunkReceived(header.requestId, chunk);
}

void DaemonClient::handleScanCompleted(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    Q_UNUSED(payload);

    std::cout << "Scan completed requestId=" << header.requestId << "\n";

    pendingRequests_.remove(header.requestId);
    emit scanCompleted(header.requestId);
}

void DaemonClient::handleScanCancelled(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    Q_UNUSED(payload);

    std::cout << "Scan cancelled requestId=" << header.requestId << "\n";

    pendingRequests_.remove(header.requestId);
    emit scanCancelled(header.requestId);
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

    emit scanFailed(header.requestId, errorText);
}