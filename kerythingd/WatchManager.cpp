// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "WatchManager.h"
#include "IndexerService.h"

#include <QSocketNotifier>
#include <QDir>
#include <QSet>
#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QVariantMap>

#include <cerrno>
#include <cstring>

#include <sys/fanotify.h>
#include <fcntl.h>
#include <unistd.h>

size_t WatchManager::KeyHash::operator()(const Key& k) const noexcept {
    size_t h = std::hash<quint32>{}(k.uid);
    h ^= (std::hash<QString>{}(k.deviceId) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    return h;
}

WatchManager::WatchManager(IndexerService* svc, QObject* parent)
    : QObject(parent), m_svc(svc) {}

WatchManager::~WatchManager() {
    for (auto& kv : m_entries) stopEntry(kv.second);
    m_entries.clear();
}

void WatchManager::stopEntry(Entry& e) {
    if (e.notifier) {
        e.notifier->setEnabled(false);
        e.notifier->deleteLater();
        e.notifier = nullptr;
    }
    if (e.batchTimer) {
        e.batchTimer->stop();
        e.batchTimer->deleteLater();
        e.batchTimer = nullptr;
    }
    if (e.fanFd >= 0) {
        ::close(e.fanFd);
        e.fanFd = -1;
    }

    // Reset pending batch state (prevents stale carry-over on re-arm)
    e.pendingTouchedByKey.clear();
    e.overflowSeen = false;
}

WatchManager::Status WatchManager::statusFor(quint32 uid, const QString& deviceId) const {
    const Key k{uid, deviceId};
    auto it = m_entries.find(k);
    if (it == m_entries.end()) {
        // Set status to pending before the first refresh has established mount state,
        // and avoid overloading "unknown" (reserved for unhandled/bug states).
        return Status{
            QStringLiteral("pending"),
            QString(),
            QString()
        };
    }

    Status out = it->second.status;

    // Ensure mode/detail are populated consistently from Entry while watching.
    // Mode must be empty unless actively watching.
    if (out.state == QStringLiteral("watching")) {
        out.mode = it->second.watchingMode;
    }
    else {
        out.mode.clear();
    }

    return out;
}

WatchManager::RetryInfo WatchManager::retryInfoFor(quint32 uid, const QString& deviceId) const {
    const Key k{uid, deviceId};
    auto it = m_entries.find(k);
    if (it == m_entries.end()) {
        return RetryInfo{};
    }

    const Entry& e = it->second;

    RetryInfo out;
    out.failCount = static_cast<quint32>(std::max(0, e.failCount));
    out.nextRetryMs = e.nextRetryMs;
    out.retryOnlyOnMountChange = e.retryOnlyOnMountChange;

    if (e.nextRetryMs > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 deltaMs = e.nextRetryMs - now;
        out.retryInSec = deltaMs > 0 ? static_cast<quint32>((deltaMs + 999) / 1000) : 0u;
    } else {
        out.retryInSec = 0;
    }

    return out;
}

static qint64 nowMsUtc() {
    return QDateTime::currentMSecsSinceEpoch();
}

static qint64 backoffMsForFailCount(int failCount) {
    // 0 -> 0ms (no backoff), 1 -> 30s, 2 -> 60s, 3 -> 120s, ... capped at 10 minutes.
    static constexpr qint64 kBase = 30'000;
    static constexpr qint64 kCap  = 10 * 60'000;

    if (failCount <= 0) return 0;

    // exponential: base * 2^(failCount-1)
    qint64 ms = kBase;
    for (int i = 1; i < failCount; ++i) {
        if (ms > (kCap / 2)) { ms = kCap; break; }
        ms *= 2;
    }
    if (ms > kCap) ms = kCap;
    return ms;
}

void WatchManager::ensureEntryWatching(const Key& k, Entry& e, const QString& mountPoint) {
    // If mount point changed, recreate and reset backoff immediately
    if (e.mountPoint != mountPoint) {
        e.failCount = 0;
        e.nextRetryMs = 0;
        e.lastArmError.clear();
        e.retryOnlyOnMountChange = false;
        e.watchingMode.clear();
    }

    // If already armed and mountpoint unchanged, keep it.
    if (e.fanFd >= 0 && e.mountPoint == mountPoint) {
        e.status = Status{QStringLiteral("watching"), QString(), e.watchingMode};
        e.failCount = 0;
        e.nextRetryMs = 0;
        e.lastArmError.clear();

        // Ensure batch timer exists
        if (!e.batchTimer) {
            e.batchTimer = new QTimer(this);
            e.batchTimer->setSingleShot(true);
            connect(e.batchTimer, &QTimer::timeout, this, [this, k]() {
                auto it = m_entries.find(k);
                if (it == m_entries.end()) return;
                Entry& ee = it->second;

                const bool overflow = ee.overflowSeen;

                QVariantList touched;
                touched.reserve(static_cast<int>(ee.pendingTouchedByKey.size()));

                for (const auto& kv : ee.pendingTouchedByKey) {
                    const Entry::PendingTouched& pt = kv.second;

                    QVariantMap m;
                    m.insert(QStringLiteral("fsidHex"), pt.fsidHex);
                    m.insert(QStringLiteral("handleHex"), pt.handleHex);
                    m.insert(QStringLiteral("name"), pt.name);
                    m.insert(QStringLiteral("mask"), QVariant::fromValue<qulonglong>(pt.mask));
                    touched.push_back(m);
                }

                const int count = touched.size();

                ee.pendingTouchedByKey.clear();
                ee.overflowSeen = false;

                qInfo().noquote() << QStringLiteral("[watch] dispatch uid=%1 device=%2 touched=%3 overflow=%4")
                                     .arg(k.uid)
                                     .arg(k.deviceId)
                                     .arg(count)
                                     .arg(overflow ? QStringLiteral("1") : QStringLiteral("0"));

                if (m_svc && (count > 0 || overflow)) {
                    m_svc->applyWatchBatch(k.uid, k.deviceId, touched, overflow);
                }
            });
        }

        return;
    }

    stopEntry(e);
    e.mountPoint = mountPoint;

    const QString cleanMp = QDir::cleanPath(mountPoint);
    const QByteArray mpBytes = QFile::encodeName(cleanMp);

    const int mpFd = ::open(mpBytes.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (mpFd < 0) {
        e.status = Status{
            QStringLiteral("error"),
            QStringLiteral("open(%1) failed (%2): %3")
                .arg(cleanMp)
                .arg(errno)
                .arg(QString::fromLocal8Bit(std::strerror(errno)))
        };

        // Backoff scheduling (treat as an arming failure)
        e.failCount = std::min(e.failCount + 1, 30);
        e.nextRetryMs = nowMsUtc() + backoffMsForFailCount(e.failCount);
        e.lastArmError = e.status.error;

        stopEntry(e);
        return;
    }

    auto setArmError = [&](int err, const QString& msg) {
        e.status = Status{QStringLiteral("error"), msg};

        // For EINVAL, assume "unsupported on this mount"; don't keep retrying.
        if (err == EINVAL) {
            e.retryOnlyOnMountChange = true;
            e.nextRetryMs = 0; // so GUI doesn't show a countdown that will never happen
        } else {
            e.retryOnlyOnMountChange = false;
            e.failCount = std::min(e.failCount + 1, 30);
            e.nextRetryMs = nowMsUtc() + backoffMsForFailCount(e.failCount);
        }

        e.lastArmError = msg;
    };

    // ---------- Attempt 1: fanotify filesystem events
    {
        const int fdFs = fanotify_init(
            FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK |
            FAN_REPORT_FID | FAN_REPORT_DFID_NAME,
            O_RDONLY | O_LARGEFILE
        );

        if (fdFs >= 0) {
            const uint64_t fsMask =
                FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO |
                FAN_ATTRIB | FAN_MODIFY | FAN_CLOSE_WRITE |
                FAN_DELETE_SELF | FAN_MOVE_SELF |
                FAN_ONDIR;

            if (fanotify_mark(fdFs,
                              FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
                              fsMask,
                              mpFd,
                              nullptr) == 0)
            {
                e.fanFd = fdFs;
                ::close(mpFd);

                e.notifier = new QSocketNotifier(fdFs, QSocketNotifier::Read, this);
                connect(e.notifier, &QSocketNotifier::activated, this, [this, k]() {
                    onFanotifyReadable(k);
                });

                // Create batch timer
                e.batchTimer = new QTimer(this);
                e.batchTimer->setSingleShot(true);
                connect(e.batchTimer, &QTimer::timeout, this, [this, k]() {
                    auto it = m_entries.find(k);
                    if (it == m_entries.end()) return;
                    Entry& ee = it->second;

                    const bool overflow = ee.overflowSeen;

                    QVariantList touched;
                    touched.reserve(static_cast<int>(ee.pendingTouchedByKey.size()));

                    for (const auto& kv : ee.pendingTouchedByKey) {
                        const Entry::PendingTouched& pt = kv.second;

                        QVariantMap m;
                        m.insert(QStringLiteral("fsidHex"), pt.fsidHex);
                        m.insert(QStringLiteral("handleHex"), pt.handleHex);
                        m.insert(QStringLiteral("name"), pt.name);
                        m.insert(QStringLiteral("mask"), QVariant::fromValue<qulonglong>(pt.mask));
                        touched.push_back(m);
                    }

                    const int count = touched.size();

                    ee.pendingTouchedByKey.clear();
                    ee.overflowSeen = false;

                    qInfo().noquote() << QStringLiteral("[watch] dispatch uid=%1 device=%2 touched=%3 overflow=%4")
                                         .arg(k.uid)
                                         .arg(k.deviceId)
                                         .arg(count)
                                         .arg(overflow ? QStringLiteral("1") : QStringLiteral("0"));

                    if (m_svc && (count > 0 || overflow)) {
                        m_svc->applyWatchBatch(k.uid, k.deviceId, touched, overflow);
                    }
                });

                e.watchingMode = QStringLiteral("filesystemEvents");
                e.status = Status{QStringLiteral("watching"), QString(), e.watchingMode};

                // Success: reset backoff
                e.failCount = 0;
                e.nextRetryMs = 0;
                e.lastArmError.clear();
                e.retryOnlyOnMountChange = false;
                return;
            }

            const int markErr = errno;
            ::close(fdFs);

            // keep going to fallback
            (void)markErr;
        }
    }

    // ---------- Attempt 2 (fallback): mount mark with simpler events
    {
        const int fdMount = fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK,
                                          O_RDONLY | O_LARGEFILE);
        if (fdMount < 0) {
            const int eInit = errno;
            ::close(mpFd);
            setArmError(eInit,
                        QStringLiteral("fanotify_init failed (%1): %2")
                            .arg(eInit)
                            .arg(QString::fromLocal8Bit(std::strerror(eInit))));
            stopEntry(e);
            return;
        }

        const uint64_t mountMask =
            FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO |
            FAN_CLOSE_WRITE | FAN_MODIFY | FAN_ATTRIB |
            FAN_ONDIR;

        if (fanotify_mark(fdMount,
                          FAN_MARK_ADD | FAN_MARK_MOUNT,
                          mountMask,
                          mpFd,
                          nullptr) != 0)
        {
            const int eMark = errno;
            ::close(fdMount);
            ::close(mpFd);
            setArmError(eMark,
                        QStringLiteral("fanotify_mark failed (%1): %2")
                            .arg(eMark)
                            .arg(QString::fromLocal8Bit(std::strerror(eMark))));
            stopEntry(e);
            return;
        }

        e.fanFd = fdMount;
        ::close(mpFd);

        e.notifier = new QSocketNotifier(fdMount, QSocketNotifier::Read, this);
        connect(e.notifier, &QSocketNotifier::activated, this, [this, k]() {
            onFanotifyReadable(k);
        });

        // Create batch timer
        e.batchTimer = new QTimer(this);
        e.batchTimer->setSingleShot(true);
        connect(e.batchTimer, &QTimer::timeout, this, [this, k]() {
            auto it = m_entries.find(k);
            if (it == m_entries.end()) return;
            Entry& ee = it->second;

            const bool overflow = ee.overflowSeen;

            QVariantList touched;
            touched.reserve(static_cast<int>(ee.pendingTouchedByKey.size()));

            for (const auto &kv: ee.pendingTouchedByKey) {
                const Entry::PendingTouched &pt = kv.second;

                QVariantMap m;
                m.insert(QStringLiteral("fsidHex"), pt.fsidHex);
                m.insert(QStringLiteral("handleHex"), pt.handleHex);
                m.insert(QStringLiteral("name"), pt.name);
                m.insert(QStringLiteral("mask"), QVariant::fromValue<qulonglong>(pt.mask));
                touched.push_back(m);
            }

            const int count = touched.size();

            ee.pendingTouchedByKey.clear();
            ee.overflowSeen = false;

            qInfo().noquote() << QStringLiteral("[watch] dispatch uid=%1 device=%2 touched=%3 overflow=%4")
                    .arg(k.uid)
                    .arg(k.deviceId)
                    .arg(count)
                    .arg(overflow ? QStringLiteral("1") : QStringLiteral("0"));

            if (m_svc && (count > 0 || overflow)) {
                m_svc->applyWatchBatch(k.uid, k.deviceId, touched, overflow);
            }
        });

        e.watchingMode = QStringLiteral("mountFallback");
        e.status = Status{QStringLiteral("watching"), QString(), e.watchingMode};

        // Success: reset backoff
        e.failCount = 0;
        e.nextRetryMs = 0;
        e.lastArmError.clear();
        e.retryOnlyOnMountChange = false;
    }
}

static QByteArray toHexCompact(const void* data, size_t n) {
    if (!data || n == 0) return {};
    return QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(n)).toHex();
}

