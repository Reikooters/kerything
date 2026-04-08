// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ClientConnection.h"

#include "ScannerWorker.h"
#include "FileRecord.h"

#include <iostream>
#include <QDataStream>
#include <QMetaObject>

ClientConnection::ClientConnection(QLocalSocket* socket, QObject* parent)
    : QObject(parent),
      socket_(socket)
{
    Q_ASSERT(socket_);

    connect(socket_, &QLocalSocket::readyRead,
            this, &ClientConnection::onReadyRead);

    connect(socket_, &QLocalSocket::disconnected,
            this, &ClientConnection::onDisconnected);

    scannerWorker_ = new ScannerWorker();
    scannerWorker_->moveToThread(&scanThread_);

    connect(&scanThread_, &QThread::finished,
            scannerWorker_, &QObject::deleteLater);

    connect(scannerWorker_, &ScannerWorker::scanStarted,
            this, [this](quint32 requestId) {
                sendScanStarted(requestId);
            });

    connect(scannerWorker_, &ScannerWorker::scanProgress,
            this, [this](quint32 requestId, quint64 filesSeen, quint64 filesEmitted) {
                sendScanProgress(requestId, filesSeen, filesEmitted);
            });

    connect(scannerWorker_, &ScannerWorker::scanChunkReady,
            this, [this](quint32 requestId, const std::vector<FileRecord>& chunk) {
                sendScanChunk(requestId, chunk);
            });

    connect(scannerWorker_, &ScannerWorker::scanCompleted,
            this, [this](quint32 requestId) {
                activeJobs_.remove(requestId);
                sendScanCompleted(requestId);
            });

    connect(scannerWorker_, &ScannerWorker::scanCancelled,
            this, [this](quint32 requestId) {
                activeJobs_.remove(requestId);
                sendScanCancelled(requestId);
            });

    connect(scannerWorker_, &ScannerWorker::scanError,
            this, [this](quint32 requestId, const QString& errorText) {
                activeJobs_.remove(requestId);
                sendError(requestId, errorText);
            });

    scanThread_.start();
}

ClientConnection::~ClientConnection()
{
    shuttingDown_ = true;
    cancelAllJobs();

    scanThread_.quit();
    scanThread_.wait();

    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

void ClientConnection::onReadyRead()
{
    if (shuttingDown_ || !socket_) {
        return;
    }

    framer_.append(socket_->readAll());

    Protocol::MessageFrame frame;
    while (framer_.tryTake(frame)) {
        handleFrame(frame.header, frame.payload);
    }
}

void ClientConnection::onDisconnected()
{
    if (shuttingDown_) {
        return;
    }

    shuttingDown_ = true;
    cancelAllJobs();

    emit disconnected(this);
    deleteLater();
}

void ClientConnection::handleFrame(const Protocol::MessageHeader& header, const QByteArray& payload)
{
    const auto type = static_cast<Protocol::MessageType>(header.type);

    std::cout << "Received message: type: " << header.type << ", requestId: " << header.requestId << ", payloadSize: " << header.payloadSize << "\n";

    switch (type) {
        case Protocol::MessageType::ScanDevice:
            handleScanDevice(header.requestId, payload);
            break;

        case Protocol::MessageType::CancelRequest:
            handleCancelRequest(header.requestId);
            break;

        default:
            sendError(header.requestId, QStringLiteral("unknown request type"));
            break;
    }
}

void ClientConnection::handleScanDevice(quint32 requestId, const QByteArray& payload)
{
    QDataStream in(payload);
    in.setByteOrder(QDataStream::BigEndian);
    in.setVersion(QDataStream::Qt_6_0);

    QString devicePath;
    QString fsType;
    in >> devicePath >> fsType;

    if (in.status() != QDataStream::Ok) {
        sendError(requestId, QStringLiteral("malformed ScanDevice payload"));
        return;
    }

    auto job = std::make_shared<ScanJob>();
    job->requestId = requestId;
    job->devicePath = devicePath;
    job->fsType = fsType;

    activeJobs_.insert(requestId, job);

    // Run the scan in the worker thread.
    QMetaObject::invokeMethod(scannerWorker_, [this, job]() {
        scannerWorker_->startScan(job);
    }, Qt::QueuedConnection);
}

void ClientConnection::handleCancelRequest(quint32 requestId)
{
    const auto it = activeJobs_.find(requestId);
    if (it == activeJobs_.end()) {
        sendError(requestId, QStringLiteral("no active job to cancel"));
        return;
    }

    it.value()->cancelled.store(true, std::memory_order_relaxed);
}

void ClientConnection::sendReady()
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);
    out << Protocol::Version << true;

    sendFrame(Protocol::MessageType::Ready, 0, payload);
}

void ClientConnection::sendScanStarted(quint32 requestId)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << QStringLiteral("started");

    sendFrame(Protocol::MessageType::ScanStarted, requestId, payload);
}

void ClientConnection::sendScanProgress(quint32 requestId,
                                        quint64 filesSeen,
                                        quint64 filesEmitted)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << filesSeen
        << filesEmitted;

    sendFrame(Protocol::MessageType::ScanProgress, requestId, payload);
}

bool ClientConnection::sendScanChunk(quint32 requestId,
                                     const std::vector<FileRecord>& chunk)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << static_cast<quint32>(chunk.size());

    for (const auto& rec : chunk) {
        out << rec.fsIndex
            << rec.parentRecordIdx
            << rec.size
            << rec.modificationTime
            << rec.nameOffset
            << rec.nameLen
            << rec.flags;
    }

    return sendFrame(Protocol::MessageType::ScanIndexResultChunk, requestId, payload);
}

void ClientConnection::sendScanCompleted(quint32 requestId)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << QStringLiteral("completed");

    sendFrame(Protocol::MessageType::ScanCompleted, requestId, payload);
}

void ClientConnection::sendScanCancelled(quint32 requestId)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << QStringLiteral("cancelled");

    sendFrame(Protocol::MessageType::ScanCancelled, requestId, payload);
}

void ClientConnection::sendError(quint32 requestId, const QString& errorText)
{
    QByteArray payload;
    QDataStream out(&payload, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << errorText;

    sendFrame(Protocol::MessageType::Error, requestId, payload);
}

bool ClientConnection::sendFrame(const Protocol::MessageType type, const quint32 requestId, const QByteArray& payload) const
{
    if (shuttingDown_ || !socket_) {
        return false;
    }

    const QByteArray bytes = encodeFrame(type, requestId, payload);
    const qint64 written = socket_->write(bytes);

    if (written != bytes.size()) {
        return false;
    }

    socket_->flush();
    return true;
}

QByteArray ClientConnection::encodeFrame(Protocol::MessageType type,
                                        quint32 requestId,
                                        const QByteArray& payload) const
{
    Protocol::MessageHeader header{};
    header.type = static_cast<quint16>(type);
    header.requestId = requestId;
    header.payloadSize = static_cast<quint32>(payload.size());

    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out.setVersion(QDataStream::Qt_6_0);

    out << header.magic
        << header.version
        << header.type
        << header.requestId
        << header.payloadSize;

    bytes.append(payload);
    return bytes;
}

void ClientConnection::cancelAllJobs()
{
    for (auto& job : activeJobs_) {
        if (job) {
            job->cancelled.store(true, std::memory_order_relaxed);
        }
    }

    activeJobs_.clear();
}