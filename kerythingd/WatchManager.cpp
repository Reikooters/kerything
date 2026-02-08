// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "WatchManager.h"
#include "IndexerService.h"

#include <QSocketNotifier>
#include <QDir>
#include <QSet>
#include <QDebug>

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
    if (e.quietTimer) {
        e.quietTimer->stop();
        e.quietTimer->deleteLater();
        e.quietTimer = nullptr;
    }
    if (e.fanFd >= 0) {
        ::close(e.fanFd);
        e.fanFd = -1;
    }
}

WatchManager::Status WatchManager::statusFor(quint32 uid, const QString& deviceId) const {
    const Key k{uid, deviceId};
    auto it = m_entries.find(k);
    if (it == m_entries.end()) {
        // Avoid showing a scary "error" if we're queried before the first refresh.
        return Status{
            QStringLiteral("notMounted"),
            QStringLiteral("Watch status pending refresh.")
        };
    }
    return it->second.status;
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
    }

    // If already armed and mountpoint unchanged, keep it.
    if (e.fanFd >= 0 && e.mountPoint == mountPoint) {
        e.status = Status{QStringLiteral("watching"), QString()};
        e.failCount = 0;
        e.nextRetryMs = 0;
        e.lastArmError.clear();
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
                FAN_ATTRIB | FAN_MODIFY | FAN_DELETE_SELF | FAN_MOVE_SELF |
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

                e.quietTimer = new QTimer(this);
                e.quietTimer->setSingleShot(true);
                connect(e.quietTimer, &QTimer::timeout, this, [this, k]() {
                    auto it = m_entries.find(k);
                    if (it == m_entries.end()) return;
                    Entry& ee = it->second;
                    if (!ee.dirty) return;
                    ee.dirty = false;

                    if (m_svc) {
                        m_svc->startAutoRescanIfAllowed(k.uid, k.deviceId);
                    }
                });

                e.status = Status{
                    QStringLiteral("watching"),
                    QStringLiteral("Watching active (fanotify filesystem events).")
                };

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

        const uint64_t mountMask = FAN_CLOSE_WRITE | FAN_MODIFY | FAN_ATTRIB | FAN_ONDIR;

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

        e.quietTimer = new QTimer(this);
        e.quietTimer->setSingleShot(true);
        connect(e.quietTimer, &QTimer::timeout, this, [this, k]() {
            auto it = m_entries.find(k);
            if (it == m_entries.end()) return;
            Entry& ee = it->second;
            if (!ee.dirty) return;
            ee.dirty = false;

            if (m_svc) {
                m_svc->startAutoRescanIfAllowed(k.uid, k.deviceId);
            }
        });

        e.status = Status{
            QStringLiteral("watching"),
            QStringLiteral("Watching active (fallback mode; limited event details).")
        };

        // Success: reset backoff
        e.failCount = 0;
        e.nextRetryMs = 0;
        e.lastArmError.clear();
        e.retryOnlyOnMountChange = false;
    }
}

void WatchManager::onFanotifyReadable(const Key& k) {
    auto it = m_entries.find(k);
    if (it == m_entries.end()) return;
    Entry& e = it->second;
    if (e.fanFd < 0) return;

    alignas(fanotify_event_metadata) char buf[4096];

    bool shouldLogDirtyEdge = false;

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
            if (meta->fd >= 0) ::close(meta->fd); // required for some event types
            meta = FAN_EVENT_NEXT(meta, len);
        }

        // Only log once per "dirty period" (edge-trigger).
        if (!e.dirty) {
            shouldLogDirtyEdge = true;
        }

        e.dirty = true;
        if (e.quietTimer) e.quietTimer->start(kQuietMs);
    }

    if (shouldLogDirtyEdge) {
        qInfo().noquote() << QStringLiteral("[watch] uid=%1 device=%2 dirty (quiet=%3ms)")
                             .arg(k.uid)
                             .arg(k.deviceId)
                             .arg(kQuietMs);
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
            e.status = Status{QStringLiteral("notMounted"), QStringLiteral("Device is not mounted.")};
            stopEntry(e);

            // Reset backoff when not mounted; next mount gets an immediate attempt.
            e.failCount = 0;
            e.nextRetryMs = 0;
            e.lastArmError.clear();
            e.retryOnlyOnMountChange = false;
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