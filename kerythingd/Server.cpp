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

#include "BlockDeviceHelper.h"

#include <sys/stat.h>
#include <unistd.h>

Server::Server(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QLocalServer::newConnection,
            this, &Server::onNewConnection);

    knownDevicesRefreshTimer_.setSingleShot(true);
    knownDevicesRefreshTimer_.setInterval(1000);
    connect(&knownDevicesRefreshTimer_, &QTimer::timeout,
            this, &Server::refreshKnownDevices);

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

    lastKnownDevices_ = BlockDeviceHelper::listKnownDevices();

    deviceChangeMonitor_ = new DeviceChangeMonitor(this);
    connect(deviceChangeMonitor_, &DeviceChangeMonitor::devicesMayHaveChanged,
            this, &Server::scheduleKnownDevicesRefresh);
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

bool Server::blockDeviceListsEqual(
    const std::vector<BlockDevice>& lhs,
    const std::vector<BlockDevice>& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    auto sorted = [](std::vector<BlockDevice> devices) {
        std::sort(devices.begin(), devices.end(), [](const BlockDevice& a, const BlockDevice& b) {
            return a.deviceId < b.deviceId;
        });

        return devices;
    };

    const std::vector<BlockDevice> aDevices = sorted(lhs);
    const std::vector<BlockDevice> bDevices = sorted(rhs);

    for (std::size_t i = 0; i < aDevices.size(); ++i) {
        const BlockDevice& a = aDevices[i];
        const BlockDevice& b = bDevices[i];

        if (a.deviceId != b.deviceId ||
            a.devNode != b.devNode ||
            a.fsType != b.fsType ||
            a.uuid != b.uuid ||
            a.partuuid != b.partuuid ||
            a.label != b.label ||
            a.diskModel != b.diskModel ||
            a.mounted != b.mounted ||
            a.mountPoints != b.mountPoints ||
            a.primaryMountPoint != b.primaryMountPoint) {
            return false;
        }
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
        connection->sendKnownDevices(0, lastKnownDevices_);

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

void Server::scheduleKnownDevicesRefresh()
{
    knownDevicesRefreshTimer_.start();
}

void Server::refreshKnownDevices()
{
    const std::vector<BlockDevice> devices = BlockDeviceHelper::listKnownDevices();

    if (blockDeviceListsEqual(lastKnownDevices_, devices)) {
        return;
    }

    lastKnownDevices_ = devices;

    std::cout << "Known devices changed; broadcasting count="
              << devices.size()
              << "\n";

    broadcastKnownDevices(devices);
}

void Server::broadcastKnownDevices(const std::vector<BlockDevice>& devices)
{
    for (ClientConnection* client : clients_) {
        if (client) {
            client->sendKnownDevices(0, devices);
        }
    }
}
