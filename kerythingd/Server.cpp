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

#include <systemd/sd-daemon.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
    constexpr int IdleShutdownTimeoutMs = 30 * 1000;
}

Server::Server(QObject* parent)
    : QObject(parent)
{
    connect(&server_, &QLocalServer::newConnection,
            this, &Server::onNewConnection);

    knownDevicesRefreshTimer_.setSingleShot(true);
    knownDevicesRefreshTimer_.setInterval(1000);
    connect(&knownDevicesRefreshTimer_, &QTimer::timeout,
            this, &Server::refreshKnownDevices);

    idleShutdownTimer_.setSingleShot(true);
    idleShutdownTimer_.setInterval(IdleShutdownTimeoutMs);
    connect(&idleShutdownTimer_, &QTimer::timeout,
            this, &Server::maybeShutdownAfterIdle);

    if (!listen()) {
        QCoreApplication::exit(1);
        return;
    }

    std::cout << "Server listening on: "
              << Protocol::ServerName.toStdString() << "\n";

    lastKnownDevices_ = BlockDeviceHelper::listKnownDevices();

    liveUpdateManager_ = new LiveUpdateManager(this);

    connect(liveUpdateManager_, &LiveUpdateManager::liveUpdateStatusChanged,
            this, [this](
                const QString& deviceId,
                LiveUpdateStatus status,
                const QString& reason
            ) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "Live update status changed deviceId="
                          << deviceId.toStdString()
                          << " status="
                          << liveUpdateStatusToString(status).toStdString()
                          << " reason="
                          << reason.toStdString()
                          << "\n";
#endif

                broadcastLiveUpdateStatusChanged(deviceId, status, reason);
            });

    connect(liveUpdateManager_, &LiveUpdateManager::deviceNeedsRescan,
            this, [](const QString& deviceId, const QString& reason) {
                std::cerr << "Live update stream became unreliable deviceId="
                          << deviceId.toStdString()
                          << " reason="
                          << reason.toStdString()
                          << "\n";
            });

    connect(liveUpdateManager_, &LiveUpdateManager::eventsReady,
            this, [this](
                const QString& deviceId,
                const QString& mountPoint,
                const std::vector<LiveUpdateEvent>& events
            ) {
#ifdef KERYTHING_ENABLE_LOGGING
                broadcastLiveUpdateBatch(deviceId, mountPoint, events);
#else
                Q_UNUSED(deviceId);
                Q_UNUSED(mountPoint);
                Q_UNUSED(events);
#endif
            });

    connect(liveUpdateManager_, &LiveUpdateManager::operationsReady,
        this, [this](
            const QString& deviceId,
            const QString& mountPoint,
            const std::vector<LiveUpdateOperation>& operations
        ) {
            broadcastLiveUpdateOperationBatch(deviceId, mountPoint, operations);
        });

    liveUpdateManager_->setKnownDevices(lastKnownDevices_);

    deviceChangeMonitor_ = new DeviceChangeMonitor(this);
    connect(deviceChangeMonitor_, &DeviceChangeMonitor::devicesMayHaveChanged,
            this, &Server::scheduleKnownDevicesRefresh);

    startIdleShutdownTimerIfIdle();
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

    if (::chmod(dirPathNative.constData(), 0750) != 0) {
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

bool Server::listen()
{
    if (listenFromSystemd()) {
        return true;
    }

    return listenStandalone();
}

bool Server::listenFromSystemd()
{
    const int fdCount = sd_listen_fds(0);

    if (fdCount < 0) {
        std::cerr << "sd_listen_fds failed: "
                  << std::strerror(-fdCount) << "\n";
        return false;
    }

    if (fdCount == 0) {
        return false;
    }

    if (fdCount != 1) {
        std::cerr << "Expected exactly one systemd socket, got "
                  << fdCount
                  << "\n";
        return false;
    }

    const int fd = SD_LISTEN_FDS_START;

    if (sd_is_socket_unix(fd, SOCK_STREAM, 1, nullptr, 0) <= 0) {
        std::cerr << "Inherited systemd fd is not a listening Unix stream socket\n";
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "Failed to get inherited systemd socket flags: "
                  << std::strerror(errno)
                  << "\n";
        return false;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        std::cerr << "Failed to set inherited systemd socket nonblocking: "
                  << std::strerror(errno)
                  << "\n";
        return false;
    }

    systemdListenFd_ = fd;
    systemdSocketNotifier_ = new QSocketNotifier(systemdListenFd_, QSocketNotifier::Read, this);

    connect(systemdSocketNotifier_, &QSocketNotifier::activated,
            this, &Server::onSystemdSocketActivated);

    std::cout << "Using systemd socket activation\n";
    return true;
}

bool Server::listenStandalone()
{
    const std::optional<gid_t> socketGroupId = resolveSocketGroupId();
    if (!socketGroupId) {
        return false;
    }

    if (!prepareSocketDirectory(*socketGroupId)) {
        return false;
    }

    QLocalServer::removeServer(Protocol::ServerName);

    if (!server_.listen(Protocol::ServerName)) {
        std::cerr << "Failed to listen: "
                  << server_.errorString().toStdString() << "\n";
        return false;
    }

    if (!applySocketPermissions(*socketGroupId)) {
        return false;
    }

    std::cout << "Using standalone socket creation fallback\n";
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

void Server::addClientConnection(QLocalSocket* socket)
{
    std::cout << "Client connected\n";

    auto* connection = new ClientConnection(socket, this);
    connect(connection, &ClientConnection::disconnected,
            this, &Server::onClientDisconnected);

    connection->sendReady();
    connection->sendKnownDevices(0, lastKnownDevices_);

    if (liveUpdateManager_) {
        for (const LiveUpdateStatusSnapshot& snapshot : liveUpdateManager_->currentStatusSnapshots()) {
            connection->sendLiveUpdateStatusChanged(
                snapshot.deviceId,
                snapshot.status,
                snapshot.reason
            );
        }
    }

    clients_.push_back(connection);
}

void Server::onNewConnection()
{
    stopIdleShutdownTimer();

    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        addClientConnection(socket);
    }
}

void Server::onSystemdSocketActivated()
{
    stopIdleShutdownTimer();

    if (systemdSocketNotifier_) {
        systemdSocketNotifier_->setEnabled(false);
    }

    while (true) {
        const int clientFd = ::accept4(
            systemdListenFd_,
            nullptr,
            nullptr,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );

        if (clientFd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                std::cerr << "accept4 failed on systemd socket: "
                          << std::strerror(errno)
                          << "\n";
            }

            break;
        }

        auto* socket = new QLocalSocket(this);

        if (!socket->setSocketDescriptor(
                static_cast<qintptr>(clientFd),
                QLocalSocket::ConnectedState,
                QIODevice::ReadWrite
            )) {
            std::cerr << "Failed to wrap accepted client socket: "
                      << socket->errorString().toStdString()
                      << "\n";

            socket->deleteLater();
            ::close(clientFd);
            continue;
            }

        addClientConnection(socket);
    }

    if (systemdSocketNotifier_) {
        systemdSocketNotifier_->setEnabled(true);
    }
}

