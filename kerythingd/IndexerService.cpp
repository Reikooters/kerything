// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexerService.h"

#include <algorithm>
#include <cctype>
#include <execution>
#include <filesystem>
#include <fstream>
#include <optional>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <QProcess>
#include <QDataStream>
#include <QRegularExpression>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QDateTime>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusError>

#include <blkid/blkid.h>

static constexpr quint32 kFlagIsDir = 1u << 0;
static constexpr quint32 kFlagIsSymlink = 1u << 1;

static constexpr quint32 kSnapshotVersion = 5;
static constexpr quint64 kSnapshotMagic   = 0x4B4552595448494EULL; // "KERYTHIN" (8 bytes)

static QString lower(QString s) {
    for (QChar& c : s) c = c.toLower();
    return s;
}

/**
 * Retrieves a specific value from the block device metadata for a given device node.
 *
 * @param devNode The device node path as a string (e.g., "/dev/sda1").
 * @param key The metadata key to retrieve (e.g., "TYPE", "UUID", "LABEL").
 * @return An optional QString containing the value associated with the given key if found,
 *         or an empty std::optional if the key cannot be retrieved or the probe fails.
 */
static std::optional<QString> blkidValueForDev(const std::string& devNode, const char* key) {
    blkid_probe pr = blkid_new_probe_from_filename(devNode.c_str());
    if (!pr) return std::nullopt;

    blkid_probe_enable_superblocks(pr, 1);
    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_UUID | BLKID_SUBLKS_LABEL);

    const int rc = blkid_do_safeprobe(pr);

    const char* data = nullptr;
    size_t len = 0;

    std::optional<QString> out;
    if (rc == 0 && blkid_probe_lookup_value(pr, key, &data, &len) == 0 && data && len > 0) {
        out = QString::fromUtf8(data, static_cast<int>(len));
    }

    blkid_free_probe(pr);
    return out;
}

/**
 * Selects the primary mount point from a list of available mount points.
 *
 * The method applies the following selection criteria:
 * 1) Prefers mount points under "/mnt" or "/media". Among these, the shortest path is chosen.
 * 2) If no mount points match the first criteria, the shortest path from the provided list is selected.
 *
 * @param mountPoints A list of mount points as strings.
 * @return A QString representing the selected primary mount point.
 *         Returns an empty QString if the input list is empty.
 */
static QString pickPrimaryMountPoint(const QStringList& mountPoints) {
    if (mountPoints.isEmpty()) return {};

    // 1) Prefer /mnt or /media
    QString best;
    for (const QString& mp : mountPoints) {
        if (mp.startsWith(QStringLiteral("/mnt/")) || mp == QStringLiteral("/mnt")) {
            if (best.isEmpty() || mp.size() < best.size()) best = mp;
        }
        if (mp.startsWith(QStringLiteral("/media/")) || mp == QStringLiteral("/media")) {
            if (best.isEmpty() || mp.size() < best.size()) best = mp;
        }
    }
    if (!best.isEmpty()) return best;

    // 2) Else shortest path
    best = mountPoints.first();
    for (const QString& mp : mountPoints) {
        if (mp.size() < best.size()) best = mp;
    }
    return best;
}

struct MountInfoEntry {
    std::string mountPoint;
    std::string mountSource;
};

/**
 * Reads and parses the contents of /proc/self/mountinfo to retrieve a list of mounted file systems.
 *
 * This method extracts the mount point and the corresponding mount source from each line of the file.
 * Only entries that conform to the expected format are included in the output.
 *
 * @return A vector of MountInfoEntry structures, where each structure contains the mount point and
 *         the associated mount source. Returns an empty vector if the file cannot be read or if
 *         no valid entries are found.
 */
static std::vector<MountInfoEntry> readMountInfo() {
    std::ifstream f("/proc/self/mountinfo");
    std::vector<MountInfoEntry> out;
    if (!f) return out;

    std::string line;
    while (std::getline(f, line)) {
        // mountinfo format:
        //  id parent major:minor root mount_point opts ... - fstype mount_source superopts
        //
        // We need mount_point and mount_source.
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) continue;

        const std::string left = line.substr(0, sep);
        const std::string right = line.substr(sep + 3);

        // left: fields separated by spaces; mount_point is field 5 (1-based)
        // We'll parse first 6 tokens: id, parent, maj:min, root, mount_point, opts
        std::string id, parent, majmin, root, mountPoint;
        {
            std::istringstream iss(left);
            std::string opts;
            if (!(iss >> id >> parent >> majmin >> root >> mountPoint >> opts)) continue;
        }

        // right: fstype mount_source superopts...
        std::string fstype, mountSource;
        {
            std::istringstream iss(right);
            std::string superopts;
            if (!(iss >> fstype >> mountSource >> superopts)) continue;
        }

        out.push_back({mountPoint, mountSource});
    }

    return out;
}

QStringList IndexerService::tokenizeQuery(const QString& query) {
    QString q = query;
    q = q.trimmed();

    // Split on whitespace, drop empties
    const QStringList parts = q.split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
    return parts;
}

void IndexerService::buildTrigramIndex(DeviceIndex& idx) {
    idx.flatIndex.clear();
    idx.flatIndex.reserve(idx.records.size() * 4); // rough heuristic

    std::vector<quint32> tris;
    tris.reserve(64);

    for (quint32 recordIdx = 0; recordIdx < idx.records.size(); ++recordIdx) {
        const auto& r = idx.records[recordIdx];
        const char* base = idx.stringPool.data() + r.nameOffset;
        const size_t len = static_cast<size_t>(r.nameLen);

        if (len < 3) {
            continue;
        }

        tris.clear();
        tris.reserve(len - 2);

        auto lower = [](unsigned char c) -> unsigned char {
            return static_cast<unsigned char>(std::tolower(c));
        };

        for (size_t i = 0; i + 2 < len; ++i) {
            const quint32 tri =
                (static_cast<quint32>(lower(static_cast<unsigned char>(base[i]))) << 16) |
                (static_cast<quint32>(lower(static_cast<unsigned char>(base[i + 1]))) << 8) |
                (static_cast<quint32>(lower(static_cast<unsigned char>(base[i + 2]))));
            tris.push_back(tri);
        }

        std::sort(tris.begin(), tris.end());
        tris.erase(std::unique(tris.begin(), tris.end()), tris.end());

        for (quint32 tri : tris) {
            idx.flatIndex.push_back(ScannerEngine::TrigramEntry{tri, recordIdx});
        }
    }

    std::sort(idx.flatIndex.begin(), idx.flatIndex.end(),
              [](const auto& a, const auto& b) {
                  if (a.trigram != b.trigram) return a.trigram < b.trigram;
                  return a.recordIdx < b.recordIdx;
              });
}

/**
 * Compares two sequences of bytes case-insensitively.
 *
 * @param a The first sequence of bytes represented as a std::string_view.
 * @param b The second sequence of bytes represented as a std::string_view.
 * @return Returns -1 if the first sequence is lexicographically less than the second,
 *         1 if the first sequence is greater than the second, or 0 if they are equal.
 */
static int ciCompareBytes(std::string_view a, std::string_view b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ac = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const unsigned char bc = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ac < bc) return -1;
        if (ac > bc) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

void IndexerService::buildSortOrders(DeviceIndex& idx) {
    const quint32 n = static_cast<quint32>(idx.records.size());

    auto initOrder = [&](std::vector<quint32>& v) {
        v.resize(n);
        for (quint32 i = 0; i < n; ++i) v[i] = i;
    };

    initOrder(idx.orderByName);
    initOrder(idx.orderByPath);
    initOrder(idx.orderBySize);
    initOrder(idx.orderByMtime);

    auto nameView = [&](quint32 i) -> std::string_view {
        const auto& r = idx.records[i];
        return std::string_view(idx.stringPool.data() + r.nameOffset, r.nameLen);
    };

    auto sortMaybePar = [&](auto& vec, auto&& comp) {
        // Parallel sort has overhead; only worth it for large N
        if (vec.size() >= 200'000) {
            std::sort(std::execution::par, vec.begin(), vec.end(), comp);
        } else {
            std::sort(vec.begin(), vec.end(), comp);
        }
    };

    sortMaybePar(idx.orderByName, [&](quint32 a, quint32 b) {
        const int c = ciCompareBytes(nameView(a), nameView(b));
        if (c != 0) return c < 0;
        return a < b;
    });

    sortMaybePar(idx.orderBySize, [&](quint32 a, quint32 b) {
        const auto sa = idx.records[a].size;
        const auto sb = idx.records[b].size;
        if (sa != sb) return sa < sb;
        const int c = ciCompareBytes(nameView(a), nameView(b));
        if (c != 0) return c < 0;
        return a < b;
    });

    sortMaybePar(idx.orderByMtime, [&](quint32 a, quint32 b) {
        const auto ta = idx.records[a].modificationTime;
        const auto tb = idx.records[b].modificationTime;
        if (ta != tb) return ta < tb;
        const int c = ciCompareBytes(nameView(a), nameView(b));
        if (c != 0) return c < 0;
        return a < b;
    });

    // “Path” approximation for now: (parentRecordIdx, name, recordIdx)
    sortMaybePar(idx.orderByPath, [&](quint32 a, quint32 b) {
        const auto pa = idx.records[a].parentRecordIdx;
        const auto pb = idx.records[b].parentRecordIdx;
        if (pa != pb) return pa < pb;
        const int c = ciCompareBytes(nameView(a), nameView(b));
        if (c != 0) return c < 0;
        return a < b;
    });

    // Build rank arrays (inverse mapping)
    auto buildRank = [&](const std::vector<quint32>& order, std::vector<quint32>& rank) {
        rank.resize(n);
        for (quint32 pos = 0; pos < n; ++pos) {
            rank[order[pos]] = pos;
        }
    };

    buildRank(idx.orderByName, idx.rankByName);
    buildRank(idx.orderByPath, idx.rankByPath);
    buildRank(idx.orderBySize, idx.rankBySize);
    buildRank(idx.orderByMtime, idx.rankByMtime);
}

