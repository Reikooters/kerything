// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexController.h"

#include <algorithm>
#include <iostream>
#include <shared_mutex>
#include <mutex>
#include <cctype>
#include <limits>
#include <numeric>
#include <utility>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QSet>
#include <QTextStream>

#include <chrono>

#include "SearchResultColumns.h"

namespace {
    using Clock = std::chrono::steady_clock;

    qint64 elapsedMsSince(const Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start
        ).count();
    }

    class PhaseTimer {
    public:
        explicit PhaseTimer(QString label, qint64 thresholdMs = 25)
            : label_(std::move(label)),
              thresholdMs_(thresholdMs),
              start_(Clock::now())
        {
        }

        ~PhaseTimer()
        {
            const qint64 elapsedMs = elapsedMsSince(start_);

#ifdef KERYTHING_ENABLE_LOGGING
            if (elapsedMs >= thresholdMs_) {
                std::cerr << label_.toStdString()
                          << " took "
                          << elapsedMs
                          << "ms\n";
            }
#endif
        }

    private:
        QString label_;
        qint64 thresholdMs_ = 0;
        Clock::time_point start_;
    };

    // Overload using TrigramEntry
    template <typename Fn>
    void forEachRecordIdxForTrigram(
        const std::vector<IndexController::TrigramEntry>& index,
        uint32_t trigram,
        Fn&& fn)
    {
        const auto range = std::equal_range(
            index.begin(),
            index.end(),
            IndexController::TrigramEntry{trigram, 0},
            [](const auto& a, const auto& b) {
                return a.trigram < b.trigram;
            }
        );

        for (auto it = range.first; it != range.second; ++it) {
            fn(it->recordIdx);
        }
    }

    // Overload using compacted trigram index
    template <typename Fn>
    void forEachRecordIdxForTrigram(
        const std::vector<IndexController::TrigramRange>& ranges,
        const std::vector<uint32_t>& postings,
        uint32_t trigram,
        Fn&& fn)
    {
        const auto it = std::lower_bound(
            ranges.begin(),
            ranges.end(),
            IndexController::TrigramRange{trigram, 0, 0},
            [](const auto& a, const auto& b) {
                return a.trigram < b.trigram;
            }
        );

        if (it == ranges.end() || it->trigram != trigram) {
            return;
        }

        const std::size_t begin = it->offset;
        const std::size_t end = begin + it->count;

        if (end > postings.size()) {
            return;
        }

        for (std::size_t i = begin; i < end; ++i) {
            fn(postings[i]);
        }
    }

    struct QueryKeyword {
        std::string text;
        std::string lowercaseText;
    };

    bool isAsciiWordCharacter(unsigned char c) noexcept
    {
        return std::isalnum(c) || c == '_';
    }

    bool isWholeWordMatchAt(
            std::string_view text,
            std::size_t pos,
            std::size_t length
        ) {
        if (length == 0 || pos > text.size() || pos + length > text.size()) {
            return false;
        }

        const bool leftBoundary =
            pos == 0 ||
            !isAsciiWordCharacter(static_cast<unsigned char>(text[pos - 1]));

        const bool rightBoundary =
            pos + length >= text.size() ||
            !isAsciiWordCharacter(static_cast<unsigned char>(text[pos + length]));

        return leftBoundary && rightBoundary;
    }

    bool containsWholeWord(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) {
            return true;
        }

        std::size_t pos = 0;

        while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
            if (isWholeWordMatchAt(haystack, pos, needle.size())) {
                return true;
            }

            pos += std::max<std::size_t>(needle.size(), 1);
        }

        return false;
    }

    bool matchesKeyword(
        std::string_view haystack,
        std::string_view needle,
        const IndexController::SearchOptions& options
    ) {
        return options.matchWholeWord
            ? containsWholeWord(haystack, needle)
            : IndexController::contains(haystack, needle);
    }

    // Overload using TrigramEntry
    bool containsTrigram(
        const std::vector<IndexController::TrigramEntry>& index,
        uint32_t trigram)
    {
        const auto range = std::equal_range(
            index.begin(),
            index.end(),
            IndexController::TrigramEntry{trigram, 0},
            [](const auto& a, const auto& b) {
                return a.trigram < b.trigram;
            }
        );

        return range.first != range.second;
    }

    // Overload using compacted trigram index
    bool containsTrigram(
        const std::vector<IndexController::TrigramRange>& ranges,
        uint32_t trigram)
    {
        const auto it = std::lower_bound(
            ranges.begin(),
            ranges.end(),
            IndexController::TrigramRange{trigram, 0, 0},
            [](const auto& a, const auto& b) {
                return a.trigram < b.trigram;
            }
        );

        return it != ranges.end() && it->trigram == trigram;
    }

    void buildCompactTrigramIndexFromSortedEntries(
        const std::vector<IndexController::TrigramEntry>& sortedEntries,
        std::vector<IndexController::TrigramRange>& ranges,
        std::vector<uint32_t>& postings)
    {
        ranges.clear();
        postings.clear();

        if (sortedEntries.empty()) {
            ranges.shrink_to_fit();
            postings.shrink_to_fit();
            return;
        }

        postings.reserve(sortedEntries.size());

        std::size_t i = 0;

        while (i < sortedEntries.size()) {
            const uint32_t trigram = sortedEntries[i].trigram;
            const uint32_t offset = static_cast<uint32_t>(postings.size());

            do {
                postings.push_back(sortedEntries[i].recordIdx);
                ++i;
            } while (i < sortedEntries.size() && sortedEntries[i].trigram == trigram);

            const uint32_t count =
                static_cast<uint32_t>(postings.size() - offset);

            ranges.push_back({
                trigram,
                offset,
                count
            });
        }

        ranges.shrink_to_fit();
        postings.shrink_to_fit();
    }

    QByteArray liveEntryKey(quint64 parentInode, std::string_view name)
    {
        QByteArray key = QByteArray::number(static_cast<qulonglong>(parentInode));
        key.append('\0');
        key.append(name.data(), static_cast<qsizetype>(name.size()));
        return key;
    }

    QByteArray liveEntryKey(quint64 parentInode, const QByteArray& name)
    {
        QByteArray key = QByteArray::number(static_cast<qulonglong>(parentInode));
        key.append('\0');
        key.append(name);
        return key;
    }

    QString formatBytes(quint64 bytes)
    {
        static constexpr double KiB = 1024.0;
        static constexpr double MiB = KiB * 1024.0;
        static constexpr double GiB = MiB * 1024.0;

        if (bytes >= static_cast<quint64>(GiB)) {
            return QStringLiteral("%1 GiB").arg(bytes / GiB, 0, 'f', 2);
        }

        if (bytes >= static_cast<quint64>(MiB)) {
            return QStringLiteral("%1 MiB").arg(bytes / MiB, 0, 'f', 2);
        }

        if (bytes >= static_cast<quint64>(KiB)) {
            return QStringLiteral("%1 KiB").arg(bytes / KiB, 0, 'f', 2);
        }

        return QStringLiteral("%1 B").arg(bytes);
    }

    QString formatBytesPerItem(quint64 bytes, std::size_t count)
    {
        if (count == 0) {
            return QStringLiteral("n/a");
        }

        const double bytesPerItem =
            static_cast<double>(bytes) / static_cast<double>(count);

        return QStringLiteral("%1 B").arg(bytesPerItem, 0, 'f', 2);
    }

    template <typename Vector>
    quint64 vectorCapacityBytes(const Vector& vector)
    {
        using ValueType = typename Vector::value_type;
        return static_cast<quint64>(vector.capacity()) * sizeof(ValueType);
    }

    template <typename Map>
    quint64 approximateUnorderedMapBucketBytes(const Map& map)
    {
        return static_cast<quint64>(map.bucket_count()) * sizeof(void*);
    }

    template <typename Map>
    quint64 approximateUnorderedMapNodePayloadBytes(const Map& map)
    {
        return static_cast<quint64>(map.size()) * sizeof(typename Map::value_type);
    }

    template <typename Map>
    quint64 estimatedUnorderedMapNodeOverheadBytes(const Map& map, quint64 overheadPerNode)
    {
        return static_cast<quint64>(map.size()) * overheadPerNode;
    }

    template <typename Map>
    void writeUnorderedMapOverheadEstimate(QTextStream& out, const Map& map, const QString& indent)
    {
        out << indent << "estimated node overhead only, assuming 16/24/32 bytes per node: "
            << formatBytes(estimatedUnorderedMapNodeOverheadBytes(map, 16))
            << " / "
            << formatBytes(estimatedUnorderedMapNodeOverheadBytes(map, 24))
            << " / "
            << formatBytes(estimatedUnorderedMapNodeOverheadBytes(map, 32))
            << '\n';

        out << indent << "estimated payload + buckets + 24-byte node overhead: "
            << formatBytes(
                approximateUnorderedMapNodePayloadBytes(map) +
                approximateUnorderedMapBucketBytes(map) +
                estimatedUnorderedMapNodeOverheadBytes(map, 24)
            )
            << '\n';
    }

    QString allocatorMemoryText()
    {
        QString text;
        QTextStream out(&text);

        out << "Allocator memory:\n";

#if defined(__GLIBC__)
        const struct mallinfo2 info = mallinfo2();

        out << "  glibc mallinfo2:\n";
        out << "    arena: " << formatBytes(static_cast<quint64>(info.arena))
            << " (non-mmapped heap space)\n";
        out << "    ordblks: " << info.ordblks
            << " (free chunks)\n";
        out << "    hblks: " << info.hblks
            << " (mmapped regions)\n";
        out << "    hblkhd: " << formatBytes(static_cast<quint64>(info.hblkhd))
            << " (mmapped region bytes)\n";
        out << "    uordblks: " << formatBytes(static_cast<quint64>(info.uordblks))
            << " (allocated bytes)\n";
        out << "    fordblks: " << formatBytes(static_cast<quint64>(info.fordblks))
            << " (free bytes retained by allocator)\n";
        out << "    keepcost: " << formatBytes(static_cast<quint64>(info.keepcost))
            << " (top-most releasable bytes estimate)\n";
#else
        out << "  unavailable: mallinfo2 is only reported for glibc builds\n";
#endif

        return text;
    }

    QString processStatusMemoryText()
    {
        const qint64 pid = static_cast<qint64>(::getpid());
        const QString statusPath = QStringLiteral("/proc/%1/status").arg(pid);
        const QString statmPath = QStringLiteral("/proc/%1/statm").arg(pid);
        const QString smapsRollupPath = QStringLiteral("/proc/%1/smaps_rollup").arg(pid);

        QString text;
        QTextStream out(&text);

        out << "Process memory:\n";
        out << "  pid: " << pid << '\n';

        QFile statusFile(statusPath);
        bool foundStatusMemoryLine = false;

        if (!statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out << "  " << statusPath << ": unavailable (could not be read)\n";
        } else {
            out << "  from " << statusPath << ":\n";

            const QByteArray contents = statusFile.readAll();
            const QList<QByteArray> lines = contents.split('\n');

            for (QByteArray line : lines) {
                line = line.trimmed();

                if (line.startsWith("VmPeak:") ||
                    line.startsWith("VmSize:") ||
                    line.startsWith("VmLck:") ||
                    line.startsWith("VmPin:") ||
                    line.startsWith("VmHWM:") ||
                    line.startsWith("VmRSS:") ||
                    line.startsWith("RssAnon:") ||
                    line.startsWith("RssFile:") ||
                    line.startsWith("RssShmem:")) {
                    out << "    " << QString::fromUtf8(line) << '\n';
                    foundStatusMemoryLine = true;
                }
            }

            if (!foundStatusMemoryLine) {
                out << "    no Vm*/Rss* lines found";
                out << " (read " << contents.size() << " bytes)\n";
            }
        }

        QFile statmFile(statmPath);

        if (!statmFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out << "  " << statmPath << ": unavailable (could not be read)\n";
            return text;
        }

        const QByteArray statmContents = statmFile.readAll().trimmed();
        const QList<QByteArray> fields = statmContents.split(' ');
        const long pageSize = ::sysconf(_SC_PAGESIZE);

        out << "  from " << statmPath << ":\n";
        out << "    raw: " << QString::fromUtf8(statmContents) << '\n';

        if (pageSize <= 0) {
            out << "    page size: unavailable\n";
            return text;
        }

        out << "    page size: " << pageSize << " bytes\n";

        auto fieldToBytes = [&](int index) -> std::optional<quint64> {
            if (index < 0 || index >= fields.size()) {
                return std::nullopt;
            }

            bool ok = false;
            const quint64 pages = fields.at(index).toULongLong(&ok);

            if (!ok) {
                return std::nullopt;
            }

            return pages * static_cast<quint64>(pageSize);
        };

        const std::optional<quint64> sizeBytes = fieldToBytes(0);
        const std::optional<quint64> residentBytes = fieldToBytes(1);
        const std::optional<quint64> sharedBytes = fieldToBytes(2);
        const std::optional<quint64> textBytes = fieldToBytes(3);
        const std::optional<quint64> dataBytes = fieldToBytes(5);

        if (sizeBytes) {
            out << "    size: " << formatBytes(*sizeBytes) << '\n';
        }

        if (residentBytes) {
            out << "    resident: " << formatBytes(*residentBytes) << '\n';
        }

        if (sharedBytes) {
            out << "    shared: " << formatBytes(*sharedBytes) << '\n';
        }

        if (textBytes) {
            out << "    text: " << formatBytes(*textBytes) << '\n';
        }

        if (dataBytes) {
            out << "    data + stack: " << formatBytes(*dataBytes) << '\n';
        }

        QFile smapsRollupFile(smapsRollupPath);

        if (!smapsRollupFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out << "  " << smapsRollupPath << ": unavailable (could not be read)\n";
            return text;
        }

        out << "  from " << smapsRollupPath << ":\n";

        const QByteArray smapsRollupContents = smapsRollupFile.readAll();
        const QList<QByteArray> smapsRollupLines = smapsRollupContents.split('\n');

        bool foundSmapsRollupLine = false;

        for (QByteArray line : smapsRollupLines) {
            line = line.trimmed();

            if (line.startsWith("Rss:") ||
                line.startsWith("Pss:") ||
                line.startsWith("Pss_Dirty:") ||
                line.startsWith("Shared_Clean:") ||
                line.startsWith("Shared_Dirty:") ||
                line.startsWith("Private_Clean:") ||
                line.startsWith("Private_Dirty:") ||
                line.startsWith("Referenced:") ||
                line.startsWith("Anonymous:") ||
                line.startsWith("AnonHugePages:") ||
                line.startsWith("Swap:") ||
                line.startsWith("SwapPss:")) {
                out << "    " << QString::fromUtf8(line) << '\n';
                foundSmapsRollupLine = true;
            }
        }

        if (!foundSmapsRollupLine) {
            out << "    no selected smaps_rollup lines found";
            out << " (read " << smapsRollupContents.size() << " bytes)\n";
        }

        return text;
    }
}

