// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "LiveUpdateManager.h"

#include "FanotifyHandleResolver.h"

#include <iostream>

#include <QSet>

#include <linux/fanotify.h>

namespace {
    const LiveUpdateEventInfo* firstRawInfoOfType(
        const LiveUpdateEvent& event,
        const QString& infoType)
    {
        for (const LiveUpdateEventInfo& info : event.infos) {
            if (info.infoType == infoType) {
                return &info;
            }
        }

        return nullptr;
    }

    const LiveUpdateEventInfo* firstRawDirectoryEntryInfo(const LiveUpdateEvent& event)
    {
        if (const LiveUpdateEventInfo* info = firstRawInfoOfType(event, QStringLiteral("DFID_NAME"))) {
            return info;
        }

        if (const LiveUpdateEventInfo* info = firstRawInfoOfType(event, QStringLiteral("OLD_DFID_NAME"))) {
            return info;
        }

        if (const LiveUpdateEventInfo* info = firstRawInfoOfType(event, QStringLiteral("NEW_DFID_NAME"))) {
            return info;
        }

        return nullptr;
    }

    LiveUpdateOperation operationFromEvent(
        const FanotifyHandleResolver& resolver,
        const LiveUpdateEvent& event)
    {
        if (event.mask & FAN_Q_OVERFLOW) {
            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = QStringLiteral("fanotify queue overflow");
            return operation;
        }

        if (event.mask & (FAN_MOVED_FROM | FAN_MOVED_TO)) {
            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = QStringLiteral("move or rename events are not supported yet");
            return operation;
        }

        if (event.mask & (FAN_DELETE_SELF | FAN_MOVE_SELF)) {
            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = QStringLiteral("object was deleted or moved");
            return operation;
        }

        if (event.mask & FAN_DELETE) {
            const LiveUpdateEventInfo* entryInfo = firstRawDirectoryEntryInfo(event);

            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::DeleteEntry;

            if (!entryInfo) {
                operation.kind = LiveUpdateOperationKind::NeedsRescan;
                operation.reason = QStringLiteral("delete event has no directory entry info");
                return operation;
            }

            operation.name = entryInfo->name;

            const ResolvedFanotifyHandle parent =
                resolver.resolveObjectHandle(entryInfo->handleHex, entryInfo->handleType);

            if (parent.ok) {
                operation.parentInode = parent.inode;
            }
            else {
                operation.reason = QStringLiteral("parent inode could not be resolved: %1")
                    .arg(parent.errorText);
            }

            return operation;
        }

        if (event.mask & FAN_CREATE) {
            const LiveUpdateEventInfo* entryInfo = firstRawDirectoryEntryInfo(event);

            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::Upsert;

            if (!entryInfo) {
                operation.kind = LiveUpdateOperationKind::NeedsRescan;
                operation.reason = QStringLiteral("create event has no directory entry info");
                return operation;
            }

            operation.name = entryInfo->name;

            const ResolvedFanotifyHandle child =
                resolver.resolveChildByParentHandleAndName(
                    entryInfo->handleHex,
                    entryInfo->handleType,
                    entryInfo->name
                );

            if (!child.ok) {
                operation.kind = LiveUpdateOperationKind::Ignored;
                operation.reason = QStringLiteral("created entry no longer exists: %1")
                    .arg(child.errorText);
                return operation;
            }

            const ResolvedFanotifyHandle parent =
                resolver.resolveObjectHandle(entryInfo->handleHex, entryInfo->handleType);

            if (parent.ok) {
                operation.parentInode = parent.inode;
            }

            operation.inode = child.inode;
            operation.size = child.size;
            operation.modificationTime = child.modificationTime;
            operation.isDirectory = child.isDirectory;

            return operation;
        }

        if (event.mask & (FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_MODIFY)) {
            const LiveUpdateEventInfo* objectInfo = firstRawInfoOfType(event, QStringLiteral("FID"));

            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::MetadataChanged;

            if (!objectInfo) {
                operation.kind = LiveUpdateOperationKind::Ignored;
                operation.reason = QStringLiteral("metadata event has no object FID");
                return operation;
            }

            const ResolvedFanotifyHandle object =
                resolver.resolveObjectHandle(objectInfo->handleHex, objectInfo->handleType);

            if (!object.ok) {
                operation.kind = LiveUpdateOperationKind::Ignored;
                operation.reason = QStringLiteral("metadata object no longer exists: %1")
                    .arg(object.errorText);
                return operation;
            }

            operation.inode = object.inode;
            operation.size = object.size;
            operation.modificationTime = object.modificationTime;
            operation.isDirectory = object.isDirectory;

            return operation;
        }

        LiveUpdateOperation operation;
        operation.kind = LiveUpdateOperationKind::Ignored;
        operation.reason = QStringLiteral("unhandled fanotify event mask");
        return operation;
    }

