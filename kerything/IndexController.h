// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_INDEXCONTROLLER_H
#define KERYTHING_INDEXCONTROLLER_H

#include <execution>
#include <iostream>
#include <optional>
#include <QObject>
#include <QStringList>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "FileRecord.h"
#include "LiveUpdateEvent.h"

#include <chrono>

class IndexController final : public QObject {
    Q_OBJECT

public:
    explicit IndexController(QObject* parent = nullptr);

    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }

        [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }

        [[nodiscard]] std::size_t operator()(const char* value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct TransparentStringEqual {
        using is_transparent = void;

        [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
        {
            return lhs == rhs;
        }
    };

    using ExtensionSet = std::unordered_set<
        std::string,
        TransparentStringHash,
        TransparentStringEqual
    >;

    struct TrigramEntry {
        uint32_t trigram;
        uint32_t recordIdx;

        // Sorting by trigram first, then recordIdx
        bool operator<(const TrigramEntry& other) const {
            if (trigram != other.trigram) {
                return trigram < other.trigram;
            }

            return recordIdx < other.recordIdx;
        }
    };

    struct TrigramRange {
        uint32_t trigram;
        uint32_t offset;
        uint32_t count;

        bool operator<(const TrigramRange& other) const {
            return trigram < other.trigram;
        }
    };

    struct DeviceIndex {
        quint64 indexId;
        QString deviceId; // stable persistent ID, e.g. partuuid:...
        QString devNode;  // resolved block node, e.g. /dev/nvme0n1p2
        QString fsType;
        quint64 generation = 0;
        bool isReady = false;

        // Runtime search visibility state.
        // Mounted devices are always searchable. Unmounted devices are searchable
        // only when showOfflineResults is enabled.
        bool mounted = false;
        bool showOfflineResults = true;

        [[nodiscard]] bool isSearchable() const noexcept
        {
            return mounted || showOfflineResults;
        }

        // Current mount points for this filesystem. A single indexed device may
        // be mounted at multiple locations.
        QStringList mountPoints;
        QString primaryMountPoint;

        // unix seconds; 0 means unknown
        qint64 lastIndexedTime = 0;

        // display metadata
        QString label;

        std::vector<FileRecord> fileRecords;
        std::vector<char> stringPool;
        std::vector<uint8_t> deletedRecordBitmap;
        std::unordered_map<uint64_t, uint32_t> directoryFsIndexToRecordIdx;

        // Most filesystem indices appear exactly once. Keep the common case as a
        // compact inode -> record index map, and only allocate vectors for true
        // duplicates such as hard links.
        std::unordered_map<uint64_t, uint32_t> fsIndexToPrimaryRecordIdx;
        std::unordered_map<uint64_t, std::vector<uint32_t>> duplicateFsIndexToRecordIndices;
        mutable std::vector<uint32_t> fsIndexLookupScratch;

        // Lowercase mirror of the string pool for sorting and searching
        std::vector<char> lowercaseStringPool;

        // Compact full-scan trigram index.
        //
        // trigramRanges maps trigram -> [offset, count] within trigramPostings.
        // trigramPostings stores sorted record indices for each trigram.
        std::vector<TrigramRange> trigramRanges;
        std::vector<uint32_t> trigramPostings;

        // Trigrams added by live updates after the last full scan.
        //
        // Keeping these separate avoids re-sorting the full trigram index for
        // every create/rename event on busy filesystems.
        std::vector<TrigramEntry> liveDeltaFlatIndex;

        // Lowercase final extension -> record indices.
        //
        // This is append-only for live updates/renames. Stale entries are filtered
        // during query refinement by checking tombstones, file/dir flags, and the
        // record's current final extension.
        std::unordered_map<
            std::string,
            std::vector<uint32_t>,
            TransparentStringHash,
            TransparentStringEqual
        > recordsByExtension;

        // Total stored entries across all extension buckets, including stale
        // live-update entries. Used as a cheap denominator for rebuild heuristics.
        std::size_t extensionIndexEntryCount = 0;

        // Number of live-update extension-index changes since the last clean
        // rebuild. This counts appended entries and tombstoned records that may
        // have left stale references behind.
        std::size_t extensionIndexLiveDeltaEntries = 0;

        [[nodiscard]] bool isDeletedRecord(uint32_t recordIdx) const noexcept
        {
            return recordIdx < deletedRecordBitmap.size() &&
                   deletedRecordBitmap[recordIdx] != 0;
        }

        void markDeletedRecord(uint32_t recordIdx)
        {
            if (recordIdx >= deletedRecordBitmap.size()) {
                deletedRecordBitmap.resize(fileRecords.size(), 0);
            }

            if (recordIdx < deletedRecordBitmap.size()) {
                deletedRecordBitmap[recordIdx] = 1;
            }
        }

        [[nodiscard]] qsizetype markDeletedRecordTree(uint32_t rootRecordIdx, bool* deletedDirectory = nullptr)
        {
            const auto start = std::chrono::steady_clock::now();

            auto logSlowDeleteTree = [&](qsizetype deletedCount) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start
                ).count();

#ifdef KERYTHING_ENABLE_LOGGING
                if (elapsedMs >= 25) {
                    std::cerr << "markDeletedRecordTree rootRecordIdx="
                              << rootRecordIdx
                              << " deletedCount="
                              << deletedCount
                              << " fileRecords="
                              << fileRecords.size()
                              << " elapsed="
                              << elapsedMs
                              << "ms\n";
                }
#endif
            };

            if (deletedDirectory) {
                *deletedDirectory = false;
            }

            if (rootRecordIdx >= fileRecords.size()) {
                return 0;
            }

            if (deletedRecordBitmap.size() < fileRecords.size()) {
                deletedRecordBitmap.resize(fileRecords.size(), 0);
            }

            if (isDeletedRecord(rootRecordIdx)) {
                logSlowDeleteTree(0);
                return 0;
            }

            const FileRecord& rootRecord = fileRecords[rootRecordIdx];

            if ((rootRecord.flags & FileRecord_IsDir) == 0) {
                deletedRecordBitmap[rootRecordIdx] = 1;
                logSlowDeleteTree(1);
                return 1;
            }

            if (deletedDirectory) {
                *deletedDirectory = true;
            }

            qsizetype deletedCount = 0;
            std::vector<uint32_t> stack;
            stack.push_back(rootRecordIdx);

            while (!stack.empty()) {
                const uint32_t currentRecordIdx = stack.back();
                stack.pop_back();

                if (currentRecordIdx >= fileRecords.size()) {
                    continue;
                }

                if (isDeletedRecord(currentRecordIdx)) {
                    continue;
                }

                const FileRecord& currentRecord = fileRecords[currentRecordIdx];

                deletedRecordBitmap[currentRecordIdx] = 1;
                ++deletedCount;

                if ((currentRecord.flags & FileRecord_IsDir) == 0) {
                    continue;
                }

                if (deletedDirectory) {
                    *deletedDirectory = true;
                }

                for (uint32_t childRecordIdx = 0;
                     childRecordIdx < static_cast<uint32_t>(fileRecords.size());
                     ++childRecordIdx) {
                    if (isDeletedRecord(childRecordIdx)) {
                        continue;
                    }

                    const FileRecord& childRecord = fileRecords[childRecordIdx];
                    if (childRecord.parentRecordIdx == currentRecordIdx) {
                        stack.push_back(childRecordIdx);
                    }
                }
            }

            logSlowDeleteTree(deletedCount);
            return deletedCount;
        }

        [[nodiscard]] std::string_view recordName(uint32_t recordIdx) const
        {
            if (recordIdx >= fileRecords.size()) {
                return {};
            }

            const FileRecord& record = fileRecords[recordIdx];
            if (record.nameOffset + record.nameLen > stringPool.size()) {
                return {};
            }

            return std::string_view(&stringPool[record.nameOffset], record.nameLen);
        }

        void rebuildFsIndexMaps() {
            directoryFsIndexToRecordIdx.clear();
            fsIndexToPrimaryRecordIdx.clear();
            duplicateFsIndexToRecordIndices.clear();
            fsIndexLookupScratch.clear();

            std::size_t directoryCount = 0;

            for (const FileRecord& rec : fileRecords) {
                if ((rec.flags & FileRecord_IsDir) != 0) {
                    ++directoryCount;
                }
            }

            directoryFsIndexToRecordIdx.reserve(directoryCount);
            fsIndexToPrimaryRecordIdx.reserve(fileRecords.size());

            for (uint32_t i = 0; i < fileRecords.size(); ++i) {
                if (isDeletedRecord(i)) {
                    continue;
                }

                const FileRecord& rec = fileRecords[i];

                // Full inode/filesystem-index map.
                //
                // The common case is one FileRecord per fsIndex. Additional entries
                // for the same fsIndex are rare and are stored separately to avoid
                // millions of tiny std::vector allocations.
                const auto [primaryIt, inserted] =
                    fsIndexToPrimaryRecordIdx.emplace(rec.fsIndex, i);

                if (!inserted) {
                    duplicateFsIndexToRecordIndices[rec.fsIndex].push_back(i);
                }

                // Directory-only map used for parent resolution.
                // Normal ext4 does not allow arbitrary hard-linked directories, so a
                // single directory inode -> one record index is suitable here.
                if ((rec.flags & FileRecord_IsDir) != 0) {
                    directoryFsIndexToRecordIdx.emplace(rec.fsIndex, i);
                }
            }
        }

        /**
         * @brief Resolves parent pointers for all file records in the current device index.
         * This is called once after the device scan is finished.
         *
         * This method maps the file system parent indices to internal record indices
         * for all entries in the `fileRecords` vector. If a parent index does not
         * exist in the `directoryFsIndexToRecordIdx` map, the corresponding record is marked
         * as belonging to the root by setting its `parentRecordIdx` to `0xFFFFFFFF`.
         *
         * The method is used to establish hierarchical relationships between file
         * records, which is critical for operations that need to navigate or
         * manipulate the directory structure.
         */
        void resolveParentPointers() {
#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Resolving parent pointers..." << std::endl;
#endif

            rebuildFsIndexMaps();

            // Convert parent filesystem indices to internal record indices.
            for (size_t i = 0; i < fileRecords.size(); ++i) {
                FileRecord& rec = fileRecords[i];

                // Root/self-parent safety.
                if (rec.fsIndex == rec.parentFsIndex) {
                    rec.parentRecordIdx = 0xFFFFFFFF;
                    continue;
                }

                auto it = directoryFsIndexToRecordIdx.find(rec.parentFsIndex);
                if (it != directoryFsIndexToRecordIdx.end()) {
                    rec.parentRecordIdx = it->second;
                } else {
                    // If parent is not in our DB, mark as root.
                    rec.parentRecordIdx = 0xFFFFFFFF;
                }
            }
        }

        void buildLowercaseStringPool() {
            // Reserve memory once
            lowercaseStringPool.resize(stringPool.size());

            // Convert the string pool to lowercase in parallel and store it in lowercaseStringPool
            std::transform(std::execution::par_unseq, stringPool.begin(), stringPool.end(),
                      lowercaseStringPool.begin(),
                           [](unsigned char c) {
                               return (c >= 'A' && c <= 'Z') ? (c | 32) : c;
                           });
        }

        void sortByNameAscendingParallel() {
            if (fileRecords.empty()) {
                return;
            }

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Pre-sorting records by name ascending (case-insensitive) in parallel..." << std::endl;
#endif

            auto compareCaseInsensitive = [&](const FileRecord& a, const FileRecord& b) {
                std::string_view s1(&lowercaseStringPool[a.nameOffset], a.nameLen);
                std::string_view s2(&lowercaseStringPool[b.nameOffset], b.nameLen);

                return std::lexicographical_compare(
                    s1.begin(), s1.end(),
                    s2.begin(), s2.end(),
                    [](char first, char second) {
                        return first < second;
                    }
                );
            };

            // NOTE: We cannot simply sort 'records' because parentRecordIdx
            // depends on the original vector indices. We must re-map them.

            // 1. Create an index mapping
            std::vector<uint32_t> p(fileRecords.size());
            std::iota(p.begin(), p.end(), 0);

            // 2. Sort the index mapping based on names
            std::sort(std::execution::par, p.begin(), p.end(), [&](uint32_t i, uint32_t j) {
                return compareCaseInsensitive(fileRecords[i], fileRecords[j]);
            });

            // // 3. Create a reverse mapping to update parentRecordIdx
            // std::vector<uint32_t> rev(p.size());
            // for (uint32_t i = 0; i < p.size(); ++i) {
            //     rev[p[i]] = i;
            // }
            //
            // // 4. Reorder records and update parent indices
            // std::vector<FileRecord> newRecords(fileRecords.size());
            // for (size_t i = 0; i < p.size(); ++i) {
            //     newRecords[i] = fileRecords[p[i]];
            //     if (newRecords[i].parentRecordIdx != 0xFFFFFFFF) {
            //         newRecords[i].parentRecordIdx = rev[newRecords[i].parentRecordIdx];
            //     }
            // }

            // 3. Reorder records. parentRecordIdx is intentionally not remapped here,
            // because parent pointers are resolved after this sort.
            std::vector<FileRecord> newRecords(fileRecords.size());
            for (size_t i = 0; i < p.size(); ++i) {
                newRecords[i] = fileRecords[p[i]];
                newRecords[i].parentRecordIdx = 0xFFFFFFFF;
            }

            fileRecords = std::move(newRecords);
        }

        void buildTrigramIndexParallel() {
#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Building compact trigram index in parallel..." << std::endl;
#endif

            // Clear live delta trigram index.
            liveDeltaFlatIndex.clear();

            // 1. Calculate how many trigrams we'll have in total to avoid reallocations
            // (Roughly: sum of all filename lengths - 2)
            size_t totalTrigrams = 0;

            for (const auto& rec : fileRecords) {
                if (rec.nameLen >= 3) {
                    totalTrigrams += (rec.nameLen - 2);
                }
            }

            std::vector<TrigramEntry> flatEntries;
            flatEntries.resize(totalTrigrams);

            // 2. Fill the temporary flat index in parallel.
            const size_t numRecords = fileRecords.size();
            std::vector<size_t> startOffsets(numRecords);

            // This part is serial but very fast (calculating where each record starts in flatEntries)
            size_t currentOffset = 0;
            for (size_t i = 0; i < numRecords; ++i) {
                startOffsets[i] = currentOffset;

                if (fileRecords[i].nameLen >= 3) {
                    currentOffset += (fileRecords[i].nameLen - 2);
                }
            }

            std::vector<uint32_t> workIndices(numRecords);
            std::iota(workIndices.begin(), workIndices.end(), 0);

            std::for_each(std::execution::par, workIndices.begin(), workIndices.end(), [&](uint32_t i) {
                const auto& rec = fileRecords[i];
                if (rec.nameLen < 3) {
                    return;
                }

                std::string_view name(&lowercaseStringPool[rec.nameOffset], rec.nameLen);
                size_t writePos = startOffsets[i];

                for (size_t j = 0; j <= name.length() - 3; ++j) {
                    uint32_t tri = (static_cast<uint32_t>(static_cast<unsigned char>(name[j])) << 16) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(name[j + 1])) << 8) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(name[j + 2])));

                    flatEntries[writePos++] = { tri, i };
                }
            });

            // 3. Sort the entire index in parallel