IndexController::IndexController(QObject* parent)
    : QObject(parent)
{
}

const IndexController::DeviceIndex* IndexController::deviceIndex(quint64 indexId) const {
    std::shared_lock lock(indexMutex_);

    const auto it = indexByIndexId_.find(indexId);
    if (it == indexByIndexId_.end()) {
        return nullptr;
    }

    return it->second.get();
}

quint64 IndexController::addDevice(
    const QString& deviceId,
    const QString& devNode,
    const QString& fsType,
    const QString& label,
    const QStringList& mountPoints,
    const QString& primaryMountPoint,
    quint32 requestId
) {
    std::unique_lock lock(indexMutex_);

    // Check whether devNode is valid
    if (devNode.isEmpty()) {
        std::cerr << "IndexController: Empty devNode provided for requestId=" << requestId << "\n";
        return 0;
    }

    // Check whether a DeviceIndex with the given devNode already exists.
    // If so, update the existing DeviceIndex with the new information and clear
    // the file records and string pool.
    const auto existingDevNodeIt = indexIdByDevNode_.find(devNode);
    if (existingDevNodeIt != indexIdByDevNode_.end()) {
        const quint64 existingIndexId = existingDevNodeIt->second;

        const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
        if (existingDeviceIndexIt != indexByIndexId_.end()) {
            indexIdByRequestId_[requestId] = existingIndexId;

            DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
            deviceIndex.fsType = fsType;
            deviceIndex.label = label;
            deviceIndex.deviceId = deviceId;
            deviceIndex.devNode = devNode;
            deviceIndex.mountPoints = mountPoints;
            deviceIndex.primaryMountPoint = primaryMountPoint;
            deviceIndex.mounted = !mountPoints.isEmpty();
            deviceIndex.isReady = false;
            deviceIndex.fileRecords.clear();
            deviceIndex.stringPool.clear();
            deviceIndex.deletedRecordBitmap.clear();
            deviceIndex.lowercaseStringPool.clear();
            deviceIndex.trigramRanges.clear();
            deviceIndex.trigramPostings.clear();
            deviceIndex.liveDeltaFlatIndex.clear();
            deviceIndex.recordsByExtension.clear();
            deviceIndex.extensionIndexEntryCount = 0;
            deviceIndex.extensionIndexLiveDeltaEntries = 0;
            deviceIndex.directoryFsIndexToRecordIdx.clear();
            deviceIndex.fsIndexToPrimaryRecordIdx.clear();
            deviceIndex.duplicateFsIndexToRecordIndices.clear();
            deviceIndex.fsIndexLookupScratch.clear();
            deviceIndex.generation++;
            deviceIndex.lastIndexedTime = 0;

#ifdef KERYTHING_ENABLE_LOGGING
            std::cout << "IndexController: Device already exists, so it was reset for devNode="
                      << devNode.toStdString()
                      << " indexId=" << existingIndexId
                      << "\n";
#endif
            return 0;
        }

        // Stale path mapping: remove it and fall through to create a new device
        indexIdByDevNode_.erase(existingDevNodeIt);
    }

    quint64 indexId = nextIndexId_++;

    std::unique_ptr<DeviceIndex> deviceIndex = std::make_unique<DeviceIndex>();
    deviceIndex->indexId = indexId;
    deviceIndex->fsType = fsType;
    deviceIndex->label = label;
    deviceIndex->deviceId = deviceId;
    deviceIndex->devNode = devNode;
    deviceIndex->mountPoints = mountPoints;
    deviceIndex->primaryMountPoint = primaryMountPoint;
    deviceIndex->mounted = !mountPoints.isEmpty();

    indexByIndexId_.emplace(indexId, std::move(deviceIndex));
    indexIdByDevNode_[devNode] = indexId;
    indexIdByRequestId_[requestId] = indexId;

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Added device " << devNode.toStdString()
              << " deviceId=" << deviceId.toStdString()
              << " devNode=" << devNode.toStdString()
              << " fsType=" << fsType.toStdString()
              << " indexId=" << indexId
              << " requestId=" << requestId << "\n";
#endif

    return indexId;
}

void IndexController::removeDeviceByIndexId(quint64 indexId) {
    bool removed = false;

    {
        std::unique_lock lock(indexMutex_);
        removed = removeDeviceByIndexIdUnlocked(indexId);
    }

    if (removed) {
        Q_EMIT deviceRemoved(indexId);
    }
}

bool IndexController::removeDeviceByDeviceId(const QString& deviceId)
{
    if (deviceId.isEmpty()) {
        return false;
    }

    quint64 removedIndexId = 0;
    bool removed = false;

    {
        std::unique_lock lock(indexMutex_);

        for (const auto& [indexId, deviceIndex] : indexByIndexId_) {
            if (deviceIndex && deviceIndex->deviceId == deviceId) {
                removedIndexId = indexId;
                removed = removeDeviceByIndexIdUnlocked(indexId);
                break;
            }
        }
    }

    if (removed) {
        Q_EMIT deviceRemoved(removedIndexId);
    }

    return removed;
}

bool IndexController::removeDeviceByIndexIdUnlocked(quint64 indexId) {
    // Look up the owning entry in the indexId -> DeviceIndex map.
    // We use find() instead of operator[] so we don't accidentally create
    // a new empty entry if the indexId does not exist.
    const auto deviceIt = indexByIndexId_.find(indexId);
    if (deviceIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: removeDeviceByIndexId: No device for indexId=" << indexId << "\n";
        return false;
    }

    // Capture the device path before removing the DeviceIndex object.
    // We need this to clean up the reverse lookup map as well.
    const QString devicePath = deviceIt->second->devNode;

    // Remove the devicePath -> indexId mapping, but only if it still points
    // to the same device we are removing.
    const auto pathIt = indexIdByDevNode_.find(devicePath);
    if (pathIt != indexIdByDevNode_.end() && pathIt->second == indexId) {
        indexIdByDevNode_.erase(pathIt);
    }

    // Remove any requestId -> indexId entries that refer to this device.
    // This keeps the request lookup table from holding stale references after
    // a cancellation or failed scan.
    for (auto it = indexIdByRequestId_.begin(); it != indexIdByRequestId_.end(); ) {
        if (it->second == indexId) {
            it = indexIdByRequestId_.erase(it);
        } else {
            ++it;
        }
    }

    // Finally remove the owned DeviceIndex itself.
    indexByIndexId_.erase(deviceIt);

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Removed device"
              << " devNode=" << devicePath.toStdString()
              << " indexId=" << indexId << "\n";
#endif

    return true;
}

bool IndexController::removeDeviceByRequestId(quint32 requestId) {
    quint64 removedIndexId = 0;
    bool removed = false;

    {
        std::unique_lock lock(indexMutex_);

        // Resolve the request to the device it belongs to.
        const auto requestIt = indexIdByRequestId_.find(requestId);
        if (requestIt == indexIdByRequestId_.end()) {
            return false;
        }

        removedIndexId = requestIt->second;

        // Remove the request mapping first so we don't leave a stale in-flight request.
        indexIdByRequestId_.erase(requestIt);

        // Remove the associated device and all of its reverse mappings.
        removed = removeDeviceByIndexIdUnlocked(removedIndexId);
    }

    if (removed) {
        Q_EMIT deviceRemoved(removedIndexId);
    }

    return removed;
}

void IndexController::appendDeviceFileRecordsByRequestId(const quint32 requestId, const std::vector<FileRecord> &records) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Appending " << records.size()
              << " file records to device " << deviceIndex.devNode.toStdString() << "\n";
#endif

    // Get the count of how many file records were the index before appending
    const std::size_t fileRecordsCountBefore = deviceIndex.fileRecords.size();

    // Reserve space for the new records
    deviceIndex.fileRecords.reserve(deviceIndex.fileRecords.size() + records.size());

    // Insert the new records into the device index.
    // Parent pointers are resolved once after the full scan has completed.
    deviceIndex.fileRecords.insert(deviceIndex.fileRecords.end(), records.begin(), records.end());
    deviceIndex.deletedRecordBitmap.resize(deviceIndex.fileRecords.size(), 0);

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: The index now contains " << deviceIndex.fileRecords.size()
              << " file records for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif
}

void IndexController::appendDeviceStringPoolByRequestId(const quint32 requestId, QByteArrayView stringPool) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Appending " << stringPool.size()
              << " string pool characters to device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif

    // Reserve space for the new records
    deviceIndex.stringPool.reserve(deviceIndex.stringPool.size() + static_cast<size_t>(stringPool.size()));

    // Insert the new string pool data into the device index
    deviceIndex.stringPool.insert(deviceIndex.stringPool.end(), stringPool.begin(), stringPool.end());

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: The index now contains " << deviceIndex.stringPool.size()
              << " string pool characters for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif
}

bool IndexController::removeRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    return indexIdByRequestId_.erase(requestId) > 0;
}

