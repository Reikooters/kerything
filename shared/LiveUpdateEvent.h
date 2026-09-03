// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_LIVEUPDATEEVENT_H
#define KERYTHING_LIVEUPDATEEVENT_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <vector>

#include <linux/fanotify.h>

struct LiveUpdateEventInfo {
    QString infoType;

    // Stored as raw binary bytes internally. Convert to hex only for logging or
    // display/debug text.
    QByteArray fsid;
    QByteArray handle;

    qint32 handleType = 0;
    QString name;
};

struct LiveUpdateEvent {
    quint64 mask = 0;
    std::vector<LiveUpdateEventInfo> infos;
};

enum class LiveUpdateStatus : quint8 {
    Watching = 0,
    NotWatching,
    StaleNeedsRescan
};

inline QString liveUpdateStatusToString(LiveUpdateStatus status)
{
    switch (status) {
        case LiveUpdateStatus::Watching:
            return QStringLiteral("Watching");
        case LiveUpdateStatus::NotWatching:
            return QStringLiteral("NotWatching");
        case LiveUpdateStatus::StaleNeedsRescan:
            return QStringLiteral("StaleNeedsRescan");
        default:
            return QStringLiteral("Unknown");
    }
}

enum class LiveUpdateOperationKind : quint8 {
    Ignored = 0,
    MetadataChanged,
    Upsert,
    DeleteEntry,
    NeedsRescan
};

struct LiveUpdateOperation {
    LiveUpdateOperationKind kind = LiveUpdateOperationKind::Ignored;

    /*
     * Filesystem namespace ids are 0 for ordinary filesystems.
     *
     * For Btrfs they contain the root/subvolume id. Btrfs inode/object ids
     * are only unique within a root, so live-update identity must use:
     *
     *   object: (fsNamespace, inode)
     *   entry:  (parentFsNamespace, parentInode, name)
     */
    quint64 fsNamespace = 0;
    quint64 parentFsNamespace = 0;

    quint64 inode = 0;
    quint64 parentInode = 0;

    QString name;

    quint64 size = 0;
    qint64 modificationTime = 0;
    bool isDirectory = false;
    bool isSymlink = false;

    QString reason;
};

inline QString liveUpdateOperationKindToString(LiveUpdateOperationKind kind)
{
    switch (kind) {
        case LiveUpdateOperationKind::Ignored:
            return QStringLiteral("Ignored");
        case LiveUpdateOperationKind::MetadataChanged:
            return QStringLiteral("MetadataChanged");
        case LiveUpdateOperationKind::Upsert:
            return QStringLiteral("Upsert");
        case LiveUpdateOperationKind::DeleteEntry:
            return QStringLiteral("DeleteEntry");
        case LiveUpdateOperationKind::NeedsRescan:
            return QStringLiteral("NeedsRescan");
        default:
            return QStringLiteral("Unknown");
    }
}

enum class NormalizedLiveUpdateKind : quint8 {
    Unknown = 0,
    Created,
    Deleted,
    MovedFrom,
    MovedTo,
    MetadataChanged,
    SelfDeletedOrMoved
};

struct NormalizedLiveUpdateEvent {
    NormalizedLiveUpdateKind kind = NormalizedLiveUpdateKind::Unknown;
    quint64 rawMask = 0;

    // From DFID_NAME / OLD_DFID_NAME / NEW_DFID_NAME where available.
    QByteArray fsid;
    QByteArray parentHandle;
    QString name;

    // From FID where available. Often present for CLOSE_WRITE/ATTRIB.
    QByteArray objectHandle;
};

inline QString normalizedLiveUpdateKindToString(NormalizedLiveUpdateKind kind)
{
    switch (kind) {
        case NormalizedLiveUpdateKind::Created:
            return QStringLiteral("Created");
        case NormalizedLiveUpdateKind::Deleted:
            return QStringLiteral("Deleted");
        case NormalizedLiveUpdateKind::MovedFrom:
            return QStringLiteral("MovedFrom");
        case NormalizedLiveUpdateKind::MovedTo:
            return QStringLiteral("MovedTo");
        case NormalizedLiveUpdateKind::MetadataChanged:
            return QStringLiteral("MetadataChanged");
        case NormalizedLiveUpdateKind::SelfDeletedOrMoved:
            return QStringLiteral("SelfDeletedOrMoved");
        case NormalizedLiveUpdateKind::Unknown:
        default:
            return QStringLiteral("Unknown");
    }
}

inline const LiveUpdateEventInfo* firstInfoOfType(
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

inline const LiveUpdateEventInfo* firstDirectoryEntryInfo(const LiveUpdateEvent& event)
{
    if (const LiveUpdateEventInfo* info = firstInfoOfType(event, QStringLiteral("DFID_NAME"))) {
        return info;
    }

    if (const LiveUpdateEventInfo* info = firstInfoOfType(event, QStringLiteral("OLD_DFID_NAME"))) {
        return info;
    }

    if (const LiveUpdateEventInfo* info = firstInfoOfType(event, QStringLiteral("NEW_DFID_NAME"))) {
        return info;
    }

    return nullptr;
}

inline NormalizedLiveUpdateEvent normalizeLiveUpdateEvent(const LiveUpdateEvent& event)
{
    NormalizedLiveUpdateEvent normalized;
    normalized.rawMask = event.mask;

    if (event.mask & FAN_CREATE) {
        normalized.kind = NormalizedLiveUpdateKind::Created;
    }
    else if (event.mask & FAN_DELETE) {
        normalized.kind = NormalizedLiveUpdateKind::Deleted;
    }
    else if (event.mask & FAN_MOVED_FROM) {
        normalized.kind = NormalizedLiveUpdateKind::MovedFrom;
    }
    else if (event.mask & FAN_MOVED_TO) {
        normalized.kind = NormalizedLiveUpdateKind::MovedTo;
    }
    else if (event.mask & (FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_MODIFY)) {
        normalized.kind = NormalizedLiveUpdateKind::MetadataChanged;
    }
    else if (event.mask & (FAN_DELETE_SELF | FAN_MOVE_SELF)) {
        normalized.kind = NormalizedLiveUpdateKind::SelfDeletedOrMoved;
    }
    else {
        normalized.kind = NormalizedLiveUpdateKind::Unknown;
    }

    if (const LiveUpdateEventInfo* entryInfo = firstDirectoryEntryInfo(event)) {
        normalized.fsid = entryInfo->fsid;
        normalized.parentHandle = entryInfo->handle;
        normalized.name = entryInfo->name;
    }

    if (const LiveUpdateEventInfo* objectInfo = firstInfoOfType(event, QStringLiteral("FID"))) {
        if (normalized.fsid.isEmpty()) {
            normalized.fsid = objectInfo->fsid;
        }

        normalized.objectHandle = objectInfo->handle;
    }

    return normalized;
}

#endif // KERYTHING_LIVEUPDATEEVENT_H