#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Sorting " << flatEntries.size() << " trigrams..." << std::endl;
#endif

            std::sort(std::execution::par, flatEntries.begin(), flatEntries.end());

            // 4. Remove exact duplicates (same trigram in same file)
#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Removing duplicate trigrams..." << std::endl;
#endif

            auto last = std::unique(std::execution::par, flatEntries.begin(), flatEntries.end(), [](const auto& a, const auto& b) {
                return a.trigram == b.trigram && a.recordIdx == b.recordIdx;
            });
            flatEntries.erase(last, flatEntries.end());

            // 5. Compress the trigram index
#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Compressing trigram index..." << std::endl;
#endif

            trigramRanges.clear();
            trigramPostings.clear();

            if (!flatEntries.empty()) {
                trigramPostings.reserve(flatEntries.size());

                std::size_t i = 0;

                while (i < flatEntries.size()) {
                    const uint32_t trigram = flatEntries[i].trigram;
                    const uint32_t offset = static_cast<uint32_t>(trigramPostings.size());

                    do {
                        trigramPostings.push_back(flatEntries[i].recordIdx);
                        ++i;
                    } while (i < flatEntries.size() && flatEntries[i].trigram == trigram);

                    const uint32_t count =
                        static_cast<uint32_t>(trigramPostings.size() - offset);

                    trigramRanges.push_back({
                        trigram,
                        offset,
                        count
                    });
                }
            }

            // 6. Reclaim memory
            trigramRanges.shrink_to_fit();
            trigramPostings.shrink_to_fit();

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Finished compact trigram index"
                      << " ranges=" << trigramRanges.size()
                      << " postings=" << trigramPostings.size()
                      << "\n";