bool IndexController::updateDeviceRuntimeStateByDeviceId(
    const QString& deviceId,
    bool mounted,
    bool showOfflineResults,
    const QStringList& mountPoints,
    const QString& primaryMountPoint
) {
    if (deviceId.isEmpty()) {
        return false;
    }

    std::unique_lock lock(indexMutex_);

    for (const auto& [indexId, deviceIndex] : indexByIndexId_) {
        if (!deviceIndex || deviceIndex->deviceId != deviceId) {
            continue;
        }

        deviceIndex->mounted = mounted;
        deviceIndex->showOfflineResults = showOfflineResults;

        if (!mountPoints.isEmpty() || !primaryMountPoint.isEmpty() || !mounted) {
            deviceIndex->mountPoints = mountPoints;
            deviceIndex->primaryMountPoint = primaryMountPoint;
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "IndexController: Updated runtime state"
                  << " deviceId=" << deviceId.toStdString()
                  << " mounted=" << (mounted ? "true" : "false")
                  << " showOfflineResults=" << (showOfflineResults ? "true" : "false")
                  << " searchable=" << (deviceIndex->isSearchable() ? "true" : "false")
                  << "\n";
#endif

        return true;
    }

    return false;
}

IndexController::LiveUpdateApplyResult IndexController::applyLiveUpdateOperations(
    const QString& deviceId,
    const std::vector<LiveUpdateOperation>& operations)
{
    static quint64 nextLiveBatchDebugId = 1;
    const quint64 liveBatchDebugId = nextLiveBatchDebugId++;
    const auto batchStart = Clock::now();

    LiveUpdateApplyResult result;

    qsizetype operationUpserts = 0;
    qsizetype operationDeletes = 0;
    qsizetype operationMetadata = 0;
    qsizetype operationNeedsRescan = 0;
    qsizetype operationOther = 0;

    for (const LiveUpdateOperation& operation : operations) {
        switch (operation.kind) {
            case LiveUpdateOperationKind::Upsert:
                ++operationUpserts;
                break;
            case LiveUpdateOperationKind::DeleteEntry:
                ++operationDeletes;
                break;
            case LiveUpdateOperationKind::MetadataChanged:
                ++operationMetadata;
                break;
            case LiveUpdateOperationKind::NeedsRescan:
                ++operationNeedsRescan;
                break;
            default:
                ++operationOther;
                break;
        }
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "live batch #" << liveBatchDebugId
              << " start deviceId=" << deviceId.toStdString()
              << " operations=" << operations.size()
              << " upserts=" << operationUpserts
              << " deletes=" << operationDeletes
              << " metadata=" << operationMetadata
              << " needsRescan=" << operationNeedsRescan
              << " other=" << operationOther
              << "\n";
#endif

    if (deviceId.isEmpty() || operations.empty()) {
        return result;
    }

    std::unique_lock lock(indexMutex_);

    DeviceIndex* targetIndex = nullptr;

    for (const auto& [indexId, deviceIndex] : indexByIndexId_) {
        if (deviceIndex && deviceIndex->deviceId == deviceId) {
            targetIndex = deviceIndex.get();
            break;
        }
    }

    if (!targetIndex || !targetIndex->isReady) {
        result.missingDevice = static_cast<qsizetype>(operations.size());

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "live batch #" << liveBatchDebugId
                  << " end missing device elapsed="
                  << elapsedMsSince(batchStart)
                  << "ms\n";
#endif

        return result;
    }

    QSet<quint64> deleteParentInodes;
    qsizetype deleteEntryOperationCount = 0;

    {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 collect delete parents").arg(liveBatchDebugId)
        );

        for (const LiveUpdateOperation& operation : operations) {
            if (operation.kind != LiveUpdateOperationKind::DeleteEntry) {
                continue;
            }

            if (operation.parentInode == 0 || operation.name.isEmpty()) {
                continue;
            }

            deleteParentInodes.insert(operation.parentInode);
            ++deleteEntryOperationCount;
        }
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "live batch #" << liveBatchDebugId
              << " delete parents=" << deleteParentInodes.size()
              << " deleteEntryOperationCount=" << deleteEntryOperationCount
              << "\n";
#endif

    QHash<QByteArray, uint32_t> liveEntryRecordByParentAndName;

    if (!deleteParentInodes.isEmpty()) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 build delete lookup").arg(liveBatchDebugId),
            10
        );

        liveEntryRecordByParentAndName.reserve(deleteEntryOperationCount * 2);

        qsizetype scannedRecords = 0;
        qsizetype parentMatchedRecords = 0;
        qsizetype insertedRecords = 0;

        for (uint32_t recordIdx = 0;
             recordIdx < static_cast<uint32_t>(targetIndex->fileRecords.size());
             ++recordIdx) {
            ++scannedRecords;

            if (targetIndex->isDeletedRecord(recordIdx)) {
                continue;
            }

            const FileRecord& record = targetIndex->fileRecords[recordIdx];

            if (!deleteParentInodes.contains(record.parentFsIndex)) {
                continue;
            }

            ++parentMatchedRecords;

            const std::string_view name = targetIndex->recordName(recordIdx);
            if (name.empty()) {
                continue;
            }

            liveEntryRecordByParentAndName.insert(
                liveEntryKey(record.parentFsIndex, name),
                recordIdx
            );
            ++insertedRecords;
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "live batch #" << liveBatchDebugId
                  << " delete lookup scannedRecords=" << scannedRecords
                  << " parentMatchedRecords=" << parentMatchedRecords
                  << " insertedRecords=" << insertedRecords
                  << " mapSize=" << liveEntryRecordByParentAndName.size()
                  << "\n";
#endif
    }

    std::vector<LiveUpdateOperation> pendingUpserts;
    pendingUpserts.reserve(operations.size());

    bool trigramIndexNeedsSort = false;

    {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 collect pending upserts").arg(liveBatchDebugId)
        );

        for (const LiveUpdateOperation& operation : operations) {
            if (operation.kind == LiveUpdateOperationKind::Upsert) {
                pendingUpserts.push_back(operation);
            }
        }
    }

    if (!pendingUpserts.empty()) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 reserve upsert storage").arg(liveBatchDebugId),
            10
        );

        std::size_t stringBytesToAppend = 0;
        std::size_t estimatedTrigramsToAppend = 0;

        for (const LiveUpdateOperation& operation : pendingUpserts) {
            const QByteArray nameUtf8 = operation.name.toUtf8();
            const std::size_t nameSize = static_cast<std::size_t>(nameUtf8.size());

            stringBytesToAppend += nameSize;

            if (nameSize >= 3) {
                estimatedTrigramsToAppend += nameSize - 2;
            }
        }

        targetIndex->fileRecords.reserve(
            targetIndex->fileRecords.size() + pendingUpserts.size()
        );

        targetIndex->deletedRecordBitmap.reserve(
            targetIndex->deletedRecordBitmap.size() + pendingUpserts.size()
        );

        targetIndex->stringPool.reserve(
            targetIndex->stringPool.size() + stringBytesToAppend
        );

        targetIndex->lowercaseStringPool.reserve(
            targetIndex->lowercaseStringPool.size() + stringBytesToAppend
        );

        targetIndex->liveDeltaFlatIndex.reserve(
            targetIndex->liveDeltaFlatIndex.size() + estimatedTrigramsToAppend
        );

        targetIndex->fsIndexToPrimaryRecordIdx.reserve(
            targetIndex->fsIndexToPrimaryRecordIdx.size() + pendingUpserts.size()
        );

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "live batch #" << liveBatchDebugId
                  << " reserved upsert storage"
                  << " pendingUpserts=" << pendingUpserts.size()
                  << " stringBytesToAppend=" << stringBytesToAppend
                  << " estimatedTrigramsToAppend=" << estimatedTrigramsToAppend
                  << " fileRecords size/capacity="
                  << targetIndex->fileRecords.size()
                  << "/"
                  << targetIndex->fileRecords.capacity()
                  << " stringPool size/capacity="
                  << targetIndex->stringPool.size()
                  << "/"
                  << targetIndex->stringPool.capacity()
                  << "\n";
#endif
    }

    std::vector<uint8_t> consumedUpserts(pendingUpserts.size(), 0);
    QHash<quint64, std::vector<std::size_t>> pendingUpsertIndicesByInode;

    {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 build pending upsert inode map").arg(liveBatchDebugId)
        );

        if (!pendingUpserts.empty()) {
            pendingUpsertIndicesByInode.reserve(static_cast<qsizetype>(pendingUpserts.size()));

            for (std::size_t upsertIdx = 0; upsertIdx < pendingUpserts.size(); ++upsertIdx) {
                const LiveUpdateOperation& pendingUpsert = pendingUpserts[upsertIdx];

                if (pendingUpsert.inode == 0) {
                    continue;
                }

                pendingUpsertIndicesByInode[pendingUpsert.inode].push_back(upsertIdx);
            }
        }
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "live batch #" << liveBatchDebugId
              << " pendingUpserts=" << pendingUpserts.size()
              << " pendingUpsertInodes=" << pendingUpsertIndicesByInode.size()
              << "\n";
#endif

    bool fsIndexMapsNeedRebuild = false;

    qsizetype deleteLookupsFound = 0;
    qsizetype deleteLookupsMissing = 0;
    qsizetype markDeletedTreeCalls = 0;
    qsizetype markDeletedTreeTotalDeleted = 0;

    {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 process non-upsert operations").arg(liveBatchDebugId),
            10
        );

        for (const LiveUpdateOperation& operation : operations) {
            if (operation.kind == LiveUpdateOperationKind::Upsert) {
                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::MetadataChanged) {
                if (operation.inode == 0) {
                    ++result.missingInode;
                    continue;
                }

                const std::vector<uint32_t>* recordIndices =
                    targetIndex->recordIndicesForFsIndex(operation.inode);

                if (!recordIndices || recordIndices->empty()) {
                    ++result.missingInode;
                    continue;
                }

                bool updatedAny = false;

                for (const uint32_t recordIdx : *recordIndices) {
                    if (recordIdx >= targetIndex->fileRecords.size() ||
                        targetIndex->isDeletedRecord(recordIdx)) {
                        continue;
                    }

                    FileRecord& record = targetIndex->fileRecords[recordIdx];
                    const bool wasDirectory = (record.flags & FileRecord_IsDir) != 0;

                    updateFileRecordMetadataFromLiveUpdateOperation(record, operation);

                    const bool isDirectory = (record.flags & FileRecord_IsDir) != 0;
                    if (wasDirectory && !isDirectory) {
                        addRecordToExtensionIndexIfApplicable(*targetIndex, recordIdx);
                    }

                    updatedAny = true;
                }

                if (updatedAny) {
                    ++result.metadataChanged;
                }
                else {
                    ++result.missingInode;
                }

                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::DeleteEntry) {
                if (operation.parentInode == 0 || operation.name.isEmpty()) {
                    ++result.missingEntry;
                    continue;
                }

                qsizetype deletedCount = 0;
                const QByteArray nameUtf8 = operation.name.toUtf8();
                const std::string_view operationName(
                    nameUtf8.constData(),
                    static_cast<std::size_t>(nameUtf8.size())
                );

                bool movedAny = false;

                if (operation.kind == LiveUpdateOperationKind::DeleteEntry) {
                    if (operation.parentInode == 0 || operation.name.isEmpty()) {
                        ++result.missingEntry;
                        continue;
                    }

                    qsizetype deletedCount = 0;
                    const QByteArray nameUtf8 = operation.name.toUtf8();
                    if (nameUtf8.isEmpty()) {
                        ++result.missingEntry;
                        continue;
                    }

                    const auto recordIt = liveEntryRecordByParentAndName.constFind(
                        liveEntryKey(operation.parentInode, nameUtf8)
                    );

                    if (recordIt == liveEntryRecordByParentAndName.cend()) {
                        ++deleteLookupsMissing;
                        ++result.missingEntry;
                        continue;
                    }

                    ++deleteLookupsFound;

                    const uint32_t recordIdx = recordIt.value();

                    if (recordIdx >= targetIndex->fileRecords.size() ||
                        targetIndex->isDeletedRecord(recordIdx)) {
                        ++result.missingEntry;
                        continue;
                    }

                    const FileRecord& record = targetIndex->fileRecords[recordIdx];

                    std::size_t matchingUpsertIdx = pendingUpserts.size();

                    const auto pendingUpsertIndicesIt =
                        pendingUpsertIndicesByInode.constFind(record.fsIndex);

                    if (pendingUpsertIndicesIt != pendingUpsertIndicesByInode.cend()) {
                        for (const std::size_t upsertIdx : pendingUpsertIndicesIt.value()) {
                            if (consumedUpserts[upsertIdx] != 0) {
                                continue;
                            }

                            matchingUpsertIdx = upsertIdx;
                            break;
                        }
                    }

                    if (matchingUpsertIdx != pendingUpserts.size()) {
                        const LiveUpdateOperation& pendingUpsert = pendingUpserts[matchingUpsertIdx];

                        if (updateRecordIdentityFromLiveUpdateOperation(
                                *targetIndex,
                                recordIdx,
                                pendingUpsert
                            )) {
                            consumedUpserts[matchingUpsertIdx] = 1;
                            trigramIndexNeedsSort = true;
                            ++result.upserted;
                            continue;
                        }
                    }

                    ++markDeletedTreeCalls;

                    bool deletedDirectory = false;
                    const qsizetype treeDeletedCount =
                        targetIndex->markDeletedRecordTree(recordIdx, &deletedDirectory);

                    deletedCount += treeDeletedCount;
                    markDeletedTreeTotalDeleted += treeDeletedCount;

                    if (deletedDirectory) {
                        fsIndexMapsNeedRebuild = true;
                    }

                    if (deletedCount > 0) {
                        targetIndex->extensionIndexLiveDeltaEntries +=
                            static_cast<std::size_t>(deletedCount);
                        result.deleted += deletedCount;
                    }
                    else {
                        ++result.missingEntry;
                    }

                    continue;
                }

                if (deletedCount > 0) {
                    targetIndex->extensionIndexLiveDeltaEntries +=
                        static_cast<std::size_t>(deletedCount);
                    result.deleted += deletedCount;
                    fsIndexMapsNeedRebuild = true;
                }
                else if (!movedAny) {
                    ++result.missingEntry;
                }

                continue;
            }

            if (operation.kind == LiveUpdateOperationKind::NeedsRescan) {
                ++result.needsRescan;
                continue;
            }

            ++result.unsupported;
        }
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "live batch #" << liveBatchDebugId
              << " non-upsert summary"
              << " deleteLookupsFound=" << deleteLookupsFound
              << " deleteLookupsMissing=" << deleteLookupsMissing
              << " markDeletedTreeCalls=" << markDeletedTreeCalls
              << " markDeletedTreeTotalDeleted=" << markDeletedTreeTotalDeleted
              << "\n";