void Server::onClientDisconnected(ClientConnection* connection)
{
    clients_.erase(
        std::remove(clients_.begin(), clients_.end(), connection),
        clients_.end());

    std::cout << "Client disconnected\n";

    startIdleShutdownTimerIfIdle();
}

void Server::startIdleShutdownTimerIfIdle()
{
    if (!clients_.empty() || idleShutdownTimer_.isActive()) {
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "No clients connected; daemon will shut down after idle timeout\n";
#endif

    idleShutdownTimer_.start();
}

void Server::stopIdleShutdownTimer()
{
    if (idleShutdownTimer_.isActive()) {
        idleShutdownTimer_.stop();
    }
}

void Server::maybeShutdownAfterIdle()
{
    if (!clients_.empty()) {
        return;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Idle timeout reached; shutting down daemon\n";
#endif

    QCoreApplication::quit();
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

    if (liveUpdateManager_) {
        liveUpdateManager_->setKnownDevices(lastKnownDevices_);
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "Known devices changed; broadcasting count="
              << devices.size()
              << "\n";
#endif

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

void Server::broadcastLiveUpdateBatch(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateEvent>& events)
{
    if (events.empty()) {
        return;
    }

    for (ClientConnection* client : clients_) {
        if (client) {
            client->sendLiveUpdateBatch(deviceId, mountPoint, events);
        }
    }
}

void Server::broadcastLiveUpdateStatusChanged(
    const QString& deviceId,
    LiveUpdateStatus status,
    const QString& reason)
{
    if (deviceId.isEmpty()) {
        return;
    }

    for (ClientConnection* client : clients_) {
        if (client) {
            client->sendLiveUpdateStatusChanged(deviceId, status, reason);
        }
    }
}

void Server::broadcastLiveUpdateOperationBatch(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateOperation>& operations)
{
    if (operations.empty()) {
        return;
    }

    for (ClientConnection* client : clients_) {
        if (client) {
            client->sendLiveUpdateOperationBatch(deviceId, mountPoint, operations);
        }
    }
}