void WatchManager::onFanotifyReadable(const Key& k) {
    auto it = m_entries.find(k);
    if (it == m_entries.end()) return;
    Entry& e = it->second;
    if (e.fanFd < 0) return;

    alignas(fanotify_event_metadata) char buf[8192];

    bool shouldLogEdge = false;

    // If we drained at least one non-overflow event but didn't parse DFID_NAME,
    // add a generic token so fallback-mode watching still triggers a rescan.
    bool sawNonOverflowEvent = false;
    bool parsedAnyDfidName = false;

    while (true) {
        const ssize_t nread = ::read(e.fanFd, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;

            // If reads error out, mark status but don’t crash the daemon.
            e.status = Status{
                QStringLiteral("error"),
                QStringLiteral("fanotify read failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)))
            };

            qWarning().noquote() << QStringLiteral("[watch] uid=%1 device=%2 fanotify read error: %3")
                                    .arg(k.uid)
                                    .arg(k.deviceId)
                                    .arg(e.status.error);
            break;
        }
        if (nread == 0) break;

        // FAN_EVENT_* macros mutate the length variable; it must be mutable.
        ssize_t len = nread;

        // Consume events; for prototype we don’t need to inspect them.
        auto* meta = reinterpret_cast<fanotify_event_metadata*>(buf);

        while (FAN_EVENT_OK(meta, len)) {
            if (meta->mask & FAN_Q_OVERFLOW) {
                e.overflowSeen = true;
            } else {
                sawNonOverflowEvent = true;

                // Parse extra info records (DFID_NAME gives parent dir handle + filename)
                const char* infoPtr = reinterpret_cast<const char*>(meta) + sizeof(*meta);
                ssize_t infoLen = static_cast<ssize_t>(meta->event_len) - static_cast<ssize_t>(sizeof(*meta));

                while (infoLen >= static_cast<ssize_t>(sizeof(fanotify_event_info_header))) {
                    auto* hdr = reinterpret_cast<const fanotify_event_info_header*>(infoPtr);
                    if (hdr->len < sizeof(*hdr) || hdr->len > static_cast<uint32_t>(infoLen)) {
                        break; // malformed; stop parsing this event
                    }

                    if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                        parsedAnyDfidName = true;

                        auto* fid = reinterpret_cast<const fanotify_event_info_fid*>(infoPtr);

                        const QByteArray fsidHexB = toHexCompact(&fid->fsid, sizeof(fid->fsid));
                        const QString fsidHex = QString::fromLatin1(fsidHexB);

                        // fanotify headers differ across kernel/libc versions:
                        // fid->handle may be a byte array payload, not a struct member.
                        const auto* fh = reinterpret_cast<const file_handle*>(
                            static_cast<const void*>(fid->handle)
                        );

                        // Defensive bounds check: we need at least a file_handle header.
                        const size_t fidBase = offsetof(fanotify_event_info_fid, handle);
                        if (hdr->len < fidBase + sizeof(file_handle)) {
                            infoPtr += hdr->len;
                            infoLen -= static_cast<ssize_t>(hdr->len);
                            continue;
                        }

                        // file_handle is variable-sized: sizeof(file_handle) + handle_bytes
                        const size_t handleBytes = static_cast<size_t>(fh->handle_bytes);
                        const size_t handleBlobSize = sizeof(file_handle) + handleBytes;

                        // Defensive bounds: ensure handle blob fits in this info record
                        if (hdr->len < fidBase + handleBlobSize) {
                            infoPtr += hdr->len;
                            infoLen -= static_cast<ssize_t>(hdr->len);
                            continue;
                        }

                        const QByteArray handleHexB = toHexCompact(
                            static_cast<const void*>(fid->handle),
                            handleBlobSize
                        );
                        const QString handleHex = QString::fromLatin1(handleHexB);

                        const size_t nameOff = fidBase + handleBlobSize;
                        QString name;

                        if (hdr->len > nameOff) {
                            const char* namePtr = reinterpret_cast<const char*>(infoPtr) + nameOff;
                            const size_t nameMax = static_cast<size_t>(hdr->len - nameOff);

                            // name is NUL-terminated (kernel provides), but be defensive.
                            size_t n = 0;
                            for (; n < nameMax; ++n) {
                                if (namePtr[n] == '\0') break;
                            }
                            if (n > 0) {
                                name = QString::fromLocal8Bit(namePtr, static_cast<int>(n));
                            }
                        }

                        if (!fsidHex.isEmpty() && !handleHex.isEmpty() && !name.isEmpty()) {
                            const QString keyStr = fsidHex + QStringLiteral(":") + handleHex + QStringLiteral(":") + name;

                            Entry::PendingTouched& pt = e.pendingTouchedByKey[keyStr];
                            pt.fsidHex = fsidHex;
                            pt.handleHex = handleHex;
                            pt.name = name;
                            pt.mask |= static_cast<quint64>(meta->mask);
                        }
                    }

                    infoPtr += hdr->len;
                    infoLen -= static_cast<ssize_t>(hdr->len);
                }
            }

            if (meta->fd >= 0) ::close(meta->fd); // required for some event types
            meta = FAN_EVENT_NEXT(meta, len);
        }

        if (!e.batchTimer) {
            // Defensive: should exist while watching, but don’t crash if not.
            e.batchTimer = new QTimer(this);
            e.batchTimer->setSingleShot(true);
            connect(e.batchTimer, &QTimer::timeout, this, [this, k]() {
                auto it2 = m_entries.find(k);
                if (it2 == m_entries.end()) return;
                Entry& ee = it2->second;

                const bool overflow = ee.overflowSeen;

                QVariantList touched;
                touched.reserve(static_cast<int>(ee.pendingTouchedByKey.size()));

                for (const auto& kv : ee.pendingTouchedByKey) {
                    const Entry::PendingTouched& pt = kv.second;

                    QVariantMap m;
                    m.insert(QStringLiteral("fsidHex"), pt.fsidHex);
                    m.insert(QStringLiteral("handleHex"), pt.handleHex);
                    m.insert(QStringLiteral("name"), pt.name);
                    m.insert(QStringLiteral("mask"), QVariant::fromValue<qulonglong>(pt.mask));
                    touched.push_back(m);
                }

                const int count = touched.size();

                ee.pendingTouchedByKey.clear();
                ee.overflowSeen = false;

                qInfo().noquote() << QStringLiteral("[watch] dispatch uid=%1 device=%2 touched=%3 overflow=%4")
                                     .arg(k.uid)
                                     .arg(k.deviceId)
                                     .arg(count)
                                     .arg(overflow ? QStringLiteral("1") : QStringLiteral("0"));

                if (m_svc && (count > 0 || overflow)) {
                    m_svc->applyWatchBatch(k.uid, k.deviceId, touched, overflow);
                }
            });
        }

        if (!e.batchTimer->isActive()) {
            shouldLogEdge = true;
        }
        e.batchTimer->start(kBatchMs);
    }

    // If we saw changes but couldn't parse DFID_NAME tokens, enqueue one generic token.
    if (sawNonOverflowEvent && !parsedAnyDfidName) {
        const QString keyStr = QStringLiteral(":: *"); // stable-ish key; content doesn't matter externally
        Entry::PendingTouched& pt = e.pendingTouchedByKey[keyStr];
        pt.fsidHex.clear();
        pt.handleHex.clear();
        pt.name = QStringLiteral("*");
        pt.mask |= 1ULL;
    }

    if (shouldLogEdge) {
        qInfo().noquote() << QStringLiteral("[watch] uid=%1 device=%2 queued batch (%3ms)")
                             .arg(k.uid)
                             .arg(k.deviceId)
                             .arg(kBatchMs);
    }
}

