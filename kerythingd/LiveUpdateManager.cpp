// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "LiveUpdateManager.h"

#include "FanotifyHandleResolver.h"
#include "FilesystemConstants.h"

#include <cstring>
#include <iostream>
#include <optional>

#include <QSet>

#include <linux/fanotify.h>

namespace {
    struct ParsedBtrfsFileHandle {
        bool ok = false;
        quint64 objectId = 0;
        quint64 rootId = 0;
        quint64 generation = 0;
        QString errorText;
    };

    /*
     * A Btrfs subvolume root directory is commonly exposed with object id 256
     * when reached through the parent subvolume.
     *
     * For live create/move-to events fanotify gives us parent handle + name. It
     * does not directly give us the child subvolume root id, so if the child looks
     * like a subvolume root, do not guess. Request a rescan instead.
     */
    bool looksLikeBtrfsSubvolumeRoot(const ResolvedFanotifyHandle& resolved)
    {
        return resolved.ok &&
               resolved.isDirectory &&
               resolved.inode == FilesystemConstants::BtrfsFirstFreeObjectId;
    }

    template <typename T>
    T readLittleEndianUnaligned(const char* data)
    {
        T value = 0;

        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<T>(
                static_cast<unsigned char>(data[i])
            ) << (i * 8);
        }

