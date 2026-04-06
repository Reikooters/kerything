// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DaemonClient.h"

#include <QDataStream>
#include <iostream>

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

void DaemonClient::resetConnectionState()
{
    setReadyState(false);
    setConnectedState(false);

    framer_ = SocketFramer{};
    pendingRequests_.clear();
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
        case Protocol::MessageType::Ready: {
            handleReadyMessage(header, payload);
            break;
        }

        // case Protocol::MessageType::FileData: {
        //     if (!ready_) {
        //         std::cerr << "Ignoring FileData before READY\n";
        //         break;
        //     }
        //
        //     QDataStream in(payload);
        //     in.setByteOrder(QDataStream::BigEndian);
        //     in.setVersion(QDataStream::Qt_6_0);
        //
        //     QString fileName;
        //     QByteArray fileBytes;
        //     in >> fileName >> fileBytes;
        //
        //     std::cout << "\nReceived FileData response\n";
        //     std::cout << "requestId=" << header.requestId << "\n";
        //     std::cout << "fileName=" << fileName.toStdString() << "\n";
        //     std::cout << "bytes:\n";
        //     std::cout << fileBytes.toStdString() << "\n";
        //
        //     pendingRequests_.remove(header.requestId);
        //     break;
        // }
        //
        // case Protocol::MessageType::FileUpdated: {
        //     if (!ready_) {
        //         std::cerr << "Ignoring FileUpdated before READY\n";
        //         break;
        //     }
        //
        //     QDataStream in(payload);
        //     in.setByteOrder(QDataStream::BigEndian);
        //     in.setVersion(QDataStream::Qt_6_0);
        //
        //     QString fileName;
        //     quint64 timestamp = 0;
        //     quint64 size = 0;
        //     in >> fileName >> timestamp >> size;
        //
        //     std::cout << "\nNotification: file updated\n";
        //     std::cout << "fileName=" << fileName.toStdString()
        //               << " timestamp=" << timestamp
        //               << " size=" << size << "\n";
        //     break;
        // }

        case Protocol::MessageType::Error: {
            QDataStream in(payload);
            in.setByteOrder(QDataStream::BigEndian);
            in.setVersion(QDataStream::Qt_6_0);

            QString errorText;
            in >> errorText;
            std::cout << "Server error: " << errorText.toStdString() << "\n";
            break;
        }

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

    std::cout << "Received READY response: "
              << protocolVersion << " "
              << daemonReady << "\n";

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