    void logOperation(const LiveUpdateOperation& operation)
    {
        std::cout << "  operation kind="
                  << liveUpdateOperationKindToString(operation.kind).toStdString();

        if (operation.inode != 0) {
            std::cout << " inode=" << operation.inode;
        }

        if (operation.parentInode != 0) {
            std::cout << " parentInode=" << operation.parentInode;
        }

        if (!operation.name.isEmpty()) {
            std::cout << " name=" << operation.name.toStdString();
        }

        if (operation.kind == LiveUpdateOperationKind::MetadataChanged ||
            operation.kind == LiveUpdateOperationKind::Upsert) {
            std::cout << " size=" << operation.size
                      << " mtime=" << operation.modificationTime
                      << " isDirectory=" << (operation.isDirectory ? "true" : "false");
        }

        if (!operation.reason.isEmpty()) {
            std::cout << " reason=" << operation.reason.toStdString();
        }

        std::cout << "\n";
    }

    std::vector<LiveUpdateOperation> coalesceOperations(
        const std::vector<LiveUpdateOperation>& operations)
    {
        QSet<quint64> upsertInodes;
        bool hasDeleteEntry = false;
        bool hasResolvedDeleteEntry = false;

        for (const LiveUpdateOperation& operation : operations) {
            if (operation.kind == LiveUpdateOperationKind::Upsert && operation.inode != 0) {
                upsertInodes.insert(operation.inode);
            }
            else if (operation.kind == LiveUpdateOperationKind::DeleteEntry) {
                hasDeleteEntry = true;

                if (operation.parentInode != 0) {
                    hasResolvedDeleteEntry = true;
                }
            }
        }

        std::vector<LiveUpdateOperation> coalesced;
        coalesced.reserve(operations.size());

        for (const LiveUpdateOperation& operation : operations) {
            if (operation.kind == LiveUpdateOperationKind::Ignored) {
                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::MetadataChanged &&
                operation.inode != 0 &&
                upsertInodes.contains(operation.inode)) {
                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::NeedsRescan &&
                hasDeleteEntry &&
                operation.reason == QStringLiteral("object was deleted or moved")) {
                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::DeleteEntry &&
                operation.parentInode == 0 &&
                hasResolvedDeleteEntry) {
                continue;
            }

            coalesced.push_back(operation);
        }

        return coalesced;
    }
}

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
                      << " handleType="
                      << info.handleType
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

                FanotifyHandleResolver resolver(mountPoint);
                std::vector<LiveUpdateOperation> operations;
                operations.reserve(events.size());

                for (const LiveUpdateEvent& event : events) {
                    operations.push_back(operationFromEvent(resolver, event));
                }

                operations = coalesceOperations(operations);

                for (const LiveUpdateOperation& operation : operations) {
                    logOperation(operation);
                }

                Q_EMIT operationsReady(
                    deviceId,
                    mountPoint,
                    std::move(operations)
                );

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