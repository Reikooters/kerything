// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "FanotifyWatcher.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>

#include <QFile>

#include <fcntl.h>
#include <linux/fanotify.h>
#include <sys/fanotify.h>
#include <unistd.h>

namespace {
    constexpr size_t EventBufferSize = 256 * 1024;

    QString bytesToHex(const unsigned char* data, int size)
    {
        QByteArray bytes(reinterpret_cast<const char*>(data), size);
        return QString::fromLatin1(bytes.toHex());
    }

    struct FanotifyNameInfo {
        QString infoType;
        QString fsidHex;
        QString handleHex;
        qint32 handleType = 0;
        QString name;
    };

    struct ParsedFanotifyInfo {
        QList<FanotifyNameInfo> nameInfos;
        QStringList otherInfoTypes;
    };

    QString infoTypeToString(uint8_t infoType)
    {
        switch (infoType) {
            case FAN_EVENT_INFO_TYPE_FID:
                return QStringLiteral("FID");
            case FAN_EVENT_INFO_TYPE_DFID_NAME:
                return QStringLiteral("DFID_NAME");
            case FAN_EVENT_INFO_TYPE_DFID:
                return QStringLiteral("DFID");
            case FAN_EVENT_INFO_TYPE_OLD_DFID_NAME:
                return QStringLiteral("OLD_DFID_NAME");
            case FAN_EVENT_INFO_TYPE_NEW_DFID_NAME:
                return QStringLiteral("NEW_DFID_NAME");
            default:
                return QStringLiteral("UNKNOWN_%1").arg(infoType);
        }
    }

    ParsedFanotifyInfo parseFanotifyInfoRecords(const fanotify_event_metadata* metadata)
    {
        ParsedFanotifyInfo parsed;

        const auto* info = reinterpret_cast<const fanotify_event_info_header*>(
            reinterpret_cast<const char*>(metadata) + FAN_EVENT_METADATA_LEN
        );

        const char* eventEnd = reinterpret_cast<const char*>(metadata) + metadata->event_len;

        while (reinterpret_cast<const char*>(info) + sizeof(fanotify_event_info_header) <= eventEnd) {
            if (info->len < sizeof(fanotify_event_info_header)) {
                break;
            }

            const char* infoEnd = reinterpret_cast<const char*>(info) + info->len;
            if (infoEnd > eventEnd) {
                break;
            }

            const QString infoTypeName = infoTypeToString(info->info_type);

            if (info->info_type == FAN_EVENT_INFO_TYPE_FID ||
                info->info_type == FAN_EVENT_INFO_TYPE_DFID ||
                info->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME ||
                info->info_type == FAN_EVENT_INFO_TYPE_OLD_DFID_NAME ||
                info->info_type == FAN_EVENT_INFO_TYPE_NEW_DFID_NAME) {
                const auto* fid = reinterpret_cast<const fanotify_event_info_fid*>(info);
                const auto* fileHandle = reinterpret_cast<const struct file_handle*>(fid->handle);

                FanotifyNameInfo nameInfo;
                nameInfo.infoType = infoTypeName;
                nameInfo.fsidHex = bytesToHex(
                    reinterpret_cast<const unsigned char*>(&fid->fsid),
                    sizeof(fid->fsid)
                );
                nameInfo.handleType = fileHandle->handle_type;
                nameInfo.handleHex = bytesToHex(
                    reinterpret_cast<const unsigned char*>(fileHandle->f_handle),
                    fileHandle->handle_bytes
                );

                const char* nameStart = reinterpret_cast<const char*>(
                    fileHandle->f_handle + fileHandle->handle_bytes
                );

                if (nameStart < infoEnd) {
                    nameInfo.name = QString::fromUtf8(nameStart);
                }

                parsed.nameInfos << std::move(nameInfo);
            } else {
                parsed.otherInfoTypes << infoTypeName;
            }

            info = reinterpret_cast<const fanotify_event_info_header*>(infoEnd);
        }

        return parsed;
    }
}

FanotifyWatcher::FanotifyWatcher(
    QString deviceId,
    QString mountPoint,
    QObject* parent
)
    : QObject(parent),
      deviceId_(std::move(deviceId)),
      mountPoint_(std::move(mountPoint))
{
    batchTimer_.setSingleShot(true);
    batchTimer_.setInterval(500);

    connect(&batchTimer_, &QTimer::timeout,
            this, &FanotifyWatcher::flushPendingEvents);
}

FanotifyWatcher::~FanotifyWatcher()
{
    flushPendingEvents();
    closeFanotifyFd();
}

