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
#include <blkid/blkid.h>

static constexpr quint32 kFlagIsDir = 1u << 0;
static constexpr quint32 kFlagIsSymlink = 1u << 1;

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
 * Resolves and constructs the path to a directory within a device index using its unique directory ID.
 *
 * @param deviceId The identifier of the device as a QString.
 * @param dirId The unique identifier of the directory within the device index.
 * @return The absolute path of the directory as a QString. If the device ID is not found,
 *         the directory ID is invalid, or any other error occurs, a fallback value (e.g., "…" or "/")
 *         is returned.
 */
QString IndexerService::dirPathFor(const QString& deviceId, quint32 dirId) const {
    auto it = m_indexes.find(deviceId);
    if (it == m_indexes.end()) return QStringLiteral("…");

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
    indexedOut.reserve(static_cast<int>(m_indexes.size()));

    for (const auto& kv : m_indexes) {
        const QString& deviceId = kv.first;
        const DeviceIndex& idx = kv.second;

        QVariantMap m;
        m.insert(QStringLiteral("deviceId"), deviceId);
        m.insert(QStringLiteral("fsType"), idx.fsType);
        m.insert(QStringLiteral("generation"), QVariant::fromValue<qulonglong>(idx.generation));
        m.insert(QStringLiteral("entryCount"), QVariant::fromValue<qulonglong>(idx.records.size()));

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
    connect(m_jobs[jobId]->proc, &QProcess::readyReadStandardError, this, [this, jobId]() {
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
        }
    });

    connect(m_jobs[jobId]->proc,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, jobId](int exitCode, QProcess::ExitStatus exitStatus) {
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
                        DeviceIndex& idx = m_indexes[j.deviceId];
                        idx.fsType = j.fsType;
                        idx.generation += 1;
                        idx.records = std::move(parsed.records);
                        idx.stringPool = std::move(parsed.stringPool);
                        idx.dirPathCache.clear();

                        buildTrigramIndex(idx);
                        buildSortOrders(idx);

                        // notify clients that the index has changed
                        Q_EMIT DeviceIndexUpdated(j.deviceId,
                                                  static_cast<quint64>(idx.generation),
                                                  static_cast<quint64>(idx.records.size()));

                        Q_EMIT JobProgress(jobId, 100, props);
                        Q_EMIT JobFinished(jobId,
                                           QStringLiteral("ok"),
                                           QStringLiteral("Indexed %1 entries (generation %2)")
                                               .arg(static_cast<qulonglong>(idx.records.size()))
                                               .arg(static_cast<qulonglong>(idx.generation)),
                                           props);
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

    rowsOut.clear();

    const bool desc = (sortDir.compare(QStringLiteral("desc"), Qt::CaseInsensitive) == 0);
    const QStringList tokens = tokenizeQuery(query);

    auto deviceAllowed = [&](const QString& dev) -> bool {
        return deviceIds.isEmpty() || deviceIds.contains(dev);
    };

    // ---- Fast path: empty query ----
    if (tokens.isEmpty()) {
        quint64 total = 0;
        struct Cursor {
            const QString* deviceId = nullptr;
            const DeviceIndex* idx = nullptr;
            const std::vector<quint32>* order = nullptr;
            quint32 pos = 0;
        };
        std::vector<Cursor> cursors;
        cursors.reserve(m_indexes.size());

        auto pickOrder = [&](const DeviceIndex& idx) -> const std::vector<quint32>* {
            if (sortKey.compare(QStringLiteral("size"), Qt::CaseInsensitive) == 0) return &idx.orderBySize;
            if (sortKey.compare(QStringLiteral("mtime"), Qt::CaseInsensitive) == 0) return &idx.orderByMtime;
            if (sortKey.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) return &idx.orderByPath;
            return &idx.orderByName; // default
        };

        for (const auto& kv : m_indexes) {
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
        std::vector<quint32> hits; // recordIdx, sorted by rank
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
    perDev.reserve(m_indexes.size());

    totalHitsOut = 0;

    for (const auto& kv : m_indexes) {
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
        quint32 pos = 0;
    };

    auto nodeLess = [&](const Node& a, const Node& b) {
        const DeviceHits& da = perDev[a.devIdx];
        const DeviceHits& db = perDev[b.devIdx];

        const quint32 ra = da.hits[a.pos];
        const quint32 rb = db.hits[b.pos];

        const auto& rankA = pickRank(*da.idx);
        const auto& rankB = pickRank(*db.idx);

        // Compare by rank, honoring desc by flipping
        const quint32 ka = rankA[ra];
        const quint32 kb = rankB[rb];

        if (ka != kb) {
            // priority_queue is max-heap; we invert comparisons accordingly
            return desc ? (ka < kb) : (ka > kb);
        }

        // tie-breakers for deterministic paging
        if (*da.deviceId != *db.deviceId) {
            return (*da.deviceId > *db.deviceId);
        }
        return ra > rb;
    };

    std::priority_queue<Node, std::vector<Node>, decltype(nodeLess)> pq(nodeLess);

    for (size_t i = 0; i < perDev.size(); ++i) {
        pq.push(Node{i, 0});
    }

    quint64 globalPos = 0;
    const quint64 endPos = static_cast<quint64>(offset) + static_cast<quint64>(limit);

    while (!pq.empty() && globalPos < endPos) {
        Node n = pq.top();
        pq.pop();

        DeviceHits& dh = perDev[n.devIdx];
        const quint32 recIdx = dh.hits[n.pos];
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
        if (n.pos < dh.hits.size()) {
            pq.push(n);
        }
    }
}

void IndexerService::ResolveDirectories(const QString& deviceId,
                                       const QVariantList& dirIds,
                                       QVariantList& out) const {
    out.clear();
    out.reserve(dirIds.size());

    // Only resolve if we have an index for this device
    if (m_indexes.find(deviceId) == m_indexes.end()) {
        return;
    }

    for (const QVariant& v : dirIds) {
        const quint32 id = v.toUInt();
        const QString path = dirPathFor(deviceId, id);

        QVariantList pair;
        pair.reserve(2);
        pair << QVariant::fromValue(id)
             << QVariant::fromValue(path);

        out.push_back(pair);
    }
}