#endif
        }

        void buildExtensionIndex()
        {
            recordsByExtension.clear();
            extensionIndexEntryCount = 0;

            std::size_t fileCountWithExtension = 0;

            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                if (isDeletedRecord(recordIdx)) {
                    continue;
                }

                const FileRecord& record = fileRecords[recordIdx];

                if ((record.flags & FileRecord_IsDir) != 0) {
                    continue;
                }

                if (record.nameOffset + record.nameLen > lowercaseStringPool.size()) {
                    continue;
                }

                const std::string_view name(
                    &lowercaseStringPool[record.nameOffset],
                    record.nameLen
                );

                if (!finalExtension(name).empty()) {
                    ++fileCountWithExtension;
                }
            }

            recordsByExtension.reserve(std::min<std::size_t>(
                fileCountWithExtension,
                4096
            ));

            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                addRecordToExtensionIndex(recordIdx);
            }

            extensionIndexLiveDeltaEntries = 0;

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Built extension index"
                      << " extensions=" << recordsByExtension.size()
                      << " indexedRecords=" << extensionIndexEntryCount
                      << "\n";
#endif
        }

        bool addRecordToExtensionIndex(uint32_t recordIdx)
        {
            if (recordIdx >= fileRecords.size()) {
                return false;
            }

            if (isDeletedRecord(recordIdx)) {
                return false;
            }

            const FileRecord& record = fileRecords[recordIdx];

            if ((record.flags & FileRecord_IsDir) != 0) {
                return false;
            }

            if (record.nameOffset + record.nameLen > lowercaseStringPool.size()) {
                return false;
            }

            const std::string_view name(
                &lowercaseStringPool[record.nameOffset],
                record.nameLen
            );

            const std::string_view extension = finalExtension(name);

            if (extension.empty()) {
                return false;
            }

            recordsByExtension[std::string(extension)].push_back(recordIdx);
            ++extensionIndexEntryCount;
            return true;
        }

        [[nodiscard]] const std::vector<uint32_t>* recordIndicesForExtension(
            std::string_view extension
        ) const {
            const auto it = recordsByExtension.find(extension);
            if (it == recordsByExtension.end()) {
                return nullptr;
            }

            return &it->second;
        }

        [[nodiscard]] std::string getFullPath(const uint32_t recordIdx) const {
            std::vector<uint32_t> chain;
            uint32_t current = recordIdx;
            size_t totalLength = 0;

            static constexpr std::string_view rootPath = "/";
            static constexpr std::string_view oneDot = ".";
            static constexpr std::string_view twoDots = "..";

            // 1. Identify the chain of parents that need resolving
            // STOP if we hit:
            // - The root marker (0xFFFFFFFF)
            // - A record that points to itself (some filesystems do this)
            while (current != 0xFFFFFFFF) {
                const auto& r = fileRecords[current];
                std::string_view name(&stringPool[r.nameOffset], r.nameLen);

                // Only count length if it's not a dot-entry and not blank
                if (name != oneDot && name != twoDots && !name.empty()) {
                    chain.push_back(current);
                    totalLength += 1; // For the "/" separator
                    totalLength += r.nameLen;
                }

                uint32_t next = r.parentRecordIdx;

                if (next == current) {
                    break; // Self-reference safety
                }

                current = next;
            }

            if (chain.empty()) {
                return std::string(rootPath);
            }

            // 2. Pre-allocate the exact size
            std::string base;
            base.reserve(totalLength);

            // 3. Build paths from top to bottom
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                uint32_t idx = *it;
                const auto& r = fileRecords[idx];
                std::string_view name(&stringPool[r.nameOffset], r.nameLen);

                // If the first element is the root "/", we don't want to double-up
                // but typically in MFT, the root is just an empty name or a specific index.
                // This check handles the edge case where the first entry is already "/"
                if (name == rootPath && base.empty()) {
                    base = rootPath;
                    continue;
                }

                // Build the string
                base += rootPath;
                base += name;
            }

            return base;
        }

        [[nodiscard]] const std::vector<uint32_t>* recordIndicesForFsIndex(uint64_t fsIndex) const {
            const auto primaryIt = fsIndexToPrimaryRecordIdx.find(fsIndex);
            if (primaryIt == fsIndexToPrimaryRecordIdx.end()) {
                return nullptr;
            }

            fsIndexLookupScratch.clear();
            fsIndexLookupScratch.push_back(primaryIt->second);

            const auto duplicateIt = duplicateFsIndexToRecordIndices.find(fsIndex);
            if (duplicateIt != duplicateFsIndexToRecordIndices.end()) {
                fsIndexLookupScratch.insert(
                    fsIndexLookupScratch.end(),
                    duplicateIt->second.begin(),
                    duplicateIt->second.end()
                );
            }

            return &fsIndexLookupScratch;
        }
    };

    struct RecordHandle {
        uint32_t indexId;
        uint32_t generation;
        uint32_t recordIdx;

        // Index into DeviceIndex::mountPoints.
        // 0xFFFFFFFF means no current mount point is available.
        uint32_t mountPointIdx = 0xFFFFFFFF;
    };

    struct SortScratch {
        std::vector<uint32_t> resultsOrder;
        std::vector<RecordHandle> sortedResults;
        std::vector<quint64> numericKeys;

        void clearAndRelease()
        {
            std::vector<uint32_t>{}.swap(resultsOrder);
            std::vector<RecordHandle>{}.swap(sortedResults);
            std::vector<quint64>{}.swap(numericKeys);
        }
    };

    struct ParsedSearchQuery {
        std::vector<std::string> keywords;
        ExtensionSet extensions;
        bool foldersOnly = false;
    };

    struct SearchOptions {
        bool matchCase = false;
        bool matchWholeWord = false;
    };

    struct LiveUpdateApplyResult {
        qsizetype metadataChanged = 0;
        qsizetype upserted = 0;
        qsizetype deleted = 0;
        qsizetype needsRescan = 0;
        qsizetype unsupported = 0;
        qsizetype missingDevice = 0;
        qsizetype missingInode = 0;
        qsizetype missingParent = 0;
        qsizetype missingEntry = 0;
    };

    const DeviceIndex* deviceIndex(quint64 indexId) const;

    quint64 addDevice(
        const QString& deviceId,
        const QString& devNode,
        const QString& fsType,
        const QString& label,
        const QStringList& mountPoints,
        const QString& primaryMountPoint,
        quint32 requestId
    );
    void removeDeviceByIndexId(quint64 indexId);
    bool removeDeviceByDeviceId(const QString& deviceId);
    bool removeDeviceByRequestId(quint32 requestId);
    void appendDeviceFileRecordsByRequestId(quint32 requestId, const std::vector<FileRecord>& records);
    void appendDeviceStringPoolByRequestId(quint32 requestId, QByteArrayView stringPool);
    bool removeRequestId(quint32 requestId);
    bool updateDeviceRuntimeStateByDeviceId(
        const QString& deviceId,
        bool mounted,
        bool showOfflineResults,
        const QStringList& mountPoints = {},
        const QString& primaryMountPoint = {}
    );
    std::vector<RecordHandle> performTrigramSearch(const std::string& query, SearchOptions options);
    std::vector<RecordHandle> sortSearchResults(std::vector<RecordHandle> results, int column, Qt::SortOrder sortOrder) const;
    std::vector<RecordHandle> sortSearchResults(
        std::vector<RecordHandle> results,
        int column,
        Qt::SortOrder sortOrder,
        SortScratch& scratch
    ) const;
    void resolveParentPointersByRequestId(quint32 requestId);
    void buildLowercaseStringPoolByRequestId(quint32 requestId);
    void sortByNameAscendingParallelByRequestId(quint32 requestId);
    void buildTrigramIndexParallelByRequestId(quint32 requestId);
    void buildExtensionIndexByRequestId(quint32 requestId);
    void setReadyState(quint32 requestId, bool isReady);
    [[nodiscard]] QString memoryStatsText() const;
    LiveUpdateApplyResult applyLiveUpdateOperations(
        const QString& deviceId,
        const std::vector<LiveUpdateOperation>& operations
    );

    template <typename Fn>
    auto withDeviceIndexRead(quint64 indexId, Fn&& fn) const
        -> std::invoke_result_t<Fn, const DeviceIndex*> {
        std::shared_lock lock(indexMutex_);

        const auto it = indexByIndexId_.find(indexId);
        if (it == indexByIndexId_.end()) {
            return std::invoke_result_t<Fn, const DeviceIndex*>{};
        }

        return std::forward<Fn>(fn)(it->second.get());
    }

    /**
     * @brief Case-insensitive substring helper.
     */
    static bool contains(std::string_view haystack, std::string_view needle);

    static ParsedSearchQuery parseSearchQuery(std::string_view query);
    static std::string normalizeExtensionToken(std::string_view extension);
    static std::string_view finalExtension(std::string_view lowercaseFileName);
    static bool matchesFileExtensionFilter(
        const FileRecord& record,
        std::string_view lowercaseFileName,
        const ExtensionSet& extensions
    );

    /**
     * @brief Returns the current upper bound for displayed search-result handles.
     *
     * This includes only ready/searchable indexes and accounts for mount-point
     * multiplication, because one indexed record can produce one row per mount point.
     */
    [[nodiscard]] std::size_t maxSearchResultCount() const;