        return value;
    }

    ParsedBtrfsFileHandle parseBtrfsFileHandle(const QByteArray& handle)
    {
        /*
         * Btrfs file handles observed from fanotify/open_by_handle_at encode:
         *
         *   u64 objectid
         *   u64 root/subvolume id
         *   u32 generation
         *
         * This parser intentionally accepts handles with trailing bytes so it
         * remains useful if the kernel returns a larger compatible handle.
         */
        static constexpr qsizetype MinimumBtrfsHandleSize =
            static_cast<qsizetype>(sizeof(quint64) + sizeof(quint64) + sizeof(quint32));

        ParsedBtrfsFileHandle parsed;

        if (handle.size() < MinimumBtrfsHandleSize) {
            parsed.errorText = QStringLiteral("Btrfs file handle too short: %1 bytes")
                .arg(handle.size());
            return parsed;
        }

        parsed.objectId = readLittleEndianUnaligned<quint64>(handle.constData());
        parsed.rootId = readLittleEndianUnaligned<quint64>(
            handle.constData() + sizeof(quint64)
        );
        parsed.generation = readLittleEndianUnaligned<quint32>(
            handle.constData() + sizeof(quint64) + sizeof(quint64)
        );

        if (parsed.objectId == 0 || parsed.rootId == 0) {
            parsed.errorText = QStringLiteral(
                "Btrfs file handle contains invalid object/root id: objectId=%1 rootId=%2"
            )
                .arg(parsed.objectId)
                .arg(parsed.rootId);
            return parsed;
        }

        parsed.ok = true;
        return parsed;
    }

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

    bool deviceIsBtrfs(const QString& fsType)
    {
        return fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0;
    }

    LiveUpdateOperation needsRescanOperation(const QString& reason)
    {
        LiveUpdateOperation operation;
        operation.kind = LiveUpdateOperationKind::NeedsRescan;
        operation.reason = reason;
        return operation;
    }

    bool applyParentIdentityFromDirectoryEntryInfo(
        LiveUpdateOperation& operation,
        const LiveUpdateEventInfo& entryInfo,
        bool isBtrfs,
        quint64 fallbackFsNamespace)
    {
        if (!isBtrfs) {
            operation.parentFsNamespace = 0;
            return true;
        }

        const ParsedBtrfsFileHandle parsed =
            parseBtrfsFileHandle(entryInfo.handle);

        if (!parsed.ok) {
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = QStringLiteral("could not parse Btrfs parent file handle: %1")
                .arg(parsed.errorText);
            return false;
        }

        operation.parentFsNamespace = parsed.rootId;

        /*
         * Do not force operation.parentInode from the parsed handle here.
         * We still resolve the parent through open_by_handle_at + fstat below
         * because that keeps ordinary and Btrfs behavior consistent and validates
         * that the handle is currently usable.
         */
        Q_UNUSED(fallbackFsNamespace);
        return true;
    }

    bool applyObjectIdentityFromObjectInfo(
        LiveUpdateOperation& operation,
        const LiveUpdateEventInfo& objectInfo,
        bool isBtrfs,
        quint64 fallbackFsNamespace)
    {
        if (!isBtrfs) {
            operation.fsNamespace = 0;
            return true;
        }

        const ParsedBtrfsFileHandle parsed =
            parseBtrfsFileHandle(objectInfo.handle);

        if (!parsed.ok) {
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = QStringLiteral("could not parse Btrfs object file handle: %1")
                .arg(parsed.errorText);
            return false;
        }

        operation.fsNamespace = parsed.rootId;

        /*
         * This should match fstat(open_by_handle_at()).st_ino for normal Btrfs
         * file handles. Keep fstat as the authoritative liveness/metadata source,
         * but log/propagate namespace from the parsed handle.
         */
        Q_UNUSED(fallbackFsNamespace);
        return true;
    }

    LiveUpdateOperation deleteOperationFromDirectoryEntryEvent(
        const FanotifyHandleResolver& resolver,
        const LiveUpdateEvent& event,
        bool isBtrfs,
        quint64 fallbackFsNamespace,
        const QString& fallbackReason)
    {
        const LiveUpdateEventInfo* entryInfo = firstRawDirectoryEntryInfo(event);

        LiveUpdateOperation operation;
        operation.kind = LiveUpdateOperationKind::DeleteEntry;
        operation.fsNamespace = isBtrfs ? fallbackFsNamespace : 0;
        operation.parentFsNamespace = isBtrfs ? fallbackFsNamespace : 0;

        if (!entryInfo) {
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = fallbackReason;
            return operation;
        }

        operation.name = entryInfo->name;

        if (!applyParentIdentityFromDirectoryEntryInfo(
                operation,
                *entryInfo,
                isBtrfs,
                fallbackFsNamespace)) {
            return operation;
        }

        const ResolvedFanotifyHandle parent =
            resolver.resolveObjectHandle(entryInfo->handle, entryInfo->handleType);

        if (parent.ok) {
            operation.parentInode = parent.inode;
        }
        else {
            operation.reason = QStringLiteral("parent inode could not be resolved: %1")
                .arg(parent.errorText);
        }

        return operation;
    }

    LiveUpdateOperation upsertOperationFromDirectoryEntryEvent(
        const FanotifyHandleResolver& resolver,
        const LiveUpdateEvent& event,
        bool isBtrfs,
        quint64 fallbackFsNamespace,
        const QString& fallbackReason)
    {
        const LiveUpdateEventInfo* entryInfo = firstRawDirectoryEntryInfo(event);

        LiveUpdateOperation operation;
        operation.kind = LiveUpdateOperationKind::Upsert;
        operation.fsNamespace = isBtrfs ? fallbackFsNamespace : 0;
        operation.parentFsNamespace = isBtrfs ? fallbackFsNamespace : 0;

        if (!entryInfo) {
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = fallbackReason;
            return operation;
        }

        operation.name = entryInfo->name;

        if (!applyParentIdentityFromDirectoryEntryInfo(
                operation,
                *entryInfo,
                isBtrfs,
                fallbackFsNamespace)) {
            return operation;
        }

        const ResolvedFanotifyHandle child =
            resolver.resolveChildByParentHandleAndName(
                entryInfo->handle,
                entryInfo->handleType,
                entryInfo->name
            );

        if (!child.ok) {
            operation.kind = LiveUpdateOperationKind::Ignored;
            operation.reason = QStringLiteral("entry no longer exists: %1")
                .arg(child.errorText);
            return operation;
        }

        const ResolvedFanotifyHandle parent =
            resolver.resolveObjectHandle(entryInfo->handle, entryInfo->handleType);

        if (parent.ok) {
            operation.parentInode = parent.inode;
        }

        /*
         * A Btrfs subvolume boundary looks like a directory with object id 256
         * from the parent filesystem view. The child belongs to a different Btrfs
         * root/subvolume namespace, but this event does not provide that child
         * root id directly.
         *
         * Do not index it as an ordinary directory in the parent namespace. That
         * would make future parent/child identity ambiguous or wrong. A rescan can
         * discover the new root id through the tree scanner and rebuild namespace
         * sidecars correctly.
         */
        if (isBtrfs && looksLikeBtrfsSubvolumeRoot(child)) {
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.reason = QStringLiteral(
                "Btrfs subvolume boundary was created or moved; rescan required"
            );
            return operation;
        }

        /*
         * For ordinary creates/renames inside a Btrfs subvolume, the child belongs
         * to the same root id as the parent directory. If the child is actually a
         * subvolume boundary, the conservative NeedsRescan path above handles it.
         */
        if (isBtrfs) {
            operation.fsNamespace = operation.parentFsNamespace;
        }

        operation.inode = child.inode;
        operation.size = child.size;
        operation.modificationTime = child.modificationTime;
        operation.isDirectory = child.isDirectory;
        operation.isSymlink = child.isSymlink;

        return operation;
    }

    LiveUpdateOperation operationFromEvent(
        const FanotifyHandleResolver& resolver,
        const LiveUpdateEvent& event,
        bool isBtrfs,
        quint64 fallbackFsNamespace)
    {
        if (event.mask & FAN_Q_OVERFLOW) {
            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::NeedsRescan;
            operation.fsNamespace = isBtrfs ? fallbackFsNamespace : 0;
            operation.parentFsNamespace = isBtrfs ? fallbackFsNamespace : 0;
            operation.reason = QStringLiteral("fanotify queue overflow");
            return operation;
        }

        if (event.mask & FAN_MOVED_FROM) {
            return deleteOperationFromDirectoryEntryEvent(
                resolver,
                event,
                isBtrfs,
                fallbackFsNamespace,
                QStringLiteral("move-from event has no directory entry info")
            );
        }

        if (event.mask & FAN_MOVED_TO) {
            return upsertOperationFromDirectoryEntryEvent(
                resolver,
                event,
                isBtrfs,
                fallbackFsNamespace,
                QStringLiteral("move-to event has no directory entry info")
            );
        }

        if (event.mask & (FAN_DELETE_SELF | FAN_MOVE_SELF)) {
            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::Ignored;
            operation.fsNamespace = isBtrfs ? fallbackFsNamespace : 0;
            operation.parentFsNamespace = isBtrfs ? fallbackFsNamespace : 0;
            operation.reason = isBtrfs
                ? QStringLiteral("redundant Btrfs self delete/move notification; directory-entry event should carry the path change")
                : QStringLiteral("redundant self delete/move notification");
            return operation;
        }

        if (event.mask & FAN_DELETE) {
            return deleteOperationFromDirectoryEntryEvent(
                resolver,
                event,
                isBtrfs,
                fallbackFsNamespace,
                QStringLiteral("delete event has no directory entry info")
            );
        }

        if (event.mask & FAN_CREATE) {
            return upsertOperationFromDirectoryEntryEvent(
                resolver,
                event,
                isBtrfs,
                fallbackFsNamespace,
                QStringLiteral("create event has no directory entry info")
            );
        }

        if (event.mask & (FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_MODIFY)) {
            const LiveUpdateEventInfo* objectInfo = firstRawInfoOfType(event, QStringLiteral("FID"));

            LiveUpdateOperation operation;
            operation.kind = LiveUpdateOperationKind::MetadataChanged;
            operation.fsNamespace = isBtrfs ? fallbackFsNamespace : 0;
            operation.parentFsNamespace = isBtrfs ? fallbackFsNamespace : 0;

            if (!objectInfo) {
                operation.kind = LiveUpdateOperationKind::Ignored;
                operation.reason = QStringLiteral("metadata event has no object FID");
                return operation;
            }

            if (!applyObjectIdentityFromObjectInfo(
                    operation,
                    *objectInfo,
                    isBtrfs,
                    fallbackFsNamespace)) {
                return operation;
            }

            const ResolvedFanotifyHandle object =
                resolver.resolveObjectHandle(objectInfo->handle, objectInfo->handleType);

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
            operation.isSymlink = object.isSymlink;

            return operation;
        }

        LiveUpdateOperation operation;
        operation.kind = LiveUpdateOperationKind::Ignored;
        operation.fsNamespace = isBtrfs ? fallbackFsNamespace : 0;
        operation.parentFsNamespace = isBtrfs ? fallbackFsNamespace : 0;
        operation.reason = QStringLiteral("unhandled fanotify event mask");
        return operation;
    }

    void logOperation(const LiveUpdateOperation& operation)
    {
        std::cout << "  operation kind="
                  << liveUpdateOperationKindToString(operation.kind).toStdString();

        if (operation.fsNamespace != 0) {
            std::cout << " fsNamespace=" << operation.fsNamespace;
        }

        if (operation.parentFsNamespace != 0) {
            std::cout << " parentFsNamespace=" << operation.parentFsNamespace;
        }

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
                      << " isDirectory=" << (operation.isDirectory ? "true" : "false")
                      << " isSymlink=" << (operation.isSymlink ? "true" : "false");
        }

        if (!operation.reason.isEmpty()) {
            std::cout << " reason=" << operation.reason.toStdString();
        }

        std::cout << "\n";
    }

    QByteArray operationObjectKey(const LiveUpdateOperation& operation)
    {
        QByteArray key = QByteArray::number(static_cast<qulonglong>(operation.fsNamespace));
        key.append('\0');
        key.append(QByteArray::number(static_cast<qulonglong>(operation.inode)));
        return key;
    }

    std::vector<LiveUpdateOperation> coalesceOperations(
        const std::vector<LiveUpdateOperation>& operations)
    {
        QSet<QByteArray> upsertObjects;
        bool hasResolvedDeleteEntry = false;

        for (const LiveUpdateOperation& operation : operations) {
            if (operation.kind == LiveUpdateOperationKind::Upsert && operation.inode != 0) {
                upsertObjects.insert(operationObjectKey(operation));
            }
            else if (operation.kind == LiveUpdateOperationKind::DeleteEntry) {
                if (operation.parentInode != 0) {
                    hasResolvedDeleteEntry = true;
                }
            }
        }

        std::vector<LiveUpdateOperation> coalescedReverse;
        coalescedReverse.reserve(operations.size());

        /*
         * Walk backwards so the last metadata snapshot for each filesystem object
         * wins. This is important with FAN_MODIFY, where a busy writer can produce
         * many metadata events for the same file in one fanotify batch.
         *
         * For Btrfs, object identity is (root/subvolume id, inode), not inode alone.
         */
        QSet<QByteArray> emittedMetadataObjects;

        for (auto it = operations.rbegin(); it != operations.rend(); ++it) {
            const LiveUpdateOperation& operation = *it;

            if (operation.kind == LiveUpdateOperationKind::Ignored) {
                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::MetadataChanged) {
                if (operation.inode == 0) {
                    continue;
                }

                const QByteArray objectKey = operationObjectKey(operation);

                if (upsertObjects.contains(objectKey)) {
                    continue;
                }

                if (emittedMetadataObjects.contains(objectKey)) {
                    continue;
                }

                emittedMetadataObjects.insert(objectKey);
                coalescedReverse.push_back(operation);
                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::DeleteEntry &&
                operation.parentInode == 0 &&
                hasResolvedDeleteEntry) {
                continue;
            }

            coalescedReverse.push_back(operation);
        }

        std::vector<LiveUpdateOperation> coalesced;
        coalesced.reserve(coalescedReverse.size());

        for (auto it = coalescedReverse.rbegin(); it != coalescedReverse.rend(); ++it) {
            coalesced.push_back(std::move(*it));
        }

        return coalesced;
    }

    quint64 liveUpdateNamespaceForWatchedMount(const BlockDevice& device)
    {
        if (device.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) != 0) {
            return 0;
        }

        for (const BlockDeviceMountInfo& mount : device.mounts) {
            if (mount.mountPoint == device.primaryMountPoint) {
                return mount.btrfsRootId;
            }
        }

        return 0;
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
            if (device.mounted &&
                device.mountedFsType.trimmed().toLower() == QStringLiteral("fuseblk")) {
                Q_EMIT liveUpdateStatusChanged(
                    device.deviceId,
                    LiveUpdateStatus::NotWatching,
                    QStringLiteral("live updates are not available for fuseblk/FUSE mounts")
                );
            }

            continue;
        }

        const std::vector<WatchTarget> targets = watchTargetsForDevice(device);

        for (const WatchTarget& target : targets) {
            if (target.key.isEmpty()) {
                continue;
            }

            desiredKeys.insert(target.key);

            if (!watchersByKey_.contains(target.key)) {
                startWatcherForTarget(target);
            }
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
        snapshot.reason = QStringLiteral("fanotify watcher active for %1")
            .arg(watcher->mountPoint());

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

QString LiveUpdateManager::watchKeyForTarget(const WatchTarget& target)
{
    if (target.deviceId.isEmpty() || target.mountPoint.isEmpty()) {
        return {};
    }

    return target.deviceId + QStringLiteral("|") + target.mountPoint;
}

bool LiveUpdateManager::isLiveUpdateEligible(const BlockDevice& device)
{
    /*
     * Live-update preferences can be enabled while a device is unmounted, but
     * actual fanotify watching can only start once the device has a mount point.
     */
    if (!device.mounted) {
        return false;
    }

    if (device.primaryMountPoint.trimmed().isEmpty()) {
        return false;
    }

    if (device.fsType.trimmed().isEmpty()) {
        return false;
    }

    /*
     * ntfs-3g and similar FUSE mounts are commonly reported as fuseblk in the
     * active mount table. FAN_REPORT_FID/open_by_handle_at based tracking does
     * not work for these mounts, so avoid starting a watcher that is known to fail.
     *
     * Kernel NTFS drivers such as ntfs3, and newer kernel ntfs mounts, are not
     * reported as fuseblk and can still be attempted.
     */
    if (device.mountedFsType.trimmed().toLower() == QStringLiteral("fuseblk")) {
        return false;
    }

    return true;
}

std::vector<LiveUpdateManager::WatchTarget>
LiveUpdateManager::watchTargetsForDevice(const BlockDevice& device)
{
    std::vector<WatchTarget> targets;

    if (!isLiveUpdateEligible(device)) {
        return targets;
    }

    const bool isBtrfs =
        device.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0;

    if (!isBtrfs) {
        WatchTarget target;
        target.deviceId = device.deviceId;
        target.mountPoint = device.primaryMountPoint;
        target.fsType = device.fsType;
        target.fsNamespace = 0;
        target.key = watchKeyForTarget(target);

        if (!target.key.isEmpty()) {
            targets.push_back(std::move(target));
        }

        return targets;
    }

    /*
     * Btrfs can have multiple mounted subvolumes for one filesystem/device.
     * Watch each mounted subvolume so live updates are generated with the
     * correct root/subvolume namespace.
     */
    targets.reserve(device.mounts.size());

    for (const BlockDeviceMountInfo& mount : device.mounts) {
        if (mount.mountPoint.trimmed().isEmpty()) {
            continue;
        }

        if (mount.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) != 0) {
            continue;
        }

        if (mount.btrfsRootId == 0) {
            continue;
        }

        WatchTarget target;
        target.deviceId = device.deviceId;
        target.mountPoint = mount.mountPoint;
        target.fsType = device.fsType;
        target.fsNamespace = mount.btrfsRootId;
        target.key = watchKeyForTarget(target);

        if (!target.key.isEmpty()) {
            targets.push_back(std::move(target));
        }
    }

    std::sort(
        targets.begin(),
        targets.end(),
        [](const WatchTarget& lhs, const WatchTarget& rhs) {
            if (lhs.key != rhs.key) {
                return lhs.key < rhs.key;
            }

            return lhs.fsNamespace < rhs.fsNamespace;
        }
    );

    targets.erase(
        std::unique(
            targets.begin(),
            targets.end(),
            [](const WatchTarget& lhs, const WatchTarget& rhs) {
                return lhs.key == rhs.key;
            }
        ),
        targets.end()
    );

    /*
     * Fallback for unusual mount metadata: if device.mounts was not populated
     * but the device is mounted, try the primary mount point using namespace 0.
     * startWatcherForTarget() will reject Btrfs namespace 0 conservatively.
     */
    if (targets.empty()) {
        WatchTarget target;
        target.deviceId = device.deviceId;
        target.mountPoint = device.primaryMountPoint;
        target.fsType = device.fsType;
        target.fsNamespace = 0;
        target.key = watchKeyForTarget(target);

        if (!target.key.isEmpty()) {
            targets.push_back(std::move(target));
        }
    }

    return targets;
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
    const QString& fsType,
    const std::vector<LiveUpdateEvent>& events)
{
    const bool isBtrfs = deviceIsBtrfs(fsType);

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
                      << info.fsid.toHex().constData()
                      << " handleType="
                      << info.handleType
                      << " handle="
                      << info.handle.toHex().constData();

            if (isBtrfs && !info.handle.isEmpty()) {
                const ParsedBtrfsFileHandle parsed =
                    parseBtrfsFileHandle(info.handle);

                if (parsed.ok) {
                    std::cout << " btrfsObjectId="
                              << parsed.objectId
                              << " btrfsRootId="
                              << parsed.rootId
                              << " btrfsGeneration="
                              << parsed.generation;
                } else {
                    std::cout << " btrfsHandleParseError="
                              << parsed.errorText.toStdString();
                }
            }

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
    const std::vector<WatchTarget> targets = watchTargetsForDevice(device);

    for (const WatchTarget& target : targets) {
        if (!target.key.isEmpty() && !watchersByKey_.contains(target.key)) {
            startWatcherForTarget(target);
        }
    }
}