#endif

    if (fsIndexMapsNeedRebuild) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 rebuild fs index maps").arg(liveBatchDebugId),
            10
        );

        // Rebuild inode maps so deleted records no longer participate in
        // metadata updates, parent lookup, or future live-update matching.
        targetIndex->rebuildFsIndexMaps();
    }

    {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 process pending upserts").arg(liveBatchDebugId),
            10
        );

        int upsertPass = 0;

        while (!pendingUpserts.empty()) {
            ++upsertPass;

            const auto passStart = Clock::now();

            bool madeProgress = false;
            std::vector<LiveUpdateOperation> stillPending;
            stillPending.reserve(pendingUpserts.size());

            qsizetype passApplied = 0;
            qsizetype passMissingParent = 0;
            qsizetype passNeedsRescan = 0;
            qsizetype passUnsupported = 0;

            for (std::size_t upsertIdx = 0; upsertIdx < pendingUpserts.size(); ++upsertIdx) {
                if (consumedUpserts[upsertIdx] != 0) {
                    continue;
                }

                const LiveUpdateOperation& operation = pendingUpserts[upsertIdx];

                const UpsertApplyResult upsertResult =
                    applyUpsertOperation(*targetIndex, operation);

                switch (upsertResult) {
                    case UpsertApplyResult::Applied:
                        ++result.upserted;
                        ++passApplied;
                        madeProgress = true;
                        trigramIndexNeedsSort = true;
                        break;

                    case UpsertApplyResult::MissingParent:
                        ++passMissingParent;
                        stillPending.push_back(operation);
                        break;

                    case UpsertApplyResult::NeedsRescan:
                        ++result.needsRescan;
                        ++passNeedsRescan;
                        break;

                    case UpsertApplyResult::Invalid:
                    case UpsertApplyResult::NotUpsert:
                        ++result.unsupported;
                        ++passUnsupported;
                        break;
                }
            }

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "live batch #" << liveBatchDebugId
                      << " upsert pass " << upsertPass
                      << " input=" << pendingUpserts.size()
                      << " applied=" << passApplied
                      << " missingParent=" << passMissingParent
                      << " needsRescan=" << passNeedsRescan
                      << " unsupported=" << passUnsupported
                      << " elapsed=" << elapsedMsSince(passStart)
                      << "ms\n";
#endif

            if (!madeProgress) {
                result.missingParent += static_cast<qsizetype>(stillPending.size());
                break;
            }

            pendingUpserts = std::move(stillPending);
            consumedUpserts.assign(pendingUpserts.size(), 0);
        }
    }

    if (trigramIndexNeedsSort) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 sort live update trigram index").arg(liveBatchDebugId),
            10
        );

        sortLiveUpdateTrigramIndex(*targetIndex);
    }

    if (shouldRebuildTrigramIndexAfterLiveUpdates(*targetIndex)) {
        PhaseTimer timer(
             QStringLiteral("live batch #%1 rebuild trigram index").arg(liveBatchDebugId),
             10
        );

        rebuildTrigramIndexAfterLiveUpdates(*targetIndex);
    }

    if (shouldRebuildExtensionIndexAfterLiveUpdates(*targetIndex)) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 rebuild extension index").arg(liveBatchDebugId),
            10
        );

        rebuildExtensionIndexAfterLiveUpdates(*targetIndex);
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: applied live update operations"
              << " deviceId=" << deviceId.toStdString()
              << " metadataChanged=" << result.metadataChanged
              << " upserted=" << result.upserted
              << " deleted=" << result.deleted
              << " needsRescan=" << result.needsRescan
              << " unsupported=" << result.unsupported
              << " missingDevice=" << result.missingDevice
              << " missingInode=" << result.missingInode
              << " missingParent=" << result.missingParent
              << " missingEntry=" << result.missingEntry
              << "\n";

    std::cerr << "live batch #" << liveBatchDebugId
              << " end elapsed=" << elapsedMsSince(batchStart)
              << "ms"
              << " result metadataChanged=" << result.metadataChanged
              << " upserted=" << result.upserted
              << " deleted=" << result.deleted
              << " needsRescan=" << result.needsRescan
              << " unsupported=" << result.unsupported
              << " missingDevice=" << result.missingDevice
              << " missingInode=" << result.missingInode
              << " missingParent=" << result.missingParent
              << " missingEntry=" << result.missingEntry
              << " extensionIndexLiveDeltaEntries=" << targetIndex->extensionIndexLiveDeltaEntries
              << " extensionIndexEntries=" << targetIndex->extensionIndexEntryCount
              << "\n";
#endif

    return result;
}

void IndexController::setReadyState(quint32 requestId, bool isReady) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: setReadyState: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: setReadyState: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.isReady = isReady;
}

