// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "LiveUpdateManager.h"

#include <iostream>

#include <QSet>

#include <linux/fanotify.h>

LiveUpdateManager::LiveUpdateManager(QObject* parent)
    : QObject(parent)
{
}

void LiveUpdateManager::setKnownDevices(const std::vector<BlockDevice>& devices)
{
    QSet<QString> desiredKeys;

    for (const BlockDevice& device : devices) {
        if (!isLiveUpdateEligible(device)) {
            continue;
        }

        const QString key = watchKeyForDevice(device);
        if (key.isEmpty()) {
            continue;
        }

        desiredKeys.insert(key);

        if (!watchersByKey_.contains(key)) {
            startWatcherForDevice(device);
        }
    }

    QList<QString> staleKeys;

    for (auto it = watchersByKey_.cbegin(); it != watchersByKey_.cend(); ++it) {
        if (!desiredKeys.contains(it.key())) {
            staleKeys << it.key();
        }
    }

    for (const QString& key : staleKeys) {
        removeWatcher(key);
    }
}

void LiveUpdateManager::stopAll()
{
    const QList<QString> keys = watchersByKey_.keys();

    for (const QString& key : keys) {
        removeWatcher(key);
    }
}

std::vector<LiveUpdateStatusSnapshot> LiveUpdateManager::currentStatusSnapshots() const
{
    std::vector<LiveUpdateStatusSnapshot> snapshots;
    snapshots.reserve(static_cast<std::size_t>(watchersByKey_.size()));

    for (auto it = watchersByKey_.cbegin(); it != watchersByKey_.cend(); ++it) {
        const FanotifyWatcher* watcher = it.value();
        if (!watcher) {
            continue;
        }

        LiveUpdateStatusSnapshot snapshot;
        snapshot.deviceId = watcher->deviceId();
        snapshot.status = LiveUpdateStatus::Watching;
        snapshot.reason = QStringLiteral("fanotify watcher active");

        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

QString LiveUpdateManager::watchKeyForDevice(const BlockDevice& device)
{
    if (device.deviceId.isEmpty() || device.primaryMountPoint.isEmpty()) {
        return {};
    }

    return device.deviceId + QStringLiteral("|") + device.primaryMountPoint;
}

bool LiveUpdateManager::isLiveUpdateEligible(const BlockDevice& device)
{
    if (!device.mounted) {
        return false;
    }

    if (device.fsType != QStringLiteral("ext4")) {
        return false;
    }

    if (device.primaryMountPoint.isEmpty()) {
        return false;
    }

    return true;
}

QString LiveUpdateManager::maskToString(quint64 mask)
{
    QStringList parts;

    if (mask & FAN_CREATE) parts << QStringLiteral("CREATE");
    if (mask & FAN_DELETE) parts << QStringLiteral("DELETE");
    if (mask & FAN_MOVED_FROM) parts << QStringLiteral("MOVED_FROM");
    if (mask & FAN_MOVED_TO) parts << QStringLiteral("MOVED_TO");
    if (mask & FAN_RENAME) parts << QStringLiteral("RENAME");
    if (mask & FAN_CLOSE_WRITE) parts << QStringLiteral("CLOSE_WRITE");
    if (mask & FAN_MODIFY) parts << QStringLiteral("MODIFY");
    if (mask & FAN_ATTRIB) parts << QStringLiteral("ATTRIB");
    if (mask & FAN_DELETE_SELF) parts << QStringLiteral("DELETE_SELF");
    if (mask & FAN_MOVE_SELF) parts << QStringLiteral("MOVE_SELF");
    if (mask & FAN_ONDIR) parts << QStringLiteral("ONDIR");
    if (mask & FAN_Q_OVERFLOW) parts << QStringLiteral("Q_OVERFLOW");

    if (parts.isEmpty()) {
        return QStringLiteral("0x%1").arg(mask, 0, 16);
    }

    return parts.join(QStringLiteral("|"));
}

void LiveUpdateManager::logEventBatch(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateEvent>& events)
{
    std::cout << "live update batch: deviceId="
              << deviceId.toStdString()
              << " mountPoint="
              << mountPoint.toStdString()
              << " count="
              << events.size()
              << "\n";

    for (const LiveUpdateEvent& event : events) {
        std::cout << "  event mask="
                  << maskToString(event.mask).toStdString();

        for (const LiveUpdateEventInfo& info : event.infos) {
            std::cout << " infoType="
                      << info.infoType.toStdString()
                      << " fsid="
                      << info.fsidHex.toStdString()
                      << " handle="
                      << info.handleHex.toStdString();

            if (!info.name.isEmpty()) {
                std::cout << " name="
                          << info.name.toStdString();
            }
        }

        std::cout << "\n";
    }
}

void LiveUpdateManager::startWatcherForDevice(const BlockDevice& device)
{
    const QString key = watchKeyForDevice(device);
    if (key.isEmpty()) {
        return;
    }

    auto* watcher = new FanotifyWatcher(
        device.deviceId,
        device.primaryMountPoint,
        this
    );

    connect(watcher, &FanotifyWatcher::overflow,
        this, [this](const QString& deviceId) {
            const QString reason = QStringLiteral("fanotify queue overflow");

            Q_EMIT liveUpdateStatusChanged(
                deviceId,
                LiveUpdateStatus::StaleNeedsRescan,
                reason
            );

            Q_EMIT deviceNeedsRescan(deviceId, reason);
        });

    connect(watcher, &FanotifyWatcher::fatalError,
            this, [this](const QString& deviceId, const QString& errorText) {
                Q_EMIT liveUpdateStatusChanged(
                    deviceId,
                    LiveUpdateStatus::StaleNeedsRescan,
                    errorText
                );

                Q_EMIT deviceNeedsRescan(deviceId, errorText);
            });

    connect(watcher, &FanotifyWatcher::eventsReady,
            this, [this](
                const QString& deviceId,
                const QString& mountPoint,
                std::vector<LiveUpdateEvent> events
            ) {
                logEventBatch(deviceId, mountPoint, events);

                Q_EMIT eventsReady(
                    deviceId,
                    mountPoint,
                    std::move(events)
                );
            });

    if (!watcher->start()) {
        Q_EMIT liveUpdateStatusChanged(
            device.deviceId,
            LiveUpdateStatus::StaleNeedsRescan,
            QStringLiteral("failed to start fanotify watcher")
        );

        watcher->deleteLater();
        return;
    }

    watchersByKey_.insert(key, watcher);

    Q_EMIT liveUpdateStatusChanged(
        device.deviceId,
        LiveUpdateStatus::Watching,
        QStringLiteral("fanotify watcher active")
    );
}

void LiveUpdateManager::removeWatcher(const QString& key)
{
    FanotifyWatcher* watcher = watchersByKey_.take(key);
    if (!watcher) {
        return;
    }

    const QString deviceId = watcher->deviceId();

    std::cout << "fanotify: stopping watcher deviceId="
              << watcher->deviceId().toStdString()
              << " mountPoint="
              << watcher->mountPoint().toStdString()
              << "\n";

    Q_EMIT liveUpdateStatusChanged(
        deviceId,
        LiveUpdateStatus::NotWatching,
        QStringLiteral("fanotify watcher stopped")
    );

    watcher->deleteLater();
}