std::vector<quint32> IndexerService::deviceCandidatesForQuery(const DeviceIndex& idx, const QStringList& tokens) {
    // If we have any >=3 tokens, use trigram filtering.
    // Otherwise fall back to "all records" (short tokens will be refined by substring check).
    bool usedIndex = false;
    std::vector<quint32> candidates;

    for (const QString& tokQ : tokens) {
        const QByteArray tokBytes = tokQ.toUtf8();
        if (tokBytes.size() < 3) {
            continue;
        }

        usedIndex = true;

        for (int i = 0; i + 2 < tokBytes.size(); ++i) {
            const quint32 tri =
                (static_cast<quint32>(std::tolower(static_cast<unsigned char>(tokBytes[i]))) << 16) |
                (static_cast<quint32>(std::tolower(static_cast<unsigned char>(tokBytes[i + 1]))) << 8) |
                (static_cast<quint32>(std::tolower(static_cast<unsigned char>(tokBytes[i + 2]))));

            const auto range = std::equal_range(
                idx.flatIndex.begin(), idx.flatIndex.end(),
                ScannerEngine::TrigramEntry{tri, 0},
                [](const auto& a, const auto& b) { return a.trigram < b.trigram; }
            );

            if (range.first == range.second) {
                return {}; // no hits for this trigram
            }

            if (candidates.empty()) {
                candidates.reserve(static_cast<size_t>(std::distance(range.first, range.second)));
                for (auto it = range.first; it != range.second; ++it) {
                    candidates.push_back(it->recordIdx);
                }
            } else {
                std::vector<quint32> next;
                next.reserve(std::min(candidates.size(),
                                      static_cast<size_t>(std::distance(range.first, range.second))));

                auto aIt = candidates.begin();
                auto bIt = range.first;
                while (aIt != candidates.end() && bIt != range.second) {
                    if (*aIt < bIt->recordIdx) {
                        ++aIt;
                    } else if (bIt->recordIdx < *aIt) {
                        ++bIt;
                    } else {
                        next.push_back(*aIt);
                        ++aIt;
                        ++bIt;
                    }
                }

                candidates = std::move(next);
                if (candidates.empty()) {
                    return {};
                }
            }
        }
    }

    if (!usedIndex) {
        candidates.resize(idx.records.size());
        for (quint32 i = 0; i < idx.records.size(); ++i) candidates[i] = i;
    }

    return candidates;
}

IndexerService::IndexerService(QObject* parent)
    : QObject(parent) {
}

IndexerService::~IndexerService() {
    // Ensure we don't destroy QProcess while helper is still running.
    for (auto& kv : m_jobs) {
        if (!kv.second) continue;

        Job& j = *kv.second;
        if (!j.proc) continue;

        // Best-effort shutdown: ask nicely, then kill, then give it a moment.
        j.proc->terminate();
        if (!j.proc->waitForFinished(500)) {
            j.proc->kill();
            j.proc->waitForFinished(1000);
        }
        j.proc->deleteLater();
        j.proc = nullptr;
    }
    m_jobs.clear();
}

// --- Begin: Empty-query global-order cache ---

void IndexerService::bumpUidEpoch(quint32 uid) const {
    m_uidEpoch[uid] = m_uidEpoch[uid] + 1;
    m_globalOrderByUid.erase(uid); // drop caches; they'll be rebuilt lazily

    // Also drop any queued warm-ups for this uid (epoch changed, so they’re stale anyway).
    // Keep it simple: linear scan is fine (small set).
    for (auto it = m_globalWarmScheduled.begin(); it != m_globalWarmScheduled.end(); ) {
        if (it->startsWith(QStringLiteral("%1:").arg(uid))) {
            it = m_globalWarmScheduled.erase(it);
        } else {
            ++it;
        }
    }
}

const IndexerService::GlobalOrderCache* IndexerService::globalOrderForUid(quint32 uid, const QString& sortKey) const {
    const quint64 epoch = m_uidEpoch.contains(uid) ? m_uidEpoch.at(uid) : 0;

    auto uIt = m_globalOrderByUid.find(uid);
    if (uIt == m_globalOrderByUid.end()) return nullptr;

    auto cIt = uIt->second.find(sortKey);
    if (cIt == uIt->second.end()) return nullptr;

    const GlobalOrderCache& c = cIt->second;
    if (c.epoch != epoch) return nullptr;
    if (c.asc.empty()) return nullptr;
    return &c;
}

void IndexerService::rebuildGlobalOrderForUid(quint32 uid, const QString& sortKey) const {
    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) return;

    const auto& indexes = uidIt->second;
    const quint64 epoch = m_uidEpoch.contains(uid) ? m_uidEpoch.at(uid) : 0;

    auto pickOrder = [&](const DeviceIndex& idx) -> const std::vector<quint32>* {
        if (sortKey.compare(QStringLiteral("size"), Qt::CaseInsensitive) == 0) return &idx.orderBySize;
        if (sortKey.compare(QStringLiteral("mtime"), Qt::CaseInsensitive) == 0) return &idx.orderByMtime;
        if (sortKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) return &idx.orderByPath;
        return &idx.orderByName;
    };

    struct Cursor {
        const QString* deviceId = nullptr;
        const DeviceIndex* idx = nullptr;
        const std::vector<quint32>* order = nullptr;
        quint32 pos = 0;
    };

    std::vector<Cursor> cursors;
    cursors.reserve(indexes.size());

    quint64 total = 0;
    for (const auto& kv : indexes) {
        const auto* ord = pickOrder(kv.second);
        if (!ord || ord->empty()) continue;

        total += static_cast<quint64>(ord->size());

        Cursor c;
        c.deviceId = &kv.first;
        c.idx = &kv.second;
        c.order = ord;
        c.pos = 0;
        cursors.push_back(c);
    }

    if (total == 0 || cursors.empty()) return;

    auto nameView = [&](const DeviceIndex& idx, quint32 recIdx) -> std::string_view {
        const auto& r = idx.records[recIdx];
        return std::string_view(idx.stringPool.data() + r.nameOffset, r.nameLen);
    };

    // NOTE: priority_queue is max-heap; comparator returns "a is worse than b" for min-heap behavior.
    auto lessNode = [&](const Cursor& a, const Cursor& b) {
        const quint32 ai = (*a.order)[a.pos];
        const quint32 bi = (*b.order)[b.pos];

        if (sortKey.compare(QStringLiteral("size"), Qt::CaseInsensitive) == 0) {
            const auto sa = a.idx->records[ai].size;
            const auto sb = b.idx->records[bi].size;
            if (sa != sb) return sa > sb;
        } else if (sortKey.compare(QStringLiteral("mtime"), Qt::CaseInsensitive) == 0) {
            const auto ta = a.idx->records[ai].modificationTime;
            const auto tb = b.idx->records[bi].modificationTime;
            if (ta != tb) return ta > tb;
        } else if (sortKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) {
            const auto pa = a.idx->records[ai].parentRecordIdx;
            const auto pb = b.idx->records[bi].parentRecordIdx;
            if (pa != pb) return pa > pb;
        }

        const int c = ciCompareBytes(nameView(*a.idx, ai), nameView(*b.idx, bi));
        if (c != 0) return c > 0;

        if (*a.deviceId != *b.deviceId) return *a.deviceId > *b.deviceId;
        return ai > bi;
    };

    std::priority_queue<Cursor, std::vector<Cursor>, decltype(lessNode)> pq(lessNode);
    for (const auto& c : cursors) pq.push(c);

    GlobalOrderCache cache;
    cache.epoch = epoch;
    cache.sortKey = sortKey;
    cache.asc.reserve(static_cast<size_t>(total));

    while (!pq.empty()) {
        Cursor cur = pq.top();
        pq.pop();

        const quint32 recIdx = (*cur.order)[cur.pos];
        cache.asc.push_back(GlobalOrderEntry{*cur.deviceId, recIdx});

        ++cur.pos;
        if (cur.pos < cur.order->size()) {
            pq.push(cur);
        }
    }

    m_globalOrderByUid[uid][sortKey] = std::move(cache);
}

QString IndexerService::warmKey(quint32 uid, quint64 epoch, const QString& sortKey) {
    return QStringLiteral("%1:%2:%3").arg(uid).arg(epoch).arg(sortKey);
}

// --- End: Empty-query global-order cache ---

// --- Begin: DeviceIndexUpdated batching scaffold ---

void IndexerService::queueDeviceIndexUpdated(quint32 uid,
                                             const QString& deviceId,
                                             quint64 generation,
                                             quint64 entryCount) {
    // Correctness: empty-query global cache must be invalidated immediately.
    bumpUidEpoch(uid);

    auto& byDev = m_pendingIndexUpdatesByUid[uid];
    PendingIndexUpdate& p = byDev[deviceId];
    p.generation = generation;
    p.entryCount = entryCount;

    if (m_indexUpdateBatchScheduled) return;
    m_indexUpdateBatchScheduled = true;

    QTimer::singleShot(kIndexUpdateBatchMs, this, [this]() {
        m_indexUpdateBatchScheduled = false;
        dispatchBatchedIndexUpdates();
    });
}

void IndexerService::dispatchBatchedIndexUpdates() {
    if (m_pendingIndexUpdatesByUid.empty()) return;

    // Emit at most one update per (uid, deviceId) for this batch window.
    // NOTE: DeviceIndexUpdated has no uid; GUI side will refresh its own uid-scoped lists.
    for (auto& uidKv : m_pendingIndexUpdatesByUid) {
        auto& perDev = uidKv.second;
        for (auto& devKv : perDev) {
            const QString& deviceId = devKv.first;
            const PendingIndexUpdate& p = devKv.second;
            Q_EMIT DeviceIndexUpdated(deviceId, p.generation, p.entryCount);
        }
    }

    m_pendingIndexUpdatesByUid.clear();
}

// --- End: DeviceIndexUpdated batching scaffold ---

// --- Begin: Persistence helpers ---

quint32 IndexerService::callerUidOr0() const {
    if (!calledFromDBus()) return 0;

    const QString callerService = message().service(); // unique name like ":1.42"
    auto* iface = connection().interface();
    if (!iface) return 0;

    const uint uid = iface->serviceUid(callerService);
    if (uid == static_cast<uint>(-1)) return 0;
    return static_cast<quint32>(uid);
}

QString IndexerService::escapeDeviceIdForFilename(const QString& deviceId) {
    // Stable + filesystem-safe: sha1 hex of deviceId
    const QByteArray h = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(h.toHex());
}

QString IndexerService::baseIndexDirForUid(quint32 uid) {
    return QStringLiteral("/var/lib/kerything/indexes/%1").arg(uid);
}

QString IndexerService::snapshotPathFor(quint32 uid, const QString& deviceId) {
    const QString base = baseIndexDirForUid(uid);
    const QString name = escapeDeviceIdForFilename(deviceId) + QStringLiteral(".kix");
    return QDir(base).filePath(name);
}

