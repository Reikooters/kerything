// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_INDEXCONTROLLER_H
#define KERYTHING_INDEXCONTROLLER_H

#include <execution>
#include <iostream>
#include <limits>
#include <optional>
#include <QDir>
#include <QObject>
#include <QStringList>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "BlockDevice.h"
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
        std::vector<BlockDeviceMountInfo> mounts;

        // unix seconds; 0 means unknown
        qint64 lastIndexedTime = 0;

        // display metadata
        QString label;

        std::vector<FileRecord> fileRecords;
        std::vector<FileRecordNamespace> fileRecordNamespaces;
        std::vector<char> stringPool;
        std::vector<uint64_t> deletedRecordBits;

        [[nodiscard]] bool hasFileRecordNamespaces() const noexcept
        {
            return fileRecordNamespaces.size() == fileRecords.size();
        }

        [[nodiscard]] FileRecordNamespace namespaceForRecord(uint32_t recordIdx) const noexcept
        {
            if (recordIdx >= fileRecordNamespaces.size()) {
                return {};
            }

            return fileRecordNamespaces[recordIdx];
        }

        [[nodiscard]] bool isBtrfs() const noexcept
        {
            return fsType.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0;
        }

        [[nodiscard]] bool usesNamespaceAwareMountExpansion() const noexcept
        {
            return isBtrfs() && hasFileRecordNamespaces();
        }

        [[nodiscard]] bool mountCanExposeRecord(uint32_t recordIdx, int mountIndex) const noexcept
        {
            if (mountIndex < 0 || mountIndex >= static_cast<int>(mounts.size())) {
                return false;
            }

            if (!usesNamespaceAwareMountExpansion()) {
                return true;
            }

            const FileRecordNamespace namespaceEntry = namespaceForRecord(recordIdx);
            const BlockDeviceMountInfo& mount = mounts[static_cast<std::size_t>(mountIndex)];

            return mount.btrfsRootId != 0 &&
                   mount.btrfsRootId == namespaceEntry.fsNamespace;
        }

        [[nodiscard]] int mountInfoIndexForMountPointIndex(int mountPointIndex) const
        {
            if (mountPointIndex < 0 || mountPointIndex >= mountPoints.size()) {
                return -1;
            }

            const QString mountPoint = mountPoints.at(mountPointIndex);

            for (std::size_t i = 0; i < mounts.size(); ++i) {
                if (mounts[i].mountPoint == mountPoint) {
                    return static_cast<int>(i);
                }
            }

            return -1;
        }

        template <typename Fn>
        void forEachVisibleMountPointForRecord(uint32_t recordIdx, Fn&& fn) const
        {
            const int mountPointCount = std::min<int>(
                mountPoints.size(),
                std::numeric_limits<uint8_t>::max()
            );

            for (int mountPointIdx = 0; mountPointIdx < mountPointCount; ++mountPointIdx) {
                if (usesNamespaceAwareMountExpansion()) {
                    const int mountInfoIndex =
                        mountInfoIndexForMountPointIndex(mountPointIdx);

                    if (!mountCanExposeRecord(recordIdx, mountInfoIndex)) {
                        continue;
                    }
                }

                fn(mountPointIdx);
            }
        }

        [[nodiscard]] std::size_t mountedResultMultiplicity(uint32_t recordIdx) const
        {
            if (recordIdx >= fileRecords.size()) {
                return 0;
            }

            if (isDeletedRecord(recordIdx)) {
                return 0;
            }

            if (mountPoints.isEmpty()) {
                return 1;
            }

            if (!usesNamespaceAwareMountExpansion()) {
                return static_cast<std::size_t>(
                    std::min<int>(
                        mountPoints.size(),
                        std::numeric_limits<uint8_t>::max()
                    )
                );
            }

            std::size_t count = 0;

            forEachVisibleMountPointForRecord(
                recordIdx,
                [&count](int mountPointIdx) {
                    Q_UNUSED(mountPointIdx);
                    ++count;
                }
            );

            return count;
        }

        [[nodiscard]] QString mountedPathForMountPointIndex(
            int mountPointIndex,
            const std::string& filesystemPath
        ) const {
            if (mountPointIndex < 0 || mountPointIndex >= mountPoints.size()) {
                return QDir::cleanPath(QString::fromStdString(filesystemPath));
            }

            const QString mountPoint = mountPoints.at(mountPointIndex);
            const QString relativePath =
                QDir::cleanPath(QString::fromStdString(filesystemPath));

            if (relativePath == QStringLiteral("/")) {
                return QDir::cleanPath(mountPoint);
            }

            return QDir::cleanPath(mountPoint + relativePath);
        }

        enum class FsIndexRefStorage : uint8_t {
            UInt32,
            UInt64
        };

        struct FsIndexRecordRef32 {
            uint32_t fsIndex = 0;
            uint32_t recordIdx = 0;

            [[nodiscard]] bool operator<(const FsIndexRecordRef32& other) const noexcept
            {
                if (fsIndex != other.fsIndex) {
                    return fsIndex < other.fsIndex;
                }

                return recordIdx < other.recordIdx;
            }
        };

        struct FsIndexRecordRef64 {
            uint64_t fsIndex = 0;
            uint32_t recordIdx = 0;

            [[nodiscard]] bool operator<(const FsIndexRecordRef64& other) const noexcept
            {
                if (fsIndex != other.fsIndex) {
                    return fsIndex < other.fsIndex;
                }

                return recordIdx < other.recordIdx;
            }
        };

        struct NamespacedFsIndexRecordRef {
            uint64_t fsNamespace = 0;
            uint64_t fsIndex = 0;
            uint32_t recordIdx = 0;

            [[nodiscard]] bool operator<(const NamespacedFsIndexRecordRef& other) const noexcept
            {
                if (fsNamespace != other.fsNamespace) {
                    return fsNamespace < other.fsNamespace;
                }

                if (fsIndex != other.fsIndex) {
                    return fsIndex < other.fsIndex;
                }

                return recordIdx < other.recordIdx;
            }
        };

        // Full-scan fs-index refs use the narrowest key width that can safely
        // represent all live records in this device. ext4 normally uses UInt32;
        // NTFS and any filesystem with wider identifiers use UInt64.
        FsIndexRefStorage fsIndexRefStorage = FsIndexRefStorage::UInt64;

        // Directory inode -> record-index references, sorted by fsIndex.
        std::vector<FsIndexRecordRef32> directoryFsIndexRecordRefs32;
        std::vector<FsIndexRecordRef64> directoryFsIndexRecordRefs64;

        // Directory refs added by live updates after the last full rebuild.
        std::vector<FsIndexRecordRef32> liveDirectoryFsIndexRecordRefs32;
        std::vector<FsIndexRecordRef64> liveDirectoryFsIndexRecordRefs64;

        // Full-scan inode -> record-index references, sorted by fsIndex then recordIdx.
        std::vector<FsIndexRecordRef32> fsIndexRecordRefs32;
        std::vector<FsIndexRecordRef64> fsIndexRecordRefs64;

        // Inode refs added by live updates after the last full rebuild.
        std::vector<FsIndexRecordRef32> liveFsIndexRecordRefs32;
        std::vector<FsIndexRecordRef64> liveFsIndexRecordRefs64;

        // Full-scan namespace-aware refs used by filesystems where fsIndex is
        // only unique inside another filesystem namespace. Btrfs uses these for
        // (root/subvolume id, objectid) lookups.
        //
        // These vectors are populated only when fileRecordNamespaces has one
        // entry per FileRecord. Non-Btrfs indexes keep them empty, so they do
        // not add per-record memory cost for EXT4/NTFS.
        std::vector<NamespacedFsIndexRecordRef> namespacedDirectoryFsIndexRecordRefs;
        std::vector<NamespacedFsIndexRecordRef> namespacedFsIndexRecordRefs;

        mutable std::vector<uint32_t> fsIndexLookupScratch;

        // Lowercase mirror of names that require ASCII case folding.
        //
        // lowercaseNameOffsetByRecord[recordIdx] == NoLowercaseNameOverride means
        // the original stringPool name already contains no ASCII uppercase bytes
        // and can be used directly for case-insensitive search/sort.
        static constexpr uint32_t NoLowercaseNameOverride = 0xFFFFFFFF;

        std::vector<char> lowercaseStringPool;
        std::vector<uint32_t> lowercaseNameOffsetByRecord;

        // Compact full-scan trigram index.
        //
        // trigramRanges maps trigram -> [byte offset, decoded count] within
        // trigramPostings.
        //
        // trigramPostings stores each trigram's sorted record indices as:
        //   first recordIdx as unsigned varint,
        //   then recordIdx deltas as unsigned varints.
        std::vector<TrigramRange> trigramRanges;
        std::vector<uint8_t> trigramPostings;

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

        [[nodiscard]] static std::size_t deletedRecordWordCount(std::size_t recordCount) noexcept
        {
            return (recordCount + 63) / 64;
        }

        void resizeDeletedRecordBitsForRecordCount(std::size_t recordCount)
        {
            deletedRecordBits.resize(deletedRecordWordCount(recordCount), 0);
        }

        void reserveDeletedRecordBitsForRecordCount(std::size_t recordCount)
        {
            deletedRecordBits.reserve(deletedRecordWordCount(recordCount));
        }

        void compactDeletedRecordBits(std::size_t slackRecords = 4096)
        {
            const std::size_t requiredWords =
                deletedRecordWordCount(fileRecords.size());

            const std::size_t slackWords =
                deletedRecordWordCount(slackRecords);

            const std::size_t targetCapacity =
                requiredWords + slackWords;

            if (deletedRecordBits.capacity() <= targetCapacity) {
                return;
            }

            std::vector<uint64_t> compacted;
            compacted.reserve(targetCapacity);
            compacted.assign(
                deletedRecordBits.begin(),
                deletedRecordBits.begin() + static_cast<std::ptrdiff_t>(
                    std::min(requiredWords, deletedRecordBits.size())
                )
            );

            deletedRecordBits.swap(compacted);
        }

        [[nodiscard]] bool isDeletedRecord(uint32_t recordIdx) const noexcept
        {
            const std::size_t wordIdx = recordIdx / 64;

            if (wordIdx >= deletedRecordBits.size()) {
                return false;
            }

            const uint64_t mask = uint64_t{1} << (recordIdx % 64);
            return (deletedRecordBits[wordIdx] & mask) != 0;
        }

        void markDeletedRecord(uint32_t recordIdx)
        {
            const std::size_t wordIdx = recordIdx / 64;

            if (wordIdx >= deletedRecordBits.size()) {
                resizeDeletedRecordBitsForRecordCount(fileRecords.size());
            }

            if (wordIdx < deletedRecordBits.size()) {
                const uint64_t mask = uint64_t{1} << (recordIdx % 64);
                deletedRecordBits[wordIdx] |= mask;
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

            resizeDeletedRecordBitsForRecordCount(fileRecords.size());

            if (isDeletedRecord(rootRecordIdx)) {
                logSlowDeleteTree(0);
                return 0;
            }

            const FileRecord& rootRecord = fileRecords[rootRecordIdx];

            if ((rootRecord.flags & FileRecord_IsDir) == 0) {
                markDeletedRecord(rootRecordIdx);
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

                markDeletedRecord(currentRecordIdx);
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

        [[nodiscard]] std::string_view lowercaseRecordName(uint32_t recordIdx) const
        {
            if (recordIdx >= fileRecords.size()) {
                return {};
            }

            return lowercaseRecordName(fileRecords[recordIdx], recordIdx);
        }

        [[nodiscard]] std::string_view lowercaseRecordName(
            const FileRecord& record,
            uint32_t recordIdx
        ) const {
            if (recordIdx >= lowercaseNameOffsetByRecord.size()) {
                return {};
            }

            const uint32_t lowercaseOffset = lowercaseNameOffsetByRecord[recordIdx];

            if (lowercaseOffset == NoLowercaseNameOverride) {
                if (record.nameOffset + record.nameLen > stringPool.size()) {
                    return {};
                }

                return std::string_view(
                    &stringPool[record.nameOffset],
                    record.nameLen
                );
            }

            if (lowercaseOffset + record.nameLen > lowercaseStringPool.size()) {
                return {};
            }

            return std::string_view(
                &lowercaseStringPool[lowercaseOffset],
                record.nameLen
            );
        }

        [[nodiscard]] std::string_view lowercaseRecordName(const FileRecord& record) const
        {
            const auto recordPtr = &record;
            const auto begin = fileRecords.data();
            const auto end = begin + fileRecords.size();

            if (recordPtr < begin || recordPtr >= end) {
                return {};
            }

            const uint32_t recordIdx = static_cast<uint32_t>(recordPtr - begin);
            return lowercaseRecordName(record, recordIdx);
        }

        static constexpr uint64_t MaxUInt32FsIndex =
            std::numeric_limits<uint32_t>::max();

        [[nodiscard]] static bool fsIndexFitsUInt32(uint64_t fsIndex) noexcept
        {
            return fsIndex <= MaxUInt32FsIndex;
        }

        template <typename Ref>
        static void sortAndDeduplicateFsIndexRecordRefs(
            std::vector<Ref>& refs
        ) {
            std::sort(refs.begin(), refs.end());

            refs.erase(
                std::unique(
                    refs.begin(),
                    refs.end(),
                    [](const Ref& lhs, const Ref& rhs) {
                        return lhs.fsIndex == rhs.fsIndex &&
                               lhs.recordIdx == rhs.recordIdx;
                    }
                ),
                refs.end()
            );
        }

        [[nodiscard]] bool allLiveFsIndexesFitUInt32() const noexcept
        {
            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                if (isDeletedRecord(recordIdx)) {
                    continue;
                }

                const FileRecord& record = fileRecords[recordIdx];

                if (!fsIndexFitsUInt32(record.fsIndex) ||
                    !fsIndexFitsUInt32(record.parentFsIndex)) {
                    return false;
                }
            }

            return true;
        }

        void clearFsIndexRefStorage()
        {
            directoryFsIndexRecordRefs32.clear();
            directoryFsIndexRecordRefs64.clear();
            liveDirectoryFsIndexRecordRefs32.clear();
            liveDirectoryFsIndexRecordRefs64.clear();

            fsIndexRecordRefs32.clear();
            fsIndexRecordRefs64.clear();
            liveFsIndexRecordRefs32.clear();
            liveFsIndexRecordRefs64.clear();

            namespacedDirectoryFsIndexRecordRefs.clear();
            namespacedFsIndexRecordRefs.clear();

            fsIndexLookupScratch.clear();
        }

        void upgradeFsIndexRefStorageToUInt64()
        {
            if (fsIndexRefStorage == FsIndexRefStorage::UInt64) {
                return;
            }

            directoryFsIndexRecordRefs64.reserve(directoryFsIndexRecordRefs32.size());
            for (const FsIndexRecordRef32& ref : directoryFsIndexRecordRefs32) {
                directoryFsIndexRecordRefs64.push_back({
                    ref.fsIndex,
                    ref.recordIdx
                });
            }

            liveDirectoryFsIndexRecordRefs64.reserve(liveDirectoryFsIndexRecordRefs32.size());
            for (const FsIndexRecordRef32& ref : liveDirectoryFsIndexRecordRefs32) {
                liveDirectoryFsIndexRecordRefs64.push_back({
                    ref.fsIndex,
                    ref.recordIdx
                });
            }

            fsIndexRecordRefs64.reserve(fsIndexRecordRefs32.size());
            for (const FsIndexRecordRef32& ref : fsIndexRecordRefs32) {
                fsIndexRecordRefs64.push_back({
                    ref.fsIndex,
                    ref.recordIdx
                });
            }

            liveFsIndexRecordRefs64.reserve(liveFsIndexRecordRefs32.size());
            for (const FsIndexRecordRef32& ref : liveFsIndexRecordRefs32) {
                liveFsIndexRecordRefs64.push_back({
                    ref.fsIndex,
                    ref.recordIdx
                });
            }

            std::vector<FsIndexRecordRef32>{}.swap(directoryFsIndexRecordRefs32);
            std::vector<FsIndexRecordRef32>{}.swap(liveDirectoryFsIndexRecordRefs32);
            std::vector<FsIndexRecordRef32>{}.swap(fsIndexRecordRefs32);
            std::vector<FsIndexRecordRef32>{}.swap(liveFsIndexRecordRefs32);

            fsIndexRefStorage = FsIndexRefStorage::UInt64;
        }

        void ensureFsIndexRefStorageCanStore(uint64_t fsIndex)
        {
            if (fsIndexRefStorage == FsIndexRefStorage::UInt32 &&
                !fsIndexFitsUInt32(fsIndex)) {
                upgradeFsIndexRefStorageToUInt64();
            }
        }

        void ensureFsIndexRefStorageCanStore(uint64_t fsIndex, uint64_t parentFsIndex)
        {
            ensureFsIndexRefStorageCanStore(fsIndex);
            ensureFsIndexRefStorageCanStore(parentFsIndex);
        }

        template <typename Ref>
        [[nodiscard]] std::optional<uint32_t> directoryRecordIdxForFsIndexInRefs(
            const std::vector<Ref>& refs,
            uint64_t fsIndex
        ) const {
            if constexpr (std::is_same_v<Ref, FsIndexRecordRef32>) {
                if (!fsIndexFitsUInt32(fsIndex)) {
                    return std::nullopt;
                }
            }

            auto validDirectoryRecord = [&](uint32_t recordIdx) -> std::optional<uint32_t> {
                if (recordIdx >= fileRecords.size()) {
                    return std::nullopt;
                }

                if (isDeletedRecord(recordIdx)) {
                    return std::nullopt;
                }

                const FileRecord& record = fileRecords[recordIdx];

                if ((record.flags & FileRecord_IsDir) == 0) {
                    return std::nullopt;
                }

                return recordIdx;
            };

            const auto searchKey = Ref{
                static_cast<decltype(Ref::fsIndex)>(fsIndex),
                0
            };

            const auto it = std::lower_bound(
                refs.begin(),
                refs.end(),
                searchKey,
                [](const Ref& lhs, const Ref& rhs) {
                    return lhs.fsIndex < rhs.fsIndex;
                }
            );

            if (it != refs.end() &&
                static_cast<uint64_t>(it->fsIndex) == fsIndex) {
                return validDirectoryRecord(it->recordIdx);
            }

            return std::nullopt;
        }

        template <typename Ref>
        [[nodiscard]] std::optional<uint32_t> liveDirectoryRecordIdxForFsIndexInRefs(
            const std::vector<Ref>& refs,
            uint64_t fsIndex
        ) const {
            if constexpr (std::is_same_v<Ref, FsIndexRecordRef32>) {
                if (!fsIndexFitsUInt32(fsIndex)) {
                    return std::nullopt;
                }
            }

            for (const Ref& ref : refs) {
                if (static_cast<uint64_t>(ref.fsIndex) != fsIndex) {
                    continue;
                }

                if (ref.recordIdx >= fileRecords.size()) {
                    continue;
                }

                if (isDeletedRecord(ref.recordIdx)) {
                    continue;
                }

                const FileRecord& record = fileRecords[ref.recordIdx];

                if ((record.flags & FileRecord_IsDir) == 0) {
                    continue;
                }

                return ref.recordIdx;
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<uint32_t> directoryRecordIdxForFsIndex(
            uint64_t fsIndex
        ) const {
            if (fsIndexRefStorage == FsIndexRefStorage::UInt32) {
                if (const std::optional<uint32_t> recordIdx =
                        directoryRecordIdxForFsIndexInRefs(
                            directoryFsIndexRecordRefs32,
                            fsIndex
                        )) {
                    return recordIdx;
                }

                return liveDirectoryRecordIdxForFsIndexInRefs(
                    liveDirectoryFsIndexRecordRefs32,
                    fsIndex
                );
            }

            if (const std::optional<uint32_t> recordIdx =
                    directoryRecordIdxForFsIndexInRefs(
                        directoryFsIndexRecordRefs64,
                        fsIndex
                    )) {
                return recordIdx;
            }

            return liveDirectoryRecordIdxForFsIndexInRefs(
                liveDirectoryFsIndexRecordRefs64,
                fsIndex
            );
        }

        template <typename Ref>
        void appendFsIndexRefsForLookup(
            const std::vector<Ref>& refs,
            uint64_t fsIndex
        ) const {
            if constexpr (std::is_same_v<Ref, FsIndexRecordRef32>) {
                if (!fsIndexFitsUInt32(fsIndex)) {
                    return;
                }
            }

            const auto searchKey = Ref{
                static_cast<decltype(Ref::fsIndex)>(fsIndex),
                0
            };

            const auto range = std::equal_range(
                refs.begin(),
                refs.end(),
                searchKey,
                [](const Ref& lhs, const Ref& rhs) {
                    return lhs.fsIndex < rhs.fsIndex;
                }
            );

            for (auto it = range.first; it != range.second; ++it) {
                if (it->recordIdx >= fileRecords.size()) {
                    continue;
                }

                if (isDeletedRecord(it->recordIdx)) {
                    continue;
                }

                fsIndexLookupScratch.push_back(it->recordIdx);
            }
        }

        [[nodiscard]] std::size_t fsIndexFullRefCount() const noexcept
        {
            return fsIndexRefStorage == FsIndexRefStorage::UInt32
                ? fsIndexRecordRefs32.size()
                : fsIndexRecordRefs64.size();
        }

        [[nodiscard]] std::size_t fsIndexLiveRefCount() const noexcept
        {
            return fsIndexRefStorage == FsIndexRefStorage::UInt32
                ? liveFsIndexRecordRefs32.size()
                : liveFsIndexRecordRefs64.size();
        }

        [[nodiscard]] std::size_t fsIndexStoredRefCount() const noexcept
        {
            return fsIndexFullRefCount() + fsIndexLiveRefCount();
        }

        [[nodiscard]] std::size_t directoryFsIndexFullRefCount() const noexcept
        {
            return fsIndexRefStorage == FsIndexRefStorage::UInt32
                ? directoryFsIndexRecordRefs32.size()
                : directoryFsIndexRecordRefs64.size();
        }

        [[nodiscard]] std::size_t directoryFsIndexLiveRefCount() const noexcept
        {
            return fsIndexRefStorage == FsIndexRefStorage::UInt32
                ? liveDirectoryFsIndexRecordRefs32.size()
                : liveDirectoryFsIndexRecordRefs64.size();
        }

        [[nodiscard]] std::optional<uint32_t> directoryRecordIdxForNamespacedFsIndex(
    uint64_t fsNamespace,
    uint64_t fsIndex
) const {
            const auto searchKey = NamespacedFsIndexRecordRef{
                fsNamespace,
                fsIndex,
                0
            };

            const auto it = std::lower_bound(
                namespacedDirectoryFsIndexRecordRefs.begin(),
                namespacedDirectoryFsIndexRecordRefs.end(),
                searchKey,
                [](const NamespacedFsIndexRecordRef& lhs, const NamespacedFsIndexRecordRef& rhs) {
                    if (lhs.fsNamespace != rhs.fsNamespace) {
                        return lhs.fsNamespace < rhs.fsNamespace;
                    }

                    return lhs.fsIndex < rhs.fsIndex;
                }
            );

            if (it == namespacedDirectoryFsIndexRecordRefs.end() ||
                it->fsNamespace != fsNamespace ||
                it->fsIndex != fsIndex ||
                it->recordIdx >= fileRecords.size() ||
                isDeletedRecord(it->recordIdx)) {
                return std::nullopt;
                }

            const FileRecord& record = fileRecords[it->recordIdx];
            if ((record.flags & FileRecord_IsDir) == 0) {
                return std::nullopt;
            }

            return it->recordIdx;
        }

        void rebuildFsIndexMaps() {
            clearFsIndexRefStorage();

            if (hasFileRecordNamespaces()) {
                rebuildNamespacedFsIndexMaps();
                fsIndexRefStorage = FsIndexRefStorage::UInt64;
                return;
            }

            fsIndexRefStorage = allLiveFsIndexesFitUInt32()
                ? FsIndexRefStorage::UInt32
                : FsIndexRefStorage::UInt64;

            std::size_t directoryCount = 0;
            std::size_t liveRecordCount = 0;

            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                if (isDeletedRecord(recordIdx)) {
                    continue;
                }

                ++liveRecordCount;

                const FileRecord& rec = fileRecords[recordIdx];
                if ((rec.flags & FileRecord_IsDir) != 0) {
                    ++directoryCount;
                }
            }

            if (fsIndexRefStorage == FsIndexRefStorage::UInt32) {
                directoryFsIndexRecordRefs32.reserve(directoryCount);
                fsIndexRecordRefs32.reserve(liveRecordCount);

                for (uint32_t i = 0; i < static_cast<uint32_t>(fileRecords.size()); ++i) {
                    if (isDeletedRecord(i)) {
                        continue;
                    }

                    const FileRecord& rec = fileRecords[i];

                    fsIndexRecordRefs32.push_back({
                        static_cast<uint32_t>(rec.fsIndex),
                        i
                    });

                    if ((rec.flags & FileRecord_IsDir) != 0) {
                        directoryFsIndexRecordRefs32.push_back({
                            static_cast<uint32_t>(rec.fsIndex),
                            i
                        });
                    }
                }

                sortAndDeduplicateFsIndexRecordRefs(fsIndexRecordRefs32);
                sortAndDeduplicateFsIndexRecordRefs(directoryFsIndexRecordRefs32);

                fsIndexRecordRefs32.shrink_to_fit();
                directoryFsIndexRecordRefs32.shrink_to_fit();
                liveDirectoryFsIndexRecordRefs32.shrink_to_fit();
                liveFsIndexRecordRefs32.shrink_to_fit();
                return;
            }

            directoryFsIndexRecordRefs64.reserve(directoryCount);
            fsIndexRecordRefs64.reserve(liveRecordCount);

            for (uint32_t i = 0; i < static_cast<uint32_t>(fileRecords.size()); ++i) {
                if (isDeletedRecord(i)) {
                    continue;
                }

                const FileRecord& rec = fileRecords[i];

                fsIndexRecordRefs64.push_back({
                    rec.fsIndex,
                    i
                });

                if ((rec.flags & FileRecord_IsDir) != 0) {
                    directoryFsIndexRecordRefs64.push_back({
                        rec.fsIndex,
                        i
                    });
                }
            }

            sortAndDeduplicateFsIndexRecordRefs(fsIndexRecordRefs64);
            sortAndDeduplicateFsIndexRecordRefs(directoryFsIndexRecordRefs64);

            fsIndexRecordRefs64.shrink_to_fit();
            directoryFsIndexRecordRefs64.shrink_to_fit();
            liveDirectoryFsIndexRecordRefs64.shrink_to_fit();
            liveFsIndexRecordRefs64.shrink_to_fit();
        }

        void rebuildNamespacedFsIndexMaps()
        {
            namespacedDirectoryFsIndexRecordRefs.clear();
            namespacedFsIndexRecordRefs.clear();

            if (!hasFileRecordNamespaces()) {
                return;
            }

            std::size_t directoryCount = 0;
            std::size_t liveRecordCount = 0;

            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                if (isDeletedRecord(recordIdx)) {
                    continue;
                }

                ++liveRecordCount;

                if ((fileRecords[recordIdx].flags & FileRecord_IsDir) != 0) {
                    ++directoryCount;
                }
            }

            namespacedFsIndexRecordRefs.reserve(liveRecordCount);
            namespacedDirectoryFsIndexRecordRefs.reserve(directoryCount);

            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                if (isDeletedRecord(recordIdx)) {
                    continue;
                }

                const FileRecord& record = fileRecords[recordIdx];
                const FileRecordNamespace namespaceEntry = fileRecordNamespaces[recordIdx];

                namespacedFsIndexRecordRefs.push_back({
                    namespaceEntry.fsNamespace,
                    record.fsIndex,
                    recordIdx
                });

                if ((record.flags & FileRecord_IsDir) != 0) {
                    namespacedDirectoryFsIndexRecordRefs.push_back({
                        namespaceEntry.fsNamespace,
                        record.fsIndex,
                        recordIdx
                    });
                }
            }

            std::sort(
                namespacedFsIndexRecordRefs.begin(),
                namespacedFsIndexRecordRefs.end()
            );

            std::sort(
                namespacedDirectoryFsIndexRecordRefs.begin(),
                namespacedDirectoryFsIndexRecordRefs.end()
            );

            namespacedFsIndexRecordRefs.shrink_to_fit();
            namespacedDirectoryFsIndexRecordRefs.shrink_to_fit();
        }

        /**
         * @brief Resolves parent pointers for all file records in the current device index.
         * This is called once after the device scan is finished.
         *
         * For ordinary filesystems, this maps FileRecord::parentFsIndex to an
         * internal record index using filesystem object ids such as inode/MFT ids.
         *
         * For namespace-aware filesystems such as Btrfs, FileRecord::fsIndex and
         * FileRecord::parentFsIndex are interpreted together with the optional
         * FileRecordNamespace sidecar. Parent lookup then uses:
         *
         *   (parentFsNamespace, parentFsIndex)
         *
         * rather than parentFsIndex alone, because Btrfs object ids are only
         * unique within a root/subvolume.
         *
         * If a parent does not exist in the directory index, the corresponding
         * record is marked as belonging to the root by setting parentRecordIdx to
         * 0xFFFFFFFF.
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

            const bool useNamespaces = hasFileRecordNamespaces();

            // Convert parent filesystem indices to internal record indices.
            for (size_t i = 0; i < fileRecords.size(); ++i) {
                FileRecord& rec = fileRecords[i];

                if (useNamespaces) {
                    const FileRecordNamespace namespaceEntry = fileRecordNamespaces[i];

                    // Root/self-parent safety.
                    if (namespaceEntry.fsNamespace == namespaceEntry.parentFsNamespace &&
                        rec.fsIndex == rec.parentFsIndex) {
                        rec.parentRecordIdx = 0xFFFFFFFF;
                        continue;
                    }

                    if (const std::optional<uint32_t> parentRecordIdx =
                            directoryRecordIdxForNamespacedFsIndex(
                                namespaceEntry.parentFsNamespace,
                                rec.parentFsIndex
                            )) {
                        rec.parentRecordIdx = *parentRecordIdx;
                    } else {
                        rec.parentRecordIdx = 0xFFFFFFFF;
                    }

                    continue;
                }

                // Root/self-parent safety.
                if (rec.fsIndex == rec.parentFsIndex) {
                    rec.parentRecordIdx = 0xFFFFFFFF;
                    continue;
                }

                if (const std::optional<uint32_t> parentRecordIdx = directoryRecordIdxForFsIndex(rec.parentFsIndex)) {
                    rec.parentRecordIdx = *parentRecordIdx;
                } else {
                    // If parent is not in our DB, mark as root.
                    rec.parentRecordIdx = 0xFFFFFFFF;
                }
            }
        }

        void buildLowercaseStringPool() {
            lowercaseStringPool.clear();
            lowercaseNameOffsetByRecord.clear();

            lowercaseNameOffsetByRecord.resize(
                fileRecords.size(),
                NoLowercaseNameOverride
            );

            std::size_t bytesNeedingLowercaseCopy = 0;

            for (const FileRecord& record : fileRecords) {
                if (record.nameOffset + record.nameLen > stringPool.size()) {
                    continue;
                }

                const std::string_view name(
                    &stringPool[record.nameOffset],
                    record.nameLen
                );

                bool hasAsciiUppercase = false;

                for (const unsigned char c : name) {
                    if (c >= 'A' && c <= 'Z') {
                        hasAsciiUppercase = true;
                        break;
                    }
                }

                if (hasAsciiUppercase) {
                    bytesNeedingLowercaseCopy += record.nameLen;
                }
            }

            lowercaseStringPool.reserve(bytesNeedingLowercaseCopy);

            for (uint32_t recordIdx = 0;
                 recordIdx < static_cast<uint32_t>(fileRecords.size());
                 ++recordIdx) {
                const FileRecord& record = fileRecords[recordIdx];

                if (record.nameOffset + record.nameLen > stringPool.size()) {
                    continue;
                }

                const std::string_view name(
                    &stringPool[record.nameOffset],
                    record.nameLen
                );

                bool hasAsciiUppercase = false;

                for (const unsigned char c : name) {
                    if (c >= 'A' && c <= 'Z') {
                        hasAsciiUppercase = true;
                        break;
                    }
                }

                if (!hasAsciiUppercase) {
                    lowercaseNameOffsetByRecord[recordIdx] =
                        NoLowercaseNameOverride;
                    continue;
                }

                const uint32_t lowercaseOffset =
                    static_cast<uint32_t>(lowercaseStringPool.size());

                lowercaseNameOffsetByRecord[recordIdx] = lowercaseOffset;

                for (const unsigned char c : name) {
                    lowercaseStringPool.push_back(
                        (c >= 'A' && c <= 'Z')
                            ? static_cast<char>(c | 32)
                            : static_cast<char>(c)
                    );
                }
            }

            lowercaseStringPool.shrink_to_fit();
            lowercaseNameOffsetByRecord.shrink_to_fit();
        }

        void sortByNameAscendingParallel() {
            if (fileRecords.empty()) {
                return;
            }

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "Pre-sorting records by name ascending (case-insensitive) in parallel..." << std::endl;
#endif

            auto compareCaseInsensitive = [&](const FileRecord& a, const FileRecord& b) {
                const std::string_view s1 = lowercaseRecordName(a);
                const std::string_view s2 = lowercaseRecordName(b);

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

            // 3. Reorder records. parentRecordIdx is intentionally not remapped here,
            // because parent pointers are resolved after this sort.
            std::vector<FileRecord> newRecords(fileRecords.size());
            std::vector<FileRecordNamespace> newFileRecordNamespaces;
            std::vector<uint32_t> newLowercaseNameOffsetByRecord(
                lowercaseNameOffsetByRecord.size(),
                NoLowercaseNameOverride
            );

            const bool reorderNamespaces = hasFileRecordNamespaces();
            if (reorderNamespaces) {
                newFileRecordNamespaces.resize(fileRecordNamespaces.size());
            }

            for (size_t i = 0; i < p.size(); ++i) {
                newRecords[i] = fileRecords[p[i]];
                newRecords[i].parentRecordIdx = 0xFFFFFFFF;

                if (reorderNamespaces) {
                    newFileRecordNamespaces[i] = fileRecordNamespaces[p[i]];
                }

                if (p[i] < lowercaseNameOffsetByRecord.size() &&
                    i < newLowercaseNameOffsetByRecord.size()) {
                    newLowercaseNameOffsetByRecord[i] =
                        lowercaseNameOffsetByRecord[p[i]];
                }
            }

            fileRecords = std::move(newRecords);

            if (reorderNamespaces) {
                fileRecordNamespaces = std::move(newFileRecordNamespaces);
            }

            lowercaseNameOffsetByRecord =
                std::move(newLowercaseNameOffsetByRecord);
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

                const std::string_view name = lowercaseRecordName(rec, i);
                if (name.size() < 3) {
                    return;
                }

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
                trigramPostings.reserve(flatEntries.size() * sizeof(uint32_t));

                std::size_t i = 0;

                while (i < flatEntries.size()) {
                    const uint32_t trigram = flatEntries[i].trigram;
                    const uint32_t offset = static_cast<uint32_t>(trigramPostings.size());

                    uint32_t count = 0;
                    uint32_t previousRecordIdx = 0;
                    bool firstPosting = true;

                    do {
                        const uint32_t recordIdx = flatEntries[i].recordIdx;
                        const uint32_t encodedValue = firstPosting
                            ? recordIdx
                            : recordIdx - previousRecordIdx;

                        uint32_t value = encodedValue;
                        while (value >= 0x80) {
                            trigramPostings.push_back(
                                static_cast<uint8_t>((value & 0x7F) | 0x80)
                            );
                            value >>= 7;
                        }

                        trigramPostings.push_back(static_cast<uint8_t>(value));

                        previousRecordIdx = recordIdx;
                        firstPosting = false;
                        ++count;
                        ++i;
                    } while (i < flatEntries.size() && flatEntries[i].trigram == trigram);

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
                      << " compressedPostingBytes=" << trigramPostings.size()
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

                const std::string_view name =
                    lowercaseRecordName(record, recordIdx);

                if (name.empty()) {
                    continue;
                }

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

            const std::string_view name =
                lowercaseRecordName(record, recordIdx);

            if (name.empty()) {
                return false;
            }

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
            fsIndexLookupScratch.clear();

            if (fsIndexRefStorage == FsIndexRefStorage::UInt32) {
                appendFsIndexRefsForLookup(fsIndexRecordRefs32, fsIndex);
                appendFsIndexRefsForLookup(liveFsIndexRecordRefs32, fsIndex);
            } else {
                appendFsIndexRefsForLookup(fsIndexRecordRefs64, fsIndex);
                appendFsIndexRefsForLookup(liveFsIndexRecordRefs64, fsIndex);
            }

            if (fsIndexLookupScratch.empty()) {
                return nullptr;
            }

            if (fsIndexLookupScratch.size() > 1) {
                std::sort(
                    fsIndexLookupScratch.begin(),
                    fsIndexLookupScratch.end()
                );

                fsIndexLookupScratch.erase(
                    std::unique(
                        fsIndexLookupScratch.begin(),
                        fsIndexLookupScratch.end()
                    ),
                    fsIndexLookupScratch.end()
                );
            }

            return &fsIndexLookupScratch;
        }
    };

    struct RecordHandle {
        static constexpr uint16_t MaxIndexId = std::numeric_limits<uint16_t>::max();
        static constexpr uint8_t NoMountPoint = std::numeric_limits<uint8_t>::max();
        static constexpr uint8_t MaxMountPointIdx = NoMountPoint - 1;

        uint32_t recordIdx = 0;
        uint16_t indexId = 0;
        uint8_t generation = 0; // use only the low 8 bits to save memory, this makes generation wrap modulo 256.

        // Index into DeviceIndex::mountPoints.
        // NoMountPoint means no current mount point is available.
        uint8_t mountPointIdx = NoMountPoint;
    };

    static_assert(sizeof(RecordHandle) == 8);

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
        const std::vector<BlockDeviceMountInfo>& mounts,
        quint32 requestId
    );
    void removeDeviceByIndexId(quint64 indexId);
    bool removeDeviceByDeviceId(const QString& deviceId);
    bool removeDeviceByRequestId(quint32 requestId);
    void appendDeviceFileRecordsByRequestId(quint32 requestId, const std::vector<FileRecord>& records);
    void appendDeviceFileRecordNamespacesByRequestId(
        quint32 requestId,
        const std::vector<FileRecordNamespace>& namespaces
    );
    void appendDeviceStringPoolByRequestId(quint32 requestId, QByteArrayView stringPool);
    bool validateScanSidecarsByRequestId(quint32 requestId, QString* errorText = nullptr);
    bool removeRequestId(quint32 requestId);
    bool updateDeviceRuntimeStateByDeviceId(
        const QString& deviceId,
        bool mounted,
        bool showOfflineResults,
        const QStringList& mountPoints = {},
        const QString& primaryMountPoint = {},
        const std::vector<BlockDeviceMountInfo>& mounts = {}
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
        NotUpsert,
        AppliedNeedsFsIndexRebuild
    };

    bool removeDeviceByIndexIdUnlocked(quint64 indexId);
    static quint8 fileRecordFlagsFromLiveUpdateOperation(const LiveUpdateOperation& operation);
    static void updateFileRecordMetadataFromLiveUpdateOperation(
        FileRecord& record,
        const LiveUpdateOperation& operation
    );
    static uint32_t appendSparseLowercaseName(
        DeviceIndex& deviceIndex,
        std::string_view name
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
    static void sortLiveDirectoryFsIndexRecordRefs(DeviceIndex& deviceIndex);
    static void sortLiveFsIndexRecordRefs(DeviceIndex& deviceIndex);
    static bool shouldRebuildFsIndexAfterLiveUpdates(const DeviceIndex& deviceIndex);
    static void rebuildFsIndexAfterLiveUpdates(DeviceIndex& deviceIndex);
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
