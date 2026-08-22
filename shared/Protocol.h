// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PROTOCOL_H
#define KERYTHING_PROTOCOL_H

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>
#include <tuple>
#include <vector>

#include "BlockDevice.h"
#include "LiveUpdateEvent.h"

namespace Protocol {

static constexpr quint32 Magic = 0x4B455259; // 'KERY'
static constexpr quint16 Version = 2;
static constexpr int HeaderSize = 16;
static const QString ServerName = "/run/kerythingd/kerythingd.sock";

enum class MessageType : quint16 {
    Ready = 1,
    ScanDevice = 2,
    CancelRequest = 3,
    ScanStarted = 4,
    ScanProgress = 5,
    ScanIndexResultFileRecordChunk = 6,
    ScanIndexResultStringPoolChunk = 7,
    ScanCompleted = 8,
    ScanCancelled = 9,
    ListKnownDevices = 10,
    KnownDevices = 11,
    LiveUpdateBatch = 12,
    LiveUpdateStatusChanged = 13,
    LiveUpdateOperationBatch = 14,
    SetLiveUpdateDevices = 15,
    Error = 99
};

struct MessageHeader {
    quint32 magic = Magic;
    quint16 version = Version;
    quint16 type = 0;
    quint32 requestId = 0;
    quint32 payloadSize = 0;
};

struct MessageFrame {
    MessageHeader header;
    QByteArray payload;
};

enum ScanIndexResultChunkType : quint8 {
    FileRecord = 1,
    StringPool = 2
};

struct ScanProgress {
    QString phase;
    QString unit;
    quint64 processed = 0;
    quint64 total = 0;

    [[nodiscard]] bool hasKnownTotal() const noexcept
    {
        return total > 0;
    }
};

inline QByteArray packMessage(MessageType type, quint32 requestId, const QByteArray& payload)
{
    QByteArray out;
    QDataStream stream(&out, QIODeviceBase::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setVersion(QDataStream::Qt_6_0);

    const quint32 magic = Magic;
    const quint16 version = Version;
    const quint16 msgType = static_cast<quint16>(type);
    const quint32 payloadSize = static_cast<quint32>(payload.size());

    stream << magic
           << version
           << msgType
           << requestId
           << payloadSize;

    out.append(payload);
    return out;
}

inline bool tryParseFrame(QByteArray& buffer, MessageFrame& frame)
{
    if (buffer.size() < HeaderSize) {
        return false;
    }

    QDataStream stream(buffer);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setVersion(QDataStream::Qt_6_0);

    stream >> frame.header.magic
           >> frame.header.version
           >> frame.header.type
           >> frame.header.requestId
           >> frame.header.payloadSize;

    if (frame.header.magic != Magic || frame.header.version != Version) {
        return false;
    }

    const int totalSize = HeaderSize + static_cast<int>(frame.header.payloadSize);
    if (buffer.size() < totalSize) {
        return false;
    }

    frame.payload = buffer.mid(HeaderSize, frame.header.payloadSize);
    buffer.remove(0, totalSize);
    return true;
}

inline QByteArray makeKnownDevicesPayload(const std::vector<BlockDevice>& devices)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << static_cast<quint32>(devices.size());

    for (const BlockDevice& device : devices) {
        out << device.deviceId
            << device.devNode
            << device.fsType
            << device.mountedFsType
            << device.uuid
            << device.partuuid
            << device.label
            << device.diskModel
            << device.mounted
            << device.mountPoints
            << device.primaryMountPoint;
    }

    return payload;
}

inline std::optional<std::vector<BlockDevice>> parseKnownDevicesPayload(const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    quint32 count = 0;
    in >> count;

    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }

    std::vector<BlockDevice> devices;
    devices.reserve(count);

    for (quint32 i = 0; i < count; ++i) {
        BlockDevice device;

        in >> device.deviceId
           >> device.devNode
           >> device.fsType
           >> device.mountedFsType
           >> device.uuid
           >> device.partuuid
           >> device.label
           >> device.diskModel
           >> device.mounted
           >> device.mountPoints
           >> device.primaryMountPoint;

        if (in.status() != QDataStream::Ok) {
            return std::nullopt;
        }

        devices.emplace_back(std::move(device));
    }

    return devices;
}

inline QByteArray makeScanDevicePayload(const QString& deviceId)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);
    out << deviceId;
    return payload;
}

inline QByteArray makeSetLiveUpdateDevicesPayload(const QStringList& deviceIds)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceIds;
    return payload;
}

inline std::optional<QStringList> parseSetLiveUpdateDevicesPayload(const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QStringList deviceIds;
    in >> deviceIds;

    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }

    deviceIds.removeDuplicates();
    deviceIds.removeAll(QString{});

    return deviceIds;
}