void IndexerService::ensureLoadedForUid(quint32 uid) const {
    if (m_loadedUids.contains(uid)) return;
    loadSnapshotsForUid(uid);
    m_loadedUids.insert(uid);
}

void IndexerService::enqueueSnapshotUpgrade(quint32 uid, const QString& deviceId) const {
    // Avoid duplicates (small queue; linear scan is fine)
    for (const auto& p : m_snapshotUpgradeQueue) {
        if (p.first == uid && p.second == deviceId) {
            return;
        }
    }

    m_snapshotUpgradeQueue.emplace_back(uid, deviceId);
    scheduleProcessSnapshotUpgradeQueue();
}

void IndexerService::scheduleProcessSnapshotUpgradeQueue() const {
    if (m_snapshotUpgradeScheduled) return;
    if (m_snapshotUpgradeQueue.empty()) return;

    m_snapshotUpgradeScheduled = true;

    QTimer::singleShot(0, const_cast<IndexerService*>(this), [this]() {
        m_snapshotUpgradeScheduled = false;
        processOneSnapshotUpgrade();

        // Keep draining one-at-a-time
        if (!m_snapshotUpgradeQueue.empty()) {
            scheduleProcessSnapshotUpgradeQueue();
        }
    });
}

void IndexerService::processOneSnapshotUpgrade() const {
    if (m_snapshotUpgradeQueue.empty()) return;

    const auto [uid, deviceId] = m_snapshotUpgradeQueue.front();
    m_snapshotUpgradeQueue.pop_front();

    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) return;

    auto devIt = uidIt->second.find(deviceId);
    if (devIt == uidIt->second.end()) return;

    QString err;
    (void)saveSnapshot(uid, deviceId, devIt->second, &err);
    // Best-effort: ignore failures here. User can still re-index if needed.
}

void IndexerService::loadSnapshotsForUid(quint32 uid) const {
    const QString dirPath = baseIndexDirForUid(uid);
    QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }

    const QStringList files = dir.entryList(QStringList() << QStringLiteral("*.kix"),
                                            QDir::Files | QDir::Readable,
                                            QDir::Name);

    auto* self = const_cast<IndexerService*>(this);
    const quint32 totalFiles = static_cast<quint32>(files.size());

    // Notify clients that snapshot loading is starting
    {
        QVariantMap props;
        props.insert(QStringLiteral("loaded"), 0u);
        props.insert(QStringLiteral("total"), totalFiles);
        Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("loadingSnapshots"), props);
    }

    quint32 loadedCount = 0;

    for (const QString& fn : files) {
        const QString fullPath = dir.filePath(fn);

        QString deviceId;
        QString err;
        auto idxOpt = loadSnapshotFile(fullPath, &deviceId, &err);
        if (!idxOpt) {
            // Ignore bad/corrupt files for now (can log later)
            ++loadedCount;
            QVariantMap props;
            props.insert(QStringLiteral("loaded"), loadedCount);
            props.insert(QStringLiteral("total"), totalFiles);
            Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("loadingSnapshots"), props);
            continue;
        }

        DeviceIndex idx = std::move(*idxOpt);
        idx.dirPathCache.clear();

        // v4 snapshots include acceleration structures; older versions need rebuild.
        const bool hasAccel =
            !idx.flatIndex.empty() &&
            !idx.orderByName.empty() && !idx.orderByPath.empty() && !idx.orderBySize.empty() && !idx.orderByMtime.empty() &&
            !idx.rankByName.empty()  && !idx.rankByPath.empty()  && !idx.rankBySize.empty()  && !idx.rankByMtime.empty();

        if (!hasAccel) {
            buildTrigramIndex(idx);
            buildSortOrders(idx);
        }

        m_indexesByUid[uid][deviceId] = std::move(idx);

        // If it was old, upgrade it to the latest snapshot format in the background (best-effort).
        if (!hasAccel) {
            enqueueSnapshotUpgrade(uid, deviceId);
        }

        ++loadedCount;

        // Progress update (kept simple: one per file)
        QVariantMap props;
        props.insert(QStringLiteral("loaded"), loadedCount);
        props.insert(QStringLiteral("total"), totalFiles);
        Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("loadingSnapshots"), props);
    }

    // Notify ready
    {
        QVariantMap props;
        Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("ready"), props);
    }
}

bool IndexerService::saveSnapshot(quint32 uid, const QString& deviceId, const DeviceIndex& idx, QString* errorOut) const {
    const QString dirPath = baseIndexDirForUid(uid);
    QDir().mkpath(dirPath);

    const QString path = snapshotPathFor(uid, deviceId);

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("Failed to open snapshot for writing: %1").arg(path);
        return false;
    }

    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);

    s << static_cast<quint64>(kSnapshotMagic);
    s << static_cast<quint32>(kSnapshotVersion);

    const QByteArray devIdBytes = deviceId.toUtf8();
    const QByteArray fsTypeBytes = idx.fsType.toUtf8();

    s << static_cast<quint32>(devIdBytes.size());
    if (devIdBytes.size() > 0) s.writeRawData(devIdBytes.constData(), devIdBytes.size());

    s << static_cast<quint32>(fsTypeBytes.size());
    if (fsTypeBytes.size() > 0) s.writeRawData(fsTypeBytes.constData(), fsTypeBytes.size());

    // v3+: labelLastKnown + uuidLastKnown (UTF-8 blobs)
    const QByteArray labelBytes = idx.labelLastKnown.toUtf8();
    const QByteArray uuidBytes  = idx.uuidLastKnown.toUtf8();

    s << static_cast<quint32>(labelBytes.size());
    if (labelBytes.size() > 0) s.writeRawData(labelBytes.constData(), labelBytes.size());

    s << static_cast<quint32>(uuidBytes.size());
    if (uuidBytes.size() > 0) s.writeRawData(uuidBytes.constData(), uuidBytes.size());

    s << static_cast<quint64>(idx.generation);

    // v2+: lastIndexedTime (unix seconds)
    s << static_cast<qint64>(idx.lastIndexedTime);

    // v5+: watchEnabled (u8)
    s << static_cast<quint8>(idx.watchEnabled ? 1 : 0);

    s << static_cast<quint64>(idx.records.size());
    if (!idx.records.empty()) {
        const auto bytes = static_cast<qint64>(idx.records.size() * sizeof(ScannerEngine::FileRecord));
        s.writeRawData(reinterpret_cast<const char*>(idx.records.data()), bytes);
    }

    s << static_cast<quint64>(idx.stringPool.size());
    if (!idx.stringPool.empty()) {
        const auto bytes = static_cast<qint64>(idx.stringPool.size());
        s.writeRawData(idx.stringPool.data(), bytes);
    }

    // v4+: acceleration structures (flatIndex + sort orders + ranks)
    static_assert(sizeof(ScannerEngine::TrigramEntry) == 8, "TrigramEntry must be 8 bytes for snapshot IO.");

    auto writeU32Vec = [&](const std::vector<quint32>& v) {
        s << static_cast<quint64>(v.size());
        if (!v.empty()) {
            const auto bytes = static_cast<qint64>(v.size() * sizeof(quint32));
            s.writeRawData(reinterpret_cast<const char*>(v.data()), bytes);
        }
    };

    // flatIndex
    s << static_cast<quint64>(idx.flatIndex.size());
    if (!idx.flatIndex.empty()) {
        const auto bytes = static_cast<qint64>(idx.flatIndex.size() * sizeof(ScannerEngine::TrigramEntry));
        s.writeRawData(reinterpret_cast<const char*>(idx.flatIndex.data()), bytes);
    }

    // orders
    writeU32Vec(idx.orderByName);
    writeU32Vec(idx.orderByPath);
    writeU32Vec(idx.orderBySize);
    writeU32Vec(idx.orderByMtime);

    // ranks
    writeU32Vec(idx.rankByName);
    writeU32Vec(idx.rankByPath);
    writeU32Vec(idx.rankBySize);
    writeU32Vec(idx.rankByMtime);

    if (s.status() != QDataStream::Ok) {
        if (errorOut) *errorOut = QStringLiteral("Failed while writing snapshot stream.");
        return false;
    }

    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("Failed to commit snapshot atomically.");
        return false;
    }

    return true;
}

