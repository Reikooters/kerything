// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "LiveUpdateManager.h"

#include <iostream>

#include <QSet>

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
                Q_EMIT deviceNeedsRescan(
                    deviceId,
                    QStringLiteral("fanotify queue overflow")
                );
            });

    connect(watcher, &FanotifyWatcher::fatalError,
            this, [this](const QString& deviceId, const QString& errorText) {
                Q_EMIT deviceNeedsRescan(deviceId, errorText);
            });

    if (!watcher->start()) {
        watcher->deleteLater();
        return;
    }

    watchersByKey_.insert(key, watcher);
}

void LiveUpdateManager::removeWatcher(const QString& key)
{
    FanotifyWatcher* watcher = watchersByKey_.take(key);
    if (!watcher) {
        return;
    }

    std::cout << "fanotify: stopping watcher deviceId="
              << watcher->deviceId().toStdString()
              << " mountPoint="
              << watcher->mountPoint().toStdString()
              << "\n";

    watcher->deleteLater();
}