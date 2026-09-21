// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Ext4ScannerEngine.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string_view>

#include "ScopedAccumulatedTimer.h"
#include "ScopedTimer.h"

namespace Ext4ScannerEngine {

    using Nanoseconds = std::chrono::nanoseconds;

    struct Ext4ScanTimings {
        Nanoseconds open{};
        Nanoseconds inodeStatsScan{};
        Nanoseconds inodeStatsSort{};
        Nanoseconds rootRecordEmit{};
        Nanoseconds directoryStreaming{};
        Nanoseconds dirIterateCalls{};
        Nanoseconds dirCallback{};
        Nanoseconds inodeStatsLookup{};
        Nanoseconds streamFlush{};
        Nanoseconds finalFlush{};
        Nanoseconds close{};
    };

    struct Ext4ScanCounters {
        uint64_t usedInodesSeen = 0;
        uint64_t inodeStatsEntries = 0;
        uint64_t directoryInodes = 0;
        uint64_t inodeStatsLookupBuckets = 0;
        uint64_t inodeStatsLookupBucketSpan = 0;
        uint64_t directoriesScanned = 0;
        uint64_t dirCallbackCalls = 0;
        uint64_t dirEntriesSeen = 0;
        uint64_t dirEntriesSkippedEmptyInode = 0;
        uint64_t dirEntriesSkippedEmptyName = 0;
        uint64_t dirEntriesSkippedDot = 0;
        uint64_t dirEntriesSkippedMissingStats = 0;
        uint64_t recordsEmitted = 0;
        uint64_t flushCalls = 0;
        uint64_t fileRecordChunks = 0;
        uint64_t stringPoolChunks = 0;
    };

    namespace {

        constexpr uint32_t kInvalidRecordIndex = 0xFFFFFFFF;
        constexpr uint64_t kProgressEvery = 4096; // must be power of two
        constexpr uint32_t kDirEntryCancelCheckEvery = 1024; // must be power of two
        constexpr uint32_t kDirectoryCancelCheckEvery = 256; // must be power of two

        [[nodiscard]] double seconds(Nanoseconds value)
        {
            return std::chrono::duration<double>(value).count();
        }

        void logExt4ScanProfile(const Ext4ScanTimings& timings,
                                const Ext4ScanCounters& counters)
        {
            const Nanoseconds measuredTotal =
                timings.open +
                timings.inodeStatsScan +
                timings.inodeStatsSort +
                timings.rootRecordEmit +
                timings.directoryStreaming +
                timings.finalFlush +
                timings.close;

            std::cerr << "[Ext4ScannerEngine] profile summary\n"
                      << "  timings:\n"
                      << "    open=" << seconds(timings.open) << "s\n"
                      << "    inodeStatsScan=" << seconds(timings.inodeStatsScan) << "s\n"
                      << "    inodeStatsSort=" << seconds(timings.inodeStatsSort) << "s\n"
                      << "    rootRecordEmit=" << seconds(timings.rootRecordEmit) << "s\n"
                      << "    directoryStreaming=" << seconds(timings.directoryStreaming) << "s\n"
                      << "      dirIterateCalls=" << seconds(timings.dirIterateCalls) << "s\n"
                      << "      dirCallback=" << seconds(timings.dirCallback) << "s\n"
                      << "      inodeStatsLookup=" << seconds(timings.inodeStatsLookup) << "s\n"
                      << "      streamFlush=" << seconds(timings.streamFlush) << "s\n"
                      << "    finalFlush=" << seconds(timings.finalFlush) << "s\n"
                      << "    close=" << seconds(timings.close) << "s\n"
                      << "    measuredTotal=" << seconds(measuredTotal) << "s\n"
                      << "  counters:\n"
                      << "    usedInodesSeen=" << counters.usedInodesSeen << "\n"
                      << "    inodeStatsEntries=" << counters.inodeStatsEntries << "\n"
                      << "    directoryInodes=" << counters.directoryInodes << "\n"
                      << "    inodeStatsLookupBuckets=" << counters.inodeStatsLookupBuckets << "\n"
                      << "    inodeStatsLookupBucketSpan=" << counters.inodeStatsLookupBucketSpan << "\n"
                      << "    directoriesScanned=" << counters.directoriesScanned << "\n"
                      << "    dirCallbackCalls=" << counters.dirCallbackCalls << "\n"
                      << "    dirEntriesSeen=" << counters.dirEntriesSeen << "\n"
                      << "    dirEntriesSkippedEmptyInode=" << counters.dirEntriesSkippedEmptyInode << "\n"
                      << "    dirEntriesSkippedEmptyName=" << counters.dirEntriesSkippedEmptyName << "\n"
                      << "    dirEntriesSkippedDot=" << counters.dirEntriesSkippedDot << "\n"
                      << "    dirEntriesSkippedMissingStats=" << counters.dirEntriesSkippedMissingStats << "\n"
                      << "    recordsEmitted=" << counters.recordsEmitted << "\n"
                      << "    flushCalls=" << counters.flushCalls << "\n"
                      << "    fileRecordChunks=" << counters.fileRecordChunks << "\n"
                      << "    stringPoolChunks=" << counters.stringPoolChunks << "\n";
        }