inline QByteArray makeLiveUpdateBatchPayload(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateEvent>& events)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceId
        << mountPoint
        << static_cast<quint32>(events.size());

    for (const LiveUpdateEvent& event : events) {
        out << event.mask
            << static_cast<quint32>(event.infos.size());

        for (const LiveUpdateEventInfo& info : event.infos) {
            out << info.infoType
                << info.fsid
                << info.handle
                << info.handleType
                << info.name;
        }
    }

    return payload;
}

inline std::optional<std::tuple<QString, QString, std::vector<LiveUpdateEvent>>>
parseLiveUpdateBatchPayload(const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    QString mountPoint;
    quint32 eventCount = 0;

    in >> deviceId
       >> mountPoint
       >> eventCount;

    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }

    std::vector<LiveUpdateEvent> events;
    events.reserve(eventCount);

    for (quint32 i = 0; i < eventCount; ++i) {
        LiveUpdateEvent event;
        quint32 infoCount = 0;

        in >> event.mask
           >> infoCount;

        if (in.status() != QDataStream::Ok) {
            return std::nullopt;
        }

        event.infos.reserve(infoCount);

        for (quint32 j = 0; j < infoCount; ++j) {
            LiveUpdateEventInfo info;

            in >> info.infoType
               >> info.fsid
               >> info.handle
               >> info.handleType
               >> info.name;

            if (in.status() != QDataStream::Ok) {
                return std::nullopt;
            }

            event.infos.push_back(std::move(info));
        }

        events.push_back(std::move(event));
    }

    return std::make_tuple(std::move(deviceId), std::move(mountPoint), std::move(events));
}

inline QByteArray makeLiveUpdateStatusChangedPayload(
    const QString& deviceId,
    LiveUpdateStatus status,
    const QString& reason)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceId
        << static_cast<quint8>(status)
        << reason;

    return payload;
}

inline std::optional<std::tuple<QString, LiveUpdateStatus, QString>>
parseLiveUpdateStatusChangedPayload(const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    quint8 rawStatus = 0;
    QString reason;

    in >> deviceId
       >> rawStatus
       >> reason;

    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }

    if (rawStatus > static_cast<quint8>(LiveUpdateStatus::StaleNeedsRescan)) {
        return std::nullopt;
    }

    return std::make_tuple(
        std::move(deviceId),
        static_cast<LiveUpdateStatus>(rawStatus),
        std::move(reason)
    );
}

inline QByteArray makeLiveUpdateOperationBatchPayload(
    const QString& deviceId,
    const QString& mountPoint,
    const std::vector<LiveUpdateOperation>& operations)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << deviceId
        << mountPoint
        << static_cast<quint32>(operations.size());

    for (const LiveUpdateOperation& operation : operations) {
        out << static_cast<quint8>(operation.kind)
            << operation.inode
            << operation.parentInode
            << operation.name
            << operation.size
            << operation.modificationTime
            << operation.isDirectory
            << operation.isSymlink
            << operation.reason;
    }

    return payload;
}

inline std::optional<std::tuple<QString, QString, std::vector<LiveUpdateOperation>>>
parseLiveUpdateOperationBatchPayload(const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString deviceId;
    QString mountPoint;
    quint32 operationCount = 0;

    in >> deviceId
       >> mountPoint
       >> operationCount;

    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }

    std::vector<LiveUpdateOperation> operations;
    operations.reserve(operationCount);

    for (quint32 i = 0; i < operationCount; ++i) {
        LiveUpdateOperation operation;
        quint8 rawKind = 0;

        in >> rawKind
           >> operation.inode
           >> operation.parentInode
           >> operation.name
           >> operation.size
           >> operation.modificationTime
           >> operation.isDirectory
           >> operation.isSymlink
           >> operation.reason;

        if (in.status() != QDataStream::Ok) {
            return std::nullopt;
        }

        if (rawKind > static_cast<quint8>(LiveUpdateOperationKind::NeedsRescan)) {
            return std::nullopt;
        }

        operation.kind = static_cast<LiveUpdateOperationKind>(rawKind);
        operations.push_back(std::move(operation));
    }

    return std::make_tuple(
        std::move(deviceId),
        std::move(mountPoint),
        std::move(operations)
    );
}

inline QByteArray makeScanProgressPayload(const ScanProgress& progress)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << progress.phase
        << progress.unit
        << progress.processed
        << progress.total;

    return payload;
}

inline std::optional<ScanProgress> parseScanProgressPayload(const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    ScanProgress progress;
    in >> progress.phase
       >> progress.unit
       >> progress.processed
       >> progress.total;

    if (in.status() != QDataStream::Ok) {
        return std::nullopt;
    }

    return progress;
}

} // namespace Protocol

#endif //KERYTHING_PROTOCOL_H