void LiveUpdateManager::startWatcherForTarget(const WatchTarget& target)
{
    if (target.key.isEmpty()) {
        return;
    }

    const bool isBtrfs = deviceIsBtrfs(target.fsType);

    if (isBtrfs && target.fsNamespace == 0) {
        Q_EMIT liveUpdateStatusChanged(
            target.deviceId,
            LiveUpdateStatus::StaleNeedsRescan,
            QStringLiteral("Btrfs live updates require a known mount subvolid for %1")
                .arg(target.mountPoint)
        );

        Q_EMIT deviceNeedsRescan(
            target.deviceId,
            QStringLiteral("Btrfs live updates require a known mount subvolid for %1")
                .arg(target.mountPoint)
        );

        return;
    }

    auto* watcher = new FanotifyWatcher(
        target.deviceId,
        target.mountPoint,
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
            this, [this, watchedFsNamespace = target.fsNamespace, fsType = target.fsType, isBtrfs](
                const QString& deviceId,
                const QString& mountPoint,
                std::vector<LiveUpdateEvent> events
            ) {
#ifdef KERYTHING_ENABLE_LOGGING
                logEventBatch(deviceId, mountPoint, fsType, events);
#endif

                FanotifyHandleResolver resolver(mountPoint);
                std::vector<LiveUpdateOperation> operations;
                operations.reserve(events.size());

                for (const LiveUpdateEvent& event : events) {
                    operations.push_back(operationFromEvent(
                        resolver,
                        event,
                        isBtrfs,
                        watchedFsNamespace
                    ));
                }

                operations = coalesceOperations(operations);

                for (const LiveUpdateOperation& operation : operations) {
#ifdef KERYTHING_ENABLE_LOGGING
                    logOperation(operation);
#endif

                    if (operation.kind == LiveUpdateOperationKind::NeedsRescan) {
                        Q_EMIT liveUpdateStatusChanged(
                            deviceId,
                            LiveUpdateStatus::StaleNeedsRescan,
                            operation.reason.isEmpty()
                                ? QStringLiteral("live update operation requires rescan")
                                : operation.reason
                        );

                        Q_EMIT deviceNeedsRescan(
                            deviceId,
                            operation.reason.isEmpty()
                                ? QStringLiteral("live update operation requires rescan")
                                : operation.reason
                        );
                    }
                }

                if (!operations.empty()) {
                    Q_EMIT operationsReady(
                        deviceId,
                        mountPoint,
                        std::move(operations)
                    );
                }

#ifdef KERYTHING_ENABLE_LOGGING
                Q_EMIT eventsReady(
                    deviceId,
                    mountPoint,
                    std::move(events)
                );
#else
                Q_UNUSED(events);
#endif
            });

    if (!watcher->start()) {
        Q_EMIT liveUpdateStatusChanged(
            target.deviceId,
            LiveUpdateStatus::StaleNeedsRescan,
            QStringLiteral("failed to start fanotify watcher for %1")
                .arg(target.mountPoint)
        );

        watcher->deleteLater();
        return;
    }

    watchersByKey_.insert(target.key, watcher);

    Q_EMIT liveUpdateStatusChanged(
        target.deviceId,
        LiveUpdateStatus::Watching,
        QStringLiteral("fanotify watcher active for %1")
            .arg(target.mountPoint)
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