std::optional<IndexerService::DeviceIndex> IndexerService::loadSnapshotFile(const QString& path,
                                                                            QString* deviceIdOut,
                                                                            QString* errorOut) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("Failed to open snapshot: %1").arg(path);
        return std::nullopt;
    }

    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);

    quint64 magic = 0;
    quint32 ver = 0;
    s >> magic >> ver;

    if (magic != kSnapshotMagic || (ver != 1 && ver != 2 && ver != 3 && ver != 4 && ver != 5)) {
        if (errorOut) *errorOut = QStringLiteral("Snapshot header/version mismatch.");
        return std::nullopt;
    }

    auto readBytes = [&](quint32 maxBytes, QByteArray& out) -> bool {
        quint32 n = 0;
        s >> n;
        if (n == 0) { out.clear(); return true; }
        if (n > maxBytes) return false;
        out.resize(static_cast<int>(n));
        return s.readRawData(out.data(), static_cast<int>(n)) == static_cast<int>(n);
    };

    QByteArray devIdBytes;
    if (!readBytes(4096, devIdBytes)) return std::nullopt;

    QByteArray fsTypeBytes;
    if (!readBytes(256, fsTypeBytes)) return std::nullopt;

    QByteArray labelBytes;
    QByteArray uuidBytes;
    if (ver >= 3) {
        if (!readBytes(4096, labelBytes)) return std::nullopt;
        if (!readBytes(4096, uuidBytes)) return std::nullopt;
    }

    quint64 generation = 0;
    s >> generation;

    qint64 lastIndexedTime = 0;
    if (ver >= 2) {
        s >> lastIndexedTime;
    }

    // v5+: watchEnabled
    bool watchEnabled = true;
    if (ver >= 5) {
        quint8 b = 1;
        s >> b;
        watchEnabled = (b != 0);
    } else {
        watchEnabled = true; // v1-v4 default
    }

    quint64 recordCount = 0;
    s >> recordCount;

    static constexpr quint64 kMaxRecords = 500'000'000ULL;
    if (recordCount == 0 || recordCount > kMaxRecords) {
        if (errorOut) *errorOut = QStringLiteral("Invalid recordCount in snapshot.");
        return std::nullopt;
    }

    DeviceIndex idx;
    idx.generation = generation;
    idx.lastIndexedTime = lastIndexedTime;
    idx.fsType = QString::fromUtf8(fsTypeBytes);
    idx.watchEnabled = watchEnabled;

    if (ver >= 3) {
        idx.labelLastKnown = QString::fromUtf8(labelBytes);
        idx.uuidLastKnown  = QString::fromUtf8(uuidBytes);
    }

    idx.records.resize(static_cast<size_t>(recordCount));
    const qint64 recordBytes = static_cast<qint64>(recordCount * sizeof(ScannerEngine::FileRecord));
    if (s.readRawData(reinterpret_cast<char*>(idx.records.data()), recordBytes) != recordBytes) {
        if (errorOut) *errorOut = QStringLiteral("Truncated snapshot (records).");
        return std::nullopt;
    }

    quint64 poolSize = 0;
    s >> poolSize;

    static constexpr quint64 kMaxPoolBytes = 8ULL * 1024 * 1024 * 1024;
    if (poolSize == 0 || poolSize > kMaxPoolBytes) {
        if (errorOut) *errorOut = QStringLiteral("Invalid string pool size in snapshot.");
        return std::nullopt;
    }

    idx.stringPool.resize(static_cast<size_t>(poolSize));
    const qint64 poolBytes = static_cast<qint64>(poolSize);
    if (s.readRawData(idx.stringPool.data(), poolBytes) != poolBytes) {
        if (errorOut) *errorOut = QStringLiteral("Truncated snapshot (string pool).");
        return std::nullopt;
    }

    // Basic sanity: ensure name ranges are in-bounds
    for (size_t i = 0; i < idx.records.size(); ++i) {
        const auto& r = idx.records[i];
        const quint64 end = static_cast<quint64>(r.nameOffset) + static_cast<quint64>(r.nameLen);
        if (end > poolSize) {
            if (errorOut) *errorOut = QStringLiteral("Corrupt snapshot: name range out of bounds.");
            return std::nullopt;
        }
    }

    if (ver >= 4) {
        static_assert(sizeof(ScannerEngine::TrigramEntry) == 8, "TrigramEntry must be 8 bytes for snapshot IO.");

        auto readU32Vec = [&](quint64 maxElems, std::vector<quint32>& out) -> bool {
            quint64 n = 0;
            s >> n;
            if (n == 0) { out.clear(); return true; }
            if (n > maxElems) return false;

            out.resize(static_cast<size_t>(n));
            const qint64 bytes = static_cast<qint64>(n * sizeof(quint32));
            return s.readRawData(reinterpret_cast<char*>(out.data()), bytes) == bytes;
        };

        quint64 flatCount = 0;
        s >> flatCount;

        static constexpr quint64 kMaxFlat = 2'000'000'000ULL; // guardrail
        if (flatCount > kMaxFlat) return std::nullopt;

        idx.flatIndex.resize(static_cast<size_t>(flatCount));
        if (flatCount > 0) {
            const qint64 bytes = static_cast<qint64>(flatCount * sizeof(ScannerEngine::TrigramEntry));
            if (s.readRawData(reinterpret_cast<char*>(idx.flatIndex.data()), bytes) != bytes) {
                if (errorOut) *errorOut = QStringLiteral("Truncated snapshot (flatIndex).");
                return std::nullopt;
            }
        }

        const quint64 nRec = recordCount;

        if (!readU32Vec(nRec, idx.orderByName)) return std::nullopt;
        if (!readU32Vec(nRec, idx.orderByPath)) return std::nullopt;
        if (!readU32Vec(nRec, idx.orderBySize)) return std::nullopt;
        if (!readU32Vec(nRec, idx.orderByMtime)) return std::nullopt;

        if (!readU32Vec(nRec, idx.rankByName)) return std::nullopt;
        if (!readU32Vec(nRec, idx.rankByPath)) return std::nullopt;
        if (!readU32Vec(nRec, idx.rankBySize)) return std::nullopt;
        if (!readU32Vec(nRec, idx.rankByMtime)) return std::nullopt;

        // Basic consistency: if any are present, they should match recordCount
        auto mustMatch = [&](const std::vector<quint32>& v) -> bool {
            return v.empty() || v.size() == static_cast<size_t>(recordCount);
        };
        if (!mustMatch(idx.orderByName) || !mustMatch(idx.orderByPath) || !mustMatch(idx.orderBySize) || !mustMatch(idx.orderByMtime) ||
            !mustMatch(idx.rankByName)  || !mustMatch(idx.rankByPath)  || !mustMatch(idx.rankBySize)  || !mustMatch(idx.rankByMtime))
        {
            if (errorOut) *errorOut = QStringLiteral("Corrupt snapshot: sort/rank vector size mismatch.");
            return std::nullopt;
        }
    }

    if (deviceIdOut) *deviceIdOut = QString::fromUtf8(devIdBytes);
    return idx;
}

// --- End: Persistence helpers ---

/**
 * Generates a unique entry identifier based on the provided device ID and record index.
 *
 * @param deviceId The unique identifier for the device as a QString.
 * @param recordIdx The index of the specific record within the device.
 * @return A 64-bit unsigned integer representing the generated entry ID,
 *         consisting of a 32-bit FNV-1a hash of the device ID in the high bits
 *         and the record index in the low bits.
 */
quint64 IndexerService::makeEntryId(const QString& deviceId, quint32 recordIdx) {
    // Stable-ish within a deviceId: FNV-1a 32-bit hash of deviceId, then record index.
    quint32 h = 2166136261u;
    for (QChar qc : deviceId) {
        const quint16 ch = qc.unicode();
        h ^= static_cast<quint8>(ch & 0xFFu);
        h *= 16777619u;
        h ^= static_cast<quint8>((ch >> 8) & 0xFFu);
        h *= 16777619u;
    }
    return (static_cast<quint64>(h) << 32) | static_cast<quint64>(recordIdx);
}

quint32 IndexerService::deviceHash32(const QString& deviceId) {
    // Must match the high-32 bits used by makeEntryId()
    quint32 h = 2166136261u;
    for (QChar qc : deviceId) {
        const quint16 ch = qc.unicode();
        h ^= static_cast<quint8>(ch & 0xFFu);
        h *= 16777619u;
        h ^= static_cast<quint8>((ch >> 8) & 0xFFu);
        h *= 16777619u;
    }
    return h;
}

QString IndexerService::joinInternalPath(const QString& internalDir, const QString& name) {
    // internalDir is like "/" or "/foo/bar"
    if (internalDir.isEmpty() || internalDir == QStringLiteral("/")) {
        return QStringLiteral("/") + name;
    }
    return internalDir + QStringLiteral("/") + name;
}

bool IndexerService::nameContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;

    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    return it != haystack.end();
}

IndexerService::ParsedScan IndexerService::parseHelperStdout(const QByteArray& raw) {
    ParsedScan out;

    if (raw.isEmpty()) {
        out.error = QStringLiteral("Helper produced no stdout data.");
        return out;
    }

    QDataStream s(raw);
    s.setByteOrder(QDataStream::LittleEndian);

    auto readExact = [&](void* dst, qint64 n) -> bool {
        return s.readRawData(reinterpret_cast<char*>(dst), n) == n;
    };

    quint64 recordCount = 0;
    if (!readExact(&recordCount, sizeof(recordCount))) {
        out.error = QStringLiteral("Failed to read recordCount.");
        return out;
    }

    static constexpr quint64 kMaxRecords = 500'000'000ULL;
    if (recordCount == 0 || recordCount > kMaxRecords) {
        out.error = QStringLiteral("Invalid recordCount: %1").arg(recordCount);
        return out;
    }

    const quint64 recordBytes64 = recordCount * static_cast<quint64>(sizeof(ScannerEngine::FileRecord));
    if (recordBytes64 / static_cast<quint64>(sizeof(ScannerEngine::FileRecord)) != recordCount) {
        out.error = QStringLiteral("Record byte size overflow.");
        return out;
    }
    if (recordBytes64 > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        out.error = QStringLiteral("Record blob too large to read safely.");
        return out;
    }

    try {
        out.records.resize(static_cast<size_t>(recordCount));
    } catch (...) {
        out.error = QStringLiteral("Memory allocation failed for records.");
        return out;
    }

    if (!readExact(out.records.data(), static_cast<qint64>(recordBytes64))) {
        out.error = QStringLiteral("Truncated stream while reading records.");
        return out;
    }

    quint64 poolSize = 0;
    if (!readExact(&poolSize, sizeof(poolSize))) {
        out.error = QStringLiteral("Failed to read string pool size.");
        return out;
    }

    static constexpr quint64 kMaxPoolBytes = 8ULL * 1024 * 1024 * 1024; // 8 GiB
    if (poolSize == 0 || poolSize > kMaxPoolBytes) {
        out.error = QStringLiteral("Invalid string pool size: %1 bytes").arg(poolSize);
        return out;
    }
    if (poolSize > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        out.error = QStringLiteral("String pool too large to read safely.");
        return out;
    }

    try {
        out.stringPool.resize(static_cast<size_t>(poolSize));
    } catch (...) {
        out.error = QStringLiteral("Memory allocation failed for string pool.");
        return out;
    }

    if (!readExact(out.stringPool.data(), static_cast<qint64>(poolSize))) {
        out.error = QStringLiteral("Truncated stream while reading string pool.");
        return out;
    }

    // Basic sanity: ensure all name ranges are in-bounds (best-effort)
    for (size_t i = 0; i < out.records.size(); ++i) {
        const auto& r = out.records[i];
        const quint64 end = static_cast<quint64>(r.nameOffset) + static_cast<quint64>(r.nameLen);
        if (end > poolSize) {
            out.error = QStringLiteral("Corrupt record %1: name range out of bounds.").arg(static_cast<qulonglong>(i));
            out.records.clear();
            out.stringPool.clear();
            return out;
        }
    }

    return out;
}

/**
 * Constructs the file system path for a given directory ID within the index associated
 * with a specific user ID and device ID.
 *
 * This method retrieves and builds the path structure by walking up the parent directory
 * references from the provided directory ID. If the directory ID is set to a special value
 * or is invalid, a fallback path is returned. Results may be cached for subsequent lookups.
 *
 * @param uid The unique identifier of the user owning the device index.
 * @param deviceId The identifier of the device containing the directory.
 * @param dirId The directory ID whose path should be resolved. A special value of 0xFFFFFFFFu
 *              is treated as the root directory.
 * @return A QString representing the resolved directory path. If the user ID, device ID,
 *         or directory ID is not found, a placeholder string ("…") is returned.
 */
