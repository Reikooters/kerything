// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PROTOCOL_H
#define KERYTHING_PROTOCOL_H

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

namespace Protocol {

static constexpr quint32 Magic = 0x4B455259; // 'KERY'
static constexpr quint16 Version = 1;
static constexpr int HeaderSize = 16;
static const QString ServerName = "kerythingd";

enum class MessageType : quint16 {
    Ready = 1,
    Error = 2
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

inline QByteArray makeReadyPayload()
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);
    out << Version << true;
    return payload;
}

inline QByteArray makeErrorPayload(const QString& errorText)
{
    QByteArray payload;
    QDataStream out(&payload, QIODeviceBase::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);
    out << errorText;
    return payload;
}

} // namespace protocol

#endif //KERYTHING_PROTOCOL_H