Q_SIGNALS:
    void deviceRemoved(quint64 indexId);

private:
    enum class UpsertApplyResult : quint8 {
        Applied,
        MissingParent,
        NeedsRescan,
        Invalid,
        NotUpsert
    };

    bool removeDeviceByIndexIdUnlocked(quint64 indexId);
    static quint8 fileRecordFlagsFromLiveUpdateOperation(const LiveUpdateOperation& operation);
    static void updateFileRecordMetadataFromLiveUpdateOperation(
        FileRecord& record,
        const LiveUpdateOperation& operation
    );
    static bool appendTrigramsForRecord(
        DeviceIndex& deviceIndex,
        uint32_t recordIdx,
        std::vector<TrigramEntry>& targetIndex
    );
    static bool shouldRebuildTrigramIndexAfterLiveUpdates(const DeviceIndex& deviceIndex);
    static void rebuildTrigramIndexAfterLiveUpdates(DeviceIndex& deviceIndex);
    static void sortLiveUpdateTrigramIndex(DeviceIndex& deviceIndex);
    static void addRecordToExtensionIndexIfApplicable(
        DeviceIndex& deviceIndex,
        uint32_t recordIdx
    );
    static bool shouldRebuildExtensionIndexAfterLiveUpdates(const DeviceIndex& deviceIndex);
    static void rebuildExtensionIndexAfterLiveUpdates(DeviceIndex& deviceIndex);
    static bool appendRecordFromLiveUpdateOperation(
        DeviceIndex& deviceIndex,
        const LiveUpdateOperation& operation
    );
    static std::optional<uint32_t> findLiveEntryRecord(
        const DeviceIndex& deviceIndex,
        quint64 parentInode,
        const QByteArray& nameUtf8,
        quint64 inode = 0
    );
    static bool updateRecordIdentityFromLiveUpdateOperation(
        DeviceIndex& deviceIndex,
        uint32_t recordIdx,
        const LiveUpdateOperation& operation
    );
    static UpsertApplyResult applyUpsertOperation(
        DeviceIndex& deviceIndex,
        const LiveUpdateOperation& operation
    );

    quint64 nextIndexId_ = 1;

    mutable std::shared_mutex indexMutex_;

    // indexId -> in-memory index
    std::unordered_map<quint64, std::unique_ptr<DeviceIndex>> indexByIndexId_;

    // devNode -> in-memory index
    std::unordered_map<QString, quint64> indexIdByDevNode_;

    // requestId -> indexId
    // To go from requestId -> in-memory index, use:
    // requestId -> indexId -> in-memory index
    std::unordered_map<quint64, quint64> indexIdByRequestId_;
};

#endif //KERYTHING_INDEXCONTROLLER_H