QString IndexerService::dirPathFor(quint32 uid, const QString& deviceId, quint32 dirId) const {
    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) return QStringLiteral("…");

    auto it = uidIt->second.find(deviceId);
    if (it == uidIt->second.end()) return QStringLiteral("…");

    const DeviceIndex& idx = it->second;

    if (dirId == 0xFFFFFFFFu) {
        return QStringLiteral("/");
    }

    // Cache hit
    auto cacheIt = idx.dirPathCache.find(dirId);
    if (cacheIt != idx.dirPathCache.end()) {
        return cacheIt->second;
    }

    if (dirId >= idx.records.size()) {
        return QStringLiteral("…");
    }

    // Walk parents and build path
    QStringList parts;
    quint32 cur = dirId;
    int safety = 0;

    while (cur != 0xFFFFFFFFu && cur < idx.records.size() && safety++ < 4096) {
        const auto& r = idx.records[cur];
        std::string_view name(&idx.stringPool[r.nameOffset], r.nameLen);

        // Skip dot entries and empty names (root-like)
        if (!(name == "." || name == ".." || name.empty())) {
            parts.push_front(QString::fromUtf8(name.data(), static_cast<int>(name.size())));
        }

        const quint32 next = r.parentRecordIdx;
        if (next == cur) break; // self-loop safety
        cur = next;
    }

    QString path = QStringLiteral("/");
    if (!parts.isEmpty()) {
        path += parts.join(QStringLiteral("/"));
    }

    // Store into cache (mutable)
    idx.dirPathCache.emplace(dirId, path);
    return path;
}

void IndexerService::Ping(QString& versionOut, quint32& apiVersionOut) const {
    versionOut = "kerythingd";
    apiVersionOut = 1;
}

void IndexerService::ListKnownDevices(QVariantList& devicesOut) const {
    namespace fs = std::filesystem;

    devicesOut.clear();

    // Read mountinfo once, then match by resolved /dev node path
    const auto mountInfo = readMountInfo();

    // Enumerate partitions via /dev/disk/by-partuuid/*
    const fs::path byPartuuidDir("/dev/disk/by-partuuid");
    if (!fs::exists(byPartuuidDir) || !fs::is_directory(byPartuuidDir)) {
        return;
    }

    for (const auto& entry : fs::directory_iterator(byPartuuidDir)) {
        if (!entry.is_symlink() && !entry.is_regular_file()) {
            continue;
        }

        const std::string partuuid = entry.path().filename().string();

        std::error_code ec;
        const fs::path resolved = fs::canonical(entry.path(), ec);
        if (ec) continue;

        const std::string devNode = resolved.string();

        QVariantMap dev;
        dev.insert(QStringLiteral("deviceId"), QStringLiteral("partuuid:") + QString::fromStdString(partuuid));
        dev.insert(QStringLiteral("devNode"), QString::fromStdString(devNode));
        dev.insert(QStringLiteral("partuuid"), QString::fromStdString(partuuid));

        const auto fsType = blkidValueForDev(devNode, "TYPE");
        const auto uuid = blkidValueForDev(devNode, "UUID");
        const auto label = blkidValueForDev(devNode, "LABEL");

        dev.insert(QStringLiteral("fsType"), fsType ? lower(*fsType) : QString());
        dev.insert(QStringLiteral("uuid"), uuid ? lower(*uuid) : QString());
        dev.insert(QStringLiteral("label"), label.value_or(QString()));

        // Mount points: match mount_source that resolves to this devNode.
        QStringList mountPoints;
        for (const auto& mi : mountInfo) {
            // mount_source can be "/dev/..." or "UUID=..." etc.
            // We only match /dev paths for now (simple + reliable).
            if (mi.mountSource.rfind("/dev/", 0) != 0) continue;

            std::error_code ec2;
            const fs::path srcResolved = fs::canonical(mi.mountSource, ec2);
            if (ec2) continue;

            if (srcResolved.string() == devNode) {
                mountPoints << QString::fromStdString(mi.mountPoint);
            }
        }

        mountPoints.removeDuplicates();
        std::sort(mountPoints.begin(), mountPoints.end());

        const bool mounted = !mountPoints.isEmpty();
        dev.insert(QStringLiteral("mounted"), mounted);
        dev.insert(QStringLiteral("mountPoints"), mountPoints);
        dev.insert(QStringLiteral("primaryMountPoint"), pickPrimaryMountPoint(mountPoints));

        devicesOut.push_back(dev);
    }
}

void IndexerService::ListIndexedDevices(QVariantList& indexedOut) const {
    indexedOut.clear();

    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) return;

    indexedOut.reserve(static_cast<int>(uidIt->second.size()));

    for (const auto& kv : uidIt->second) {
        const QString& deviceId = kv.first;
        const DeviceIndex& idx = kv.second;

        QVariantMap m;
        m.insert(QStringLiteral("deviceId"), deviceId);
        m.insert(QStringLiteral("fsType"), idx.fsType);
        m.insert(QStringLiteral("generation"), QVariant::fromValue<qulonglong>(idx.generation));
        m.insert(QStringLiteral("entryCount"), QVariant::fromValue<qulonglong>(idx.records.size()));
        m.insert(QStringLiteral("lastIndexedTime"), QVariant::fromValue<qlonglong>(idx.lastIndexedTime));
        m.insert(QStringLiteral("label"), idx.labelLastKnown);
        m.insert(QStringLiteral("uuid"), idx.uuidLastKnown);
        m.insert(QStringLiteral("watchEnabled"), idx.watchEnabled);

        indexedOut.push_back(m);
    }
}

std::optional<QVariantMap> IndexerService::findDeviceById(const QString& deviceId) const {
    QVariantList devices;
    ListKnownDevices(devices);
    for (const QVariant& v : devices) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("deviceId")).toString() == deviceId) {
            return m;
        }
    }
    return std::nullopt;
}

quint64 IndexerService::StartIndex(const QString& deviceId) {
    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    const auto devOpt = findDeviceById(deviceId);
    if (!devOpt) {
        // Emit a finished signal immediately so the GUI has something deterministic to react to.
        const quint64 jobId = m_nextJobId++;
        QVariantMap props;
        props.insert(QStringLiteral("deviceId"), deviceId);
        Q_EMIT JobAdded(jobId, props);
        Q_EMIT JobFinished(jobId, QStringLiteral("error"), QStringLiteral("Unknown deviceId"), props);
        return jobId;
    }

    const QVariantMap dev = *devOpt;
    const QString devNode = dev.value(QStringLiteral("devNode")).toString();
    const QString fsType = dev.value(QStringLiteral("fsType")).toString().toLower();

    const quint64 jobId = m_nextJobId++;

    auto job = std::make_unique<Job>();
    job->jobId = jobId;
    job->ownerUid = uid;
    job->deviceId = deviceId;
    job->devNode = devNode;
    job->fsType = fsType;

    // Qt-owned process; we deleteLater() when finished.
    job->proc = new QProcess(this);

    QVariantMap props;
    props.insert(QStringLiteral("deviceId"), deviceId);
    props.insert(QStringLiteral("devNode"), devNode);
    props.insert(QStringLiteral("fsType"), fsType);

    Q_EMIT JobAdded(jobId, props);
    Q_EMIT JobProgress(jobId, 0, props);

    // Tell GUIs we are actively rescanning (per-user)
    {
        auto* self = const_cast<IndexerService*>(this);
        QVariantMap st;
        st.insert(QStringLiteral("deviceId"), deviceId);
        st.insert(QStringLiteral("percent"), 0u);
        Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("rescanning"), st);
    }

    // Insert job BEFORE connecting lambdas that will look it up.
    m_jobs.emplace(jobId, std::move(job));

    // Capture stdout (binary scan output)
    connect(m_jobs[jobId]->proc, &QProcess::readyReadStandardOutput, this, [this, jobId]() {
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end() || !it->second) return;
        Job& j = *it->second;
        if (!j.proc) return;

        j.stdoutBuf += j.proc->readAllStandardOutput();
    });

    // Read progress from stderr (KERYTHING_PROGRESS lines)
    connect(m_jobs[jobId]->proc, &QProcess::readyReadStandardError, this, [this, jobId, uid]() {
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end() || !it->second) return;
        Job& j = *it->second;
        if (!j.proc) return;

        j.stderrBuf += j.proc->readAllStandardError();

        std::optional<int> latestPctSeen;

        auto consumeLine = [&](QByteArrayView lineView) {
            static constexpr QByteArrayView kPrefix("KERYTHING_PROGRESS ");
            if (!lineView.startsWith(kPrefix)) return;

            bool ok = false;
            int pct = QByteArray(lineView.mid(kPrefix.size())).trimmed().toInt(&ok);
            if (!ok) return;

            pct = std::clamp(pct, 0, 100);
            latestPctSeen = pct;
        };

        while (true) {
            const int nl = j.stderrBuf.indexOf('\n');
            if (nl < 0) break;

            QByteArrayView lineView(j.stderrBuf.constData(), nl);
            consumeLine(lineView);
            j.stderrBuf.remove(0, nl + 1);
        }

        // If we are cancelling, don't spam progress; the UI is already in "cancelling" mode.
        if (j.state == Job::State::Cancelling) {
            return;
        }

        if (latestPctSeen && *latestPctSeen != j.lastPct) {
            j.lastPct = *latestPctSeen;

            QVariantMap props;
            props.insert(QStringLiteral("deviceId"), j.deviceId);
            props.insert(QStringLiteral("devNode"), j.devNode);
            props.insert(QStringLiteral("fsType"), j.fsType);

            Q_EMIT JobProgress(jobId, static_cast<quint32>(j.lastPct), props);

            // Mirror progress into DaemonStateChanged so the main GUI can show it
            {
                auto* self = const_cast<IndexerService*>(this);
                QVariantMap st;
                st.insert(QStringLiteral("deviceId"), j.deviceId);
                st.insert(QStringLiteral("percent"), static_cast<quint32>(j.lastPct));
                Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("rescanning"), st);
            }
        }
    });

    connect(m_jobs[jobId]->proc,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, jobId, uid](int exitCode, QProcess::ExitStatus exitStatus) {
                auto it = m_jobs.find(jobId);
                if (it == m_jobs.end() || !it->second) return;
                Job& j = *it->second;

                // Drain any remaining stdout after process exit
                if (j.proc) {
                    j.stdoutBuf += j.proc->readAllStandardOutput();
                }

                QVariantMap props;
                props.insert(QStringLiteral("deviceId"), j.deviceId);
                props.insert(QStringLiteral("devNode"), j.devNode);
                props.insert(QStringLiteral("fsType"), j.fsType);

                // Decide final status once, from the finished handler only.
                if (j.state == Job::State::Cancelling) {
                    Q_EMIT JobFinished(jobId, QStringLiteral("cancelled"), QStringLiteral("Cancelled by request"), props);
                } else if (exitStatus == QProcess::CrashExit) {
                    Q_EMIT JobFinished(jobId, QStringLiteral("error"), QStringLiteral("Scanner helper crashed"), props);
                } else if (exitCode != 0) {
                    Q_EMIT JobFinished(jobId,
                                       QStringLiteral("error"),
                                       QStringLiteral("Scanner helper failed (exit code %1)").arg(exitCode),
                                       props);
                } else {
                    // Parse + store the index in memory
                    ParsedScan parsed = parseHelperStdout(j.stdoutBuf);
                    if (!parsed.error.isEmpty()) {
                        Q_EMIT JobFinished(jobId, QStringLiteral("error"),
                                           QStringLiteral("Failed to parse scan output: %1").arg(parsed.error),
                                           props);
                    } else {
                        DeviceIndex& idx = m_indexesByUid[j.ownerUid][j.deviceId];
                        idx.fsType = j.fsType;
                        idx.generation += 1;
                        idx.records = std::move(parsed.records);
                        idx.stringPool = std::move(parsed.stringPool);
                        idx.dirPathCache.clear();

                        buildTrigramIndex(idx);
                        buildSortOrders(idx);

                        // Set lastIndexedTime only when we successfully persist the snapshot.
                        idx.lastIndexedTime = QDateTime::currentSecsSinceEpoch();

                        const auto devOpt = findDeviceById(j.deviceId);
                        if (devOpt) {
                            const QVariantMap dev = *devOpt;
                            idx.labelLastKnown = dev.value(QStringLiteral("label")).toString();
                            idx.uuidLastKnown = dev.value(QStringLiteral("uuid")).toString();
                        } else {
                            idx.labelLastKnown.clear();
                            idx.uuidLastKnown.clear();
                        }

                        QString saveErr;
                        if (!saveSnapshot(j.ownerUid, j.deviceId, idx, &saveErr)) {
                            Q_EMIT JobFinished(jobId,
                                               QStringLiteral("error"),
                                               QStringLiteral("Indexed, but failed to save snapshot: %1").arg(saveErr),
                                               props);
                        } else {
                            // Batch/coalesce index updates
                            queueDeviceIndexUpdated(j.ownerUid,
                                                    j.deviceId,
                                                    static_cast<quint64>(idx.generation),
                                                    static_cast<quint64>(idx.records.size()));

                            Q_EMIT JobProgress(jobId, 100, props);

                            // Final "rescanning" update (100%) so GUI can clear/replace it
                            {
                                auto* self = const_cast<IndexerService*>(this);
                                QVariantMap st;
                                st.insert(QStringLiteral("deviceId"), j.deviceId);
                                st.insert(QStringLiteral("percent"), 100u);
                                Q_EMIT self->DaemonStateChanged(uid, QStringLiteral("rescanning"), st);
                            }

                            Q_EMIT JobFinished(jobId,
                                               QStringLiteral("ok"),
                                               QStringLiteral("Indexed %1 entries (generation %2)")
                                                   .arg(static_cast<qulonglong>(idx.records.size()))
                                                   .arg(static_cast<qulonglong>(idx.generation)),
                                               props);
                        }
                    }
                }

                if (j.proc) {
                    j.proc->deleteLater();
                    j.proc = nullptr;
                }

                // Erase asynchronously to avoid any re-entrancy surprises during signal delivery.
                QTimer::singleShot(0, this, [this, jobId]() {
                    m_jobs.erase(jobId);
                });
            });

    // Spawn helper (daemon is root, so no pkexec)
    const QString helperPath = QStringLiteral("/usr/bin/kerything-scanner-helper");
    m_jobs[jobId]->proc->setProgram(helperPath);
    m_jobs[jobId]->proc->setArguments({ devNode, fsType });

    m_jobs[jobId]->proc->start();

    return jobId;
}

