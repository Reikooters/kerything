// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Server.h"

#include <iostream>
#include <QCoreApplication>

Server::Server(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QLocalServer::newConnection,
            this, &Server::onNewConnection);

    QLocalServer::removeServer(Protocol::ServerName);

    if (!server_.listen(Protocol::ServerName)) {
        std::cerr << "Failed to listen: "
                  << server_.errorString().toStdString() << "\n";
        QCoreApplication::exit(1);
        return;
    }

    std::cout << "Server listening on: " << Protocol::ServerName.toStdString() << "\n";
}

void Server::onNewConnection()
{
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        std::cout << "Client connected\n";

        auto* connection = new ClientConnection(socket, this);
        connect(connection, &ClientConnection::disconnected,
                this, &Server::onClientDisconnected);

        sendReady(connection);

        clients_.push_back(connection);
    }
}

void Server::onClientDisconnected(ClientConnection* connection)
{
    clients_.erase(
        std::remove(clients_.begin(), clients_.end(), connection),
        clients_.end());

    std::cout << "Client disconnected\n";
}

void Server::sendReady(const ClientConnection* clientConnection) {
    const QByteArray payload = Protocol::makeReadyPayload();
    const QByteArray msg = Protocol::packMessage(Protocol::MessageType::Ready, 0, payload);
    clientConnection->send(msg);
}