QString IndexController::memoryStatsText() const
{
    std::shared_lock lock(indexMutex_);

    QString text;
    QTextStream out(&text);

    out << processStatusMemoryText();
    out << '\n';

    out << allocatorMemoryText();
    out << '\n';

    out << "Type sizes:\n";
    out << "  sizeof(FileRecord): " << sizeof(FileRecord) << " bytes\n";
    out << "  sizeof(TrigramEntry): " << sizeof(TrigramEntry) << " bytes\n";
    out << "  sizeof(TrigramRange): " << sizeof(TrigramRange) << " bytes\n";
    out << "  sizeof(RecordHandle): " << sizeof(RecordHandle) << " bytes\n";
    out << "  sizeof(std::vector<uint32_t>): " << sizeof(std::vector<uint32_t>) << " bytes\n";
    out << "  sizeof(directoryFsIndexToRecordIdx::value_type): "
        << sizeof(typename decltype(std::declval<DeviceIndex>().directoryFsIndexToRecordIdx)::value_type)
        << " bytes\n";
    out << "  sizeof(fsIndexToPrimaryRecordIdx::value_type): "
        << sizeof(typename decltype(std::declval<DeviceIndex>().fsIndexToPrimaryRecordIdx)::value_type)
        << " bytes\n";
    out << "  sizeof(duplicateFsIndexToRecordIndices::value_type): "
        << sizeof(typename decltype(std::declval<DeviceIndex>().duplicateFsIndexToRecordIndices)::value_type)
        << " bytes\n";
    out << "  sizeof(recordsByExtension::value_type): "
        << sizeof(typename decltype(std::declval<DeviceIndex>().recordsByExtension)::value_type)
        << " bytes\n";
    out << '\n';

    out << "IndexController:\n";
    out << "  devices: " << indexByIndexId_.size() << '\n';
    out << "  indexByIndexId buckets: " << indexByIndexId_.bucket_count() << '\n';
    out << "  indexIdByDevNode entries/buckets: "
        << indexIdByDevNode_.size()
        << '/'
        << indexIdByDevNode_.bucket_count()
        << '\n';
    out << "  indexIdByRequestId entries/buckets: "
        << indexIdByRequestId_.size()
        << '/'
        << indexIdByRequestId_.bucket_count()
        << "\n\n";

    quint64 grandVectorBytes = 0;
    quint64 grandApproxHashPayloadBytes = 0;
    quint64 grandApproxHashBucketBytes = 0;
    quint64 grandEstimatedHashNodeOverhead24Bytes = 0;

    std::size_t grandRecords = 0;
    std::size_t grandFlatTrigrams = 0;
    std::size_t grandLiveDeltaTrigrams = 0;
    std::size_t grandStringBytes = 0;
    std::size_t grandLowercaseStringBytes = 0;
    std::size_t grandFsIndexStoredRecordRefs = 0;
    std::size_t grandExtensionStoredRecordRefs = 0;
    std::size_t maxSearchResultsFromReadySearchableDevices = 0;

    for (const auto& [indexId, deviceIndexPtr] : indexByIndexId_) {
        if (!deviceIndexPtr) {
            continue;
        }

        const DeviceIndex& device = *deviceIndexPtr;

        const quint64 fileRecordsBytes = vectorCapacityBytes(device.fileRecords);
        const quint64 stringPoolBytes = vectorCapacityBytes(device.stringPool);
        const quint64 lowercaseStringPoolBytes = vectorCapacityBytes(device.lowercaseStringPool);
        const quint64 deletedBitmapBytes = vectorCapacityBytes(device.deletedRecordBitmap);
        const quint64 trigramRangesBytes = vectorCapacityBytes(device.trigramRanges);
        const quint64 trigramPostingsBytes = vectorCapacityBytes(device.trigramPostings);
        const quint64 liveDeltaFlatIndexBytes = vectorCapacityBytes(device.liveDeltaFlatIndex);

        quint64 fsIndexDuplicateVectorObjectBytes = 0;
        quint64 fsIndexDuplicateVectorStorageBytes = 0;
        std::size_t fsIndexDuplicateStoredRecordRefs = 0;

        for (const auto& [fsIndex, recordIndices] : device.duplicateFsIndexToRecordIndices) {
            Q_UNUSED(fsIndex);
            fsIndexDuplicateVectorObjectBytes += sizeof(recordIndices);
            fsIndexDuplicateVectorStorageBytes += vectorCapacityBytes(recordIndices);
            fsIndexDuplicateStoredRecordRefs += recordIndices.size();
        }

        const std::size_t fsIndexStoredRecordRefs =
                device.fsIndexToPrimaryRecordIdx.size() +
                fsIndexDuplicateStoredRecordRefs;

        const quint64 fsIndexPrimaryPayloadBytes =
            approximateUnorderedMapNodePayloadBytes(device.fsIndexToPrimaryRecordIdx);
        const quint64 fsIndexPrimaryBucketBytes =
            approximateUnorderedMapBucketBytes(device.fsIndexToPrimaryRecordIdx);
        const quint64 fsIndexDuplicatePayloadBytes =
            approximateUnorderedMapNodePayloadBytes(device.duplicateFsIndexToRecordIndices) +
            fsIndexDuplicateVectorObjectBytes;
        const quint64 fsIndexDuplicateBucketBytes =
            approximateUnorderedMapBucketBytes(device.duplicateFsIndexToRecordIndices);

        quint64 extensionVectorObjectBytes = 0;
        quint64 extensionVectorStorageBytes = 0;
        std::size_t extensionStoredRecordRefs = 0;

        for (const auto& [extension, recordIndices] : device.recordsByExtension) {
            Q_UNUSED(extension);
            extensionVectorObjectBytes += sizeof(recordIndices);
            extensionVectorStorageBytes += vectorCapacityBytes(recordIndices);
            extensionStoredRecordRefs += recordIndices.size();
        }

        const quint64 deviceVectorBytes =
            fileRecordsBytes +
            stringPoolBytes +
            lowercaseStringPoolBytes +
            deletedBitmapBytes +
            trigramRangesBytes +
            trigramPostingsBytes +
            liveDeltaFlatIndexBytes +
            fsIndexDuplicateVectorStorageBytes +
            vectorCapacityBytes(device.fsIndexLookupScratch) +
            extensionVectorStorageBytes;

        const quint64 deviceApproxHashPayloadBytes =
            approximateUnorderedMapNodePayloadBytes(device.directoryFsIndexToRecordIdx) +
            approximateUnorderedMapNodePayloadBytes(device.fsIndexToPrimaryRecordIdx) +
            approximateUnorderedMapNodePayloadBytes(device.duplicateFsIndexToRecordIndices) +
            approximateUnorderedMapNodePayloadBytes(device.recordsByExtension) +
            fsIndexDuplicateVectorObjectBytes +
            extensionVectorObjectBytes;

        const quint64 deviceApproxHashBucketBytes =
            approximateUnorderedMapBucketBytes(device.directoryFsIndexToRecordIdx) +
            approximateUnorderedMapBucketBytes(device.fsIndexToPrimaryRecordIdx) +
            approximateUnorderedMapBucketBytes(device.duplicateFsIndexToRecordIndices) +
            approximateUnorderedMapBucketBytes(device.recordsByExtension);

        const quint64 deviceEstimatedHashNodeOverhead24Bytes =
            estimatedUnorderedMapNodeOverheadBytes(device.directoryFsIndexToRecordIdx, 24) +
            estimatedUnorderedMapNodeOverheadBytes(device.fsIndexToPrimaryRecordIdx, 24) +
            estimatedUnorderedMapNodeOverheadBytes(device.duplicateFsIndexToRecordIndices, 24) +
            estimatedUnorderedMapNodeOverheadBytes(device.recordsByExtension, 24);

        grandVectorBytes += deviceVectorBytes;
        grandApproxHashPayloadBytes += deviceApproxHashPayloadBytes;
        grandApproxHashBucketBytes += deviceApproxHashBucketBytes;
        grandEstimatedHashNodeOverhead24Bytes += deviceEstimatedHashNodeOverhead24Bytes;

        grandRecords += device.fileRecords.size();
        grandFlatTrigrams += device.trigramPostings.size();
        grandLiveDeltaTrigrams += device.liveDeltaFlatIndex.size();
        grandStringBytes += device.stringPool.size();
        grandLowercaseStringBytes += device.lowercaseStringPool.size();
        grandFsIndexStoredRecordRefs += fsIndexStoredRecordRefs;
        grandExtensionStoredRecordRefs += extensionStoredRecordRefs;

        if (device.isReady && device.isSearchable()) {
            const std::size_t mountMultiplier = device.mountPoints.isEmpty()
                ? 1
                : static_cast<std::size_t>(device.mountPoints.size());

            maxSearchResultsFromReadySearchableDevices +=
                device.fileRecords.size() * mountMultiplier;
        }

        out << "Device indexId=" << device.indexId << ":\n";
        out << "  label: " << device.label << '\n';
        out << "  deviceId: " << device.deviceId << '\n';
        out << "  devNode: " << device.devNode << '\n';
        out << "  fsType: " << device.fsType << '\n';
        out << "  ready/searchable/mounted: "
            << (device.isReady ? "true" : "false")
            << '/'
            << (device.isSearchable() ? "true" : "false")
            << '/'
            << (device.mounted ? "true" : "false")
            << '\n';
        out << "  mountPoints: " << device.mountPoints.size() << '\n';
        out << '\n';

        out << "  vectors:\n";
        out << "    fileRecords size/capacity: "
            << device.fileRecords.size()
            << '/'
            << device.fileRecords.capacity()
            << " => "
            << formatBytes(fileRecordsBytes)
            << '\n';
        out << "    stringPool size/capacity: "
            << device.stringPool.size()
            << '/'
            << device.stringPool.capacity()
            << " => "
            << formatBytes(stringPoolBytes)
            << '\n';
        out << "    lowercaseStringPool size/capacity: "
            << device.lowercaseStringPool.size()
            << '/'
            << device.lowercaseStringPool.capacity()
            << " => "
            << formatBytes(lowercaseStringPoolBytes)
            << '\n';
        out << "    deletedRecordBitmap size/capacity: "
                << device.deletedRecordBitmap.size()
                << '/'
                << device.deletedRecordBitmap.capacity()
                << " => "
                << formatBytes(deletedBitmapBytes)
                << '\n';
        out << "    trigramRanges size/capacity: "
            << device.trigramRanges.size()
            << '/'
            << device.trigramRanges.capacity()
            << " => "
            << formatBytes(trigramRangesBytes)
            << '\n';
        out << "    trigramPostings size/capacity: "
            << device.trigramPostings.size()
            << '/'
            << device.trigramPostings.capacity()
            << " => "
            << formatBytes(trigramPostingsBytes)
            << '\n';
        out << "    liveDeltaFlatIndex size/capacity: "
            << device.liveDeltaFlatIndex.size()
            << '/'
            << device.liveDeltaFlatIndex.capacity()
            << " => "
            << formatBytes(liveDeltaFlatIndexBytes)
            << '\n';
        out << '\n';

        out << "  maps:\n";
        out << "    directoryFsIndexToRecordIdx entries/buckets: "
                << device.directoryFsIndexToRecordIdx.size()
                << '/'
                << device.directoryFsIndexToRecordIdx.bucket_count()
                << '\n';
        writeUnorderedMapOverheadEstimate(
            out,
            device.directoryFsIndexToRecordIdx,
            QStringLiteral("      ")
        );

        out << "    fsIndexToPrimaryRecordIdx entries/buckets: "
                << device.fsIndexToPrimaryRecordIdx.size()
                << '/'
                << device.fsIndexToPrimaryRecordIdx.bucket_count()
                << '\n';
        out << "      payload bytes, excluding allocator/node overhead: "
            << formatBytes(fsIndexPrimaryPayloadBytes)
            << '\n';
        out << "      bucket bytes: "
            << formatBytes(fsIndexPrimaryBucketBytes)
            << '\n';
        writeUnorderedMapOverheadEstimate(
            out,
            device.fsIndexToPrimaryRecordIdx,
            QStringLiteral("      ")
        );

        out << "    duplicateFsIndexToRecordIndices entries/buckets: "
            << device.duplicateFsIndexToRecordIndices.size()
            << '/'
            << device.duplicateFsIndexToRecordIndices.bucket_count()
            << '\n';
        out << "      payload bytes, excluding allocator/node overhead: "
            << formatBytes(fsIndexDuplicatePayloadBytes)
            << '\n';
        out << "      bucket bytes: "
            << formatBytes(fsIndexDuplicateBucketBytes)
            << '\n';
        writeUnorderedMapOverheadEstimate(
            out,
            device.duplicateFsIndexToRecordIndices,
            QStringLiteral("      ")
        );
        out << "      stored record refs total: " << fsIndexStoredRecordRefs << '\n';
        out << "      primary record refs: " << device.fsIndexToPrimaryRecordIdx.size() << '\n';
        out << "      duplicate record refs: " << fsIndexDuplicateStoredRecordRefs << '\n';
        out << "      duplicate vector object bytes: " << formatBytes(fsIndexDuplicateVectorObjectBytes) << '\n';
        out << "      duplicate vector storage bytes: " << formatBytes(fsIndexDuplicateVectorStorageBytes) << '\n';
        out << "      lookup scratch size/capacity: "
            << device.fsIndexLookupScratch.size()
            << '/'
            << device.fsIndexLookupScratch.capacity()
            << " => "
            << formatBytes(vectorCapacityBytes(device.fsIndexLookupScratch))
            << '\n';
        out << "    recordsByExtension entries/buckets: "
            << device.recordsByExtension.size()
            << '/'
            << device.recordsByExtension.bucket_count()
            << '\n';
        writeUnorderedMapOverheadEstimate(
            out,
            device.recordsByExtension,
            QStringLiteral("      ")
        );
        out << "      stored record refs: " << extensionStoredRecordRefs << '\n';
        out << "      extensionIndexEntryCount: " << device.extensionIndexEntryCount << '\n';
        out << "      extensionIndexLiveDeltaEntries: " << device.extensionIndexLiveDeltaEntries << '\n';
        out << "      vector object bytes: " << formatBytes(extensionVectorObjectBytes) << '\n';
        out << "      vector storage bytes: " << formatBytes(extensionVectorStorageBytes) << '\n';
        out << '\n';

        out << "  approximate subtotal:\n";
        out << "    vector capacity bytes: " << formatBytes(deviceVectorBytes) << '\n';
        out << "    hash payload bytes, excluding allocator/node overhead: "
            << formatBytes(deviceApproxHashPayloadBytes)
            << '\n';
        out << "    hash bucket bytes: " << formatBytes(deviceApproxHashBucketBytes) << '\n';
        out << "    estimated hash node overhead, assuming 24 bytes per node: "
            << formatBytes(deviceEstimatedHashNodeOverhead24Bytes)
            << '\n';
        out << "    rough accounted subtotal: "
            << formatBytes(deviceVectorBytes + deviceApproxHashPayloadBytes + deviceApproxHashBucketBytes)
            << '\n';
        out << "    rough subtotal including 24-byte/node hash overhead estimate: "
            << formatBytes(
                deviceVectorBytes +
                deviceApproxHashPayloadBytes +
                deviceApproxHashBucketBytes +
                deviceEstimatedHashNodeOverhead24Bytes
            )
            << "\n\n";
    }

    out << "Grand totals:\n";
    out << "  file records: " << grandRecords << '\n';
    out << "  stringPool bytes used: " << grandStringBytes << '\n';
    out << "  lowercaseStringPool bytes used: " << grandLowercaseStringBytes << '\n';
    out << "  trigram posting entries: " << grandFlatTrigrams << '\n';
    out << "  live delta trigram entries: " << grandLiveDeltaTrigrams << '\n';
    out << "  fs-index stored record refs: " << grandFsIndexStoredRecordRefs << '\n';
    out << "  extension stored record refs: " << grandExtensionStoredRecordRefs << '\n';
    out << "  vector capacity bytes: " << formatBytes(grandVectorBytes) << '\n';
    out << "  hash payload bytes, excluding allocator/node overhead: "
        << formatBytes(grandApproxHashPayloadBytes)
        << '\n';
    out << "  hash bucket bytes: " << formatBytes(grandApproxHashBucketBytes) << '\n';
    out << "  estimated hash node overhead, assuming 24 bytes per node: "
        << formatBytes(grandEstimatedHashNodeOverhead24Bytes)
        << '\n';
    out << "  rough accounted index subtotal: "
        << formatBytes(grandVectorBytes + grandApproxHashPayloadBytes + grandApproxHashBucketBytes)
        << '\n';
    out << "  rough index subtotal including 24-byte/node hash overhead estimate: "
        << formatBytes(
            grandVectorBytes +
            grandApproxHashPayloadBytes +
            grandApproxHashBucketBytes +
            grandEstimatedHashNodeOverhead24Bytes
        )
        << '\n';

    out << '\n';
    out << "  per-record averages:\n";
    out << "    stringPool used bytes per record: "
        << formatBytesPerItem(grandStringBytes, grandRecords)
        << '\n';
    out << "    lowercaseStringPool used bytes per record: "
        << formatBytesPerItem(grandLowercaseStringBytes, grandRecords)
        << '\n';
    out << "    trigram postings per record: ";

    if (grandRecords == 0) {
        out << "n/a\n";
    } else {
        out << QStringLiteral("%1")
            .arg(
                static_cast<double>(grandFlatTrigrams) /
                static_cast<double>(grandRecords),
                0,
                'f',
                2
            )
            << '\n';
    }

    out << "    vector capacity bytes per record: "
        << formatBytesPerItem(grandVectorBytes, grandRecords)
        << '\n';
    out << "    rough accounted index bytes per record: "
        << formatBytesPerItem(
            grandVectorBytes + grandApproxHashPayloadBytes + grandApproxHashBucketBytes,
            grandRecords
        )
        << '\n';
    out << "    rough index bytes per record including 24-byte/node hash overhead estimate: "
        << formatBytesPerItem(
            grandVectorBytes +
            grandApproxHashPayloadBytes +
            grandApproxHashBucketBytes +
            grandEstimatedHashNodeOverhead24Bytes,
            grandRecords
        )
        << '\n';

    out << '\n';
    out << "  search result handle what-if for ready/searchable indexes:\n";
    out << "    max result handles: " << maxSearchResultsFromReadySearchableDevices << '\n';
    out << "    at current sizeof(RecordHandle) "
        << sizeof(RecordHandle)
        << " bytes: "
        << formatBytes(
            static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) *
            sizeof(RecordHandle)
        )
        << '\n';
    out << "    at 16 bytes per handle: "
        << formatBytes(static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) * 16)
        << '\n';
    out << "    at 12 bytes per handle: "
        << formatBytes(static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) * 12)
        << '\n';
    out << "    at 8 bytes per handle: "
        << formatBytes(static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) * 8)
        << '\n';

    return text;
}

bool IndexController::contains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }

    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return ch1 == ch2;
        }
    );

    return it != haystack.end();
}

std::string IndexController::normalizeExtensionToken(std::string_view extension)
{
    while (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }

    std::string out;
    out.reserve(extension.size());

    for (const unsigned char c : extension) {
        if (std::isspace(c) || c == ';') {
            continue;
        }

        out.push_back(static_cast<char>(std::tolower(c)));
    }

    return out;
}

std::string_view IndexController::finalExtension(std::string_view lowercaseFileName)
{
    if (lowercaseFileName.empty()) {
        return {};
    }

    const std::size_t dotPos = lowercaseFileName.rfind('.');
    if (dotPos == std::string_view::npos) {
        return {};
    }

    // Treat ".bashrc" as extensionless.
    if (dotPos == 0) {
        return {};
    }

    // Treat "file." as extensionless.
    if (dotPos + 1 >= lowercaseFileName.size()) {
        return {};
    }

    return lowercaseFileName.substr(dotPos + 1);
}

bool IndexController::matchesFileExtensionFilter(
    const FileRecord& record,
    std::string_view lowercaseFileName,
    const ExtensionSet& extensions
) {
    if (extensions.empty()) {
        return true;
    }

    // Extension filters apply to files only. A directory named "Videos.mp4"
    // should not match ext:mp4.
    if ((record.flags & FileRecord_IsDir) != 0) {
        return false;
    }

    const std::string_view extension = finalExtension(lowercaseFileName);
    if (extension.empty()) {
        return false;
    }

    // If only single extension being filtered, we can avoid the set lookup.
    if (extensions.size() == 1) {
        return extension == *extensions.begin();
    }

    return extensions.contains(extension);
}

std::size_t IndexController::maxSearchResultCount() const
{
    std::shared_lock lock(indexMutex_);

    std::size_t total = 0;

    for (const auto& [indexId, indexPtr] : indexByIndexId_) {
        Q_UNUSED(indexId);

        if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
            continue;
        }

        const std::size_t mountMultiplier = indexPtr->mountPoints.isEmpty()
            ? 1
            : static_cast<std::size_t>(indexPtr->mountPoints.size());

        total += indexPtr->fileRecords.size() * mountMultiplier;
    }

    return total;
}

quint8 IndexController::fileRecordFlagsFromLiveUpdateOperation(
    const LiveUpdateOperation& operation)
{
    quint8 flags = 0;

    if (operation.isDirectory) {
        flags |= FileRecord_IsDir;
    }

    if (operation.isSymlink) {
        flags |= FileRecord_IsSymlink;
    }

    return flags;
}

void IndexController::updateFileRecordMetadataFromLiveUpdateOperation(
    FileRecord& record,
    const LiveUpdateOperation& operation)
{
    record.size = operation.size;
    record.modificationTime = operation.modificationTime;
    record.flags = fileRecordFlagsFromLiveUpdateOperation(operation);
}

