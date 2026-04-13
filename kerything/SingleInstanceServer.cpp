// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SingleInstanceServer.h"

#include <QByteArray>
#include <QLocalSocket>
#include <QTextStream>

SingleInstanceServer::SingleInstanceServer(QString serverName, QObject* parent)
    : QObject(parent),
      serverName_(std::move(serverName)) {
    // Remove a stale server entry from a previous crash/shutdown if needed.
    removeStaleServerIfNeeded();

    // Try to become the primary instance by listening on the local server name.
    if (server_.listen(serverName_)) {
        primary_ = true;

        // When a second process connects, handle its incoming command.
        connect(&server_, &QLocalServer::newConnection,
                this, &SingleInstanceServer::onNewConnection);
    }
}

bool SingleInstanceServer::isPrimary() const noexcept {
    return primary_;
}

bool SingleInstanceServer::notifyPrimary(const QString& command) const {
    // Client-side IPC: connect to the primary instance and send a command.
    QLocalSocket socket;
    socket.connectToServer(serverName_, QIODevice::WriteOnly);

    if (!socket.waitForConnected(1000)) {
        return false;
    }

    // Commands are encoded as plain text for simplicity.
    QTextStream out(&socket);
    out << command << '\n';
    out.flush();

    socket.flush();
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

void SingleInstanceServer::onNewConnection() {
    // Iterate through all pending connections and process them.
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            // Read the command as UTF-8 text and trim the newline.
            const QString message = QString::fromUtf8(socket->readAll()).trimmed();

            if (message == QStringLiteral("OPEN_WINDOW")) {
                Q_EMIT requestOpenWindow();
            } else if (!message.isEmpty()) {
                Q_EMIT requestCommand(message);
            }

            // Close and delete the socket once the message has been processed.
            socket->disconnectFromServer();
            socket->deleteLater();
        });

        // Extra cleanup if the socket disconnects normally.
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void SingleInstanceServer::removeStaleServerIfNeeded() {
    // Probe whether something is already listening on this server name.
    QLocalSocket socket;
    socket.connectToServer(serverName_);

    if (socket.waitForConnected(100)) {
        socket.disconnectFromServer();
        return;
    }

    // If nothing is actually listening, remove stale state from a previous run.
    QLocalServer::removeServer(serverName_);
}