// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Server.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <grp.h>
#include <iostream>
#include <optional>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sys/stat.h>
#include <unistd.h>

Server::Server(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QLocalServer::newConnection,
            this, &Server::onNewConnection);

    const std::optional<gid_t> socketGroupId = resolveSocketGroupId();
    if (!socketGroupId) {
        QCoreApplication::exit(1);
        return;
    }

    if (!prepareSocketDirectory(*socketGroupId)) {
        QCoreApplication::exit(1);
        return;
    }

    QLocalServer::removeServer(Protocol::ServerName);

    if (!server_.listen(Protocol::ServerName)) {
        std::cerr << "Failed to listen: "
                  << server_.errorString().toStdString() << "\n";
        QCoreApplication::exit(1);
        return;
    }

    if (!applySocketPermissions(*socketGroupId)) {
        QCoreApplication::exit(1);
        return;
    }

    std::cout << "Server listening on: "
              << Protocol::ServerName.toStdString() << "\n";
}

std::optional<gid_t> Server::resolveSocketGroupId()
{
    errno = 0;
    struct group* grp = getgrnam("kerything");
    if (!grp) {
        std::cerr << "Failed to resolve group 'kerything': "
                  << std::strerror(errno) << "\n";
        return std::nullopt;
    }

    return grp->gr_gid;
}

bool Server::prepareSocketDirectory(gid_t socketGroupId)
{
    const QString socketDirPath = QFileInfo(Protocol::ServerName).absolutePath();

    // Create /run/kerythingd if it does not exist.
    QDir dir;
    if (!dir.mkpath(socketDirPath)) {
        std::cerr << "Failed to create socket directory: "
                  << socketDirPath.toStdString() << "\n";
        return false;
    }

    const QByteArray dirPathNative = QFile::encodeName(socketDirPath);

    // Lock down the directory so only root and the kerything group can traverse it.
    if (::chown(dirPathNative.constData(), 0, socketGroupId) != 0) {
        std::cerr << "Failed to chown socket directory: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    if (::chmod(dirPathNative.constData(), 0770) != 0) {
        std::cerr << "Failed to chmod socket directory: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    return true;
}

bool Server::applySocketPermissions(gid_t socketGroupId)
{
    const QByteArray socketPathNative = QFile::encodeName(Protocol::ServerName);

    // Make sure the socket is group-accessible.
    if (::chown(socketPathNative.constData(), 0, socketGroupId) != 0) {
        std::cerr << "Failed to chown socket: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    if (::chmod(socketPathNative.constData(), 0660) != 0) {
        std::cerr << "Failed to chmod socket: "
                  << std::strerror(errno) << "\n";
        return false;
    }

    return true;
}

void Server::onNewConnection()
{
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        std::cout << "Client connected\n";

        ClientConnection *connection = new ClientConnection(socket, this);
        connect(connection, &ClientConnection::disconnected,
                this, &Server::onClientDisconnected);

        connection->sendReady();

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