bool IndexController::appendTrigramsForRecord(
    DeviceIndex& deviceIndex,
    uint32_t recordIdx,
    std::vector<TrigramEntry>& targetIndex)
{
    if (recordIdx >= deviceIndex.fileRecords.size()) {
        return false;
    }

    const FileRecord& record = deviceIndex.fileRecords[recordIdx];

    if (record.nameOffset + record.nameLen > deviceIndex.lowercaseStringPool.size()) {
        return false;
    }

    if (record.nameLen < 3) {
        return false;
    }

    const std::string_view name(
        &deviceIndex.lowercaseStringPool[record.nameOffset],
        record.nameLen
    );

    std::unordered_set<uint32_t> uniqueTrigrams;
    uniqueTrigrams.reserve(record.nameLen);

    for (std::size_t i = 0; i <= name.size() - 3; ++i) {
        const uint32_t trigram =
            (static_cast<uint32_t>(static_cast<unsigned char>(name[i])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(name[i + 1])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(name[i + 2]));

        uniqueTrigrams.insert(trigram);
    }

    if (uniqueTrigrams.empty()) {
        return false;
    }

    targetIndex.reserve(targetIndex.size() + uniqueTrigrams.size());

    for (const uint32_t trigram : uniqueTrigrams) {
        targetIndex.push_back({
            trigram,
            recordIdx
        });
    }

    return true;
}

bool IndexController::shouldRebuildTrigramIndexAfterLiveUpdates(const DeviceIndex& deviceIndex)
{
    if (deviceIndex.liveDeltaFlatIndex.empty()) {
        return false;
    }

    static constexpr std::size_t LiveDeltaRebuildMinEntries = 100'000;
    static constexpr std::size_t LiveDeltaRebuildRatioDivisor = 10;

    if (deviceIndex.liveDeltaFlatIndex.size() < LiveDeltaRebuildMinEntries) {
        return false;
    }

    if (deviceIndex.trigramPostings.empty()) {
        return true;
    }

    return deviceIndex.liveDeltaFlatIndex.size() >=
           deviceIndex.trigramPostings.size() / LiveDeltaRebuildRatioDivisor;
}

void IndexController::rebuildTrigramIndexAfterLiveUpdates(DeviceIndex& deviceIndex)
{
#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Rebuilding compact trigram index after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " ranges=" << deviceIndex.trigramRanges.size()
              << " postings=" << deviceIndex.trigramPostings.size()
              << " liveDeltaFlatIndex=" << deviceIndex.liveDeltaFlatIndex.size()
              << "\n";
#endif

    std::vector<TrigramEntry> rebuiltIndex;

    std::size_t estimatedTrigrams = 0;
    for (uint32_t recordIdx = 0;
         recordIdx < static_cast<uint32_t>(deviceIndex.fileRecords.size());
         ++recordIdx) {
        if (deviceIndex.isDeletedRecord(recordIdx)) {
            continue;
        }

        const FileRecord& record = deviceIndex.fileRecords[recordIdx];
        if (record.nameLen >= 3 &&
            record.nameOffset + record.nameLen <= deviceIndex.lowercaseStringPool.size()) {
            estimatedTrigrams += record.nameLen - 2;
        }
    }

    rebuiltIndex.reserve(estimatedTrigrams);

    for (uint32_t recordIdx = 0;
         recordIdx < static_cast<uint32_t>(deviceIndex.fileRecords.size());
         ++recordIdx) {
        if (deviceIndex.isDeletedRecord(recordIdx)) {
            continue;
        }

        appendTrigramsForRecord(deviceIndex, recordIdx, rebuiltIndex);
    }

    static constexpr std::size_t ParallelSortThreshold = 500;

    if (rebuiltIndex.size() >= ParallelSortThreshold) {
        std::sort(
            std::execution::par,
            rebuiltIndex.begin(),
            rebuiltIndex.end()
        );
    } else {
        std::sort(
            rebuiltIndex.begin(),
            rebuiltIndex.end()
        );
    }

    auto last = std::unique(
        rebuiltIndex.begin(),
        rebuiltIndex.end(),
        [](const auto& a, const auto& b) {
            return a.trigram == b.trigram && a.recordIdx == b.recordIdx;
        }
    );

    rebuiltIndex.erase(last, rebuiltIndex.end());

    buildCompactTrigramIndexFromSortedEntries(
        rebuiltIndex,
        deviceIndex.trigramRanges,
        deviceIndex.trigramPostings
    );

    deviceIndex.liveDeltaFlatIndex.clear();

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Finished rebuilding compact trigram index after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " ranges=" << deviceIndex.trigramRanges.size()
              << " postings=" << deviceIndex.trigramPostings.size()
              << "\n";
#endif
}

void IndexController::sortLiveUpdateTrigramIndex(DeviceIndex& deviceIndex)
{
    if (deviceIndex.liveDeltaFlatIndex.size() < 2) {
        return;
    }

    static constexpr std::size_t ParallelSortThreshold = 500;

    if (deviceIndex.liveDeltaFlatIndex.size() >= ParallelSortThreshold) {
        std::sort(
            std::execution::par,
            deviceIndex.liveDeltaFlatIndex.begin(),
            deviceIndex.liveDeltaFlatIndex.end()
        );
    } else {
        std::sort(
            deviceIndex.liveDeltaFlatIndex.begin(),
            deviceIndex.liveDeltaFlatIndex.end()
        );
    }

    auto last = std::unique(
        deviceIndex.liveDeltaFlatIndex.begin(),
        deviceIndex.liveDeltaFlatIndex.end(),
        [](const auto& a, const auto& b) {
            return a.trigram == b.trigram && a.recordIdx == b.recordIdx;
        }
    );

    deviceIndex.liveDeltaFlatIndex.erase(last, deviceIndex.liveDeltaFlatIndex.end());
}

void IndexController::addRecordToExtensionIndexIfApplicable(
    DeviceIndex& deviceIndex,
    uint32_t recordIdx)
{
    if (deviceIndex.addRecordToExtensionIndex(recordIdx)) {
        ++deviceIndex.extensionIndexLiveDeltaEntries;
    }
}

bool IndexController::shouldRebuildExtensionIndexAfterLiveUpdates(const DeviceIndex& deviceIndex)
{
    if (deviceIndex.extensionIndexLiveDeltaEntries == 0) {
        return false;
    }

    static constexpr std::size_t ExtensionDeltaRebuildMinEntries = 25'000;
    static constexpr std::size_t ExtensionDeltaRebuildRatioDivisor = 5;

    if (deviceIndex.extensionIndexLiveDeltaEntries < ExtensionDeltaRebuildMinEntries) {
        return false;
    }

    const std::size_t indexedEntries = deviceIndex.extensionIndexEntryCount;

    if (indexedEntries == 0) {
        return true;
    }

    return deviceIndex.extensionIndexLiveDeltaEntries >=
           indexedEntries / ExtensionDeltaRebuildRatioDivisor;
}

void IndexController::rebuildExtensionIndexAfterLiveUpdates(DeviceIndex& deviceIndex)
{
#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Rebuilding extension index after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " extensions=" << deviceIndex.recordsByExtension.size()
              << " indexedEntries=" << deviceIndex.extensionIndexEntryCount
              << " liveDeltaEntries=" << deviceIndex.extensionIndexLiveDeltaEntries
              << "\n";
#endif

    deviceIndex.buildExtensionIndex();

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Finished rebuilding extension index after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " extensions=" << deviceIndex.recordsByExtension.size()
              << " indexedEntries=" << deviceIndex.extensionIndexEntryCount
              << "\n";
#endif
}

bool IndexController::appendRecordFromLiveUpdateOperation(
    DeviceIndex& deviceIndex,
    const LiveUpdateOperation& operation)
{
    const auto appendStart = Clock::now();

    const std::size_t oldFileRecordCapacity = deviceIndex.fileRecords.capacity();
    const std::size_t oldStringPoolCapacity = deviceIndex.stringPool.capacity();
    const std::size_t oldLowercaseStringPoolCapacity = deviceIndex.lowercaseStringPool.capacity();
    const std::size_t oldDeletedBitmapCapacity = deviceIndex.deletedRecordBitmap.capacity();
    const std::size_t oldLiveDeltaCapacity = deviceIndex.liveDeltaFlatIndex.capacity();

    if (operation.inode == 0 || operation.name.isEmpty()) {
        return false;
    }

    if (operation.parentInode == 0) {
        return false;
    }

    const QByteArray nameUtf8 = operation.name.toUtf8();
    if (nameUtf8.isEmpty()) {
        return false;
    }

    const uint32_t recordIdx = static_cast<uint32_t>(deviceIndex.fileRecords.size());
    const uint32_t nameOffset = static_cast<uint32_t>(deviceIndex.stringPool.size());
    const uint32_t nameLen = static_cast<uint32_t>(nameUtf8.size());

    FileRecord record{};
    record.fsIndex = operation.inode;
    record.parentFsIndex = operation.parentInode;
    record.parentRecordIdx = 0xFFFFFFFF;
    record.size = operation.size;
    record.modificationTime = operation.modificationTime;
    record.nameOffset = nameOffset;
    record.nameLen = nameLen;
    record.flags = fileRecordFlagsFromLiveUpdateOperation(operation);

    if (operation.inode != operation.parentInode) {
        const auto parentIt = deviceIndex.directoryFsIndexToRecordIdx.find(operation.parentInode);
        if (parentIt != deviceIndex.directoryFsIndexToRecordIdx.end()) {
            record.parentRecordIdx = parentIt->second;
        }
    }

    deviceIndex.stringPool.insert(
        deviceIndex.stringPool.end(),
        nameUtf8.begin(),
        nameUtf8.end()
    );

    deviceIndex.lowercaseStringPool.reserve(
        deviceIndex.lowercaseStringPool.size() + static_cast<std::size_t>(nameUtf8.size())
    );

    for (const unsigned char c : nameUtf8) {
        deviceIndex.lowercaseStringPool.push_back(
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 32) : static_cast<char>(c)
        );
    }

    deviceIndex.fileRecords.push_back(record);
    deviceIndex.deletedRecordBitmap.push_back(0);

    const auto [primaryIt, inserted] =
                deviceIndex.fsIndexToPrimaryRecordIdx.emplace(record.fsIndex, recordIdx);

    if (!inserted) {
        deviceIndex.duplicateFsIndexToRecordIndices[record.fsIndex].push_back(recordIdx);
    }

    if ((record.flags & FileRecord_IsDir) != 0) {
        deviceIndex.directoryFsIndexToRecordIdx[record.fsIndex] = recordIdx;
    }

    appendTrigramsForRecord(deviceIndex, recordIdx, deviceIndex.liveDeltaFlatIndex);
    addRecordToExtensionIndexIfApplicable(deviceIndex, recordIdx);

    const qint64 elapsedMs = elapsedMsSince(appendStart);

#ifdef KERYTHING_ENABLE_LOGGING
    if (elapsedMs >= 25 ||
        oldFileRecordCapacity != deviceIndex.fileRecords.capacity() ||
        oldStringPoolCapacity != deviceIndex.stringPool.capacity() ||
        oldLowercaseStringPoolCapacity != deviceIndex.lowercaseStringPool.capacity() ||
        oldDeletedBitmapCapacity != deviceIndex.deletedRecordBitmap.capacity() ||
        oldLiveDeltaCapacity != deviceIndex.liveDeltaFlatIndex.capacity()) {
        std::cerr << "appendRecordFromLiveUpdateOperation"
                  << " name=" << operation.name.toStdString()
                  << " elapsed=" << elapsedMs << "ms"
                  << " fileRecords capacity "
                  << oldFileRecordCapacity
                  << " -> "
                  << deviceIndex.fileRecords.capacity()
                  << " stringPool capacity "
                  << oldStringPoolCapacity
                  << " -> "
                  << deviceIndex.stringPool.capacity()
                  << " lowercaseStringPool capacity "
                  << oldLowercaseStringPoolCapacity
                  << " -> "
                  << deviceIndex.lowercaseStringPool.capacity()
                  << " deletedBitmap capacity "
                  << oldDeletedBitmapCapacity
                  << " -> "
                  << deviceIndex.deletedRecordBitmap.capacity()
                  << " liveDelta capacity "
                  << oldLiveDeltaCapacity
                  << " -> "
                  << deviceIndex.liveDeltaFlatIndex.capacity()
                  << "\n";
    }
#endif

    return true;
}

std::optional<uint32_t> IndexController::findLiveEntryRecord(
    const DeviceIndex& deviceIndex,
    quint64 parentInode,
    const QByteArray& nameUtf8,
    quint64 inode)
{
    if (parentInode == 0 || nameUtf8.isEmpty()) {
        return std::nullopt;
    }

    const std::string_view name(
        nameUtf8.constData(),
        static_cast<std::size_t>(nameUtf8.size())
    );

    auto matches = [&](uint32_t recordIdx) -> bool {
        if (recordIdx >= deviceIndex.fileRecords.size()) {
            return false;
        }

        if (deviceIndex.isDeletedRecord(recordIdx)) {
            return false;
        }

        const FileRecord& record = deviceIndex.fileRecords[recordIdx];

        if (record.parentFsIndex != parentInode) {
            return false;
        }

        if (inode != 0 && record.fsIndex != inode) {
            return false;
        }

        return deviceIndex.recordName(recordIdx) == name;
    };

    if (inode != 0) {
        const std::vector<uint32_t>* recordIndices =
            deviceIndex.recordIndicesForFsIndex(inode);

        if (!recordIndices) {
            return std::nullopt;
        }

        for (const uint32_t recordIdx : *recordIndices) {
            if (matches(recordIdx)) {
                return recordIdx;
            }
        }

        return std::nullopt;
    }

    for (uint32_t recordIdx = 0;
         recordIdx < static_cast<uint32_t>(deviceIndex.fileRecords.size());
         ++recordIdx) {
        if (matches(recordIdx)) {
            return recordIdx;
        }
    }

    return std::nullopt;
}