        [[nodiscard]] std::string makeExt2Error(const char* prefix, errcode_t code) {
            std::ostringstream out;
            out << prefix << ": " << error_message(code);
            return out.str();
        }

        void reportError(const ScannerHelper::ErrorCallback& onError, const std::string& message) {
            if (onError) {
                onError(QString::fromStdString(message));
            }

            std::cerr << "[Ext4ScannerEngine] " << message << "\n";
        }

        [[nodiscard]] FileStats makeFileStats(const ext2_inode& inode) {
            FileStats stats{};
            stats.size = EXT2_I_SIZE(&inode);
            stats.modificationTime = inode.i_mtime;

            if (LINUX_S_ISDIR(inode.i_mode)) {
                stats.flags |= FileRecord_IsDir;
            }

            if (LINUX_S_ISLNK(inode.i_mode)) {
                stats.flags |= FileRecord_IsSymlink;
            }

            return stats;
        }

        [[nodiscard]] const FileStats* findStatsByInode(const std::vector<InodeStatsEntry>& inodeStats,
                                                        uint32_t inode) {
            const auto it = std::lower_bound(
                inodeStats.begin(),
                inodeStats.end(),
                inode,
                [](const InodeStatsEntry& entry, uint32_t value) {
                    return entry.inode < value;
                });

            if (it == inodeStats.end() || it->inode != inode) {
                return nullptr;
            }

            return &it->stats;
        }

        struct InodeStatsBucket {
            uint32_t offset = 0;
            uint32_t count = 0;
        };

        struct InodeStatsLookup {
            const std::vector<InodeStatsEntry>* entries = nullptr;
            uint32_t inodeSpanPerBucket = 0;
            std::vector<InodeStatsBucket> buckets;
        };

        [[nodiscard]] uint32_t inodeGroupForLookup(uint32_t inode, uint32_t inodeSpanPerBucket) noexcept
        {
            if (inode == 0 || inodeSpanPerBucket == 0) {
                return 0;
            }

            return (inode - 1) / inodeSpanPerBucket;
        }

        [[nodiscard]] uint32_t chooseInodeLookupBucketSpan(uint32_t totalInodes,
                                                           uint32_t fsInodesPerGroup,
                                                           std::size_t usedInodeCount) noexcept
        {
            if (totalInodes == 0 || usedInodeCount == 0) {
                return 0;
            }

            /*
             * Aim for reasonably small binary-search ranges without creating a huge
             * mostly-empty bucket table on sparse large filesystems.
             *
             * This stays parallel-friendly because buckets are still immutable
             * inode-number ranges.
             */
            static constexpr std::size_t TargetUsedInodesPerBucket = 256;
            static constexpr std::size_t MinUsefulBucketCount = 64;
            static constexpr std::size_t MaxBucketCount = 65536;

            std::size_t targetBucketCount =
                (usedInodeCount + TargetUsedInodesPerBucket - 1) /
                TargetUsedInodesPerBucket;

            targetBucketCount = std::clamp<std::size_t>(
                targetBucketCount,
                MinUsefulBucketCount,
                MaxBucketCount
            );

            const uint64_t adaptiveSpan =
                (static_cast<uint64_t>(totalInodes) + targetBucketCount - 1) /
                targetBucketCount;

            const uint64_t span = std::max<uint64_t>(
                std::max<uint32_t>(fsInodesPerGroup, 1),
                adaptiveSpan
            );

            return static_cast<uint32_t>(
                std::min<uint64_t>(span, std::numeric_limits<uint32_t>::max())
            );
        }

