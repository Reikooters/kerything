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

    void appendUnsignedVarint(uint32_t value, std::vector<uint8_t>& out)
    {
        while (value >= 0x80) {
            out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
            value >>= 7;
        }

        out.push_back(static_cast<uint8_t>(value));
    }

    bool readUnsignedVarint(
        const std::vector<uint8_t>& bytes,
        std::size_t& offset,
        std::size_t end,
        uint32_t& value
    ) {
        value = 0;
        uint32_t shift = 0;

        while (offset < end && shift <= 28) {
            const uint8_t byte = bytes[offset++];
            value |= static_cast<uint32_t>(byte & 0x7F) << shift;

            if ((byte & 0x80) == 0) {
                return true;
            }

            shift += 7;
        }

        return false;
    }

    void appendCompressedPosting(
        std::vector<uint8_t>& postings,
        uint32_t recordIdx,
        uint32_t& previousRecordIdx,
        bool& firstPosting
    ) {
        const uint32_t encodedValue = firstPosting
            ? recordIdx
            : recordIdx - previousRecordIdx;

        appendUnsignedVarint(encodedValue, postings);
        previousRecordIdx = recordIdx;
        firstPosting = false;
    }

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
        const std::vector<uint8_t>& postings,
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

        std::size_t readOffset = it->offset;

        if (readOffset > postings.size()) {
            return;
        }

        uint32_t previousRecordIdx = 0;

        for (uint32_t decodedCount = 0; decodedCount < it->count; ++decodedCount) {
            uint32_t encodedValue = 0;

            if (!readUnsignedVarint(
                    postings,
                    readOffset,
                    postings.size(),
                    encodedValue
                )) {
                return;
                }

            const uint32_t recordIdx = decodedCount == 0
                ? encodedValue
                : previousRecordIdx + encodedValue;

            previousRecordIdx = recordIdx;
            fn(recordIdx);
        }
    }

    struct QueryKeyword {
        std::string text;
        std::string lowercaseText;
    };

    struct QueryTrigram {
        uint32_t trigram = 0;
        std::size_t postingCount = 0;
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

    std::size_t trigramPostingCount(
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

        return static_cast<std::size_t>(std::distance(range.first, range.second));
    }

    std::size_t trigramPostingCount(
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

        if (it == ranges.end() || it->trigram != trigram) {
            return 0;
        }

        return it->count;
    }

    std::size_t totalDecodedTrigramPostingCount(
        const std::vector<IndexController::TrigramRange>& ranges
    ) {
        std::size_t total = 0;

        for (const IndexController::TrigramRange& range : ranges) {
            total += range.count;
        }

        return total;
    }

    void buildCompactTrigramIndexFromSortedEntries(
        const std::vector<IndexController::TrigramEntry>& sortedEntries,
        std::vector<IndexController::TrigramRange>& ranges,
        std::vector<uint8_t>& postings)
    {
        ranges.clear();
        postings.clear();

        if (sortedEntries.empty()) {
            ranges.shrink_to_fit();
            postings.shrink_to_fit();
            return;
        }

        // Varint-delta encoded postings are usually much smaller than the raw
        // uint32_t posting stream. Reserving the raw byte size prevents repeated
        // reallocations during the first build while still allowing shrink_to_fit()
        // to release the unused tail below.
        postings.reserve(sortedEntries.size() * sizeof(uint32_t));

        std::size_t i = 0;

        while (i < sortedEntries.size()) {
            const uint32_t trigram = sortedEntries[i].trigram;
            const uint32_t offset = static_cast<uint32_t>(postings.size());

            uint32_t count = 0;
            uint32_t previousRecordIdx = 0;
            bool firstPosting = true;

            do {
                appendCompressedPosting(
                    postings,
                    sortedEntries[i].recordIdx,
                    previousRecordIdx,
                    firstPosting
                );

                ++count;
                ++i;
            } while (i < sortedEntries.size() && sortedEntries[i].trigram == trigram);

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

    QByteArray namespacedLiveEntryKey(
        quint64 parentFsNamespace,
        quint64 parentInode,
        std::string_view name)
    {
        QByteArray key = QByteArray::number(static_cast<qulonglong>(parentFsNamespace));
        key.append('\0');
        key.append(QByteArray::number(static_cast<qulonglong>(parentInode)));
        key.append('\0');
        key.append(name.data(), static_cast<qsizetype>(name.size()));
        return key;
    }

    QByteArray namespacedLiveEntryKey(
        quint64 parentFsNamespace,
        quint64 parentInode,
        const QByteArray& name)
    {
        QByteArray key = QByteArray::number(static_cast<qulonglong>(parentFsNamespace));
        key.append('\0');
        key.append(QByteArray::number(static_cast<qulonglong>(parentInode)));
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

    struct LowercasePoolOpportunity {
        std::size_t validRecords = 0;
        std::size_t invalidStringRefs = 0;

        std::size_t recordsWithAsciiUppercase = 0;
        std::size_t recordsWithoutAsciiUppercase = 0;

        std::size_t nameBytesWithAsciiUppercase = 0;
        std::size_t nameBytesWithoutAsciiUppercase = 0;
        std::size_t asciiUppercaseByteCount = 0;

        [[nodiscard]] std::size_t sparseLowercasePoolBytes() const noexcept
        {
            return nameBytesWithAsciiUppercase;
        }

        [[nodiscard]] std::size_t totalNameBytes() const noexcept
        {
            return nameBytesWithAsciiUppercase + nameBytesWithoutAsciiUppercase;
        }
    };

    struct TrigramDistributionStats {
        std::size_t rangeCount = 0;
        std::size_t postingCount = 0;

        uint32_t minPostingListSize = 0;
        uint32_t p50PostingListSize = 0;
        uint32_t p90PostingListSize = 0;
        uint32_t p95PostingListSize = 0;
        uint32_t p99PostingListSize = 0;
        uint32_t maxPostingListSize = 0;

        std::size_t top10PostingEntries = 0;
        std::size_t top100PostingEntries = 0;
        std::size_t postingListsWithOneEntry = 0;
        std::size_t postingListsWithAtLeast1024Entries = 0;
        std::size_t postingListsWithAtLeast65536Entries = 0;

        quint64 rawUint32PostingBytes = 0;
        quint64 compressedPostingBytes = 0;
        quint64 compressedPostingSavingBytes = 0;
    };

    struct CompressedTrigramValidationStats {
        std::size_t rangesChecked = 0;
        std::size_t invalidOffsets = 0;
        std::size_t decodeFailures = 0;
        std::size_t nonMonotonicLists = 0;
        std::size_t outOfRangeRecordRefs = 0;
        std::size_t trailingBytesInRanges = 0;
    };

    CompressedTrigramValidationStats validateCompressedTrigramIndex(
        const std::vector<IndexController::TrigramRange>& ranges,
        const std::vector<uint8_t>& postings,
        std::size_t fileRecordCount)
    {
        CompressedTrigramValidationStats stats;
        stats.rangesChecked = ranges.size();

        for (std::size_t rangeIdx = 0; rangeIdx < ranges.size(); ++rangeIdx) {
            const IndexController::TrigramRange& range = ranges[rangeIdx];

            if (range.offset > postings.size()) {
                ++stats.invalidOffsets;
                ++stats.decodeFailures;
                continue;
            }

            const std::size_t rangeEnd =
                rangeIdx + 1 < ranges.size()
                    ? static_cast<std::size_t>(ranges[rangeIdx + 1].offset)
                    : postings.size();

            if (rangeEnd < range.offset || rangeEnd > postings.size()) {
                ++stats.invalidOffsets;
                ++stats.decodeFailures;
                continue;
            }

            std::size_t readOffset = range.offset;
            uint32_t previousRecordIdx = 0;

            for (uint32_t decodedCount = 0; decodedCount < range.count; ++decodedCount) {
                uint32_t encodedValue = 0;

                if (!readUnsignedVarint(
                        postings,
                        readOffset,
                        rangeEnd,
                        encodedValue
                    )) {
                    ++stats.decodeFailures;
                    break;
                }

                const uint32_t recordIdx = decodedCount == 0
                    ? encodedValue
                    : previousRecordIdx + encodedValue;

                if (decodedCount != 0 && recordIdx < previousRecordIdx) {
                    ++stats.nonMonotonicLists;
                }

                if (recordIdx >= fileRecordCount) {
                    ++stats.outOfRangeRecordRefs;
                }

                previousRecordIdx = recordIdx;
            }

            if (readOffset != rangeEnd) {
                ++stats.trailingBytesInRanges;
            }
        }

        return stats;
    }

    TrigramDistributionStats calculateTrigramDistributionStats(
        const std::vector<IndexController::TrigramRange>& ranges,
        const std::vector<uint8_t>& postings)
    {
        TrigramDistributionStats stats;
        stats.rangeCount = ranges.size();
        stats.compressedPostingBytes = postings.capacity();

        if (ranges.empty()) {
            return stats;
        }

        std::vector<uint32_t> counts;
        counts.reserve(ranges.size());

        for (const IndexController::TrigramRange& range : ranges) {
            counts.push_back(range.count);
            stats.postingCount += range.count;

            if (range.count == 1) {
                ++stats.postingListsWithOneEntry;
            }

            if (range.count >= 1024) {
                ++stats.postingListsWithAtLeast1024Entries;
            }

            if (range.count >= 65536) {
                ++stats.postingListsWithAtLeast65536Entries;
            }
        }

        std::sort(counts.begin(), counts.end());

        auto percentile = [&](double ratio) -> uint32_t {
            if (counts.empty()) {
                return 0;
            }

            const std::size_t index = std::min<std::size_t>(
                counts.size() - 1,
                static_cast<std::size_t>(
                    static_cast<double>(counts.size() - 1) * ratio
                )
            );

            return counts[index];
        };

        stats.minPostingListSize = counts.front();
        stats.p50PostingListSize = percentile(0.50);
        stats.p90PostingListSize = percentile(0.90);
        stats.p95PostingListSize = percentile(0.95);
        stats.p99PostingListSize = percentile(0.99);
        stats.maxPostingListSize = counts.back();

        for (std::size_t i = 0; i < counts.size() && i < 10; ++i) {
            stats.top10PostingEntries += counts[counts.size() - 1 - i];
        }

        for (std::size_t i = 0; i < counts.size() && i < 100; ++i) {
            stats.top100PostingEntries += counts[counts.size() - 1 - i];
        }

        stats.rawUint32PostingBytes =
            static_cast<quint64>(stats.postingCount) * sizeof(uint32_t);

        stats.compressedPostingSavingBytes =
            stats.rawUint32PostingBytes > stats.compressedPostingBytes
                ? stats.rawUint32PostingBytes - stats.compressedPostingBytes
                : 0;

        return stats;
    }

    struct FsIndexKeyWidthStats {
        std::size_t liveRecordsChecked = 0;
        std::size_t deletedRecordsSkipped = 0;
        std::size_t recordsRequiringUInt64Refs = 0;

        uint64_t maxFsIndex = 0;
        uint64_t maxParentFsIndex = 0;

        bool allLiveRefsFitUInt32 = true;
        bool selectedStorageMatchesLiveRecords = true;
    };

    FsIndexKeyWidthStats calculateFsIndexKeyWidthStats(
        const IndexController::DeviceIndex& device)
    {
        FsIndexKeyWidthStats stats;

        for (uint32_t recordIdx = 0;
             recordIdx < static_cast<uint32_t>(device.fileRecords.size());
             ++recordIdx) {
            if (device.isDeletedRecord(recordIdx)) {
                ++stats.deletedRecordsSkipped;
                continue;
            }

            ++stats.liveRecordsChecked;

            const FileRecord& record = device.fileRecords[recordIdx];

            stats.maxFsIndex = std::max<uint64_t>(
                stats.maxFsIndex,
                record.fsIndex
            );

            stats.maxParentFsIndex = std::max<uint64_t>(
                stats.maxParentFsIndex,
                record.parentFsIndex
            );

            if (!IndexController::DeviceIndex::fsIndexFitsUInt32(record.fsIndex) ||
                !IndexController::DeviceIndex::fsIndexFitsUInt32(record.parentFsIndex)) {
                ++stats.recordsRequiringUInt64Refs;
                }
             }

        stats.allLiveRefsFitUInt32 =
            stats.recordsRequiringUInt64Refs == 0;

        stats.selectedStorageMatchesLiveRecords =
            device.fsIndexRefStorage == IndexController::DeviceIndex::FsIndexRefStorage::UInt64 ||
            stats.allLiveRefsFitUInt32;

        return stats;
    }

    bool containsAsciiUppercase(std::string_view text, std::size_t* uppercaseByteCount = nullptr)
    {
        bool found = false;

        for (const unsigned char c : text) {
            if (c >= 'A' && c <= 'Z') {
                found = true;

                if (uppercaseByteCount) {
                    ++(*uppercaseByteCount);
                }
            }
        }

        return found;
    }

    LowercasePoolOpportunity calculateLowercasePoolOpportunity(
        const IndexController::DeviceIndex& device)
    {
        LowercasePoolOpportunity opportunity;

        for (const FileRecord& record : device.fileRecords) {
            if (record.nameOffset + record.nameLen > device.stringPool.size()) {
                ++opportunity.invalidStringRefs;
                continue;
            }

            ++opportunity.validRecords;

            const std::string_view name(
                &device.stringPool[record.nameOffset],
                record.nameLen
            );

            std::size_t uppercaseByteCount = 0;
            const bool hasAsciiUppercase =
                containsAsciiUppercase(name, &uppercaseByteCount);

            opportunity.asciiUppercaseByteCount += uppercaseByteCount;

            if (hasAsciiUppercase) {
                ++opportunity.recordsWithAsciiUppercase;
                opportunity.nameBytesWithAsciiUppercase += record.nameLen;
            } else {
                ++opportunity.recordsWithoutAsciiUppercase;
                opportunity.nameBytesWithoutAsciiUppercase += record.nameLen;
            }
        }

        return opportunity;
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
    const std::vector<BlockDeviceMountInfo>& mounts,
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
            deviceIndex.mounts = mounts;
            deviceIndex.mounted = !mountPoints.isEmpty();
            deviceIndex.isReady = false;
            deviceIndex.fileRecords.clear();
            deviceIndex.fileRecordNamespaces.clear();
            deviceIndex.stringPool.clear();
            deviceIndex.deletedRecordBits.clear();
            deviceIndex.lowercaseStringPool.clear();
            deviceIndex.lowercaseNameOffsetByRecord.clear();
            deviceIndex.trigramRanges.clear();
            deviceIndex.trigramPostings.clear();
            deviceIndex.liveDeltaFlatIndex.clear();
            deviceIndex.recordsByExtension.clear();
            deviceIndex.extensionIndexEntryCount = 0;
            deviceIndex.extensionIndexLiveDeltaEntries = 0;
            deviceIndex.fsIndexRefStorage = DeviceIndex::FsIndexRefStorage::UInt64;
            deviceIndex.directoryFsIndexRecordRefs32.clear();
            deviceIndex.directoryFsIndexRecordRefs64.clear();
            deviceIndex.liveDirectoryFsIndexRecordRefs32.clear();
            deviceIndex.liveDirectoryFsIndexRecordRefs64.clear();
            deviceIndex.fsIndexRecordRefs32.clear();
            deviceIndex.fsIndexRecordRefs64.clear();
            deviceIndex.liveFsIndexRecordRefs32.clear();
            deviceIndex.liveFsIndexRecordRefs64.clear();
            deviceIndex.namespacedDirectoryFsIndexRecordRefs.clear();
            deviceIndex.namespacedFsIndexRecordRefs.clear();
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
    deviceIndex->mounts = mounts;
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
    deviceIndex.fileRecords.reserve(fileRecordsCountBefore + records.size());

    // Insert the new records into the device index.
    // Parent pointers are resolved once after the full scan has completed.
    deviceIndex.fileRecords.insert(deviceIndex.fileRecords.end(), records.begin(), records.end());
    deviceIndex.resizeDeletedRecordBitsForRecordCount(deviceIndex.fileRecords.size());

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: The index now contains " << deviceIndex.fileRecords.size()
              << " file records for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif
}

void IndexController::appendDeviceFileRecordNamespacesByRequestId(
    const quint32 requestId,
    const std::vector<FileRecordNamespace>& namespaces)
{
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecordNamespaces: No device index for requestId="
                  << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecordNamespaces: No device index for indexId="
                  << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

    deviceIndex.fileRecordNamespaces.reserve(
        deviceIndex.fileRecordNamespaces.size() + namespaces.size()
    );

    deviceIndex.fileRecordNamespaces.insert(
        deviceIndex.fileRecordNamespaces.end(),
        namespaces.begin(),
        namespaces.end()
    );

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Appended " << namespaces.size()
              << " file record namespace entries to device "
              << deviceIndex.devNode.toStdString()
              << " namespace entries now="
              << deviceIndex.fileRecordNamespaces.size()
              << " fileRecords="
              << deviceIndex.fileRecords.size()
              << "\n";
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

bool IndexController::validateScanSidecarsByRequestId(quint32 requestId, QString* errorText)
{
    std::shared_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        if (errorText) {
            *errorText = QStringLiteral("No device index for requestId=%1").arg(requestId);
        }

        return false;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end() || !existingDeviceIndexIt->second) {
        if (errorText) {
            *errorText = QStringLiteral("No device index for indexId=%1 requestId=%2")
                .arg(existingIndexId)
                .arg(requestId);
        }

        return false;
    }

    const DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

    const bool namespaceSidecarRequired =
        deviceIndex.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0;

    if (namespaceSidecarRequired && deviceIndex.fileRecordNamespaces.empty()) {
        if (errorText) {
            *errorText = QStringLiteral(
                "FileRecordNamespace sidecar is required for Btrfs scans"
            );
        }

        return false;
    }

    if (!deviceIndex.fileRecordNamespaces.empty() &&
        deviceIndex.fileRecordNamespaces.size() != deviceIndex.fileRecords.size()) {
        if (errorText) {
            *errorText = QStringLiteral(
                "FileRecordNamespace sidecar count mismatch: namespaces=%1 records=%2"
            )
                .arg(deviceIndex.fileRecordNamespaces.size())
                .arg(deviceIndex.fileRecords.size());
        }

        return false;
    }

    return true;
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
    const QString& primaryMountPoint,
    const std::vector<BlockDeviceMountInfo>& mounts
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
            deviceIndex->mounts = mounts;
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

    const bool useNamespaces = targetIndex->hasFileRecordNamespaces();

#ifdef KERYTHING_ENABLE_LOGGING
    if (targetIndex->fsType.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0 &&
        !useNamespaces) {
        std::cerr << "live batch #" << liveBatchDebugId
                  << " warning: Btrfs live update batch is not using namespace-aware matching"
                  << " fileRecords=" << targetIndex->fileRecords.size()
                  << " fileRecordNamespaces=" << targetIndex->fileRecordNamespaces.size()
                  << "\n";
    }
#endif

    QSet<quint64> deleteParentInodes;
    QSet<QByteArray> deleteParentNamespaceKeys;
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

            if (useNamespaces) {
                deleteParentNamespaceKeys.insert(
                    QByteArray::number(static_cast<qulonglong>(operation.parentFsNamespace)) +
                    QByteArrayLiteral("\0") +
                    QByteArray::number(static_cast<qulonglong>(operation.parentInode))
                );
            } else {
                deleteParentInodes.insert(operation.parentInode);
            }

            ++deleteEntryOperationCount;
        }
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "live batch #" << liveBatchDebugId
              << " delete parents=" << deleteParentInodes.size()
              << " namespacedDeleteParents=" << deleteParentNamespaceKeys.size()
              << " deleteEntryOperationCount=" << deleteEntryOperationCount
              << "\n";
#endif

    QHash<QByteArray, uint32_t> liveEntryRecordByParentAndName;

    if (!deleteParentInodes.isEmpty() || !deleteParentNamespaceKeys.isEmpty()) {
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

            if (useNamespaces) {
                const FileRecordNamespace namespaceEntry = targetIndex->namespaceForRecord(recordIdx);

                const QByteArray parentNamespaceKey =
                    QByteArray::number(static_cast<qulonglong>(namespaceEntry.parentFsNamespace)) +
                    QByteArrayLiteral("\0") +
                    QByteArray::number(static_cast<qulonglong>(record.parentFsIndex));

                if (!deleteParentNamespaceKeys.contains(parentNamespaceKey)) {
                    continue;
                }

                ++parentMatchedRecords;

                const std::string_view name = targetIndex->recordName(recordIdx);
                if (name.empty()) {
                    continue;
                }

                liveEntryRecordByParentAndName.insert(
                    namespacedLiveEntryKey(
                        namespaceEntry.parentFsNamespace,
                        record.parentFsIndex,
                        name
                    ),
                    recordIdx
                );

                ++insertedRecords;
                continue;
            }

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
        std::size_t lowercaseStringBytesToAppend = 0;
        std::size_t estimatedTrigramsToAppend = 0;
        std::size_t estimatedDirectoriesToAppend = 0;

        for (const LiveUpdateOperation& operation : pendingUpserts) {
            const QByteArray nameUtf8 = operation.name.toUtf8();
            const std::size_t nameSize = static_cast<std::size_t>(nameUtf8.size());

            stringBytesToAppend += nameSize;

            if (operation.isDirectory) {
                ++estimatedDirectoriesToAppend;
            }

            bool hasAsciiUppercase = false;

            for (const unsigned char c : nameUtf8) {
                if (c >= 'A' && c <= 'Z') {
                    hasAsciiUppercase = true;
                    break;
                }
            }

            if (hasAsciiUppercase) {
                lowercaseStringBytesToAppend += nameSize;
            }

            if (nameSize >= 3) {
                estimatedTrigramsToAppend += nameSize - 2;
            }
        }

        targetIndex->fileRecords.reserve(
            targetIndex->fileRecords.size() + pendingUpserts.size()
        );

        targetIndex->reserveDeletedRecordBitsForRecordCount(
            targetIndex->fileRecords.size() + pendingUpserts.size()
        );

        targetIndex->stringPool.reserve(
            targetIndex->stringPool.size() + stringBytesToAppend
        );

        targetIndex->lowercaseStringPool.reserve(
            targetIndex->lowercaseStringPool.size() + lowercaseStringBytesToAppend
        );

        targetIndex->lowercaseNameOffsetByRecord.reserve(
            targetIndex->lowercaseNameOffsetByRecord.size() + pendingUpserts.size()
        );

        targetIndex->liveDeltaFlatIndex.reserve(
            targetIndex->liveDeltaFlatIndex.size() + estimatedTrigramsToAppend
        );

        if (targetIndex->fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32) {
            targetIndex->liveFsIndexRecordRefs32.reserve(
                targetIndex->liveFsIndexRecordRefs32.size() + pendingUpserts.size()
            );

            targetIndex->liveDirectoryFsIndexRecordRefs32.reserve(
                targetIndex->liveDirectoryFsIndexRecordRefs32.size() + estimatedDirectoriesToAppend
            );
        } else {
            targetIndex->liveFsIndexRecordRefs64.reserve(
                targetIndex->liveFsIndexRecordRefs64.size() + pendingUpserts.size()
            );

            targetIndex->liveDirectoryFsIndexRecordRefs64.reserve(
                targetIndex->liveDirectoryFsIndexRecordRefs64.size() + estimatedDirectoriesToAppend
            );
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "live batch #" << liveBatchDebugId
                  << " reserved upsert storage"
                  << " pendingUpserts=" << pendingUpserts.size()
                  << " stringBytesToAppend=" << stringBytesToAppend
                  << " lowercaseStringBytesToAppend=" << lowercaseStringBytesToAppend
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

                bool updatedAny = false;

                if (useNamespaces) {
                    const std::optional<uint32_t> recordIdx =
                        targetIndex->recordIdxForNamespacedFsIndex(
                            operation.fsNamespace,
                            operation.inode
                        );

                    if (!recordIdx) {
                        ++result.missingInode;
                        continue;
                    }

                    if (*recordIdx < targetIndex->fileRecords.size() &&
                        !targetIndex->isDeletedRecord(*recordIdx)) {
                        FileRecord& record = targetIndex->fileRecords[*recordIdx];
                        const bool wasDirectory = (record.flags & FileRecord_IsDir) != 0;

                        updateFileRecordMetadataFromLiveUpdateOperation(record, operation);

                        const bool isDirectory = (record.flags & FileRecord_IsDir) != 0;
                        if (wasDirectory && !isDirectory) {
                            addRecordToExtensionIndexIfApplicable(*targetIndex, *recordIdx);
                        }

                        updatedAny = true;
                    }
                } else {
                    const std::vector<uint32_t>* recordIndices =
                        targetIndex->recordIndicesForFsIndex(operation.inode);

                    if (!recordIndices || recordIndices->empty()) {
                        ++result.missingInode;
                        continue;
                    }

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

                    const auto recordIt = useNamespaces
                        ? liveEntryRecordByParentAndName.constFind(
                            namespacedLiveEntryKey(
                                operation.parentFsNamespace,
                                operation.parentInode,
                                nameUtf8
                            )
                        )
                        : liveEntryRecordByParentAndName.constFind(
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
                    const FileRecordNamespace namespaceEntry = targetIndex->namespaceForRecord(recordIdx);

                    std::size_t matchingUpsertIdx = pendingUpserts.size();

                    const auto pendingUpsertIndicesIt =
                        pendingUpsertIndicesByInode.constFind(record.fsIndex);

                    if (pendingUpsertIndicesIt != pendingUpsertIndicesByInode.cend()) {
                        for (const std::size_t upsertIdx : pendingUpsertIndicesIt.value()) {
                            if (consumedUpserts[upsertIdx] != 0) {
                                continue;
                            }

                            const LiveUpdateOperation& pendingUpsert = pendingUpserts[upsertIdx];

                            if (useNamespaces &&
                                pendingUpsert.fsNamespace != namespaceEntry.fsNamespace) {
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

                            if (useNamespaces) {
                                fsIndexMapsNeedRebuild = true;
                            }

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

                    case UpsertApplyResult::AppliedNeedsFsIndexRebuild:
                        ++result.upserted;
                        ++passApplied;
                        madeProgress = true;
                        trigramIndexNeedsSort = true;
                        fsIndexMapsNeedRebuild = true;
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

    if (fsIndexMapsNeedRebuild) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 rebuild fs index maps").arg(liveBatchDebugId),
            10
        );

        // Rebuild inode maps so deleted/replaced records no longer participate
        // in metadata updates, parent lookup, or future live-update matching.
        targetIndex->rebuildFsIndexMaps();
    }

    if (trigramIndexNeedsSort) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 sort live update trigram index").arg(liveBatchDebugId),
            10
        );

        sortLiveUpdateTrigramIndex(*targetIndex);
    }

    if (targetIndex->fsIndexLiveRefCount() > 0) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 sort live fs-index refs").arg(liveBatchDebugId),
            10
        );

        sortLiveFsIndexRecordRefs(*targetIndex);
    }

    if (targetIndex->directoryFsIndexLiveRefCount() > 0) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 sort live directory fs-index refs").arg(liveBatchDebugId),
            10
        );

        sortLiveDirectoryFsIndexRecordRefs(*targetIndex);
    }

    if (shouldRebuildFsIndexAfterLiveUpdates(*targetIndex)) {
        PhaseTimer timer(
            QStringLiteral("live batch #%1 rebuild fs-index refs").arg(liveBatchDebugId),
            10
        );

        rebuildFsIndexAfterLiveUpdates(*targetIndex);
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

    if (targetIndex->fsType.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0 &&
        !targetIndex->hasFileRecordNamespaces()) {
        ++result.needsRescan;

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "live batch #" << liveBatchDebugId
                  << " error: Btrfs namespace sidecar invariant broken"
                  << " fileRecords=" << targetIndex->fileRecords.size()
                  << " fileRecordNamespaces=" << targetIndex->fileRecordNamespaces.size()
                  << "\n";
#endif
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
    bool shouldTrimAllocator = false;

    {
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

        const bool becameReady = isReady && !deviceIndex.isReady;
        deviceIndex.isReady = isReady;

        if (becameReady) {
            deviceIndex.compactDeletedRecordBits();
            deviceIndex.fileRecordNamespaces.shrink_to_fit();
            deviceIndex.namespacedDirectoryFsIndexRecordRefs.shrink_to_fit();
            deviceIndex.namespacedFsIndexRecordRefs.shrink_to_fit();
            shouldTrimAllocator = true;
        }
    }

#if defined(__GLIBC__)
    if (shouldTrimAllocator) {
        ::malloc_trim(0);
    }
#endif
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
    out << "  sizeof(FileRecordNamespace): " << sizeof(FileRecordNamespace) << " bytes\n";
    out << "  sizeof(TrigramEntry): " << sizeof(TrigramEntry) << " bytes\n";
    out << "  sizeof(TrigramRange): " << sizeof(TrigramRange) << " bytes\n";
    out << "  sizeof(RecordHandle): " << sizeof(RecordHandle) << " bytes\n";
    out << "  sizeof(std::vector<uint32_t>): " << sizeof(std::vector<uint32_t>) << " bytes\n";
    out << "  sizeof(FsIndexRecordRef32): "
        << sizeof(DeviceIndex::FsIndexRecordRef32)
        << " bytes\n";
    out << "  sizeof(FsIndexRecordRef64): "
        << sizeof(DeviceIndex::FsIndexRecordRef64)
        << " bytes\n";
    out << "  sizeof(NamespacedFsIndexRecordRef): "
        << sizeof(DeviceIndex::NamespacedFsIndexRecordRef)
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
    std::size_t grandFileRecordNamespaceEntries = 0;
    std::size_t grandNamespacedDirectoryRefs = 0;
    std::size_t grandNamespacedFsIndexRefs = 0;
    std::size_t grandIndexesWithNamespaceSidecars = 0;
    std::size_t grandIndexesWithInvalidNamespaceSidecars = 0;
    std::size_t grandFlatTrigrams = 0;
    std::size_t grandLiveDeltaTrigrams = 0;
    std::size_t grandStringBytes = 0;
    std::size_t grandLowercaseStringBytes = 0;
    std::size_t grandLowercaseNameOffsetEntries = 0;
    std::size_t grandFsIndexStoredRecordRefs = 0;
    std::size_t grandFsIndexFullRecordRefs = 0;
    std::size_t grandFsIndexLiveRecordRefs = 0;
    std::size_t grandFsIndexRecordsOmittedBecauseDeleted = 0;
    std::size_t grandFsIndexLiveRecordsChecked = 0;
    std::size_t grandFsIndexDeletedRecordsSkippedForWidth = 0;
    std::size_t grandFsIndexRecordsRequiringUInt64Refs = 0;
    uint64_t grandMaxFsIndex = 0;
    uint64_t grandMaxParentFsIndex = 0;
    bool grandFsIndexSelectedStorageMatchesLiveRecords = true;
    std::size_t grandExtensionStoredRecordRefs = 0;
    std::size_t maxSearchResultsFromReadySearchableDevices = 0;

    quint64 grandTrigramPostingsCapacityBytes = 0;
    quint64 grandRawUint32TrigramPostingBytes = 0;

    std::size_t grandCompressedTrigramRangesChecked = 0;
    std::size_t grandCompressedTrigramInvalidOffsets = 0;
    std::size_t grandCompressedTrigramDecodeFailures = 0;
    std::size_t grandCompressedTrigramNonMonotonicLists = 0;
    std::size_t grandCompressedTrigramOutOfRangeRecordRefs = 0;
    std::size_t grandCompressedTrigramTrailingBytesInRanges = 0;

    std::size_t grandLowercaseValidRecords = 0;
    std::size_t grandLowercaseInvalidStringRefs = 0;
    std::size_t grandRecordsWithAsciiUppercase = 0;
    std::size_t grandRecordsWithoutAsciiUppercase = 0;
    std::size_t grandNameBytesWithAsciiUppercase = 0;
    std::size_t grandNameBytesWithoutAsciiUppercase = 0;
    std::size_t grandAsciiUppercaseByteCount = 0;

    for (const auto& [indexId, deviceIndexPtr] : indexByIndexId_) {
        if (!deviceIndexPtr) {
            continue;
        }

        const DeviceIndex& device = *deviceIndexPtr;

        const quint64 fileRecordsBytes = vectorCapacityBytes(device.fileRecords);
        const quint64 fileRecordNamespacesBytes = vectorCapacityBytes(device.fileRecordNamespaces);
        const quint64 stringPoolBytes = vectorCapacityBytes(device.stringPool);
        const quint64 lowercaseStringPoolBytes = vectorCapacityBytes(device.lowercaseStringPool);
        const quint64 lowercaseNameOffsetByRecordBytes =
            vectorCapacityBytes(device.lowercaseNameOffsetByRecord);
        const quint64 deletedBitsBytes = vectorCapacityBytes(device.deletedRecordBits);
        const quint64 trigramRangesBytes = vectorCapacityBytes(device.trigramRanges);
        const quint64 trigramPostingsBytes = vectorCapacityBytes(device.trigramPostings);
        const quint64 liveDeltaFlatIndexBytes = vectorCapacityBytes(device.liveDeltaFlatIndex);
        const quint64 directoryFsIndexRecordRefs32Bytes =
            vectorCapacityBytes(device.directoryFsIndexRecordRefs32);
        const quint64 directoryFsIndexRecordRefs64Bytes =
            vectorCapacityBytes(device.directoryFsIndexRecordRefs64);
        const quint64 liveDirectoryFsIndexRecordRefs32Bytes =
            vectorCapacityBytes(device.liveDirectoryFsIndexRecordRefs32);
        const quint64 liveDirectoryFsIndexRecordRefs64Bytes =
            vectorCapacityBytes(device.liveDirectoryFsIndexRecordRefs64);
        const quint64 fsIndexRecordRefs32Bytes =
            vectorCapacityBytes(device.fsIndexRecordRefs32);
        const quint64 fsIndexRecordRefs64Bytes =
            vectorCapacityBytes(device.fsIndexRecordRefs64);
        const quint64 liveFsIndexRecordRefs32Bytes =
            vectorCapacityBytes(device.liveFsIndexRecordRefs32);
        const quint64 liveFsIndexRecordRefs64Bytes =
            vectorCapacityBytes(device.liveFsIndexRecordRefs64);
        const quint64 namespacedDirectoryFsIndexRecordRefsBytes =
            vectorCapacityBytes(device.namespacedDirectoryFsIndexRecordRefs);
        const quint64 namespacedFsIndexRecordRefsBytes =
            vectorCapacityBytes(device.namespacedFsIndexRecordRefs);

        const quint64 directoryFsIndexRecordRefsBytes =
            directoryFsIndexRecordRefs32Bytes +
            directoryFsIndexRecordRefs64Bytes;

        const quint64 liveDirectoryFsIndexRecordRefsBytes =
            liveDirectoryFsIndexRecordRefs32Bytes +
            liveDirectoryFsIndexRecordRefs64Bytes;

        const quint64 fsIndexRecordRefsBytes =
            fsIndexRecordRefs32Bytes +
            fsIndexRecordRefs64Bytes;

        const quint64 liveFsIndexRecordRefsBytes =
            liveFsIndexRecordRefs32Bytes +
            liveFsIndexRecordRefs64Bytes;

        const TrigramDistributionStats trigramStats =
            calculateTrigramDistributionStats(
                device.trigramRanges,
                device.trigramPostings
            );

        const CompressedTrigramValidationStats trigramValidationStats =
            validateCompressedTrigramIndex(
                device.trigramRanges,
                device.trigramPostings,
                device.fileRecords.size()
            );

        grandTrigramPostingsCapacityBytes += trigramPostingsBytes;
        grandRawUint32TrigramPostingBytes += trigramStats.rawUint32PostingBytes;

        grandCompressedTrigramRangesChecked += trigramValidationStats.rangesChecked;
        grandCompressedTrigramInvalidOffsets += trigramValidationStats.invalidOffsets;
        grandCompressedTrigramDecodeFailures += trigramValidationStats.decodeFailures;
        grandCompressedTrigramNonMonotonicLists += trigramValidationStats.nonMonotonicLists;
        grandCompressedTrigramOutOfRangeRecordRefs += trigramValidationStats.outOfRangeRecordRefs;
        grandCompressedTrigramTrailingBytesInRanges += trigramValidationStats.trailingBytesInRanges;

        const LowercasePoolOpportunity lowercaseOpportunity =
            calculateLowercasePoolOpportunity(device);

        const quint64 sparseLowercasePoolBytes =
            static_cast<quint64>(lowercaseOpportunity.sparseLowercasePoolBytes());

        const quint64 lowercasePoolOnlySavingBytes =
            lowercaseStringPoolBytes > sparseLowercasePoolBytes
                ? lowercaseStringPoolBytes - sparseLowercasePoolBytes
                : 0;

        const quint64 perRecordUint32MetadataBytes =
            static_cast<quint64>(device.fileRecords.size()) * sizeof(uint32_t);

        const quint64 sparseLowercaseWithPerRecordUint32Bytes =
            sparseLowercasePoolBytes + perRecordUint32MetadataBytes;

        const quint64 lowercaseSavingWithPerRecordUint32Bytes =
            lowercaseStringPoolBytes > sparseLowercaseWithPerRecordUint32Bytes
                ? lowercaseStringPoolBytes - sparseLowercaseWithPerRecordUint32Bytes
                : 0;

        const std::size_t fsIndexStoredRecordRefs =
            device.fsIndexStoredRefCount();

        std::size_t fsIndexRecordsOmittedBecauseDeleted = 0;

        for (uint32_t recordIdx = 0;
             recordIdx < static_cast<uint32_t>(device.fileRecords.size());
             ++recordIdx) {
            if (device.isDeletedRecord(recordIdx)) {
                ++fsIndexRecordsOmittedBecauseDeleted;
            }
             }

        const FsIndexKeyWidthStats fsIndexKeyWidthStats =
            calculateFsIndexKeyWidthStats(device);

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
            fileRecordNamespacesBytes +
            stringPoolBytes +
            lowercaseStringPoolBytes +
            lowercaseNameOffsetByRecordBytes +
            deletedBitsBytes +
            trigramRangesBytes +
            trigramPostingsBytes +
            liveDeltaFlatIndexBytes +
            directoryFsIndexRecordRefsBytes +
            liveDirectoryFsIndexRecordRefsBytes +
            fsIndexRecordRefsBytes +
            liveFsIndexRecordRefsBytes +
            namespacedDirectoryFsIndexRecordRefsBytes +
            namespacedFsIndexRecordRefsBytes +
            vectorCapacityBytes(device.fsIndexLookupScratch) +
            extensionVectorStorageBytes;

        const quint64 deviceApproxHashPayloadBytes =
            approximateUnorderedMapNodePayloadBytes(device.recordsByExtension) +
            extensionVectorObjectBytes;

        const quint64 deviceApproxHashBucketBytes =
            approximateUnorderedMapBucketBytes(device.recordsByExtension);

        const quint64 deviceEstimatedHashNodeOverhead24Bytes =
            estimatedUnorderedMapNodeOverheadBytes(device.recordsByExtension, 24);

        grandVectorBytes += deviceVectorBytes;
        grandApproxHashPayloadBytes += deviceApproxHashPayloadBytes;
        grandApproxHashBucketBytes += deviceApproxHashBucketBytes;
        grandEstimatedHashNodeOverhead24Bytes += deviceEstimatedHashNodeOverhead24Bytes;

        grandRecords += device.fileRecords.size();
        grandFileRecordNamespaceEntries += device.fileRecordNamespaces.size();
        grandNamespacedDirectoryRefs += device.namespacedDirectoryFsIndexRecordRefs.size();
        grandNamespacedFsIndexRefs += device.namespacedFsIndexRecordRefs.size();

        if (!device.fileRecordNamespaces.empty()) {
            ++grandIndexesWithNamespaceSidecars;

            if (device.fileRecordNamespaces.size() != device.fileRecords.size()) {
                ++grandIndexesWithInvalidNamespaceSidecars;
            }
        }

        grandFlatTrigrams += trigramStats.postingCount;
        grandLiveDeltaTrigrams += device.liveDeltaFlatIndex.size();
        grandStringBytes += device.stringPool.size();
        grandLowercaseStringBytes += device.lowercaseStringPool.size();
        grandLowercaseNameOffsetEntries += device.lowercaseNameOffsetByRecord.size();
        grandFsIndexStoredRecordRefs += fsIndexStoredRecordRefs;
        grandFsIndexFullRecordRefs += device.fsIndexFullRefCount();
        grandFsIndexLiveRecordRefs += device.fsIndexLiveRefCount();
        grandFsIndexRecordsOmittedBecauseDeleted += fsIndexRecordsOmittedBecauseDeleted;
        grandFsIndexLiveRecordsChecked += fsIndexKeyWidthStats.liveRecordsChecked;
        grandFsIndexDeletedRecordsSkippedForWidth += fsIndexKeyWidthStats.deletedRecordsSkipped;
        grandFsIndexRecordsRequiringUInt64Refs += fsIndexKeyWidthStats.recordsRequiringUInt64Refs;
        grandMaxFsIndex = std::max<uint64_t>(
            grandMaxFsIndex,
            fsIndexKeyWidthStats.maxFsIndex
        );
        grandMaxParentFsIndex = std::max<uint64_t>(
            grandMaxParentFsIndex,
            fsIndexKeyWidthStats.maxParentFsIndex
        );
        grandFsIndexSelectedStorageMatchesLiveRecords =
            grandFsIndexSelectedStorageMatchesLiveRecords &&
            fsIndexKeyWidthStats.selectedStorageMatchesLiveRecords;
        grandExtensionStoredRecordRefs += extensionStoredRecordRefs;

        grandLowercaseValidRecords += lowercaseOpportunity.validRecords;
        grandLowercaseInvalidStringRefs += lowercaseOpportunity.invalidStringRefs;
        grandRecordsWithAsciiUppercase += lowercaseOpportunity.recordsWithAsciiUppercase;
        grandRecordsWithoutAsciiUppercase += lowercaseOpportunity.recordsWithoutAsciiUppercase;
        grandNameBytesWithAsciiUppercase += lowercaseOpportunity.nameBytesWithAsciiUppercase;
        grandNameBytesWithoutAsciiUppercase += lowercaseOpportunity.nameBytesWithoutAsciiUppercase;
        grandAsciiUppercaseByteCount += lowercaseOpportunity.asciiUppercaseByteCount;

        if (device.isReady && device.isSearchable()) {
            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(device.fileRecords.size());
                 ++recordIdx) {
                maxSearchResultsFromReadySearchableDevices +=
                    device.mountedResultMultiplicity(recordIdx);
            }
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
        out << "    fileRecordNamespaces size/capacity: "
            << device.fileRecordNamespaces.size()
            << '/'
            << device.fileRecordNamespaces.capacity()
            << " => "
            << formatBytes(fileRecordNamespacesBytes)
            << '\n';
        out << "      namespace sidecar valid: "
            << (device.fileRecordNamespaces.empty() || device.hasFileRecordNamespaces()
                ? "true"
                : "false")
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
        out << "    lowercaseNameOffsetByRecord size/capacity: "
            << device.lowercaseNameOffsetByRecord.size()
            << '/'
            << device.lowercaseNameOffsetByRecord.capacity()
            << " => "
            << formatBytes(lowercaseNameOffsetByRecordBytes)
            << '\n';
        out << "      sparse lowercase opportunity:\n";
        out << "        valid records measured: "
            << lowercaseOpportunity.validRecords
            << '\n';
        out << "        invalid string refs skipped: "
            << lowercaseOpportunity.invalidStringRefs
            << '\n';
        out << "        records with ASCII uppercase: "
            << lowercaseOpportunity.recordsWithAsciiUppercase
            << '\n';
        out << "        records without ASCII uppercase: "
            << lowercaseOpportunity.recordsWithoutAsciiUppercase
            << '\n';
        out << "        ASCII uppercase bytes: "
            << lowercaseOpportunity.asciiUppercaseByteCount
            << '\n';
        out << "        name bytes needing lowercase copy: "
            << lowercaseOpportunity.nameBytesWithAsciiUppercase
            << " => "
            << formatBytes(static_cast<quint64>(lowercaseOpportunity.nameBytesWithAsciiUppercase))
            << '\n';
        out << "        name bytes reusable from original stringPool: "
            << lowercaseOpportunity.nameBytesWithoutAsciiUppercase
            << " => "
            << formatBytes(static_cast<quint64>(lowercaseOpportunity.nameBytesWithoutAsciiUppercase))
            << '\n';
        out << "        estimated sparse lowercase pool bytes: "
            << formatBytes(sparseLowercasePoolBytes)
            << '\n';
        out << "        estimated saving, pool only: "
            << formatBytes(lowercasePoolOnlySavingBytes)
            << '\n';
        out << "        estimated per-record uint32 metadata: "
            << formatBytes(perRecordUint32MetadataBytes)
            << '\n';
        out << "        estimated saving, pool plus per-record uint32 metadata: "
            << formatBytes(lowercaseSavingWithPerRecordUint32Bytes)
            << '\n';
        out << "    deletedRecordBits words size/capacity: "
            << device.deletedRecordBits.size()
            << '/'
            << device.deletedRecordBits.capacity()
            << " => "
            << formatBytes(deletedBitsBytes)
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
            << " compressed bytes\n";
        out << "      trigram posting distribution:\n";
        out << "        unique trigrams/ranges: "
            << trigramStats.rangeCount
            << '\n';
        out << "        decoded posting entries: "
            << trigramStats.postingCount
            << '\n';
        out << "        posting-list size min/p50/p90/p95/p99/max: "
            << trigramStats.minPostingListSize
            << '/'
            << trigramStats.p50PostingListSize
            << '/'
            << trigramStats.p90PostingListSize
            << '/'
            << trigramStats.p95PostingListSize
            << '/'
            << trigramStats.p99PostingListSize
            << '/'
            << trigramStats.maxPostingListSize
            << '\n';
        out << "        posting lists with 1 entry: "
            << trigramStats.postingListsWithOneEntry
            << '\n';
        out << "        posting lists with >=1024 entries: "
            << trigramStats.postingListsWithAtLeast1024Entries
            << '\n';
        out << "        posting lists with >=65536 entries: "
            << trigramStats.postingListsWithAtLeast65536Entries
            << '\n';
        out << "        entries in top 10/top 100 largest posting lists: "
            << trigramStats.top10PostingEntries
            << '/'
            << trigramStats.top100PostingEntries
            << '\n';
        out << "        raw uint32 posting bytes would be: "
            << formatBytes(trigramStats.rawUint32PostingBytes)
            << '\n';
        out << "        compressed posting saving vs raw uint32: "
            << formatBytes(trigramStats.compressedPostingSavingBytes)
            << '\n';
        out << "      compressed trigram validation:\n";
        out << "        ranges checked: "
            << trigramValidationStats.rangesChecked
            << '\n';
        out << "        invalid offsets: "
            << trigramValidationStats.invalidOffsets
            << '\n';
        out << "        decode failures: "
            << trigramValidationStats.decodeFailures
            << '\n';
        out << "        non-monotonic lists: "
            << trigramValidationStats.nonMonotonicLists
            << '\n';
        out << "        out-of-range record refs: "
            << trigramValidationStats.outOfRangeRecordRefs
            << '\n';
        out << "        trailing bytes in ranges: "
            << trigramValidationStats.trailingBytesInRanges
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
        out << "    fs-index ref storage: "
            << (device.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32
                ? "uint32"
                : "uint64")
            << '\n';

        out << "    directoryFsIndexRecordRefs32 size/capacity: "
            << device.directoryFsIndexRecordRefs32.size()
            << '/'
            << device.directoryFsIndexRecordRefs32.capacity()
            << " => "
            << formatBytes(directoryFsIndexRecordRefs32Bytes)
            << '\n';
        out << "    directoryFsIndexRecordRefs64 size/capacity: "
            << device.directoryFsIndexRecordRefs64.size()
            << '/'
            << device.directoryFsIndexRecordRefs64.capacity()
            << " => "
            << formatBytes(directoryFsIndexRecordRefs64Bytes)
            << '\n';
        out << "    liveDirectoryFsIndexRecordRefs32 size/capacity: "
            << device.liveDirectoryFsIndexRecordRefs32.size()
            << '/'
            << device.liveDirectoryFsIndexRecordRefs32.capacity()
            << " => "
            << formatBytes(liveDirectoryFsIndexRecordRefs32Bytes)
            << '\n';
        out << "    liveDirectoryFsIndexRecordRefs64 size/capacity: "
            << device.liveDirectoryFsIndexRecordRefs64.size()
            << '/'
            << device.liveDirectoryFsIndexRecordRefs64.capacity()
            << " => "
            << formatBytes(liveDirectoryFsIndexRecordRefs64Bytes)
            << '\n';

        out << "    fsIndexRecordRefs32 size/capacity: "
            << device.fsIndexRecordRefs32.size()
            << '/'
            << device.fsIndexRecordRefs32.capacity()
            << " => "
            << formatBytes(fsIndexRecordRefs32Bytes)
            << '\n';
        out << "    fsIndexRecordRefs64 size/capacity: "
            << device.fsIndexRecordRefs64.size()
            << '/'
            << device.fsIndexRecordRefs64.capacity()
            << " => "
            << formatBytes(fsIndexRecordRefs64Bytes)
            << '\n';
        out << "    liveFsIndexRecordRefs32 size/capacity: "
            << device.liveFsIndexRecordRefs32.size()
            << '/'
            << device.liveFsIndexRecordRefs32.capacity()
            << " => "
            << formatBytes(liveFsIndexRecordRefs32Bytes)
            << '\n';
        out << "    liveFsIndexRecordRefs64 size/capacity: "
            << device.liveFsIndexRecordRefs64.size()
            << '/'
            << device.liveFsIndexRecordRefs64.capacity()
            << " => "
            << formatBytes(liveFsIndexRecordRefs64Bytes)
            << '\n';
        out << "    namespacedDirectoryFsIndexRecordRefs size/capacity: "
            << device.namespacedDirectoryFsIndexRecordRefs.size()
            << '/'
            << device.namespacedDirectoryFsIndexRecordRefs.capacity()
            << " => "
            << formatBytes(namespacedDirectoryFsIndexRecordRefsBytes)
            << '\n';
        out << "    namespacedFsIndexRecordRefs size/capacity: "
            << device.namespacedFsIndexRecordRefs.size()
            << '/'
            << device.namespacedFsIndexRecordRefs.capacity()
            << " => "
            << formatBytes(namespacedFsIndexRecordRefsBytes)
            << '\n';
        out << "      full refs: "
            << device.fsIndexFullRefCount()
            << '\n';
        out << "      live refs: "
            << device.fsIndexLiveRefCount()
            << '\n';
        out << "      stored record refs total: "
            << fsIndexStoredRecordRefs
            << '\n';
        out << "      records omitted because deleted: "
            << fsIndexRecordsOmittedBecauseDeleted
            << '\n';
        out << "      key width validation:\n";
        out << "        live records checked: "
            << fsIndexKeyWidthStats.liveRecordsChecked
            << '\n';
        out << "        deleted records skipped: "
            << fsIndexKeyWidthStats.deletedRecordsSkipped
            << '\n';
        out << "        max fsIndex: "
            << fsIndexKeyWidthStats.maxFsIndex
            << '\n';
        out << "        max parentFsIndex: "
            << fsIndexKeyWidthStats.maxParentFsIndex
            << '\n';
        out << "        records requiring uint64 refs: "
            << fsIndexKeyWidthStats.recordsRequiringUInt64Refs
            << '\n';
        out << "        all live refs fit uint32: "
            << (fsIndexKeyWidthStats.allLiveRefsFitUInt32 ? "true" : "false")
            << '\n';
        out << "        selected storage matches live records: "
            << (fsIndexKeyWidthStats.selectedStorageMatchesLiveRecords ? "true" : "false")
            << '\n';
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
    out << "  fileRecordNamespace entries: "
        << grandFileRecordNamespaceEntries
        << '\n';
    out << "  indexes with namespace sidecars: "
        << grandIndexesWithNamespaceSidecars
        << '\n';
    out << "  indexes with invalid namespace sidecars: "
        << grandIndexesWithInvalidNamespaceSidecars
        << '\n';
    out << "  namespaced directory fs-index refs: "
        << grandNamespacedDirectoryRefs
        << '\n';
    out << "  namespaced fs-index refs: "
        << grandNamespacedFsIndexRefs
        << '\n';
    out << "  stringPool bytes used: " << grandStringBytes << '\n';
    out << "  lowercaseStringPool bytes used: " << grandLowercaseStringBytes << '\n';
    out << "  lowercaseNameOffsetByRecord entries: "
        << grandLowercaseNameOffsetEntries
        << '\n';
    out << "  trigram posting entries: " << grandFlatTrigrams << '\n';
    out << "  compressed trigram posting capacity bytes: "
        << formatBytes(grandTrigramPostingsCapacityBytes)
        << '\n';
    out << "  raw uint32 trigram posting bytes would be: "
        << formatBytes(grandRawUint32TrigramPostingBytes)
        << '\n';
    out << "  compressed trigram posting saving vs raw uint32: "
        << formatBytes(
            grandRawUint32TrigramPostingBytes > grandTrigramPostingsCapacityBytes
                ? grandRawUint32TrigramPostingBytes - grandTrigramPostingsCapacityBytes
                : 0
        )
        << '\n';
    out << "  compressed trigram validation:\n";
    out << "    ranges checked: "
        << grandCompressedTrigramRangesChecked
        << '\n';
    out << "    invalid offsets: "
        << grandCompressedTrigramInvalidOffsets
        << '\n';
    out << "    decode failures: "
        << grandCompressedTrigramDecodeFailures
        << '\n';
    out << "    non-monotonic lists: "
        << grandCompressedTrigramNonMonotonicLists
        << '\n';
    out << "    out-of-range record refs: "
        << grandCompressedTrigramOutOfRangeRecordRefs
        << '\n';
    out << "    trailing bytes in ranges: "
        << grandCompressedTrigramTrailingBytesInRanges
        << '\n';
    out << "  live delta trigram entries: " << grandLiveDeltaTrigrams << '\n';
    out << "  fs-index stored record refs: " << grandFsIndexStoredRecordRefs << '\n';
    out << "  fs-index full refs: " << grandFsIndexFullRecordRefs << '\n';
    out << "  fs-index live refs: " << grandFsIndexLiveRecordRefs << '\n';
    out << "  fs-index records omitted because deleted: "
        << grandFsIndexRecordsOmittedBecauseDeleted
        << '\n';
    out << "  fs-index key width validation:\n";
    out << "    live records checked: "
        << grandFsIndexLiveRecordsChecked
        << '\n';
    out << "    deleted records skipped: "
        << grandFsIndexDeletedRecordsSkippedForWidth
        << '\n';
    out << "    max fsIndex: "
        << grandMaxFsIndex
        << '\n';
    out << "    max parentFsIndex: "
        << grandMaxParentFsIndex
        << '\n';
    out << "    records requiring uint64 refs: "
        << grandFsIndexRecordsRequiringUInt64Refs
        << '\n';
    out << "    all live refs fit uint32: "
        << (grandFsIndexRecordsRequiringUInt64Refs == 0 ? "true" : "false")
        << '\n';
    out << "    selected storage matches live records: "
        << (grandFsIndexSelectedStorageMatchesLiveRecords ? "true" : "false")
        << '\n';
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

    const quint64 grandSparseLowercasePoolBytes =
        static_cast<quint64>(grandNameBytesWithAsciiUppercase);

    const quint64 grandLowercasePoolCurrentBytes =
        static_cast<quint64>(grandLowercaseStringBytes);

    const quint64 grandOldFullLowercaseMirrorBytes =
        static_cast<quint64>(
            grandNameBytesWithAsciiUppercase +
            grandNameBytesWithoutAsciiUppercase
        );

    const quint64 grandLowercasePoolOnlySavingBytes =
        grandOldFullLowercaseMirrorBytes > grandLowercasePoolCurrentBytes
            ? grandOldFullLowercaseMirrorBytes - grandLowercasePoolCurrentBytes
            : 0;

    const quint64 grandPerRecordUint32MetadataBytes =
        static_cast<quint64>(grandRecords) * sizeof(uint32_t);

    const quint64 grandSparseLowercaseWithPerRecordUint32Bytes =
        grandLowercasePoolCurrentBytes + grandPerRecordUint32MetadataBytes;

    const quint64 grandLowercaseSavingWithPerRecordUint32Bytes =
        grandOldFullLowercaseMirrorBytes > grandSparseLowercaseWithPerRecordUint32Bytes
            ? grandOldFullLowercaseMirrorBytes - grandSparseLowercaseWithPerRecordUint32Bytes
            : 0;

    out << '\n';
    out << "  sparse lowercase pool opportunity:\n";
    out << "    valid records measured: "
        << grandLowercaseValidRecords
        << '\n';
    out << "    invalid string refs skipped: "
        << grandLowercaseInvalidStringRefs
        << '\n';
    out << "    records with ASCII uppercase: "
        << grandRecordsWithAsciiUppercase
        << '\n';
    out << "    records without ASCII uppercase: "
        << grandRecordsWithoutAsciiUppercase
        << '\n';
    out << "    ASCII uppercase bytes: "
        << grandAsciiUppercaseByteCount
        << '\n';
    out << "    name bytes needing lowercase copy: "
        << grandNameBytesWithAsciiUppercase
        << " => "
        << formatBytes(static_cast<quint64>(grandNameBytesWithAsciiUppercase))
        << '\n';
    out << "    name bytes reusable from original stringPool: "
        << grandNameBytesWithoutAsciiUppercase
        << " => "
        << formatBytes(static_cast<quint64>(grandNameBytesWithoutAsciiUppercase))
        << '\n';
    out << "    current sparse lowercaseStringPool used bytes: "
        << formatBytes(grandLowercasePoolCurrentBytes)
        << '\n';
    out << "    measured name bytes requiring lowercase copies: "
        << formatBytes(grandSparseLowercasePoolBytes)
        << '\n';
    out << "    old full lowercase mirror would have used: "
        << formatBytes(
            static_cast<quint64>(
                grandNameBytesWithAsciiUppercase +
                grandNameBytesWithoutAsciiUppercase
            )
        )
        << '\n';
    out << "    estimated saving vs old full lowercase mirror, pool only: "
        << formatBytes(grandLowercasePoolOnlySavingBytes)
        << '\n';
    out << "    estimated per-record uint32 metadata: "
        << formatBytes(grandPerRecordUint32MetadataBytes)
        << '\n';
    out << "    estimated saving, pool plus per-record uint32 metadata: "
        << formatBytes(grandLowercaseSavingWithPerRecordUint32Bytes)
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
    out << "    if handles were 16 bytes: "
        << formatBytes(static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) * 16)
        << " (saving "
        << formatBytes(
            static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) *
            (16 - sizeof(RecordHandle))
        )
        << " at current size)\n";
    out << "    if handles were 12 bytes: "
        << formatBytes(static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) * 12)
        << " (saving "
        << formatBytes(
            sizeof(RecordHandle) < 12
                ? static_cast<quint64>(maxSearchResultsFromReadySearchableDevices) *
                  (12 - sizeof(RecordHandle))
                : 0
        )
        << " at current size)\n";

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

        for (uint32_t recordIdx = 0;
             recordIdx < static_cast<uint32_t>(indexPtr->fileRecords.size());
             ++recordIdx) {
            total += indexPtr->mountedResultMultiplicity(recordIdx);
        }
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

uint32_t IndexController::appendSparseLowercaseName(
    DeviceIndex& deviceIndex,
    std::string_view name)
{
    bool hasAsciiUppercase = false;

    for (const unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') {
            hasAsciiUppercase = true;
            break;
        }
    }

    if (!hasAsciiUppercase) {
        return DeviceIndex::NoLowercaseNameOverride;
    }

    const uint32_t lowercaseOffset =
        static_cast<uint32_t>(deviceIndex.lowercaseStringPool.size());

    deviceIndex.lowercaseStringPool.reserve(
        deviceIndex.lowercaseStringPool.size() + name.size()
    );

    for (const unsigned char c : name) {
        deviceIndex.lowercaseStringPool.push_back(
            (c >= 'A' && c <= 'Z')
                ? static_cast<char>(c | 32)
                : static_cast<char>(c)
        );
    }

    return lowercaseOffset;
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

    if (record.nameLen < 3) {
        return false;
    }

    const std::string_view name = deviceIndex.lowercaseRecordName(record, recordIdx);

    if (name.size() < 3) {
        return false;
    }

    std::vector<uint32_t> uniqueTrigrams;
    uniqueTrigrams.reserve(name.size() - 2);

    for (std::size_t i = 0; i <= name.size() - 3; ++i) {
        const uint32_t trigram =
            (static_cast<uint32_t>(static_cast<unsigned char>(name[i])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(name[i + 1])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(name[i + 2]));

        uniqueTrigrams.push_back(trigram);
    }

    std::sort(uniqueTrigrams.begin(), uniqueTrigrams.end());

    uniqueTrigrams.erase(
        std::unique(uniqueTrigrams.begin(), uniqueTrigrams.end()),
        uniqueTrigrams.end()
    );

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

    const std::size_t mainPostingCount =
        totalDecodedTrigramPostingCount(deviceIndex.trigramRanges);

    if (mainPostingCount == 0) {
        return true;
    }

    return deviceIndex.liveDeltaFlatIndex.size() >=
           mainPostingCount / LiveDeltaRebuildRatioDivisor;
}

void IndexController::rebuildTrigramIndexAfterLiveUpdates(DeviceIndex& deviceIndex)
{
#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Rebuilding compact trigram index after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " ranges=" << deviceIndex.trigramRanges.size()
              << " compressedPostingBytes=" << deviceIndex.trigramPostings.size()
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

        const std::string_view name = deviceIndex.lowercaseRecordName(recordIdx);
        if (name.size() >= 3) {
            estimatedTrigrams += name.size() - 2;
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
              << " compressedPostingBytes=" << deviceIndex.trigramPostings.size()
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

void IndexController::sortLiveDirectoryFsIndexRecordRefs(DeviceIndex& deviceIndex)
{
    if (deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32) {
        if (deviceIndex.liveDirectoryFsIndexRecordRefs32.size() < 2) {
            return;
        }

        DeviceIndex::sortAndDeduplicateFsIndexRecordRefs(
            deviceIndex.liveDirectoryFsIndexRecordRefs32
        );
        return;
    }

    if (deviceIndex.liveDirectoryFsIndexRecordRefs64.size() < 2) {
        return;
    }

    DeviceIndex::sortAndDeduplicateFsIndexRecordRefs(
        deviceIndex.liveDirectoryFsIndexRecordRefs64
    );
}

void IndexController::sortLiveFsIndexRecordRefs(DeviceIndex& deviceIndex)
{
    if (deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32) {
        if (deviceIndex.liveFsIndexRecordRefs32.size() < 2) {
            return;
        }

        DeviceIndex::sortAndDeduplicateFsIndexRecordRefs(
            deviceIndex.liveFsIndexRecordRefs32
        );
        return;
    }

    if (deviceIndex.liveFsIndexRecordRefs64.size() < 2) {
        return;
    }

    DeviceIndex::sortAndDeduplicateFsIndexRecordRefs(
        deviceIndex.liveFsIndexRecordRefs64
    );
}

bool IndexController::shouldRebuildFsIndexAfterLiveUpdates(const DeviceIndex& deviceIndex)
{
    const std::size_t liveRefCount = deviceIndex.fsIndexLiveRefCount();

    if (liveRefCount == 0) {
        return false;
    }

    static constexpr std::size_t LiveFsIndexRebuildMinEntries = 25'000;
    static constexpr std::size_t LiveFsIndexRebuildRatioDivisor = 10;

    if (liveRefCount < LiveFsIndexRebuildMinEntries) {
        return false;
    }

    const std::size_t fullRefCount = deviceIndex.fsIndexFullRefCount();

    if (fullRefCount == 0) {
        return true;
    }

    return liveRefCount >= fullRefCount / LiveFsIndexRebuildRatioDivisor;
}

void IndexController::rebuildFsIndexAfterLiveUpdates(DeviceIndex& deviceIndex)
{
#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Rebuilding fs-index refs after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " storage="
              << (deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32
                  ? "uint32"
                  : "uint64")
              << " refs=" << deviceIndex.fsIndexFullRefCount()
              << " liveRefs=" << deviceIndex.fsIndexLiveRefCount()
              << "\n";
#endif

    deviceIndex.rebuildFsIndexMaps();

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "Finished rebuilding fs-index refs after live updates"
              << " deviceId=" << deviceIndex.deviceId.toStdString()
              << " storage="
              << (deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32
                  ? "uint32"
                  : "uint64")
              << " refs=" << deviceIndex.fsIndexFullRefCount()
              << " liveRefs=" << deviceIndex.fsIndexLiveRefCount()
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
    const std::size_t oldLowercaseNameOffsetCapacity = deviceIndex.lowercaseNameOffsetByRecord.capacity();
    const std::size_t oldDeletedBitsCapacity = deviceIndex.deletedRecordBits.capacity();
    const std::size_t oldLiveDeltaCapacity = deviceIndex.liveDeltaFlatIndex.capacity();
    const std::size_t oldLiveFsIndexCapacity =
        deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32
            ? deviceIndex.liveFsIndexRecordRefs32.capacity()
            : deviceIndex.liveFsIndexRecordRefs64.capacity();

    if (operation.inode == 0 || operation.name.isEmpty()) {
        return false;
    }

    if (operation.parentInode == 0) {
        return false;
    }

    const bool useNamespaces = deviceIndex.hasFileRecordNamespaces();

    if (useNamespaces &&
        (operation.fsNamespace == 0 || operation.parentFsNamespace == 0)) {
        return false;
    }

    deviceIndex.ensureFsIndexRefStorageCanStore(
        operation.inode,
        operation.parentInode
    );

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

    const bool isRootSelfParent =
        operation.inode == operation.parentInode &&
        (!useNamespaces || operation.fsNamespace == operation.parentFsNamespace);

    if (!isRootSelfParent) {
        const std::optional<uint32_t> parentRecordIdx =
            useNamespaces
                ? deviceIndex.directoryRecordIdxForNamespacedFsIndex(
                    operation.parentFsNamespace,
                    operation.parentInode
                )
                : deviceIndex.directoryRecordIdxForFsIndex(operation.parentInode);

        if (parentRecordIdx) {
            record.parentRecordIdx = *parentRecordIdx;
        }
    }

    deviceIndex.stringPool.insert(
        deviceIndex.stringPool.end(),
        nameUtf8.begin(),
        nameUtf8.end()
    );

    const std::string_view nameView(
        nameUtf8.constData(),
        static_cast<std::size_t>(nameUtf8.size())
    );

    const uint32_t lowercaseNameOffset =
        appendSparseLowercaseName(deviceIndex, nameView);

    deviceIndex.fileRecords.push_back(record);

    if (useNamespaces) {
        deviceIndex.fileRecordNamespaces.push_back({
            operation.fsNamespace,
            operation.parentFsNamespace
        });
    }

    deviceIndex.lowercaseNameOffsetByRecord.push_back(lowercaseNameOffset);
    deviceIndex.resizeDeletedRecordBitsForRecordCount(deviceIndex.fileRecords.size());

    if (deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32) {
        deviceIndex.liveFsIndexRecordRefs32.push_back({
            static_cast<uint32_t>(record.fsIndex),
            recordIdx
        });

        if ((record.flags & FileRecord_IsDir) != 0) {
            deviceIndex.liveDirectoryFsIndexRecordRefs32.push_back({
                static_cast<uint32_t>(record.fsIndex),
                recordIdx
            });
        }
    } else {
        deviceIndex.liveFsIndexRecordRefs64.push_back({
            record.fsIndex,
            recordIdx
        });

        if ((record.flags & FileRecord_IsDir) != 0) {
            deviceIndex.liveDirectoryFsIndexRecordRefs64.push_back({
                record.fsIndex,
                recordIdx
            });
        }
    }

    if (useNamespaces) {
        deviceIndex.namespacedFsIndexRecordRefs.push_back({
            operation.fsNamespace,
            operation.inode,
            recordIdx
        });

        if ((record.flags & FileRecord_IsDir) != 0) {
            deviceIndex.namespacedDirectoryFsIndexRecordRefs.push_back({
                operation.fsNamespace,
                operation.inode,
                recordIdx
            });
        }
    }

    appendTrigramsForRecord(deviceIndex, recordIdx, deviceIndex.liveDeltaFlatIndex);
    addRecordToExtensionIndexIfApplicable(deviceIndex, recordIdx);

    const qint64 elapsedMs = elapsedMsSince(appendStart);

#ifdef KERYTHING_ENABLE_LOGGING
    const std::size_t newLiveFsIndexCapacity =
            deviceIndex.fsIndexRefStorage == DeviceIndex::FsIndexRefStorage::UInt32
                ? deviceIndex.liveFsIndexRecordRefs32.capacity()
                : deviceIndex.liveFsIndexRecordRefs64.capacity();

    if (elapsedMs >= 25 ||
    oldFileRecordCapacity != deviceIndex.fileRecords.capacity() ||
    oldStringPoolCapacity != deviceIndex.stringPool.capacity() ||
    oldLowercaseStringPoolCapacity != deviceIndex.lowercaseStringPool.capacity() ||
    oldLowercaseNameOffsetCapacity != deviceIndex.lowercaseNameOffsetByRecord.capacity() ||
    oldDeletedBitsCapacity != deviceIndex.deletedRecordBits.capacity() ||
    oldLiveDeltaCapacity != deviceIndex.liveDeltaFlatIndex.capacity() ||
    oldLiveFsIndexCapacity != newLiveFsIndexCapacity) {
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
                  << " lowercaseNameOffsetByRecord capacity "
                  << oldLowercaseNameOffsetCapacity
                  << " -> "
                  << deviceIndex.lowercaseNameOffsetByRecord.capacity()
                  << " deletedRecordBits capacity "
                  << oldDeletedBitsCapacity
                  << " -> "
                  << deviceIndex.deletedRecordBits.capacity()
                  << " liveDelta capacity "
                  << oldLiveDeltaCapacity
                  << " -> "
                  << deviceIndex.liveDeltaFlatIndex.capacity()
                  << " liveFsIndex capacity "
                  << oldLiveFsIndexCapacity
                  << " -> "
                  << newLiveFsIndexCapacity
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

    const bool useNamespaces = deviceIndex.hasFileRecordNamespaces();

    if (useNamespaces) {
        if (recordIdx >= deviceIndex.fileRecordNamespaces.size()) {
            return false;
        }

        const FileRecordNamespace existingNamespace =
            deviceIndex.fileRecordNamespaces[recordIdx];

        if (existingNamespace.fsNamespace != operation.fsNamespace) {
            return false;
        }
    }

    deviceIndex.ensureFsIndexRefStorageCanStore(
        operation.inode,
        operation.parentInode
    );

    uint32_t parentRecordIdx = 0xFFFFFFFF;

    if (operation.parentInode != operation.inode ||
        (useNamespaces && operation.parentFsNamespace != operation.fsNamespace)) {
        std::optional<uint32_t> parentRecord;

        if (useNamespaces) {
            parentRecord = deviceIndex.directoryRecordIdxForNamespacedFsIndex(
                operation.parentFsNamespace,
                operation.parentInode
            );
        } else {
            parentRecord = deviceIndex.directoryRecordIdxForFsIndex(operation.parentInode);
        }

        if (!parentRecord) {
            return false;
        }

        parentRecordIdx = *parentRecord;
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

    const std::string_view nameView(
        nameUtf8.constData(),
        static_cast<std::size_t>(nameUtf8.size())
    );

    const uint32_t lowercaseNameOffset =
        appendSparseLowercaseName(deviceIndex, nameView);

    if (recordIdx >= deviceIndex.lowercaseNameOffsetByRecord.size()) {
        deviceIndex.lowercaseNameOffsetByRecord.resize(
            deviceIndex.fileRecords.size(),
            DeviceIndex::NoLowercaseNameOverride
        );
    }

    deviceIndex.lowercaseNameOffsetByRecord[recordIdx] =
        lowercaseNameOffset;

    record.parentFsIndex = operation.parentInode;
    record.parentRecordIdx = parentRecordIdx;
    record.nameOffset = nameOffset;
    record.nameLen = nameLen;

    if (useNamespaces) {
        deviceIndex.fileRecordNamespaces[recordIdx] = {
            operation.fsNamespace,
            operation.parentFsNamespace
        };
    }

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

    const bool useNamespaces = deviceIndex.hasFileRecordNamespaces();

    /*
     * First match by directory entry identity.
     *
     * For ordinary filesystems this is:
     *
     *   (parent inode, name)
     *
     * For namespace-aware filesystems such as Btrfs this is:
     *
     *   (parent root/subvolume id, parent inode, name)
     *
     * A browser/download manager can replace an existing path by creating a
     * temp file and renaming it over the final path. In that case the visible
     * directory entry is the same, but the object inode can change. Keeping
     * both records live would create duplicate search results with the same
     * path/name.
     */
    std::optional<uint32_t> existingSamePath;

    if (useNamespaces) {
        existingSamePath =
            deviceIndex.findLiveEntryRecordByNamespacedParentAndName(
                operation.parentFsNamespace,
                operation.parentInode,
                std::string_view(
                    nameUtf8.constData(),
                    static_cast<std::size_t>(nameUtf8.size())
                )
            );
    } else {
        existingSamePath =
            findLiveEntryRecord(deviceIndex, operation.parentInode, nameUtf8, 0);
    }

    if (existingSamePath) {
        FileRecord& existingRecord = deviceIndex.fileRecords[*existingSamePath];

        if (existingRecord.fsIndex == operation.inode) {
            if (useNamespaces) {
                const FileRecordNamespace existingNamespace =
                    deviceIndex.namespaceForRecord(*existingSamePath);

                if (existingNamespace.fsNamespace != operation.fsNamespace) {
                    logSlowUpsert("same-path-namespace-mismatch");
                    return UpsertApplyResult::NeedsRescan;
                }
            }

            updateFileRecordMetadataFromLiveUpdateOperation(existingRecord, operation);
            logSlowUpsert("updated-existing");
            return UpsertApplyResult::Applied;
        }

        bool deletedDirectory = false;
        const qsizetype deletedCount =
            deviceIndex.markDeletedRecordTree(*existingSamePath, &deletedDirectory);

        if (deletedCount > 0) {
            deviceIndex.extensionIndexLiveDeltaEntries +=
                static_cast<std::size_t>(deletedCount);
        }

        if (!appendRecordFromLiveUpdateOperation(deviceIndex, operation)) {
            logSlowUpsert("replace-append-invalid");
            return UpsertApplyResult::Invalid;
        }

        logSlowUpsert("replaced-existing-path");

        return deletedDirectory
            ? UpsertApplyResult::AppliedNeedsFsIndexRebuild
            : UpsertApplyResult::Applied;
    }

    /*
     * If the filesystem object already exists but this exact directory-entry
     * identity does not, this is usually a new directory entry for the same
     * object, for example a hard link.
     *
     * For ordinary filesystems, object identity is the inode/MFT id.
     * For namespace-aware filesystems such as Btrfs, object identity is:
     *
     *   (root/subvolume id, inode)
     *
     * Paired renames/moves are handled earlier by DeleteEntry matching a
     * pending Upsert with the same object identity and updating that existing
     * record's directory-entry identity. Reaching this point means this Upsert
     * was not consumed by a matching delete, so appending another FileRecord is
     * the safest representation for hard links and avoids false stale-index
     * warnings.
     */

    /*
     * New non-root entries need their parent directory to exist in the index
     * before we append them, otherwise parentRecordIdx cannot be resolved.
     *
     * For Btrfs and other namespace-aware filesystems, parent lookup uses:
     *
     *   (parent root/subvolume id, parent inode)
     *
     * rather than parent inode alone.
     */
    const bool isRootSelfParent =
        operation.parentInode == operation.inode &&
        (!useNamespaces || operation.parentFsNamespace == operation.fsNamespace);

    if (!isRootSelfParent) {
        const std::optional<uint32_t> parentRecord =
            useNamespaces
                ? deviceIndex.directoryRecordIdxForNamespacedFsIndex(
                    operation.parentFsNamespace,
                    operation.parentInode
                )
                : deviceIndex.directoryRecordIdxForFsIndex(operation.parentInode);

        if (!parentRecord) {
            logSlowUpsert("missing-parent");
            return UpsertApplyResult::MissingParent;
        }
    }

    if (!appendRecordFromLiveUpdateOperation(deviceIndex, operation)) {
        logSlowUpsert("append-invalid");
        return UpsertApplyResult::Invalid;
    }

    logSlowUpsert("appended");

    return useNamespaces
        ? UpsertApplyResult::AppliedNeedsFsIndexRebuild
        : UpsertApplyResult::Applied;
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
        if (index.indexId > RecordHandle::MaxIndexId) {
            return;
        }

        if (index.isDeletedRecord(recordIdx)) {
            return;
        }

        const auto indexId = static_cast<uint16_t>(index.indexId);
        const auto generation = static_cast<uint8_t>(index.generation);

        if (index.mountPoints.isEmpty()) {
            results.push_back({
                recordIdx,
                indexId,
                generation,
                RecordHandle::NoMountPoint
            });
            return;
        }

        index.forEachVisibleMountPointForRecord(
            recordIdx,
            [&](int mountPointIdx) {
                results.push_back({
                    recordIdx,
                    indexId,
                    generation,
                    static_cast<uint8_t>(mountPointIdx)
                });
            }
        );
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

            if (hasExtensionFilter) {
                const auto extensionCandidates =
                    collectExtensionCandidates(*indexPtr, extensionFilter);

                for (const uint32_t recordIdx : extensionCandidates) {
                    totalSize += indexPtr->mountedResultMultiplicity(recordIdx);
                }
            } else {
                for (uint32_t recordIdx = 0;
                     recordIdx < static_cast<uint32_t>(indexPtr->fileRecords.size());
                     ++recordIdx) {
                    totalSize += indexPtr->mountedResultMultiplicity(recordIdx);
                }
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

                const std::string_view name = index.lowercaseRecordName(rec);
                if (name.empty()) {
                    return;
                }

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

        // 2. Candidate filtering via trigrams.
        //
        // For each keyword, process its trigrams from rarest to most common.
        // Starting with the smallest postings list keeps the candidate vector
        // small and reduces both temporary memory and refinement work.
        std::vector<uint32_t> candidates;
        bool firstTrigram = true;
        bool trigramsUsed = false; // Track if we actually used the index
        bool skipDevice = false;

        for (const auto& queryKeyword : queryKeywords) {
            const std::string& kw = queryKeyword.lowercaseText;

            if (kw.length() < 3) {
                // Short keywords cannot use the trigram index and are handled
                // later during refinement.
                continue;
            }

            std::vector<QueryTrigram> keywordTrigrams;
            keywordTrigrams.reserve(kw.length() - 2);

            for (size_t i = 0; i <= kw.length() - 3; ++i) {
                const uint32_t tri =
                    (static_cast<uint32_t>(static_cast<unsigned char>(kw[i])) << 16) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(kw[i + 1])) << 8) |
                    static_cast<uint32_t>(static_cast<unsigned char>(kw[i + 2]));

                keywordTrigrams.push_back({
                    tri,
                    trigramPostingCount(indexPtr->trigramRanges, tri) +
                    trigramPostingCount(indexPtr->liveDeltaFlatIndex, tri)
                });
            }

            std::sort(
                keywordTrigrams.begin(),
                keywordTrigrams.end(),
                [](const QueryTrigram& lhs, const QueryTrigram& rhs) {
                    if (lhs.trigram != rhs.trigram) {
                        return lhs.trigram < rhs.trigram;
                    }

                    return lhs.postingCount < rhs.postingCount;
                }
            );

            keywordTrigrams.erase(
                std::unique(
                    keywordTrigrams.begin(),
                    keywordTrigrams.end(),
                    [](const QueryTrigram& lhs, const QueryTrigram& rhs) {
                        return lhs.trigram == rhs.trigram;
                    }
                ),
                keywordTrigrams.end()
            );

            for (QueryTrigram& queryTrigram : keywordTrigrams) {
                queryTrigram.postingCount =
                    trigramPostingCount(indexPtr->trigramRanges, queryTrigram.trigram) +
                    trigramPostingCount(indexPtr->liveDeltaFlatIndex, queryTrigram.trigram);
            }

            std::sort(
                keywordTrigrams.begin(),
                keywordTrigrams.end(),
                [](const QueryTrigram& lhs, const QueryTrigram& rhs) {
                    if (lhs.postingCount != rhs.postingCount) {
                        return lhs.postingCount < rhs.postingCount;
                    }

                    return lhs.trigram < rhs.trigram;
                }
            );

            for (const QueryTrigram& queryTrigram : keywordTrigrams) {
                trigramsUsed = true;

                if (queryTrigram.postingCount == 0) {
                    // A required trigram is absent from this device, so this
                    // keyword cannot match anything on the device.
                    skipDevice = true;
                    break;
                }

                // 3. Intersect candidates (Candidate Filtering)
                if (firstTrigram) {
                    candidates.reserve(queryTrigram.postingCount);

                    forEachRecordIdxForTrigram(
                        indexPtr->trigramRanges,
                        indexPtr->trigramPostings,
                        queryTrigram.trigram,
                        [&](uint32_t recordIdx) {
                            candidates.push_back(recordIdx);
                        }
                    );

                    forEachRecordIdxForTrigram(
                        indexPtr->liveDeltaFlatIndex,
                        queryTrigram.trigram,
                        [&](uint32_t recordIdx) {
                            candidates.push_back(recordIdx);
                        }
                    );

                    std::sort(candidates.begin(), candidates.end());
                    candidates.erase(
                        std::unique(candidates.begin(), candidates.end()),
                        candidates.end()
                    );

                    firstTrigram = false;
                } else {
                    std::vector<uint32_t> trigramRecordIndices;
                    trigramRecordIndices.reserve(queryTrigram.postingCount);

                    forEachRecordIdxForTrigram(
                        indexPtr->trigramRanges,
                        indexPtr->trigramPostings,
                        queryTrigram.trigram,
                        [&](uint32_t recordIdx) {
                            trigramRecordIndices.push_back(recordIdx);
                        }
                    );

                    forEachRecordIdxForTrigram(
                        indexPtr->liveDeltaFlatIndex,
                        queryTrigram.trigram,
                        [&](uint32_t recordIdx) {
                            trigramRecordIndices.push_back(recordIdx);
                        }
                    );

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

            const std::string_view lowercaseName =
                indexPtr->lowercaseRecordName(rec, recordIdx);

            if (lowercaseName.empty()) {
                return;
            }

            if (hasExtensionFilter &&
                !matchesFileExtensionFilter(rec, lowercaseName, extensionFilter)) {
                return;
            }

            std::string_view name;

            if (options.matchCase) {
                if (rec.nameOffset + rec.nameLen > indexPtr->stringPool.size()) {
                    return;
                }

                name = std::string_view(
                    &indexPtr->stringPool[rec.nameOffset],
                    rec.nameLen
                );
            } else {
                name = lowercaseName;
            }

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

#ifdef KERYTHING_ENABLE_LOGGING
    const auto sortStart = Clock::now();
#endif

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
                if (!device ||
                    !device->isReady ||
                    static_cast<uint8_t>(device->generation) != handle.generation ||
                    handle.recordIdx >= device->fileRecords.size() ||
                    device->isDeletedRecord(handle.recordIdx)) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                const std::string_view lowercaseName =
                    device->lowercaseRecordName(record);

                if (!lowercaseName.empty()) {
                    key.nameKey = lowercaseName;
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
                if (!device ||
                    !device->isReady ||
                    static_cast<uint8_t>(device->generation) != handle.generation ||
                    handle.recordIdx >= device->fileRecords.size() ||
                    device->isDeletedRecord(handle.recordIdx)) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                const std::string parentPath =
                    device->getFullPath(record.parentRecordIdx);

                if (handle.mountPointIdx != RecordHandle::NoMountPoint &&
                    handle.mountPointIdx < static_cast<uint8_t>(device->mountPoints.size())) {
                    key.pathKey = device->mountedPathForMountPointIndex(
                        static_cast<int>(handle.mountPointIdx),
                        parentPath
                    ).toStdString();
                } else {
                    key.pathKey = parentPath;
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
                if (!device ||
                    !device->isReady ||
                    static_cast<uint8_t>(device->generation) != handle.generation ||
                    handle.recordIdx >= device->fileRecords.size() ||
                    device->isDeletedRecord(handle.recordIdx)) {
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

#ifdef KERYTHING_ENABLE_LOGGING
    const auto applyStart = Clock::now();
#endif

    scratch.sortedResults.resize(results.size());

    for (std::size_t i = 0; i < resultsOrder.size(); ++i) {
        scratch.sortedResults[i] = std::move(results[resultsOrder[i]]);
    }

    results.swap(scratch.sortedResults);

    // After the swap, scratch.sortedResults owns the old unsorted result buffer.
    // Release it so we keep the fast linear gather path without retaining a
    // second full RecordHandle vector between sorts.
    std::vector<RecordHandle>{}.swap(scratch.sortedResults);

    // Numeric sort keys are only needed while sorting by size/date. Release them
    // after the sorted order has been applied so sorting by date does not retain
    // another full-result-sized scratch buffer.
    std::vector<quint64>{}.swap(scratch.numericKeys);

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "sortSearchResults"
              << " results=" << results.size()
              << " column=" << column
              << " applyOrderMs=" << elapsedMsSince(applyStart)
              << " totalMs=" << elapsedMsSince(sortStart)
              << "\n";
#endif

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