void IndexerService::CancelJob(quint64 jobId) {
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end() || !it->second) {
        return;
    }

    Job& j = *it->second;

    if (!j.proc) {
        return;
    }

    // Idempotent cancel.
    if (j.state == Job::State::Cancelling) {
        return;
    }

    j.state = Job::State::Cancelling;

    // Ask nicely first; kill shortly after if it's still alive.
    j.proc->terminate();

    QTimer::singleShot(500, this, [this, jobId]() {
        auto it2 = m_jobs.find(jobId);
        if (it2 == m_jobs.end() || !it2->second) return;
        Job& j2 = *it2->second;

        if (!j2.proc) return;
        if (j2.proc->state() != QProcess::NotRunning) {
            j2.proc->kill();
        }
    });

    // IMPORTANT: Do NOT emit JobFinished or erase here.
    // We only finalize from the QProcess::finished handler.
}

void IndexerService::Search(const QString& query,
                            const QStringList& deviceIds,
                            const QString& sortKey,
                            const QString& sortDir,
                            quint32 offset,
                            quint32 limit,
                            const QVariantMap& options,
                            quint64& totalHitsOut,
                            QVariantList& rowsOut) const {
    Q_UNUSED(options);

    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    rowsOut.clear();

    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) {
        totalHitsOut = 0;
        return;
    }
    const auto& indexes = uidIt->second;

    const bool desc = (sortDir.compare(QStringLiteral("desc"), Qt::CaseInsensitive) == 0);
    const QStringList tokens = tokenizeQuery(query);

    auto deviceAllowed = [&](const QString& dev) -> bool {
        return deviceIds.isEmpty() || deviceIds.contains(dev);
    };

    // ---- Fast path: empty query ----
    if (tokens.isEmpty()) {
        // If no device filter, use cached global order for O(limit) paging.
        const QString key = sortKey.isEmpty() ? QStringLiteral("name") : sortKey.toLower();

        if (deviceIds.isEmpty()) {
            // Heuristic: only build global-order cache when the user is paging far enough down
            // that the old O(offset) merge-walk becomes expensive.
            quint64 totalAll = 0;
            for (const auto& kv : indexes) {
                totalAll += static_cast<quint64>(kv.second.records.size());
            }
            totalHitsOut = totalAll;

            if (limit == 0 || totalAll == 0) return;

            static constexpr quint64 kCacheOffsetThreshold = 100'000ULL;

            const bool farByOffset = static_cast<quint64>(offset) >= kCacheOffsetThreshold;
            const bool farByFraction = (totalAll >= 1'000'000ULL) &&
                                       (static_cast<quint64>(offset) >= (totalAll / 4ULL));

            const bool shouldUseCache = farByOffset || farByFraction;

            // ---- Opportunistic warm-up for the common initial empty-query page ----
            // Goal: after daemon restart, the *first big jump* becomes instant without adding threads.
            if (offset == 0 && totalAll >= 1'000'000ULL) {
                const quint64 epoch = m_uidEpoch.contains(uid) ? m_uidEpoch.at(uid) : 0;
                const QString wk = warmKey(uid, epoch, key);

                // Only schedule if cache is missing and we haven’t already queued it for this epoch.
                if (!globalOrderForUid(uid, key) && !m_globalWarmScheduled.contains(wk)) {
                    m_globalWarmScheduled.insert(wk);

                    // Defer build until after we return this D-Bus reply.
                    QTimer::singleShot(0, const_cast<IndexerService*>(this), [this, uid, key, epoch, wk]() {
                        // If indexes changed since scheduling, skip.
                        const quint64 curEpoch = m_uidEpoch.contains(uid) ? m_uidEpoch.at(uid) : 0;
                        if (curEpoch != epoch) {
                            m_globalWarmScheduled.erase(wk);
                            return;
                        }

                        // If somebody already built it, nothing to do.
                        if (!globalOrderForUid(uid, key)) {
                            rebuildGlobalOrderForUid(uid, key);
                        }

                        m_globalWarmScheduled.erase(wk);
                    });
                }
            }

            if (shouldUseCache) {
                const auto* cache = globalOrderForUid(uid, key);
                if (!cache) {
                    rebuildGlobalOrderForUid(uid, key);
                    cache = globalOrderForUid(uid, key);
                }

                if (cache) {
                    // Override totalHitsOut with cached size (should match totalAll, but cache is authoritative here)
                    totalHitsOut = static_cast<quint64>(cache->asc.size());

                    const quint64 start = static_cast<quint64>(offset);
                    const quint64 end = std::min(start + static_cast<quint64>(limit), totalHitsOut);
                    if (start >= end) return;

                    rowsOut.reserve(static_cast<int>(end - start));

                    // Re-find uidIt (safe; but cheap) for device lookup
                    auto uidIt2 = m_indexesByUid.find(uid);
                    if (uidIt2 == m_indexesByUid.end()) return;

                    for (quint64 i = start; i < end; ++i) {
                        const quint64 idxPos = desc ? (totalHitsOut - 1 - i) : i;
                        const auto& e = cache->asc[static_cast<size_t>(idxPos)];

                        auto devIt = uidIt2->second.find(e.deviceId);
                        if (devIt == uidIt2->second.end()) continue;

                        const DeviceIndex& devIdx = devIt->second;
                        if (e.recordIdx >= devIdx.records.size()) continue;

                        const auto& r = devIdx.records[e.recordIdx];
                        std::string_view nm(devIdx.stringPool.data() + r.nameOffset, r.nameLen);

                        const quint64 entryId = makeEntryId(e.deviceId, e.recordIdx);
                        const quint32 flags = (r.isDir ? kFlagIsDir : 0u) | (r.isSymlink ? kFlagIsSymlink : 0u);

                        QVariantList row;
                        row.reserve(7);
                        row << QVariant::fromValue(entryId)
                            << QVariant::fromValue(e.deviceId)
                            << QVariant::fromValue(QString::fromUtf8(nm.data(), static_cast<int>(nm.size())))
                            << QVariant::fromValue(r.parentRecordIdx)
                            << QVariant::fromValue(static_cast<quint64>(r.size))
                            << QVariant::fromValue(static_cast<qint64>(r.modificationTime))
                            << QVariant::fromValue(flags);

                        rowsOut.push_back(row);
                    }
                    return;
                }
            }

            // If heuristic says "not far" OR cache wasn't available, fall through to old merge path below.
        }

        // Fallback: old merge behavior (used when deviceIds filter is active, or cache rebuild failed)
        quint64 total = 0;

        struct Cursor {
            const QString* deviceId = nullptr;
            const DeviceIndex* idx = nullptr;
            const std::vector<quint32>* order = nullptr;
            quint32 pos = 0;
        };
        std::vector<Cursor> cursors;
        cursors.reserve(indexes.size());

        auto pickOrder = [&](const DeviceIndex& idx) -> const std::vector<quint32>* {
            if (sortKey.compare(QStringLiteral("size"), Qt::CaseInsensitive) == 0) return &idx.orderBySize;
            if (sortKey.compare(QStringLiteral("mtime"), Qt::CaseInsensitive) == 0) return &idx.orderByMtime;
            if (sortKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) return &idx.orderByPath;
            return &idx.orderByName; // default
        };

        for (const auto& kv : indexes) {
            if (!deviceAllowed(kv.first)) continue;
            const DeviceIndex& idx = kv.second;
            total += static_cast<quint64>(idx.records.size());

            const auto* ord = pickOrder(idx);
            if (!ord || ord->empty()) continue;

            Cursor c;
            c.deviceId = &kv.first;
            c.idx = &idx;
            c.order = ord;
            c.pos = 0;
            cursors.push_back(c);
        }

        totalHitsOut = total;
        if (limit == 0 || total == 0) return;

        auto nameView = [&](const DeviceIndex& idx, quint32 recIdx) -> std::string_view {
            const auto& r = idx.records[recIdx];
            return std::string_view(idx.stringPool.data() + r.nameOffset, r.nameLen);
        };

        auto lessNode = [&](const Cursor& a, const Cursor& b) {
            const quint32 ai = (*a.order)[desc ? (static_cast<quint32>(a.order->size() - 1 - a.pos)) : a.pos];
            const quint32 bi = (*b.order)[desc ? (static_cast<quint32>(b.order->size() - 1 - b.pos)) : b.pos];

            if (sortKey.compare(QStringLiteral("size"), Qt::CaseInsensitive) == 0) {
                const auto sa = a.idx->records[ai].size;
                const auto sb = b.idx->records[bi].size;
                if (sa != sb) return sa > sb; // reversed for min-heap emulation
            } else if (sortKey.compare(QStringLiteral("mtime"), Qt::CaseInsensitive) == 0) {
                const auto ta = a.idx->records[ai].modificationTime;
                const auto tb = b.idx->records[bi].modificationTime;
                if (ta != tb) return ta > tb;
            } else if (sortKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) {
                const auto pa = a.idx->records[ai].parentRecordIdx;
                const auto pb = b.idx->records[bi].parentRecordIdx;
                if (pa != pb) return pa > pb;
            }

            const int c = ciCompareBytes(nameView(*a.idx, ai), nameView(*b.idx, bi));
            if (c != 0) return c > 0;

            // tie-breakers: deviceId then recordIdx
            if (*a.deviceId != *b.deviceId) return *a.deviceId > *b.deviceId;
            return ai > bi;
        };

        std::priority_queue<Cursor, std::vector<Cursor>, decltype(lessNode)> pq(lessNode);
        for (const auto& c : cursors) pq.push(c);

        quint64 globalPos = 0;
        const quint64 endPos = static_cast<quint64>(offset) + static_cast<quint64>(limit);

        while (!pq.empty() && globalPos < endPos) {
            Cursor cur = pq.top();
            pq.pop();

            const quint32 ordIdx = desc
                ? static_cast<quint32>(cur.order->size() - 1 - cur.pos)
                : cur.pos;

            const quint32 recIdx = (*cur.order)[ordIdx];

            if (globalPos >= offset) {
                const auto& r = cur.idx->records[recIdx];
                std::string_view nm = nameView(*cur.idx, recIdx);

                const quint64 entryId = makeEntryId(*cur.deviceId, recIdx);
                const quint32 flags = (r.isDir ? kFlagIsDir : 0u) | (r.isSymlink ? kFlagIsSymlink : 0u);

                QVariantList row;
                row.reserve(7);
                row << QVariant::fromValue(entryId)
                    << QVariant::fromValue(*cur.deviceId)
                    << QVariant::fromValue(QString::fromUtf8(nm.data(), static_cast<int>(nm.size())))
                    << QVariant::fromValue(r.parentRecordIdx)
                    << QVariant::fromValue(static_cast<quint64>(r.size))
                    << QVariant::fromValue(static_cast<qint64>(r.modificationTime))
                    << QVariant::fromValue(flags);

                rowsOut.push_back(row);
            }

            ++globalPos;

            ++cur.pos;
            if (cur.pos < cur.order->size()) {
                pq.push(cur);
            }
        }

        return;
    }

    // ---- Non-empty query: trigram filter + refine ----

    struct DeviceHits {
        const QString* deviceId = nullptr;
        const DeviceIndex* idx = nullptr;
        std::vector<quint32> hits; // recordIdx, sorted by rank (ascending)
    };

    auto pickRank = [&](const DeviceIndex& idx) -> const std::vector<quint32>& {
        if (sortKey.compare(QStringLiteral("size"), Qt::CaseInsensitive) == 0) return idx.rankBySize;
        if (sortKey.compare(QStringLiteral("mtime"), Qt::CaseInsensitive) == 0) return idx.rankByMtime;
        if (sortKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) return idx.rankByPath;
        return idx.rankByName;
    };

    const std::vector<QByteArray> tokBytes = [&]() {
        std::vector<QByteArray> out;
        out.reserve(tokens.size());
        for (const auto& t : tokens) out.push_back(t.toUtf8());
        return out;
    }();

    std::vector<DeviceHits> perDev;
    perDev.reserve(indexes.size());

    totalHitsOut = 0;

    for (const auto& kv : indexes) {
        const QString& devId = kv.first;
        if (!deviceAllowed(devId)) continue;

        const DeviceIndex& idx = kv.second;
        const auto candidates = deviceCandidatesForQuery(idx, tokens);
        if (candidates.empty()) continue;

        tbb::enumerable_thread_specific<std::vector<quint32>> tlsHits;

        // Parallel refinement: check tokens against name (case-insensitive substring)
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, candidates.size(), 4096),
            [&](const tbb::blocked_range<size_t>& r) {
                auto& local = tlsHits.local();
                local.reserve(local.size() + (r.size() / 8)); // small heuristic

                for (size_t i = r.begin(); i != r.end(); ++i) {
                    const quint32 recIdx = candidates[i];
                    const auto& rec = idx.records[recIdx];
                    std::string_view nm(idx.stringPool.data() + rec.nameOffset, rec.nameLen);

                    for (const auto& tb : tokBytes) {
                        const std::string_view needle(tb.constData(), static_cast<size_t>(tb.size()));
                        if (!nameContainsCaseInsensitive(nm, needle)) {
                            goto no_match;
                        }
                    }

                    local.push_back(recIdx);

                no_match:
                    (void)0;
                }
            }
        );

        // Merge thread-local buffers
        DeviceHits dh;
        dh.deviceId = &devId;
        dh.idx = &idx;

        size_t totalLocal = 0;
        for (const auto& v : tlsHits) totalLocal += v.size();

        dh.hits.reserve(totalLocal);
        for (auto& v : tlsHits) {
            dh.hits.insert(dh.hits.end(), v.begin(), v.end());
        }

        if (dh.hits.empty()) continue;

        totalHitsOut += static_cast<quint64>(dh.hits.size());

        const auto& rank = pickRank(idx);
        auto cmpByRankAsc = [&](quint32 a, quint32 b) {
            const quint32 ra = rank[a];
            const quint32 rb = rank[b];
            if (ra != rb) return ra < rb;
            return a < b;
        };

        if (dh.hits.size() >= 200'000) {
            std::sort(std::execution::par, dh.hits.begin(), dh.hits.end(), cmpByRankAsc);
        } else {
            std::sort(dh.hits.begin(), dh.hits.end(), cmpByRankAsc);
        }

        perDev.push_back(std::move(dh));
    }

    if (limit == 0 || totalHitsOut == 0) {
        return;
    }

    // k-way merge across devices for the requested page
    struct Node {
        size_t devIdx = 0;
        quint32 pos = 0; // position within that device's logical stream (asc or desc)
    };

    auto hitAt = [&](const DeviceHits& dh, quint32 pos) -> quint32 {
        // dh.hits is always stored ascending by rank.
        if (!desc) {
            return dh.hits[pos];
        }
        const size_t n = dh.hits.size();
        return dh.hits[(n - 1u) - static_cast<size_t>(pos)];
    };

    auto nodeLess = [&](const Node& a, const Node& b) {
        const DeviceHits& da = perDev[a.devIdx];
        const DeviceHits& db = perDev[b.devIdx];

        const quint32 ra = hitAt(da, a.pos);
        const quint32 rb = hitAt(db, b.pos);

        const auto& rankA = pickRank(*da.idx);
        const auto& rankB = pickRank(*db.idx);

        // Compare by rank, honoring desc by flipping
        const quint32 ka = rankA[ra];
        const quint32 kb = rankB[rb];

        // priority_queue is max-heap; return true when "a should come after b"
        if (ka != kb) return ka > kb;

        // tie-breakers for deterministic paging
        if (*da.deviceId != *db.deviceId) {
            return (*da.deviceId > *db.deviceId);
        }
        return ra > rb;
    };

    std::priority_queue<Node, std::vector<Node>, decltype(nodeLess)> pq(nodeLess);

    for (size_t i = 0; i < perDev.size(); ++i) {
        if (!perDev[i].hits.empty()) {
            pq.push(Node{i, 0});
        }
    }

    quint64 globalPos = 0;
    const quint64 endPos = static_cast<quint64>(offset) + static_cast<quint64>(limit);

    while (!pq.empty() && globalPos < endPos) {
        Node n = pq.top();
        pq.pop();

        DeviceHits& dh = perDev[n.devIdx];
        const quint32 recIdx = hitAt(dh, n.pos);
        const auto& r = dh.idx->records[recIdx];

        if (globalPos >= offset) {
            std::string_view nm(dh.idx->stringPool.data() + r.nameOffset, r.nameLen);

            const quint64 entryId = makeEntryId(*dh.deviceId, recIdx);
            const quint32 flags = (r.isDir ? kFlagIsDir : 0u) | (r.isSymlink ? kFlagIsSymlink : 0u);

            QVariantList row;
            row.reserve(7);
            row << QVariant::fromValue(entryId)
                << QVariant::fromValue(*dh.deviceId)
                << QVariant::fromValue(QString::fromUtf8(nm.data(), static_cast<int>(nm.size())))
                << QVariant::fromValue(r.parentRecordIdx)
                << QVariant::fromValue(static_cast<quint64>(r.size))
                << QVariant::fromValue(static_cast<qint64>(r.modificationTime))
                << QVariant::fromValue(flags);

            rowsOut.push_back(row);
        }

        ++globalPos;

        ++n.pos;
        if (n.pos < static_cast<quint32>(dh.hits.size())) {
            pq.push(n);
        }
    }
}