        [[nodiscard]] InodeStatsLookup buildInodeStatsLookup(const std::vector<InodeStatsEntry>& inodeStats,
                                                             uint32_t totalInodes,
                                                             uint32_t fsInodesPerGroup)
        {
            InodeStatsLookup lookup;
            lookup.entries = &inodeStats;
            lookup.inodeSpanPerBucket = chooseInodeLookupBucketSpan(
                totalInodes,
                fsInodesPerGroup,
                inodeStats.size()
            );

            if (inodeStats.empty() || totalInodes == 0 || lookup.inodeSpanPerBucket == 0) {
                return lookup;
            }

            const uint64_t bucketCount64 =
                (static_cast<uint64_t>(totalInodes) + lookup.inodeSpanPerBucket - 1) /
                lookup.inodeSpanPerBucket;

            const uint32_t bucketCount =
                static_cast<uint32_t>(std::min<uint64_t>(
                    bucketCount64,
                    std::numeric_limits<uint32_t>::max()
                ));

            lookup.buckets.assign(bucketCount, InodeStatsBucket{});

            uint32_t currentBucket = inodeGroupForLookup(
                inodeStats.front().inode,
                lookup.inodeSpanPerBucket
            );

            std::size_t bucketStart = 0;

            for (std::size_t i = 0; i < inodeStats.size(); ++i) {
                const uint32_t bucket = inodeGroupForLookup(
                    inodeStats[i].inode,
                    lookup.inodeSpanPerBucket
                );

                if (bucket == currentBucket) {
                    continue;
                }

                if (currentBucket < lookup.buckets.size()) {
                    lookup.buckets[currentBucket] = InodeStatsBucket{
                        .offset = static_cast<uint32_t>(bucketStart),
                        .count = static_cast<uint32_t>(i - bucketStart)
                    };
                }

                currentBucket = bucket;
                bucketStart = i;
            }

            if (currentBucket < lookup.buckets.size()) {
                lookup.buckets[currentBucket] = InodeStatsBucket{
                    .offset = static_cast<uint32_t>(bucketStart),
                    .count = static_cast<uint32_t>(inodeStats.size() - bucketStart)
                };
            }

            return lookup;
        }

        [[nodiscard]] const FileStats* findStatsByInode(const InodeStatsLookup& lookup,
                                                        uint32_t inode)
        {
            if (!lookup.entries || lookup.inodeSpanPerBucket == 0 || lookup.buckets.empty()) {
                return nullptr;
            }

            const uint32_t bucketIdx = inodeGroupForLookup(
                inode,
                lookup.inodeSpanPerBucket
            );

            if (bucketIdx >= lookup.buckets.size()) {
                return nullptr;
            }

            const InodeStatsBucket bucket = lookup.buckets[bucketIdx];

            if (bucket.count == 0) {
                return nullptr;
            }

            const std::vector<InodeStatsEntry>& entries = *lookup.entries;

            if (bucket.offset >= entries.size()) {
                return nullptr;
            }

            const std::size_t beginOffset = bucket.offset;
            const std::size_t endOffset = std::min<std::size_t>(
                entries.size(),
                beginOffset + bucket.count
            );

            const auto begin = entries.begin() + static_cast<std::ptrdiff_t>(beginOffset);
            const auto end = entries.begin() + static_cast<std::ptrdiff_t>(endOffset);

            const auto it = std::lower_bound(
                begin,
                end,
                inode,
                [](const InodeStatsEntry& entry, uint32_t value) {
                    return entry.inode < value;
                });

            if (it == end || it->inode != inode) {
                return nullptr;
            }

            return &it->stats;
        }