bool IndexController::updateRecordIdentityFromLiveUpdateOperation(
    DeviceIndex& deviceIndex,
    uint32_t recordIdx,
    const LiveUpdateOperation& operation)
{
    if (recordIdx >= deviceIndex.fileRecords.size()) {
        return false;
    }

    if (operation.inode == 0 || operation.parentInode == 0 || operation.name.isEmpty()) {
        return false;
    }

    FileRecord& record = deviceIndex.fileRecords[recordIdx];

    if (record.fsIndex != operation.inode) {
        return false;
    }

    uint32_t parentRecordIdx = 0xFFFFFFFF;

    if (operation.parentInode != operation.inode) {
        const auto parentIt = deviceIndex.directoryFsIndexToRecordIdx.find(operation.parentInode);
        if (parentIt == deviceIndex.directoryFsIndexToRecordIdx.end()) {
            return false;
        }

        parentRecordIdx = parentIt->second;
    }

    const QByteArray nameUtf8 = operation.name.toUtf8();
    if (nameUtf8.isEmpty()) {
        return false;
    }

    const uint32_t nameOffset = static_cast<uint32_t>(deviceIndex.stringPool.size());
    const uint32_t nameLen = static_cast<uint32_t>(nameUtf8.size());

    deviceIndex.stringPool.insert(
        deviceIndex.stringPool.end(),
        nameUtf8.begin(),
        nameUtf8.end()
    );

    deviceIndex.lowercaseStringPool.reserve(
        deviceIndex.lowercaseStringPool.size() + static_cast<std::size_t>(nameUtf8.size())
    );

    for (const unsigned char c : nameUtf8) {
        deviceIndex.lowercaseStringPool.push_back(
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 32) : static_cast<char>(c)
        );
    }

    record.parentFsIndex = operation.parentInode;
    record.parentRecordIdx = parentRecordIdx;
    record.nameOffset = nameOffset;
    record.nameLen = nameLen;

    updateFileRecordMetadataFromLiveUpdateOperation(record, operation);
    appendTrigramsForRecord(deviceIndex, recordIdx, deviceIndex.liveDeltaFlatIndex);
    addRecordToExtensionIndexIfApplicable(deviceIndex, recordIdx);

    return true;
}

IndexController::UpsertApplyResult IndexController::applyUpsertOperation(
    DeviceIndex& deviceIndex,
    const LiveUpdateOperation& operation)
{
    const auto upsertStart = Clock::now();

    auto logSlowUpsert = [&](const char* outcome) {
        const qint64 elapsedMs = elapsedMsSince(upsertStart);

#ifdef KERYTHING_ENABLE_LOGGING
        if (elapsedMs >= 25) {
            std::cerr << "applyUpsertOperation"
                      << " outcome=" << outcome
                      << " kind=" << liveUpdateOperationKindToString(operation.kind).toStdString()
                      << " name=" << operation.name.toStdString()
                      << " inode=" << operation.inode
                      << " parentInode=" << operation.parentInode
                      << " elapsed=" << elapsedMs
                      << "ms\n";
        }
#endif
    };

    if (operation.kind != LiveUpdateOperationKind::Upsert) {
        logSlowUpsert("not-upsert");
        return UpsertApplyResult::NotUpsert;
    }

    if (operation.inode == 0 || operation.parentInode == 0 || operation.name.isEmpty()) {
        logSlowUpsert("invalid");
        return UpsertApplyResult::Invalid;
    }

    const QByteArray nameUtf8 = operation.name.toUtf8();
    if (nameUtf8.isEmpty()) {
        logSlowUpsert("invalid-empty-name");
        return UpsertApplyResult::Invalid;
    }

    const std::optional<uint32_t> existingSameEntry =
        findLiveEntryRecord(deviceIndex, operation.parentInode, nameUtf8, operation.inode);

    if (existingSameEntry) {
        FileRecord& record = deviceIndex.fileRecords[*existingSameEntry];
        updateFileRecordMetadataFromLiveUpdateOperation(record, operation);
        logSlowUpsert("updated-existing");
        return UpsertApplyResult::Applied;
    }

    /*
     * If the inode already exists but this exact parent/name pair does not, this
     * is usually a new directory entry for the same inode, for example a hard link.
     *
     * Paired renames/moves are handled earlier by DeleteEntry matching a pending
     * Upsert with the same inode and updating that existing record's identity.
     * Reaching this point means this Upsert is not consumed by a matching delete,
     * so appending a second FileRecord is the safest representation for EXT4 hard
     * links and avoids false stale-index warnings.
     */

    /*
     * New non-root entries need their parent directory to exist in the index
     * before we append them, otherwise parentRecordIdx cannot be resolved.
     */
    if (operation.parentInode != operation.inode &&
        !deviceIndex.directoryFsIndexToRecordIdx.contains(operation.parentInode)) {
        logSlowUpsert("missing-parent");
        return UpsertApplyResult::MissingParent;
    }

    if (!appendRecordFromLiveUpdateOperation(deviceIndex, operation)) {
        logSlowUpsert("append-invalid");
        return UpsertApplyResult::Invalid;
    }

    logSlowUpsert("appended");
    return UpsertApplyResult::Applied;
}

IndexController::ParsedSearchQuery IndexController::parseSearchQuery(std::string_view query)
{
    ParsedSearchQuery parsed;

    std::string lowercaseQuery(query);
    std::transform(
        lowercaseQuery.begin(),
        lowercaseQuery.end(),
        lowercaseQuery.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    auto consumeExtensionList = [&parsed](std::string_view extensionList) {
        const std::size_t estimatedTokenCount =
            1 + static_cast<std::size_t>(
                std::count(extensionList.begin(), extensionList.end(), ';')
            );

        parsed.extensions.reserve(parsed.extensions.size() + estimatedTokenCount);

        std::size_t start = 0;

        while (start <= extensionList.size()) {
            const std::size_t end = extensionList.find(';', start);
            const std::string_view token = end == std::string_view::npos
                ? extensionList.substr(start)
                : extensionList.substr(start, end - start);

            std::string normalized = normalizeExtensionToken(token);
            if (!normalized.empty()) {
                parsed.extensions.insert(std::move(normalized));
            }

            if (end == std::string_view::npos) {
                break;
            }

            start = end + 1;
        }
    };

    std::size_t start = 0;
    while (start < query.size()) {
        while (start < query.size() &&
               std::isspace(static_cast<unsigned char>(query[start]))) {
            ++start;
        }

        if (start >= query.size()) {
            break;
        }

        std::size_t end = start;
        while (end < query.size() &&
               !std::isspace(static_cast<unsigned char>(query[end]))) {
            ++end;
        }

        const std::string_view token(query.data() + start, end - start);
        const std::string_view lowercaseToken(lowercaseQuery.data() + start, end - start);

        if (lowercaseToken.starts_with("ext:")) {
            consumeExtensionList(lowercaseToken.substr(4));
        } else if (lowercaseToken.starts_with("extension:")) {
            consumeExtensionList(lowercaseToken.substr(10));
        } else if (lowercaseToken == "folder:" ||
                   lowercaseToken == "folders:" ||
                   lowercaseToken == "type:folder" ||
                   lowercaseToken == "type:folders") {
            parsed.foldersOnly = true;
       } else {
           parsed.keywords.emplace_back(token);
       }

        start = end;
    }

    return parsed;
}

std::vector<IndexController::RecordHandle> IndexController::performTrigramSearch(
    const std::string& query,
    SearchOptions options
) {
    std::shared_lock lock(indexMutex_);

    // 1. Tokenize query: "valley dragonforce" -> ["valley", "dragonforce"]
    // 2. For each word >= 3 chars, get candidate IDs from trigramIndex
    // 3. Intersect the ID lists (Candidate Filtering)
    // 4. For remaining candidates, do a case-insensitive sub-string check (Refinement)

    std::vector<RecordHandle> results;

    auto appendResult = [&results](const DeviceIndex& index, uint32_t recordIdx) {
        if (index.indexId > std::numeric_limits<uint32_t>::max() ||
            index.generation > std::numeric_limits<uint32_t>::max()) {
            return;
            }

        const auto indexId = static_cast<uint32_t>(index.indexId);
        const auto generation = static_cast<uint32_t>(index.generation);

        if (index.mountPoints.isEmpty()) {
            results.emplace_back(indexId, generation, recordIdx, 0xFFFFFFFF);
            return;
        }

        for (int mountPointIdx = 0; mountPointIdx < index.mountPoints.size(); ++mountPointIdx) {
            results.emplace_back(
                indexId,
                generation,
                recordIdx,
                static_cast<uint32_t>(mountPointIdx)
            );
        }
    };

    if (indexByIndexId_.empty()) {
        return results;
    }

    const ParsedSearchQuery parsedQuery = parseSearchQuery(query);
    const std::vector<std::string>& keywords = parsedQuery.keywords;
    const ExtensionSet& extensionFilter = parsedQuery.extensions;
    const bool hasExtensionFilter = !extensionFilter.empty();
    const bool foldersOnly = parsedQuery.foldersOnly;

    // Extension filters are file-only, so this combination can never match.
    if (foldersOnly && hasExtensionFilter) {
        return results;
    }

    std::vector<QueryKeyword> queryKeywords;
    queryKeywords.reserve(keywords.size());

    for (const std::string& keyword : keywords) {
        QueryKeyword queryKeyword;
        queryKeyword.text = keyword;
        queryKeyword.lowercaseText = keyword;

        std::transform(
            queryKeyword.lowercaseText.begin(),
            queryKeyword.lowercaseText.end(),
            queryKeyword.lowercaseText.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );

        queryKeywords.push_back(std::move(queryKeyword));
    }

    auto collectExtensionCandidates = [](
        const DeviceIndex& index,
        const ExtensionSet& extensions
    ) {
        std::vector<uint32_t> candidates;

        if (extensions.empty()) {
            return candidates;
        }

        std::size_t estimatedSize = 0;

        for (const std::string& extension : extensions) {
            if (const std::vector<uint32_t>* records =
                    index.recordIndicesForExtension(extension)) {
                estimatedSize += records->size();
                    }
        }

        candidates.reserve(estimatedSize);

        for (const std::string& extension : extensions) {
            const std::vector<uint32_t>* records =
                index.recordIndicesForExtension(extension);

            if (!records) {
                continue;
            }

            candidates.insert(
                candidates.end(),
                records->begin(),
                records->end()
            );
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(
            std::unique(candidates.begin(), candidates.end()),
            candidates.end()
        );

        return candidates;
    };

    // IF EMPTY: Return everything matching non-trigram filters.
    if (keywords.empty()) {
        std::size_t totalSize = 0;

        for (const auto& [indexId, indexPtr] : indexByIndexId_) {
            if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
                continue;
            }

            const std::size_t mountMultiplier = indexPtr->mountPoints.isEmpty()
                ? 1
                : static_cast<std::size_t>(indexPtr->mountPoints.size());

            if (hasExtensionFilter) {
                const auto extensionCandidates =
                    collectExtensionCandidates(*indexPtr, extensionFilter);

                totalSize += extensionCandidates.size() * mountMultiplier;
            } else {
                totalSize += indexPtr->fileRecords.size() * mountMultiplier;
            }
        }

        results.reserve(totalSize);

        for (const auto& [indexId, indexPtr] : indexByIndexId_) {
            if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
                continue;
            }

            const auto& index = *indexPtr;

            auto appendIfMatches = [&](uint32_t i) {
                if (index.isDeletedRecord(i)) {
                    return;
                }

                const FileRecord& rec = index.fileRecords[i];
                const bool isDirectory = (rec.flags & FileRecord_IsDir) != 0;

                if (foldersOnly && !isDirectory) {
                    return;
                }

                // Extension filters apply to files only. Reject directories before
                // touching the string pool or scanning the name for a final extension.
                if (hasExtensionFilter && isDirectory) {
                    return;
                }

                if (rec.nameOffset + rec.nameLen > index.lowercaseStringPool.size()) {
                    return;
                }

                const std::string_view name(
                    &index.lowercaseStringPool[rec.nameOffset],
                    rec.nameLen
                );

                if (hasExtensionFilter &&
                    !matchesFileExtensionFilter(rec, name, extensionFilter)) {
                    return;
                }

                appendResult(index, i);
            };

            if (hasExtensionFilter) {
                const auto extensionCandidates =
                    collectExtensionCandidates(index, extensionFilter);

                for (const uint32_t recordIdx : extensionCandidates) {
                    appendIfMatches(recordIdx);
                }
            } else {
                for (uint32_t i = 0; i < index.fileRecords.size(); ++i) {
                    appendIfMatches(i);
                }
            }
        }

        return results;
    }

    for (const auto& [indexId, indexPtr] : indexByIndexId_) {
        if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
            continue;
        }

        // 2. Candidate Filtering via Trigrams
        std::vector<uint32_t> candidates;
        bool firstKeyword = true;
        bool trigramsUsed = false; // Track if we actually used the index
        bool skipDevice = false;

        for (const auto& queryKeyword : queryKeywords) {
            const std::string& kw = queryKeyword.lowercaseText;

            if (kw.length() < 3) {
                // Skip short words for now, as they won't be in our trigram index
                continue;
            }

            // Generate all trigrams for this keyword and intersect them
            for (size_t i = 0; i <= kw.length() - 3; ++i) {
                trigramsUsed = true;
                uint32_t tri =
                    (static_cast<uint32_t>(static_cast<unsigned char>(kw[i])) << 16) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(kw[i + 1])) << 8) |
                    static_cast<uint32_t>(static_cast<unsigned char>(kw[i + 2]));

                const bool foundInMainIndex = containsTrigram(indexPtr->trigramRanges, tri);
                const bool foundInLiveDeltaIndex = containsTrigram(indexPtr->liveDeltaFlatIndex, tri);

                if (!foundInMainIndex && !foundInLiveDeltaIndex) {
                    // No matches for this trigram on this device.
                    skipDevice = true;
                    break;
                }

                // 3. Intersect candidates (Candidate Filtering)
                if (firstKeyword) {
                    forEachRecordIdxForTrigram(
                        indexPtr->trigramRanges,
                        indexPtr->trigramPostings,
                        tri,
                        [&](uint32_t recordIdx) {
                            candidates.push_back(recordIdx);
                        }
                    );

                    forEachRecordIdxForTrigram(indexPtr->liveDeltaFlatIndex, tri, [&](uint32_t recordIdx) {
                        candidates.push_back(recordIdx);
                    });

                    std::sort(candidates.begin(), candidates.end());
                    candidates.erase(
                        std::unique(candidates.begin(), candidates.end()),
                        candidates.end()
                    );

                    firstKeyword = false;
                } else {
                    std::vector<uint32_t> trigramRecordIndices;

                    forEachRecordIdxForTrigram(
                        indexPtr->trigramRanges,
                        indexPtr->trigramPostings,
                        tri,
                        [&](uint32_t recordIdx) {
                            trigramRecordIndices.push_back(recordIdx);
                        }
                    );

                    forEachRecordIdxForTrigram(indexPtr->liveDeltaFlatIndex, tri, [&](uint32_t recordIdx) {
                        trigramRecordIndices.push_back(recordIdx);
                    });

                    std::sort(trigramRecordIndices.begin(), trigramRecordIndices.end());
                    trigramRecordIndices.erase(
                        std::unique(trigramRecordIndices.begin(), trigramRecordIndices.end()),
                        trigramRecordIndices.end()
                    );

                    std::vector<uint32_t> nextCandidates;
                    nextCandidates.reserve(std::min(candidates.size(), trigramRecordIndices.size()));

                    std::set_intersection(
                        candidates.begin(),
                        candidates.end(),
                        trigramRecordIndices.begin(),
                        trigramRecordIndices.end(),
                        std::back_inserter(nextCandidates)
                    );

                    candidates = std::move(nextCandidates);
                }

                if (candidates.empty()) {
                    skipDevice = true;
                    break;
                }
            }

            if (skipDevice) {
                break;
            }
        }

        if (skipDevice) {
            continue;
        }

        // 4. Refinement Phase
        // If no trigrams were used (all keywords < 3 chars), we scan everything.
        // Otherwise, we only scan the filtered candidates.
        auto resultCallback = [&](uint32_t recordIdx) {
            if (indexPtr->isDeletedRecord(recordIdx)) {
                return;
            }

            const auto& rec = indexPtr->fileRecords[recordIdx];
            const bool isDirectory = (rec.flags & FileRecord_IsDir) != 0;

            if (foldersOnly && !isDirectory) {
                return;
            }

            // Extension filters apply to files only. Reject directories before
            // touching the string pool or scanning the name for a final extension.
            if (hasExtensionFilter && isDirectory) {
                return;
            }

            if (rec.nameOffset + rec.nameLen > indexPtr->lowercaseStringPool.size()) {
                return;
            }

            const std::string_view lowercaseName(
                &indexPtr->lowercaseStringPool[rec.nameOffset],
                rec.nameLen
            );

            if (hasExtensionFilter &&
                !matchesFileExtensionFilter(rec, lowercaseName, extensionFilter)) {
                return;
            }

            const std::vector<char>& searchStringPool = options.matchCase
                    ? indexPtr->stringPool
                    : indexPtr->lowercaseStringPool;

            if (rec.nameOffset + rec.nameLen > searchStringPool.size()) {
                return;
            }

            const std::string_view name(
                &searchStringPool[rec.nameOffset],
                rec.nameLen
            );

            for (const auto& queryKeyword : queryKeywords) {
                const std::string& kw = options.matchCase
                    ? queryKeyword.text
                    : queryKeyword.lowercaseText;

                if (!matchesKeyword(name, kw, options)) {
                    return;
                }
            }

            appendResult(*indexPtr, recordIdx);
        };

        if (!trigramsUsed) {
            // Fallback: if all keywords are too short for trigrams, use the
            // extension index as the candidate source when possible.
            if (hasExtensionFilter) {
                const auto extensionCandidates =
                    collectExtensionCandidates(*indexPtr, extensionFilter);

                for (const uint32_t recordIdx : extensionCandidates) {
                    resultCallback(recordIdx);
                }
            } else {
                // Fallback: Linear scan of all records (All keywords were too short)
                for (uint32_t i = 0; i < static_cast<uint32_t>(indexPtr->fileRecords.size()); ++i) {
                    resultCallback(i);
                }
            }
        } else {
            // High-speed scan of candidates
            for (uint32_t idx : candidates) {
                resultCallback(idx);
            }
        }
    }

    return results;
}

