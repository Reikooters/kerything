// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ClientConnection.h"

ClientConnection::ClientConnection(QLocalSocket* socket, QObject* parent)
    : QObject(parent)
    , socket_(socket)
{
    Q_ASSERT(socket_);

    connect(socket_, &QLocalSocket::readyRead,
            this, &ClientConnection::onReadyRead);

    connect(socket_, &QLocalSocket::disconnected,
            this, &ClientConnection::onDisconnected);
}

[[nodiscard]] bool ClientConnection::isAlive() const
{
    return socket_ && socket_->state() == QLocalSocket::ConnectedState;
}

void ClientConnection::send(const QByteArray& message) const
{
    if (!isAlive()) {
        return;
    }

    socket_->write(message);
    socket_->flush();
}

void ClientConnection::onReadyRead()
{
    if (!socket_) {
        return;
    }

    framer_.append(socket_->readAll());

    Protocol::MessageFrame frame;
    while (framer_.tryTake(frame)) {
        handleMessage(frame.header, frame.payload);
    }
}

void ClientConnection::onDisconnected()
{
    emit disconnected(this);
    deleteLater();
}

void ClientConnection::handleMessage(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    const auto type = static_cast<Protocol::MessageType>(header.type);

    switch (type) {
        // TODO

        default:
            std::cerr << "Unhandled message type: " << header.type << "\n";
            break;
    }
}