        [[nodiscard]] bool collectInodeStats(ext2_filsys fs,
                                             std::vector<InodeStatsEntry>& inodeStats,
                                             std::vector<uint32_t>& directoryInodes,
                                             uint64_t inodesInUse,
                                             const ScannerHelper::ErrorCallback& onError,
                                             const ScannerHelper::CancelCallback& shouldCancel,
                                             const ScannerHelper::ProgressCallback& onProgress,
                                             Ext4ScanCounters* counters) {
            ScopedTimer timer("[Ext4ScannerEngine] inode stats scan");

            ext2_inode_scan scan = nullptr;
            constexpr int bufferBlocks = 4096;

            errcode_t retval = ext2fs_open_inode_scan(fs, bufferBlocks, &scan);
            if (retval) {
                reportError(onError, makeExt2Error("ext2fs_open_inode_scan failed", retval));
                return false;
            }

            ext2_ino_t ino = 0;
            ext2_inode inode{};

            uint64_t usedInodesSeen = 0;

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Reading inodes"),
                    .unit = QStringLiteral("inodes"),
                    .processed = 0,
                    .total = inodesInUse
                });
            }

            while (true) {
                retval = ext2fs_get_next_inode(scan, &ino, &inode);
                if (retval) {
                    ext2fs_close_inode_scan(scan);
                    reportError(onError, makeExt2Error("ext2fs_get_next_inode failed", retval));
                    return false;
                }

                if (ino == 0) {
                    break;
                }

                if (inode.i_links_count == 0) {
                    continue;
                }

                ++usedInodesSeen;

                const FileStats stats = makeFileStats(inode);

                inodeStats.push_back(InodeStatsEntry{
                    static_cast<uint32_t>(ino),
                    stats
                });

                if ((stats.flags & FileRecord_IsDir) != 0) {
                    directoryInodes.push_back(static_cast<uint32_t>(ino));
                }

                if (onProgress && ((usedInodesSeen & (kProgressEvery - 1)) == 0)) {
                    onProgress(Protocol::ScanProgress{
                        .phase = QStringLiteral("Reading inodes"),
                        .unit = QStringLiteral("inodes"),
                        .processed = usedInodesSeen,
                        .total = inodesInUse
                    });
                }

                if (shouldCancel && shouldCancel()) {
                    ext2fs_close_inode_scan(scan);
                    return false;
                }
            }

            ext2fs_close_inode_scan(scan);

            if (counters) {
                counters->usedInodesSeen = usedInodesSeen;
                counters->inodeStatsEntries = inodeStats.size();
                counters->directoryInodes = directoryInodes.size();
            }

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Reading inodes"),
                    .unit = QStringLiteral("inodes"),
                    .processed = usedInodesSeen,
                    .total = inodesInUse
                });
            }

            return true;
        }

    } // namespace

    struct DirCallbackContext {
        ext2_filsys fs = nullptr;
        Ext4StreamState& stream;
        const InodeStatsLookup& inodeStatsLookup;
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk;
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk;
        const ScannerHelper::CancelCallback& shouldCancel;
        Ext4ScanTimings* timings = nullptr;
        Ext4ScanCounters* counters = nullptr;
        uint32_t entriesSinceCancelCheck = 0;
        bool cancelled = false;
        bool failed = false;
    };

    bool Ext4StreamState::flush(const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                                const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                                Ext4ScanTimings* timings,
                                Ext4ScanCounters* counters) {
        std::optional<ScopedAccumulatedTimer> flushTimer;
        if (timings) {
            flushTimer.emplace(timings->streamFlush);
        }

        if (counters) {
            ++counters->flushCalls;
        }

        if (!records.empty()) {
            std::vector<FileRecord> fileRecordChunk = std::move(records);
            records.clear();
            records.reserve(kRecordsPerIpcChunk);

            if (counters) {
                ++counters->fileRecordChunks;
            }

            if (!onFileRecordChunk(fileRecordChunk)) {
                std::cerr << "[Ext4ScannerEngine] scan aborted by file record receiver\n";
                return false;
            }
        }

        if (!stringPool.empty()) {
            std::vector<char> stringPoolChunk = std::move(stringPool);

            totalStringPoolLength += static_cast<uint32_t>(stringPoolChunk.size());

            stringPool.clear();
            stringPool.reserve(kMaxIpcBufferSizeBytes);

            if (counters) {
                ++counters->stringPoolChunks;
            }

            if (!onStringPoolChunk(stringPoolChunk)) {
                std::cerr << "[Ext4ScannerEngine] scan aborted by string pool receiver\n";
                return false;
            }
        }

        return true;
    }

    bool Ext4StreamState::addRecord(uint32_t inode,
                                    uint32_t parentInode,
                                    std::string_view name,
                                    const FileStats& stats,
                                    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                                    Ext4ScanTimings* timings,
                                    Ext4ScanCounters* counters) {
        if (name.size() > std::numeric_limits<uint16_t>::max()) {
            return true;
        }

        if (records.size() >= kRecordsPerIpcChunk ||
            stringPool.size() + name.size() >= kMaxIpcBufferSizeBytes) {
            if (!flush(onFileRecordChunk, onStringPoolChunk, timings, counters)) {
                return false;
            }
        }

        FileRecord record{};
        record.fsIndex = inode;
        record.parentFsIndex = parentInode;
        record.parentRecordIdx = kInvalidRecordIndex;
        record.size = stats.size;
        record.modificationTime = stats.modificationTime;
        record.nameOffset = totalStringPoolLength + static_cast<uint32_t>(stringPool.size());
        record.nameLen = static_cast<uint16_t>(name.size());
        record.flags = stats.flags;

        records.push_back(record);
        stringPool.insert(stringPool.end(), name.begin(), name.end());

        if (counters) {
            ++counters->recordsEmitted;
        }

        return true;
    }

    int dirCallback(ext2_ino_t dir_ino, int entry_flags, struct ext2_dir_entry *dirent,
                    int offset, int blocksize, char *buf, void *priv_data) {
        Q_UNUSED(entry_flags);
        Q_UNUSED(offset);
        Q_UNUSED(blocksize);
        Q_UNUSED(buf);

        auto* ctx = static_cast<DirCallbackContext*>(priv_data);

        if (!ctx || ctx->failed || ctx->cancelled) {
            return 1;
        }

        std::optional<ScopedAccumulatedTimer> callbackTimer;
        if (ctx->timings) {
            callbackTimer.emplace(ctx->timings->dirCallback);
        }

        if (ctx->counters) {
            ++ctx->counters->dirCallbackCalls;
        }

        ++ctx->entriesSinceCancelCheck;
        if ((ctx->entriesSinceCancelCheck & (kDirEntryCancelCheckEvery - 1)) == 0) {
            if (ctx->shouldCancel && ctx->shouldCancel()) {
                ctx->cancelled = true;
                return 1;
            }
        }

        // Ignore invalid entries or empty inodes
        if (dirent->inode == 0) {
            if (ctx->counters) {
                ++ctx->counters->dirEntriesSkippedEmptyInode;
            }

            return 0;
        }

        if (ctx->counters) {
            ++ctx->counters->dirEntriesSeen;
        }

        // dirent->name_len is sometimes encoded with file type info in modern EXT4,
        // so we mask it with 0xFF to get the actual length.
        const uint16_t len = dirent->name_len & 0xFF;
        if (len == 0) {
            if (ctx->counters) {
                ++ctx->counters->dirEntriesSkippedEmptyName;
            }

            return 0;
        }

        // Ignore '.' and '..'
        if (len == 1 && dirent->name[0] == '.') {
            if (ctx->counters) {
                ++ctx->counters->dirEntriesSkippedDot;
            }

            return 0;
        }
        if (len == 2 && dirent->name[0] == '.' && dirent->name[1] == '.') {
            if (ctx->counters) {
                ++ctx->counters->dirEntriesSkippedDot;
            }

            return 0;
        }

        const FileStats* stats = nullptr;
        {
            std::optional<ScopedAccumulatedTimer> lookupTimer;
            if (ctx->timings) {
                lookupTimer.emplace(ctx->timings->inodeStatsLookup);
            }

            stats = findStatsByInode(ctx->inodeStatsLookup, dirent->inode);
        }

        if (!stats) {
            if (ctx->counters) {
                ++ctx->counters->dirEntriesSkippedMissingStats;
            }

            return 0;
        }

        const std::string_view name(dirent->name, len);

        if (!ctx->stream.addRecord(static_cast<uint32_t>(dirent->inode),
                                   static_cast<uint32_t>(dir_ino),
                                   name,
                                   *stats,
                                   ctx->onFileRecordChunk,
                                   ctx->onStringPoolChunk,
                                   ctx->timings,
                                   ctx->counters)) {
            ctx->failed = true;
            return 1;
        }

        return 0;
    }

    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                    const ScannerHelper::ErrorCallback& onError,
                    const ScannerHelper::CancelCallback& shouldCancel,
                    const ScannerHelper::ProgressCallback& onProgress) {
        ScopedTimer totalTimer("[Ext4ScannerEngine] total ext4 scan");

        Ext4ScanTimings timings;
        Ext4ScanCounters counters;

        ext2_filsys fs = nullptr;
        const std::string devicePathStd = devicePath.toStdString();

        errcode_t retval = 0;
        {
            ScopedAccumulatedTimer timer(timings.open);
            retval = ext2fs_open(devicePathStd.c_str(), 0, 0, 0, unix_io_manager, &fs);
        }

        if (retval) {
            reportError(onError, makeExt2Error("ext2fs_open failed", retval));
            return false;
        }

        const uint32_t totalInodes = fs->super->s_inodes_count;
        const uint32_t freeInodes  = fs->super->s_free_inodes_count;
        const uint32_t inodesInUse = (freeInodes <= totalInodes) ? (totalInodes - freeInodes) : totalInodes;

        std::cerr << "[Ext4ScannerEngine] totalInodes=" << totalInodes
                  << " freeInodes=" << freeInodes
                  << " estimatedInodesInUse=" << inodesInUse
                  << "\n";

        std::vector<InodeStatsEntry> inodeStats;
        std::vector<uint32_t> directoryInodes;

        inodeStats.reserve(static_cast<std::size_t>(inodesInUse));

        const std::size_t estimatedDirectoryInodes = std::clamp<std::size_t>(
            static_cast<std::size_t>(inodesInUse) / 8,
            4096,
            static_cast<std::size_t>(inodesInUse)
        );

        directoryInodes.reserve(estimatedDirectoryInodes);

        bool inodeStatsCollected = false;
        {
            ScopedAccumulatedTimer timer(timings.inodeStatsScan);
            inodeStatsCollected = collectInodeStats(fs,
                                                    inodeStats,
                                                    directoryInodes,
                                                    inodesInUse,
                                                    onError,
                                                    shouldCancel,
                                                    onProgress,
                                                    &counters);
        }

        if (!inodeStatsCollected) {
            {
                ScopedAccumulatedTimer timer(timings.close);
                ext2fs_close(fs);
            }

            logExt4ScanProfile(timings, counters);
            return false;
        }

        {
            ScopedTimer timer("[Ext4ScannerEngine] inode stats sort");
            ScopedAccumulatedTimer profileTimer(timings.inodeStatsSort);

            if (!std::is_sorted(inodeStats.begin(),
                    inodeStats.end(),
                    [](const InodeStatsEntry& lhs, const InodeStatsEntry& rhs) {
                        return lhs.inode < rhs.inode;
                    })) {
                std::sort(inodeStats.begin(),
                          inodeStats.end(),
                          [](const InodeStatsEntry& lhs, const InodeStatsEntry& rhs) {
                              return lhs.inode < rhs.inode;
                          });
            }
        }

        const uint32_t inodesPerGroup = fs->super->s_inodes_per_group;
        const InodeStatsLookup inodeStatsLookup =
            buildInodeStatsLookup(inodeStats, totalInodes, inodesPerGroup);

        counters.inodeStatsLookupBuckets = inodeStatsLookup.buckets.size();
        counters.inodeStatsLookupBucketSpan = inodeStatsLookup.inodeSpanPerBucket;

        Ext4StreamState stream{};
        stream.records.reserve(Ext4StreamState::kRecordsPerIpcChunk);
        stream.stringPool.reserve(Ext4StreamState::kMaxIpcBufferSizeBytes);

        {
            ScopedTimer timer("[Ext4ScannerEngine] directory entry streaming");
            ScopedAccumulatedTimer directoryStreamingTimer(timings.directoryStreaming);

            const FileStats* rootStats = findStatsByInode(inodeStatsLookup, EXT2_ROOT_INO);
            if (rootStats) {
                ScopedAccumulatedTimer rootTimer(timings.rootRecordEmit);

                if (!stream.addRecord(EXT2_ROOT_INO,
                                      EXT2_ROOT_INO,
                                      std::string_view{},
                                      *rootStats,
                                      onFileRecordChunk,
                                      onStringPoolChunk,
                                      &timings,
                                      &counters)) {
                    {
                        ScopedAccumulatedTimer closeTimer(timings.close);
                        ext2fs_close(fs);
                    }

                    logExt4ScanProfile(timings, counters);
                    return false;
                }
            }

            DirCallbackContext ctx{
                fs,
                stream,
                inodeStatsLookup,
                onFileRecordChunk,
                onStringPoolChunk,
                shouldCancel,
                &timings,
                &counters
            };

            uint64_t directoriesScanned = 0;

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Scanning directories"),
                    .unit = QStringLiteral("directories"),
                    .processed = 0,
                    .total = static_cast<quint64>(directoryInodes.size())
                });
            }

            for (const uint32_t dirInode : directoryInodes) {
                if ((directoriesScanned & (kDirectoryCancelCheckEvery - 1)) == 0) {
                    if (shouldCancel && shouldCancel()) {
                        {
                            ScopedAccumulatedTimer closeTimer(timings.close);
                            ext2fs_close(fs);
                        }

                        logExt4ScanProfile(timings, counters);
                        return false;
                    }
                }

                {
                    ScopedAccumulatedTimer iterateTimer(timings.dirIterateCalls);
                    retval = ext2fs_dir_iterate2(fs,
                                                 dirInode,
                                                 0,
                                                 nullptr,
                                                 dirCallback,
                                                 &ctx);
                }

                if (ctx.cancelled) {
                    {
                        ScopedAccumulatedTimer closeTimer(timings.close);
                        ext2fs_close(fs);
                    }

                    logExt4ScanProfile(timings, counters);
                    return false;
                }

                if (ctx.failed) {
                    {
                        ScopedAccumulatedTimer closeTimer(timings.close);
                        ext2fs_close(fs);
                    }

                    logExt4ScanProfile(timings, counters);
                    return false;
                }

                if (retval) {
                    // Some directories may be unreadable/corrupt. Report it, but continue.
                    std::cerr << "[Ext4ScannerEngine] ext2fs_dir_iterate2 failed for inode="
                              << dirInode
                              << ": "
                              << error_message(retval)
                              << "\n";
                }

                ++directoriesScanned;
                counters.directoriesScanned = directoriesScanned;

                if (onProgress && ((directoriesScanned & (kProgressEvery - 1)) == 0)) {
                    onProgress(Protocol::ScanProgress{
                        .phase = QStringLiteral("Scanning directories"),
                        .unit = QStringLiteral("directories"),
                        .processed = directoriesScanned,
                        .total = static_cast<quint64>(directoryInodes.size())
                    });
                }
            }
        }

        {
            ScopedAccumulatedTimer timer(timings.finalFlush);
            if (!stream.flush(onFileRecordChunk, onStringPoolChunk, &timings, &counters)) {
                {
                    ScopedAccumulatedTimer closeTimer(timings.close);
                    ext2fs_close(fs);
                }

                logExt4ScanProfile(timings, counters);
                return false;
            }
        }

        {
            ScopedAccumulatedTimer timer(timings.close);
            ext2fs_close(fs);
        }

        std::cerr << "[Ext4ScannerEngine] emitted stringPoolBytes="
                  << stream.totalStringPoolLength
                  << " inodeStatsCount="
                  << inodeStats.size()
                  << " directoryCount="
                  << directoryInodes.size()
                  << "\n";

        if (onProgress) {
            onProgress(Protocol::ScanProgress{
                .phase = QStringLiteral("Scanning directories"),
                .unit = QStringLiteral("directories"),
                .processed = static_cast<quint64>(directoryInodes.size()),
                .total = static_cast<quint64>(directoryInodes.size())
            });
        }

        logExt4ScanProfile(timings, counters);

        return true;
    }

} // namespace Ext4ScannerEngine