std::vector<IndexController::RecordHandle> IndexController::sortSearchResults(
    std::vector<RecordHandle> results,
    int column,
    Qt::SortOrder sortOrder
) const {
    SortScratch scratch;
    return sortSearchResults(std::move(results), column, sortOrder, scratch);
}

std::vector<IndexController::RecordHandle> IndexController::sortSearchResults(
    std::vector<RecordHandle> results,
    int column,
    Qt::SortOrder sortOrder,
    SortScratch& scratch
) const {
    if (results.size() < 2) {
        return results;
    }

    static constexpr std::size_t ParallelSortThreshold = 500;

    auto handleLess = [](const RecordHandle& a, const RecordHandle& b) {
        if (a.indexId != b.indexId) {
            return a.indexId < b.indexId;
        }
        if (a.generation != b.generation) {
            return a.generation < b.generation;
        }
        if (a.recordIdx != b.recordIdx) {
            return a.recordIdx < b.recordIdx;
        }
        return a.mountPointIdx < b.mountPointIdx;
    };

    scratch.resultsOrder.resize(results.size());
    std::iota(scratch.resultsOrder.begin(), scratch.resultsOrder.end(), uint32_t{0});

    auto& resultsOrder = scratch.resultsOrder;

    auto sortOrderIndices = [&](auto&& comparator) {
        if (resultsOrder.size() >= ParallelSortThreshold) {
            std::sort(
                std::execution::par,
                resultsOrder.begin(),
                resultsOrder.end(),
                std::forward<decltype(comparator)>(comparator)
            );
        } else {
            std::sort(
                resultsOrder.begin(),
                resultsOrder.end(),
                std::forward<decltype(comparator)>(comparator)
            );
        }
    };

    if (column == SearchResultColumn::Name) {
        struct NameKey {
            std::string_view nameKey;
            bool valid = false;
        };

        std::vector<NameKey> keys(results.size());

        {
            std::shared_lock lock(indexMutex_);

            for (size_t i = 0; i < results.size(); ++i) {
                const auto& handle = results[i];
                auto& key = keys[i];

                const auto it = indexByIndexId_.find(handle.indexId);
                if (it == indexByIndexId_.end()) {
                    continue;
                }

                const auto* device = it->second.get();
                if (!device || !device->isReady || device->generation != handle.generation
                    || handle.recordIdx >= device->fileRecords.size()
                    || device->isDeletedRecord(handle.recordIdx)) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                if (record.nameOffset + record.nameLen <= device->lowercaseStringPool.size()) {
                    key.nameKey = std::string_view(
                        &device->lowercaseStringPool[record.nameOffset],
                        record.nameLen
                    );
                    key.valid = true;
                }
            }
        }

        auto lessByIndex = [&](uint32_t lhs, uint32_t rhs) {
            const auto& a = keys[lhs];
            const auto& b = keys[rhs];

            if (!a.valid || !b.valid) {
                return handleLess(results[lhs], results[rhs]);
            }

            if (a.nameKey != b.nameKey) {
                return a.nameKey < b.nameKey;
            }

            return handleLess(results[lhs], results[rhs]);
        };

        if (sortOrder == Qt::AscendingOrder) {
            sortOrderIndices(lessByIndex);
        } else {
            sortOrderIndices([&](uint32_t lhs, uint32_t rhs) {
                return lessByIndex(rhs, lhs);
            });
        }
    } else if (column == SearchResultColumn::Path) {
        struct PathKey {
            std::string pathKey;
            bool valid = false;
        };

        std::vector<PathKey> keys(results.size());

        {
            std::shared_lock lock(indexMutex_);

            for (size_t i = 0; i < results.size(); ++i) {
                const auto& handle = results[i];
                auto& key = keys[i];

                const auto it = indexByIndexId_.find(handle.indexId);
                if (it == indexByIndexId_.end()) {
                    continue;
                }

                const auto* device = it->second.get();
                if (!device || !device->isReady || device->generation != handle.generation
                    || handle.recordIdx >= device->fileRecords.size()
                    || device->isDeletedRecord(handle.recordIdx)) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                key.pathKey = device->getFullPath(record.parentRecordIdx);

                if (handle.mountPointIdx != 0xFFFFFFFF &&
                    handle.mountPointIdx < static_cast<uint32_t>(device->mountPoints.size())) {
                    const QString mountPoint = device->mountPoints.at(static_cast<int>(handle.mountPointIdx));
                    key.pathKey = (mountPoint + QStringLiteral("/") + QString::fromStdString(key.pathKey))
                        .toStdString();
                }

                key.valid = true;
            }
        }

        auto lessByIndex = [&](uint32_t lhs, uint32_t rhs) {
            const auto& a = keys[lhs];
            const auto& b = keys[rhs];

            if (!a.valid || !b.valid) {
                return handleLess(results[lhs], results[rhs]);
            }

            if (a.pathKey != b.pathKey) {
                return a.pathKey < b.pathKey;
            }

            return handleLess(results[lhs], results[rhs]);
        };

        if (sortOrder == Qt::AscendingOrder) {
            sortOrderIndices(lessByIndex);
        } else {
            sortOrderIndices([&](uint32_t lhs, uint32_t rhs) {
                return lessByIndex(rhs, lhs);
            });
        }
    } else if (column == SearchResultColumn::Size ||
               column == SearchResultColumn::DateModified) {
        scratch.numericKeys.clear();
        scratch.numericKeys.resize(results.size());

        auto& numericKeys = scratch.numericKeys;

        {
            std::shared_lock lock(indexMutex_);

            for (size_t i = 0; i < results.size(); ++i) {
                const auto& handle = results[i];

                const auto it = indexByIndexId_.find(handle.indexId);
                if (it == indexByIndexId_.end()) {
                    continue;
                }

                const auto* device = it->second.get();
                if (!device || !device->isReady || device->generation != handle.generation
                    || handle.recordIdx >= device->fileRecords.size()
                    || device->isDeletedRecord(handle.recordIdx)) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                numericKeys[i] = column == SearchResultColumn::Size
                    ? record.size
                    : record.modificationTime;
            }
        }

        auto lessByIndex = [&](uint32_t lhs, uint32_t rhs) {
            const auto aKey = numericKeys[lhs];
            const auto bKey = numericKeys[rhs];

            if (aKey != bKey) {
                return sortOrder == Qt::AscendingOrder
                    ? aKey < bKey
                    : aKey > bKey;
            }

            return handleLess(results[lhs], results[rhs]);
        };

        sortOrderIndices(lessByIndex);
    } else {
        return results;
    }

    scratch.sortedResults.resize(results.size());

    for (size_t i = 0; i < resultsOrder.size(); ++i) {
        scratch.sortedResults[i] = std::move(results[resultsOrder[i]]);
    }

    results.swap(scratch.sortedResults);
    return results;
}

void IndexController::resolveParentPointersByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: resolveParentPointersByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: resolveParentPointersByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.resolveParentPointers();
}

void IndexController::buildLowercaseStringPoolByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: buildLowercaseStringPoolByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: buildLowercaseStringPoolByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildLowercaseStringPool();
}

void IndexController::sortByNameAscendingParallelByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: sortByNameAscendingParallelByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: sortByNameAscendingParallelByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.sortByNameAscendingParallel();
}

void IndexController::buildTrigramIndexParallelByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: buildTrigramIndexParallelByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: buildTrigramIndexParallelByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildTrigramIndexParallel();
}

void IndexController::buildExtensionIndexByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: buildExtensionIndexByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: buildExtensionIndexByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildExtensionIndex();
}