void IndexerService::ResolveDirectories(const QString& deviceId,
                                       const QVariantList& dirIds,
                                       QVariantList& out) const {
    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    out.clear();
    out.reserve(dirIds.size());

    // Only resolve if we have an index for this device
    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) return;
    if (uidIt->second.find(deviceId) == uidIt->second.end()) return;

    // Build a display prefix for this device (mountpoint preferred)
    QString prefix;
    {
        const auto devOpt = findDeviceById(deviceId);
        if (devOpt) {
            const QVariantMap dev = *devOpt;
            const bool mounted = dev.value(QStringLiteral("mounted")).toBool();
            const QString mp = dev.value(QStringLiteral("primaryMountPoint")).toString().trimmed();
            const QString label = dev.value(QStringLiteral("label")).toString().trimmed();

            if (mounted && !mp.isEmpty()) {
                prefix = mp;
            } else if (!label.isEmpty()) {
                prefix = QStringLiteral("[") + label + QStringLiteral("]");
            } else {
                prefix = QStringLiteral("[") + deviceId + QStringLiteral("]");
            }
        } else {
            prefix = QStringLiteral("[") + deviceId + QStringLiteral("]");
        }
    }

    auto joinPrefix = [&](const QString& internalPath) -> QString {
        // internalPath is like "/foo/bar" or "/"
        if (prefix.isEmpty()) return internalPath;

        // If prefix is a mount point like "/mnt/Data", prefer "/mnt/Data" + "/foo"
        if (prefix.startsWith('/')) {
            if (internalPath == QStringLiteral("/")) return prefix;
            return prefix + internalPath;
        }

        // If prefix is "[Label]" use "[Label]/foo"
        if (internalPath == QStringLiteral("/")) return prefix + QStringLiteral("/");
        return prefix + internalPath;
    };

    for (const QVariant& v : dirIds) {
        const quint32 id = v.toUInt();
        const QString internal = dirPathFor(uid, deviceId, id);
        const QString shown = joinPrefix(internal);

        QVariantList pair;
        pair.reserve(2);
        pair << QVariant::fromValue(id)
             << QVariant::fromValue(shown);

        out.push_back(pair);
    }
}