bool FanotifyWatcher::start()
{
    if (fanotifyFd_ >= 0) {
        return true;
    }

    const unsigned int initFlags =
        FAN_CLASS_NOTIF |
        FAN_CLOEXEC |
        FAN_NONBLOCK |
        FAN_REPORT_FID |
        FAN_REPORT_DFID_NAME;

    fanotifyFd_ = ::fanotify_init(initFlags, O_RDONLY | O_LARGEFILE | O_CLOEXEC);

    if (fanotifyFd_ < 0) {
        const QString errorText = QStringLiteral("fanotify_init failed for %1: %2")
            .arg(mountPoint_, QString::fromLocal8Bit(std::strerror(errno)));

        std::cerr << errorText.toStdString() << "\n";
        Q_EMIT fatalError(deviceId_, errorText);
        return false;
    }

    const QByteArray mountPointNative = QFile::encodeName(mountPoint_);
    const int mountFd = ::open(
        mountPointNative.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );

    if (mountFd < 0) {
        const QString errorText = QStringLiteral("failed to open mount point %1 for fanotify: %2")
            .arg(mountPoint_, QString::fromLocal8Bit(std::strerror(errno)));

        std::cerr << errorText.toStdString() << "\n";
        closeFanotifyFd();
        Q_EMIT fatalError(deviceId_, errorText);
        return false;
    }

    const uint64_t mask =
        FAN_CREATE |
        FAN_DELETE |
        FAN_MOVED_FROM |
        FAN_MOVED_TO |
        FAN_CLOSE_WRITE |
        FAN_ATTRIB |
        FAN_DELETE_SELF |
        FAN_MOVE_SELF |
        FAN_ONDIR;

    if (::fanotify_mark(
            fanotifyFd_,
            FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
            mask,
            mountFd,
            nullptr
        ) != 0) {
        const int markErrno = errno;
        ::close(mountFd);

        const QString errorText = QStringLiteral("fanotify_mark failed for %1: %2")
            .arg(mountPoint_, QString::fromLocal8Bit(std::strerror(markErrno)));

        std::cerr << errorText.toStdString() << "\n";
        closeFanotifyFd();
        Q_EMIT fatalError(deviceId_, errorText);
        return false;
    }

    ::close(mountFd);

    notifier_ = new QSocketNotifier(fanotifyFd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated,
            this, &FanotifyWatcher::onActivated);

    std::cout << "fanotify: watching EXT4 filesystem deviceId="
              << deviceId_.toStdString()
              << " mountPoint="
              << mountPoint_.toStdString()
              << "\n";

    return true;
}

bool FanotifyWatcher::isRunning() const noexcept
{
    return fanotifyFd_ >= 0;
}

QString FanotifyWatcher::deviceId() const
{
    return deviceId_;
}

QString FanotifyWatcher::mountPoint() const
{
    return mountPoint_;
}

void FanotifyWatcher::onActivated()
{
    if (notifier_) {
        notifier_->setEnabled(false);
    }

    drainEvents();

    if (notifier_ && fanotifyFd_ >= 0) {
        notifier_->setEnabled(true);
    }
}

void FanotifyWatcher::drainEvents()
{
    std::array<char, EventBufferSize> buffer{};

    while (fanotifyFd_ >= 0) {
        ssize_t bytesRead = ::read(fanotifyFd_, buffer.data(), buffer.size());

        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            if (errno == EINTR) {
                continue;
            }

            const QString errorText = QStringLiteral("fanotify read failed for %1: %2")
                .arg(mountPoint_, QString::fromLocal8Bit(std::strerror(errno)));

            std::cerr << errorText.toStdString() << "\n";
            Q_EMIT fatalError(deviceId_, errorText);
            closeFanotifyFd();
            return;
        }

        if (bytesRead == 0) {
            return;
        }

        const auto* metadata = reinterpret_cast<const fanotify_event_metadata*>(buffer.data());

        while (FAN_EVENT_OK(metadata, bytesRead)) {
            captureEvent(metadata);
            metadata = FAN_EVENT_NEXT(metadata, bytesRead);
        }
    }
}

void FanotifyWatcher::captureEvent(const fanotify_event_metadata* metadata)
{
    if (metadata->vers != FANOTIFY_METADATA_VERSION) {
        const QString errorText = QStringLiteral("fanotify metadata version mismatch for %1")
            .arg(mountPoint_);

        std::cerr << errorText.toStdString() << "\n";
        Q_EMIT fatalError(deviceId_, errorText);
        closeFanotifyFd();
        return;
    }

    if (metadata->mask & FAN_Q_OVERFLOW) {
        std::cerr << "fanotify: queue overflow deviceId="
                  << deviceId_.toStdString()
                  << " mountPoint="
                  << mountPoint_.toStdString()
                  << "\n";

        pendingEvents_.clear();
        Q_EMIT overflow(deviceId_);

        if (metadata->fd >= 0) {
            ::close(metadata->fd);
        }

        return;
    }

    const ParsedFanotifyInfo parsedInfo = parseFanotifyInfoRecords(metadata);

    LiveUpdateEvent pending;
    pending.mask = metadata->mask;
    pending.infos.reserve(static_cast<std::size_t>(parsedInfo.nameInfos.size()));

    for (const FanotifyNameInfo& info : parsedInfo.nameInfos) {
        LiveUpdateEventInfo pendingInfo;
        pendingInfo.infoType = info.infoType;
        pendingInfo.fsidHex = info.fsidHex;
        pendingInfo.handleHex = info.handleHex;
        pendingInfo.handleType = info.handleType;
        pendingInfo.name = info.name;

        pending.infos.push_back(std::move(pendingInfo));
    }

    pendingEvents_.push_back(std::move(pending));

    if (!batchTimer_.isActive()) {
        batchTimer_.start();
    }

    if (metadata->fd >= 0) {
        ::close(metadata->fd);
    }
}

void FanotifyWatcher::flushPendingEvents()
{
    if (pendingEvents_.empty()) {
        return;
    }

    Q_EMIT eventsReady(
        deviceId_,
        mountPoint_,
        std::move(pendingEvents_)
    );

    pendingEvents_.clear();
}

void FanotifyWatcher::closeFanotifyFd()
{
    if (batchTimer_.isActive()) {
        batchTimer_.stop();
    }

    if (notifier_) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }

    if (fanotifyFd_ >= 0) {
        ::close(fanotifyFd_);
        fanotifyFd_ = -1;
    }
}