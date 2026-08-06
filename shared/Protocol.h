// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PROTOCOL_H
#define KERYTHING_PROTOCOL_H

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QString>

#include <optional>
#include <tuple>
#include <vector>

#include "BlockDevice.h"
#include "LiveUpdateEvent.h"

namespace Protocol {

static constexpr quint32 Magic = 0x4B455259; // 'KERY'
static constexpr quint16 Version = 1;
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
                << info.fsidHex
                << info.handleHex
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
               >> info.fsidHex
               >> info.handleHex
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

} // namespace Protocol

#endif //KERYTHING_PROTOCOL_H