void IndexerService::ResolveEntries(const QVariantList& entryIds,
                                    QVariantList& out) const {
    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    out.clear();
    out.reserve(entryIds.size());

    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) return;

    const auto& indexes = uidIt->second;

    // Cache device inventory lookups per call
    QHash<QString, QVariantMap> deviceInfoCache;

    auto getDeviceInfo = [&](const QString& deviceId) -> QVariantMap {
        auto it = deviceInfoCache.find(deviceId);
        if (it != deviceInfoCache.end()) return *it;

        QVariantMap info;
        const auto devOpt = findDeviceById(deviceId);
        if (devOpt) {
            info = *devOpt;
        }
        deviceInfoCache.insert(deviceId, info);
        return info;
    };

    auto makeDisplayPrefix = [&](const QString& deviceId, const DeviceIndex& idx) -> QString {
        const QVariantMap dev = getDeviceInfo(deviceId);
        if (!dev.isEmpty()) {
            const bool mounted = dev.value(QStringLiteral("mounted")).toBool();
            const QString mp = dev.value(QStringLiteral("primaryMountPoint")).toString().trimmed();
            const QString label = dev.value(QStringLiteral("label")).toString().trimmed();

            if (mounted && !mp.isEmpty()) return mp;
            if (!label.isEmpty()) return QStringLiteral("[") + label + QStringLiteral("]");
        }

        // Fallback to snapshot metadata if available
        if (!idx.labelLastKnown.trimmed().isEmpty()) {
            return QStringLiteral("[") + idx.labelLastKnown.trimmed() + QStringLiteral("]");
        }

        return QStringLiteral("[") + deviceId + QStringLiteral("]");
    };

    auto joinPrefix = [&](const QString& prefix, const QString& internalPath) -> QString {
        // internalPath is like "/foo/bar" or "/"
        if (prefix.isEmpty()) return internalPath;

        // Prefix is a mount point like "/mnt/Data": "/mnt/Data" + "/foo"
        if (prefix.startsWith('/')) {
            if (internalPath == QStringLiteral("/")) return prefix;
            return prefix + internalPath;
        }

        // Prefix is "[Label]": "[Label]/foo"
        if (internalPath == QStringLiteral("/")) return prefix + QStringLiteral("/");
        return prefix + internalPath;
    };

    for (const QVariant& v : entryIds) {
        const quint64 entryId = v.toULongLong();
        const quint32 wantHash = static_cast<quint32>(entryId >> 32);
        const quint32 recordIdx = static_cast<quint32>(entryId & 0xFFFFFFFFu);

        // Find matching device by hash (then verify exact entryId)
        const QString* matchedDeviceId = nullptr;
        const DeviceIndex* matchedIdx = nullptr;

        for (const auto& kv : indexes) {
            const QString& devId = kv.first;
            const DeviceIndex& idx = kv.second;

            if (deviceHash32(devId) != wantHash) continue;
            if (recordIdx >= idx.records.size()) continue;
            if (makeEntryId(devId, recordIdx) != entryId) continue;

            matchedDeviceId = &devId;
            matchedIdx = &idx;
            break;
        }

        if (!matchedDeviceId || !matchedIdx) {
            // Unknown / stale entryId. Return a minimal record so GUI can skip gracefully.
            QVariantMap m;
            m.insert(QStringLiteral("entryId"), QVariant::fromValue<qulonglong>(entryId));
            m.insert(QStringLiteral("deviceId"), QString());
            m.insert(QStringLiteral("name"), QString());
            m.insert(QStringLiteral("isDir"), false);
            m.insert(QStringLiteral("mounted"), false);
            m.insert(QStringLiteral("primaryMountPoint"), QString());
            m.insert(QStringLiteral("internalPath"), QString());
            m.insert(QStringLiteral("displayPath"), QString());
            m.insert(QStringLiteral("internalDir"), QString());
            m.insert(QStringLiteral("displayDir"), QString());
            out.push_back(m);
            continue;
        }

        const QString& deviceId = *matchedDeviceId;
        const DeviceIndex& idx = *matchedIdx;
        const auto& rec = idx.records[recordIdx];

        const QString name = QString::fromUtf8(idx.stringPool.data() + rec.nameOffset,
                                              static_cast<int>(rec.nameLen));

        const QString internalDir = dirPathFor(uid, deviceId, rec.parentRecordIdx);
        const QString internalPath = joinInternalPath(internalDir, name);

        const QVariantMap dev = getDeviceInfo(deviceId);
        const bool mounted = dev.value(QStringLiteral("mounted")).toBool();
        const QString primaryMountPoint = dev.value(QStringLiteral("primaryMountPoint")).toString();

        const QString prefix = makeDisplayPrefix(deviceId, idx);
        const QString displayDir = joinPrefix(prefix, internalDir);
        const QString displayPath = joinPrefix(prefix, internalPath);

        QVariantMap m;
        m.insert(QStringLiteral("entryId"), QVariant::fromValue<qulonglong>(entryId));
        m.insert(QStringLiteral("deviceId"), deviceId);
        m.insert(QStringLiteral("name"), name);
        m.insert(QStringLiteral("isDir"), rec.isDir);
        m.insert(QStringLiteral("mounted"), mounted && !primaryMountPoint.trimmed().isEmpty());
        m.insert(QStringLiteral("primaryMountPoint"), primaryMountPoint);
        m.insert(QStringLiteral("internalPath"), internalPath);
        m.insert(QStringLiteral("displayPath"), displayPath);
        m.insert(QStringLiteral("internalDir"), internalDir);
        m.insert(QStringLiteral("displayDir"), displayDir);

        out.push_back(m);
    }
}

void IndexerService::ForgetIndex(const QString& deviceId) {
    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    // Refuse if there is a running job for this uid+deviceId (user should cancel first).
    for (const auto& kv : m_jobs) {
        if (!kv.second) continue;
        const Job& j = *kv.second;
        if (j.ownerUid == uid && j.deviceId == deviceId && j.proc && j.proc->state() != QProcess::NotRunning) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("Indexing job is running for this device. Cancel it first."));
            return;
        }
    }

    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) {
        // Nothing loaded; still attempt snapshot deletion.
        uidIt = m_indexesByUid.emplace(uid, std::unordered_map<QString, DeviceIndex>{}).first;
    }

    bool removed = false;

    // 1) Drop in-memory index
    {
        auto& map = uidIt->second;
        auto it = map.find(deviceId);
        if (it != map.end()) {
            map.erase(it);
            removed = true;
        }
    }

    // 2) Delete snapshot file (best-effort)
    {
        const QString snap = snapshotPathFor(uid, deviceId);
        QFile f(snap);
        if (f.exists()) {
            if (!f.remove()) {
                // If we removed from memory but couldn't remove file, report error to caller.
                // (Keeps behavior explicit; avoids “it came back after reboot” surprises.)
                sendErrorReply(QDBusError::Failed,
                               QStringLiteral("Removed in-memory index but failed to delete snapshot: %1").arg(snap));
                return;
            }
            removed = true;
        }
    }

    if (!removed) {
        // Not an error: idempotent
        return;
    }

    // Invalidate cached global search orders for this uid.
    bumpUidEpoch(uid);

    Q_EMIT DeviceIndexRemoved(deviceId);
}

void IndexerService::SetWatchEnabled(const QString& deviceId, bool enabled) {
    const quint32 uid = callerUidOr0();
    ensureLoadedForUid(uid);

    auto uidIt = m_indexesByUid.find(uid);
    if (uidIt == m_indexesByUid.end()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("No indexes loaded for this user."));
        return;
    }

    auto it = uidIt->second.find(deviceId);
    if (it == uidIt->second.end()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("No index exists for this deviceId."));
        return;
    }

    DeviceIndex& idx = it->second;
    if (idx.watchEnabled == enabled) {
        return; // idempotent
    }

    idx.watchEnabled = enabled;

    // Persist best-effort so the toggle survives daemon restart.
    QString err;
    if (!saveSnapshot(uid, deviceId, idx, &err)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to persist watch setting: %1").arg(err));
        return;
    }
}