void WatchManager::refreshWatchesForUid(quint32 uid) {
    if (!m_svc) return;

    // Ask IndexerService which (deviceId -> mountPoint) are eligible right now.
    const auto targets = m_svc->watchTargetsForUid(uid);

    QSet<QString> wantDeviceIds;
    wantDeviceIds.reserve(static_cast<int>(targets.size()));
    for (const auto& t : targets) wantDeviceIds.insert(t.deviceId);

    // Remove any entries for this uid that are no longer wanted (e.g. watch disabled, index removed).
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (it->first.uid != uid) {
            ++it;
            continue;
        }

        if (!wantDeviceIds.contains(it->first.deviceId)) {
            stopEntry(it->second);
            it = m_entries.erase(it);
            continue;
        }

        ++it;
    }

    const qint64 now = nowMsUtc();

    // Ensure each target is correctly armed (or marked notMounted).
    for (const auto& t : targets) {
        const Key k{uid, t.deviceId};
        Entry& e = m_entries[k];

        if (t.mountPoint.trimmed().isEmpty()) {
            e.status = Status{
                QStringLiteral("notMounted"),
                QStringLiteral("Device is not mounted."),
                QString()
            };
            stopEntry(e);

            // Reset backoff when not mounted; next mount gets an immediate attempt.
            e.failCount = 0;
            e.nextRetryMs = 0;
            e.lastArmError.clear();
            e.retryOnlyOnMountChange = false;
            e.watchingMode.clear();
            continue;
        }

        // If we previously failed to arm, respect backoff unless mountpoint changed.
        const QString cleanMp = QDir::cleanPath(t.mountPoint);
        const bool mountChanged = (e.mountPoint != cleanMp);

        // If we hit a "permanent" arming error (e.g. EINVAL), don't retry until mount changes.
        if (!mountChanged && e.fanFd < 0 && e.status.state == QStringLiteral("error") && e.retryOnlyOnMountChange) {
            continue;
        }

        // Otherwise respect timed backoff.
        if (!mountChanged && e.fanFd < 0 && e.status.state == QStringLiteral("error") && e.nextRetryMs > now) {
            // Keep status as-is; just skip re-arming until backoff expires.
            continue;
        }

        ensureEntryWatching(k, e, cleanMp);
    }
}