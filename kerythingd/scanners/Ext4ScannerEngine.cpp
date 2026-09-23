// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Ext4ScannerEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#ifdef KERYTHING_ENABLE_LOGGING
#include "ScopedAccumulatedTimer.h"
#include "ScopedTimer.h"
#endif

namespace Ext4ScannerEngine {

    using Nanoseconds = std::chrono::nanoseconds;

    struct Ext4ScanTimings {
        Nanoseconds open{};
        Nanoseconds inodeStatsScan{};
        Nanoseconds inodeStatsSort{};
        Nanoseconds inodeStatsLookupBuild{};
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
        constexpr uint32_t kInodeSlotProgressEvery = 262144; // must be power of two
        constexpr uint32_t kDirEntryCancelCheckEvery = 1024; // must be power of two
        constexpr uint32_t kDirectoryCancelCheckEvery = 256; // must be power of two
        constexpr uint32_t kParallelDirectoryChunkSize = 128;
        constexpr uint32_t kMinDirectoriesForParallelScan = 1024;
        constexpr uint32_t kMaxParallelDirectoryWorkers = 16;
        constexpr uint32_t kMinBlockGroupsForParallelInodeScan = 256;
        constexpr uint32_t kMinInodeSlotsForParallelInodeScan = 1'000'000;
        constexpr uint32_t kMinUsedInodesForParallelInodeScan = 100'000;
        constexpr uint32_t kMaxParallelInodeWorkers = 8;

#ifdef KERYTHING_ENABLE_LOGGING
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
                timings.inodeStatsLookupBuild +
                timings.rootRecordEmit +
                timings.directoryStreaming +
                timings.finalFlush +
                timings.close;

            std::cerr << "[Ext4ScannerEngine] profile summary\n"
                      << "  timings:\n"
                      << "    open=" << seconds(timings.open) << "s\n"
                      << "    inodeStatsScan=" << seconds(timings.inodeStatsScan) << "s\n"
                      << "    inodeStatsSort=" << seconds(timings.inodeStatsSort) << "s\n"
                      << "    inodeStatsLookupBuild=" << seconds(timings.inodeStatsLookupBuild) << "s\n"
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
#endif

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

        struct InodeStatsWorkerResult {
            std::vector<InodeStatsEntry> inodeStats;
            std::vector<uint32_t> directoryInodes;
            uint64_t usedInodesSeen = 0;
            bool failed = false;
            bool cancelled = false;
            std::string errorMessage;
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

        [[nodiscard]] uint32_t chooseInodeScanWorkerCount(ext2_filsys fs,
                                                          uint32_t totalInodes,
                                                          uint32_t inodesInUse,
                                                          const ScanOptions& options) noexcept
        {
            if (!fs || !fs->super) {
                return 1;
            }

            if (options.deviceIsRotational) {
                return 1;
            }

            if (totalInodes < kMinInodeSlotsForParallelInodeScan ||
                inodesInUse < kMinUsedInodesForParallelInodeScan) {
                return 1;
            }

            const dgrp_t groupCount = fs->group_desc_count;

            if (groupCount < kMinBlockGroupsForParallelInodeScan) {
                return 1;
            }

            const uint32_t hardwareThreads =
                std::max(1u, std::thread::hardware_concurrency());

            return static_cast<uint32_t>(
                std::clamp<std::size_t>(
                    std::min<std::size_t>(groupCount, hardwareThreads),
                    1,
                    kMaxParallelInodeWorkers
                )
            );
        }

        [[nodiscard]] bool collectInodeStats(ext2_filsys fs,
                                             std::vector<InodeStatsEntry>& inodeStats,
                                             std::vector<uint32_t>& directoryInodes,
                                             uint64_t totalInodes,
                                             const ScannerHelper::ErrorCallback& onError,
                                             const ScannerHelper::CancelCallback& shouldCancel,
                                             const ScannerHelper::ProgressCallback& onProgress,
                                             Ext4ScanCounters* counters) {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedTimer timer("[Ext4ScannerEngine] inode stats scan");
#endif

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
            uint64_t lastProgressInode = 0;

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Reading inode table"),
                    .unit = QStringLiteral("slots"),
                    .processed = 0,
                    .total = totalInodes
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

                if (onProgress &&
                    static_cast<uint64_t>(ino) >= lastProgressInode + kInodeSlotProgressEvery) {
                    lastProgressInode = static_cast<uint64_t>(ino);

                    onProgress(Protocol::ScanProgress{
                        .phase = QStringLiteral("Reading inode table"),
                        .unit = QStringLiteral("slots"),
                        .processed = std::min<uint64_t>(lastProgressInode, totalInodes),
                        .total = totalInodes
                    });
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
                    .phase = QStringLiteral("Reading inode table"),
                    .unit = QStringLiteral("slots"),
                    .processed = totalInodes,
                    .total = totalInodes
                });
            }

            return true;
        }

        [[nodiscard]] bool collectInodeStatsParallelByGroup(const QString& devicePath,
                                                            ext2_filsys fs,
                                                            std::vector<InodeStatsEntry>& inodeStats,
                                                            std::vector<uint32_t>& directoryInodes,
                                                            uint64_t totalInodes,
                                                            uint64_t inodesInUse,
                                                            uint32_t workerCount,
                                                            const ScannerHelper::ErrorCallback& onError,
                                                            const ScannerHelper::CancelCallback& shouldCancel,
                                                            const ScannerHelper::ProgressCallback& onProgress,
                                                            Ext4ScanCounters* counters)
        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedTimer timer("[Ext4ScannerEngine] parallel inode stats scan");
#endif

            if (!fs || !fs->super || workerCount <= 1) {
                return collectInodeStats(
                    fs,
                    inodeStats,
                    directoryInodes,
                    totalInodes,
                    onError,
                    shouldCancel,
                    onProgress,
                    counters
                );
            }

            const dgrp_t groupCount = fs->group_desc_count;
            const uint32_t inodesPerGroup = fs->super->s_inodes_per_group;

            if (groupCount == 0 || inodesPerGroup == 0) {
                return collectInodeStats(
                    fs,
                    inodeStats,
                    directoryInodes,
                    totalInodes,
                    onError,
                    shouldCancel,
                    onProgress,
                    counters
                );
            }

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Reading inode table"),
                    .unit = QStringLiteral("slots"),
                    .processed = 0,
                    .total = totalInodes
                });
            }

            const std::string devicePathStd = devicePath.toStdString();

            std::atomic_bool sharedAbort{false};
            std::atomic_uint64_t completedGroups{0};

            std::mutex progressMutex;
            std::mutex errorMutex;

            std::vector<InodeStatsWorkerResult> workerResults(workerCount);
            std::vector<std::thread> workers;
            workers.reserve(workerCount);

            const dgrp_t groupsPerWorker =
                static_cast<dgrp_t>((groupCount + workerCount - 1) / workerCount);

            for (uint32_t workerIdx = 0; workerIdx < workerCount; ++workerIdx) {
                const dgrp_t beginGroup =
                    static_cast<dgrp_t>(workerIdx * groupsPerWorker);

                if (beginGroup >= groupCount) {
                    break;
                }

                const dgrp_t endGroup = std::min<dgrp_t>(
                    groupCount,
                    static_cast<dgrp_t>(beginGroup + groupsPerWorker)
                );

                workers.emplace_back([&, workerIdx, beginGroup, endGroup]() {
                    InodeStatsWorkerResult& result = workerResults[workerIdx];

                    ext2_filsys workerFs = nullptr;
                    errcode_t retval = ext2fs_open(
                        devicePathStd.c_str(),
                        0,
                        0,
                        0,
                        unix_io_manager,
                        &workerFs
                    );

                    if (retval) {
                        result.failed = true;
                        result.errorMessage =
                            makeExt2Error("ext2fs_open failed for ext4 inode scan worker", retval);
                        sharedAbort.store(true, std::memory_order_relaxed);
                        return;
                    }

                    ext2_inode_scan scan = nullptr;
                    constexpr int bufferBlocks = 4096;

                    retval = ext2fs_open_inode_scan(workerFs, bufferBlocks, &scan);
                    if (retval) {
                        result.failed = true;
                        result.errorMessage =
                            makeExt2Error("ext2fs_open_inode_scan failed for ext4 inode scan worker", retval);
                        sharedAbort.store(true, std::memory_order_relaxed);
                        ext2fs_close(workerFs);
                        return;
                    }

                    retval = ext2fs_inode_scan_goto_blockgroup(scan, beginGroup);
                    if (retval) {
                        result.failed = true;
                        result.errorMessage =
                            makeExt2Error("ext2fs_inode_scan_goto_blockgroup failed for ext4 inode scan worker", retval);
                        sharedAbort.store(true, std::memory_order_relaxed);
                        ext2fs_close_inode_scan(scan);
                        ext2fs_close(workerFs);
                        return;
                    }

                    const uint64_t beginInode =
                        static_cast<uint64_t>(beginGroup) * inodesPerGroup + 1;

                    const uint64_t endInodeExclusive = std::min<uint64_t>(
                        totalInodes + 1,
                        static_cast<uint64_t>(endGroup) * inodesPerGroup + 1
                    );

                    if (endInodeExclusive <= beginInode) {
                        ext2fs_close_inode_scan(scan);
                        ext2fs_close(workerFs);
                        return;
                    }

                    const uint64_t workerInodeSlots = endInodeExclusive - beginInode;
                    const uint64_t workerEstimatedUsedInodes =
                        std::max<uint64_t>(
                            1024,
                            (inodesInUse * workerInodeSlots) / std::max<uint64_t>(totalInodes, 1)
                        );

                    result.inodeStats.reserve(static_cast<std::size_t>(workerEstimatedUsedInodes));
                    result.directoryInodes.reserve(static_cast<std::size_t>(
                        std::max<uint64_t>(256, workerEstimatedUsedInodes / 8)
                    ));

                    ext2_ino_t ino = 0;
                    ext2_inode inode{};
                    dgrp_t lastCompletedGroup = beginGroup;

                    while (!sharedAbort.load(std::memory_order_relaxed)) {
                        retval = ext2fs_get_next_inode(scan, &ino, &inode);
                        if (retval) {
                            result.failed = true;
                            result.errorMessage =
                                makeExt2Error("ext2fs_get_next_inode failed for ext4 inode scan worker", retval);
                            sharedAbort.store(true, std::memory_order_relaxed);
                            break;
                        }

                        if (ino == 0 || static_cast<uint64_t>(ino) >= endInodeExclusive) {
                            break;
                        }

                        if (static_cast<uint64_t>(ino) < beginInode) {
                            continue;
                        }

                        const dgrp_t currentGroup = static_cast<dgrp_t>(
                            (static_cast<uint64_t>(ino) - 1) / inodesPerGroup
                        );

                        while (lastCompletedGroup < currentGroup &&
                               lastCompletedGroup < endGroup) {
                            const uint64_t groupsDone =
                                completedGroups.fetch_add(1, std::memory_order_relaxed) + 1;

                            ++lastCompletedGroup;

                            if (onProgress && ((groupsDone & 31u) == 0)) {
                                const quint64 processed = std::min<quint64>(
                                    totalInodes,
                                    groupsDone * static_cast<uint64_t>(inodesPerGroup)
                                );

                                std::lock_guard lock(progressMutex);
                                onProgress(Protocol::ScanProgress{
                                    .phase = QStringLiteral("Reading inode table"),
                                    .unit = QStringLiteral("slots"),
                                    .processed = processed,
                                    .total = totalInodes
                                });
                            }
                        }

                        if (shouldCancel && shouldCancel()) {
                            result.cancelled = true;
                            sharedAbort.store(true, std::memory_order_relaxed);
                            break;
                        }

                        if (inode.i_links_count == 0) {
                            continue;
                        }

                        ++result.usedInodesSeen;

                        const FileStats stats = makeFileStats(inode);

                        result.inodeStats.push_back(InodeStatsEntry{
                            static_cast<uint32_t>(ino),
                            stats
                        });

                        if ((stats.flags & FileRecord_IsDir) != 0) {
                            result.directoryInodes.push_back(static_cast<uint32_t>(ino));
                        }
                    }

                    while (!result.failed &&
                           !result.cancelled &&
                           lastCompletedGroup < endGroup) {
                        const uint64_t groupsDone =
                            completedGroups.fetch_add(1, std::memory_order_relaxed) + 1;

                        ++lastCompletedGroup;

                        if (onProgress && ((groupsDone & 31u) == 0)) {
                            const quint64 processed = std::min<quint64>(
                                totalInodes,
                                groupsDone * static_cast<uint64_t>(inodesPerGroup)
                            );

                            std::lock_guard lock(progressMutex);
                            onProgress(Protocol::ScanProgress{
                                .phase = QStringLiteral("Reading inode table"),
                                .unit = QStringLiteral("slots"),
                                .processed = processed,
                                .total = totalInodes
                            });
                        }
                    }

                    ext2fs_close_inode_scan(scan);
                    ext2fs_close(workerFs);
                });
            }

            for (std::thread& worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }

            for (const InodeStatsWorkerResult& result : workerResults) {
                if (result.failed) {
                    reportError(
                        onError,
                        result.errorMessage.empty()
                            ? std::string("parallel ext4 inode scan worker failed")
                            : result.errorMessage
                    );
                    return false;
                }

                if (result.cancelled) {
                    return false;
                }
            }

            std::size_t totalStats = 0;
            std::size_t totalDirectories = 0;
            uint64_t totalUsedInodesSeen = 0;

            for (const InodeStatsWorkerResult& result : workerResults) {
                totalStats += result.inodeStats.size();
                totalDirectories += result.directoryInodes.size();
                totalUsedInodesSeen += result.usedInodesSeen;
            }

            inodeStats.clear();
            directoryInodes.clear();

            inodeStats.reserve(totalStats);
            directoryInodes.reserve(totalDirectories);

            for (InodeStatsWorkerResult& result : workerResults) {
                inodeStats.insert(
                    inodeStats.end(),
                    std::make_move_iterator(result.inodeStats.begin()),
                    std::make_move_iterator(result.inodeStats.end())
                );

                directoryInodes.insert(
                    directoryInodes.end(),
                    std::make_move_iterator(result.directoryInodes.begin()),
                    std::make_move_iterator(result.directoryInodes.end())
                );
            }

            if (counters) {
                counters->usedInodesSeen = totalUsedInodesSeen;
                counters->inodeStatsEntries = inodeStats.size();
                counters->directoryInodes = directoryInodes.size();
            }

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Reading inode table"),
                    .unit = QStringLiteral("slots"),
                    .processed = totalInodes,
                    .total = totalInodes
                });
            }

            return true;
        }

        [[nodiscard]] uint32_t chooseDirectoryScanWorkerCount(
            std::size_t directoryCount,
            const ScanOptions& options
        ) noexcept {
            if (directoryCount < kMinDirectoriesForParallelScan) {
                return 1;
            }

            if (options.deviceIsRotational) {
                return 1;
            }

            const uint32_t hardwareThreads =
                std::max(1u, std::thread::hardware_concurrency());

            return static_cast<uint32_t>(
                std::clamp<std::size_t>(
                    std::min<std::size_t>(directoryCount, hardwareThreads),
                    1,
                    kMaxParallelDirectoryWorkers
                )
            );
        }

        void mergeCounters(Ext4ScanCounters& target, const Ext4ScanCounters& source)
        {
            target.directoriesScanned += source.directoriesScanned;
            target.dirCallbackCalls += source.dirCallbackCalls;
            target.dirEntriesSeen += source.dirEntriesSeen;
            target.dirEntriesSkippedEmptyInode += source.dirEntriesSkippedEmptyInode;
            target.dirEntriesSkippedEmptyName += source.dirEntriesSkippedEmptyName;
            target.dirEntriesSkippedDot += source.dirEntriesSkippedDot;
            target.dirEntriesSkippedMissingStats += source.dirEntriesSkippedMissingStats;
            target.recordsEmitted += source.recordsEmitted;
            target.flushCalls += source.flushCalls;
            target.fileRecordChunks += source.fileRecordChunks;
            target.stringPoolChunks += source.stringPoolChunks;
        }

#ifdef KERYTHING_ENABLE_LOGGING
        void mergeTimings(Ext4ScanTimings& target, const Ext4ScanTimings& source)
        {
            target.dirIterateCalls += source.dirIterateCalls;
            target.dirCallback += source.dirCallback;
            target.inodeStatsLookup += source.inodeStatsLookup;
            target.streamFlush += source.streamFlush;
        }
#endif

        bool flushWorkerStream(Ext4StreamState& stream,
                               std::mutex& emitMutex,
                               uint32_t& globalStringPoolLength,
                               const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                               const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                               Ext4ScanTimings* timings,
                               Ext4ScanCounters* counters)
        {
#ifdef KERYTHING_ENABLE_LOGGING
            std::optional<ScopedAccumulatedTimer> flushTimer;
            if (timings) {
                flushTimer.emplace(timings->streamFlush);
            }
#else
            Q_UNUSED(timings);
#endif

            if (stream.records.empty() && stream.stringPool.empty()) {
                return true;
            }

            if (counters) {
                ++counters->flushCalls;
            }

            std::vector<FileRecord> fileRecordChunk = std::move(stream.records);
            std::vector<char> stringPoolChunk = std::move(stream.stringPool);

            stream.records.clear();
            stream.records.reserve(Ext4StreamState::kRecordsPerIpcChunk);

            stream.stringPool.clear();
            stream.stringPool.reserve(Ext4StreamState::kMaxIpcBufferSizeBytes);

            std::lock_guard lock(emitMutex);

            const uint32_t globalBase = globalStringPoolLength;
            globalStringPoolLength += static_cast<uint32_t>(stringPoolChunk.size());

            for (FileRecord& record : fileRecordChunk) {
                record.nameOffset += globalBase;
            }

            if (!fileRecordChunk.empty()) {
                if (counters) {
                    ++counters->fileRecordChunks;
                }

                if (!onFileRecordChunk(fileRecordChunk)) {
                    std::cerr << "[Ext4ScannerEngine] scan aborted by file record receiver\n";
                    return false;
                }
            }

            if (!stringPoolChunk.empty()) {
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

        bool addRecordToWorkerStream(Ext4StreamState& stream,
                                     uint32_t inode,
                                     uint32_t parentInode,
                                     std::string_view name,
                                     const FileStats& stats,
                                     std::mutex& emitMutex,
                                     uint32_t& globalStringPoolLength,
                                     const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                                     const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                                     Ext4ScanTimings* timings,
                                     Ext4ScanCounters* counters)
        {
            if (name.size() > std::numeric_limits<uint16_t>::max()) {
                return true;
            }

            if (stream.records.size() >= Ext4StreamState::kRecordsPerIpcChunk ||
                stream.stringPool.size() + name.size() >= Ext4StreamState::kMaxIpcBufferSizeBytes) {
                if (!flushWorkerStream(
                        stream,
                        emitMutex,
                        globalStringPoolLength,
                        onFileRecordChunk,
                        onStringPoolChunk,
                        timings,
                        counters
                    )) {
                    return false;
                }
            }

            FileRecord record{};
            record.fsIndex = inode;
            record.parentFsIndex = parentInode;
            record.parentRecordIdx = kInvalidRecordIndex;
            record.size = stats.size;
            record.modificationTime = stats.modificationTime;
            record.nameOffset = stream.totalStringPoolLength + static_cast<uint32_t>(stream.stringPool.size());
            record.nameLen = static_cast<uint16_t>(name.size());
            record.flags = stats.flags;

            stream.records.push_back(record);
            stream.stringPool.insert(stream.stringPool.end(), name.begin(), name.end());

            if (counters) {
                ++counters->recordsEmitted;
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
        std::mutex* emitMutex = nullptr;
        uint32_t* globalStringPoolLength = nullptr;
        std::atomic_bool* sharedAbort = nullptr;
        uint32_t entriesSinceCancelCheck = 0;
        bool cancelled = false;
        bool failed = false;
    };

    bool Ext4StreamState::flush(const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                                const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                                Ext4ScanTimings* timings,
                                Ext4ScanCounters* counters) {
#ifdef KERYTHING_ENABLE_LOGGING
        std::optional<ScopedAccumulatedTimer> flushTimer;
        if (timings) {
            flushTimer.emplace(timings->streamFlush);
        }
#else
        Q_UNUSED(timings);
#endif

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

        if (!ctx || ctx->failed || ctx->cancelled ||
            (ctx->sharedAbort && ctx->sharedAbort->load(std::memory_order_relaxed))) {
            return 1;
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::optional<ScopedAccumulatedTimer> callbackTimer;
        if (ctx->timings) {
            callbackTimer.emplace(ctx->timings->dirCallback);
        }
#endif

        if (ctx->counters) {
            ++ctx->counters->dirCallbackCalls;
        }

        ++ctx->entriesSinceCancelCheck;
        if ((ctx->entriesSinceCancelCheck & (kDirEntryCancelCheckEvery - 1)) == 0) {
            if ((ctx->sharedAbort && ctx->sharedAbort->load(std::memory_order_relaxed)) ||
                (ctx->shouldCancel && ctx->shouldCancel())) {
                ctx->cancelled = true;

                if (ctx->sharedAbort) {
                    ctx->sharedAbort->store(true, std::memory_order_relaxed);
                }

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
#ifdef KERYTHING_ENABLE_LOGGING
            std::optional<ScopedAccumulatedTimer> lookupTimer;
            if (ctx->timings) {
                lookupTimer.emplace(ctx->timings->inodeStatsLookup);
            }
#endif

            stats = findStatsByInode(ctx->inodeStatsLookup, dirent->inode);
        }

        if (!stats) {
            if (ctx->counters) {
                ++ctx->counters->dirEntriesSkippedMissingStats;
            }

            return 0;
        }

        const std::string_view name(dirent->name, len);

        bool added = false;

        if (ctx->emitMutex && ctx->globalStringPoolLength) {
            added = addRecordToWorkerStream(ctx->stream,
                                            static_cast<uint32_t>(dirent->inode),
                                            static_cast<uint32_t>(dir_ino),
                                            name,
                                            *stats,
                                            *ctx->emitMutex,
                                            *ctx->globalStringPoolLength,
                                            ctx->onFileRecordChunk,
                                            ctx->onStringPoolChunk,
                                            ctx->timings,
                                            ctx->counters);
        } else {
            added = ctx->stream.addRecord(static_cast<uint32_t>(dirent->inode),
                                          static_cast<uint32_t>(dir_ino),
                                          name,
                                          *stats,
                                          ctx->onFileRecordChunk,
                                          ctx->onStringPoolChunk,
                                          ctx->timings,
                                          ctx->counters);
        }

        if (!added) {
            ctx->failed = true;

            if (ctx->sharedAbort) {
                ctx->sharedAbort->store(true, std::memory_order_relaxed);
            }

            return 1;
        }

        return 0;
    }

    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                    const ScannerHelper::ErrorCallback& onError,
                    const ScannerHelper::CancelCallback& shouldCancel,
                    const ScannerHelper::ProgressCallback& onProgress,
                    const ScanOptions& options) {
#ifdef KERYTHING_ENABLE_LOGGING
        ScopedTimer totalTimer("[Ext4ScannerEngine] total ext4 scan");

        Ext4ScanTimings timings;
        Ext4ScanCounters counters;
        Ext4ScanTimings* profileTimings = &timings;
        Ext4ScanCounters* profileCounters = &counters;
#else
        Ext4ScanTimings* profileTimings = nullptr;
        Ext4ScanCounters* profileCounters = nullptr;
#endif

        ext2_filsys fs = nullptr;
        const std::string devicePathStd = devicePath.toStdString();

        errcode_t retval = 0;
        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedAccumulatedTimer timer(profileTimings->open);
#endif
            retval = ext2fs_open(devicePathStd.c_str(), 0, 0, 0, unix_io_manager, &fs);
        }

        if (retval) {
            reportError(onError, makeExt2Error("ext2fs_open failed", retval));
            return false;
        }

        const uint32_t totalInodes = fs->super->s_inodes_count;
        const uint32_t freeInodes  = fs->super->s_free_inodes_count;
        const uint32_t inodesInUse = (freeInodes <= totalInodes) ? (totalInodes - freeInodes) : totalInodes;

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "[Ext4ScannerEngine] totalInodes=" << totalInodes
                  << " freeInodes=" << freeInodes
                  << " estimatedInodesInUse=" << inodesInUse
                  << "\n";
#endif

        std::vector<InodeStatsEntry> inodeStats;
        std::vector<uint32_t> directoryInodes;

        inodeStats.reserve(static_cast<std::size_t>(inodesInUse));

        const std::size_t estimatedDirectoryInodes = std::min<std::size_t>(
            static_cast<std::size_t>(inodesInUse),
            std::max<std::size_t>(
                static_cast<std::size_t>(inodesInUse) / 8,
                4096
            )
        );

        directoryInodes.reserve(estimatedDirectoryInodes);

        const uint32_t inodeScanWorkerCount =
            chooseInodeScanWorkerCount(fs, totalInodes, inodesInUse, options);

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "[Ext4ScannerEngine] inode scan workers="
                  << inodeScanWorkerCount
                  << " deviceIsRotational="
                  << (options.deviceIsRotational ? "true" : "false")
                  << "\n";
#endif

        bool inodeStatsCollected = false;
        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedAccumulatedTimer timer(profileTimings->inodeStatsScan);
#endif

            if (inodeScanWorkerCount > 1) {
                inodeStatsCollected = collectInodeStatsParallelByGroup(
                    devicePath,
                    fs,
                    inodeStats,
                    directoryInodes,
                    totalInodes,
                    inodesInUse,
                    inodeScanWorkerCount,
                    onError,
                    shouldCancel,
                    onProgress,
                    profileCounters
                );
            } else {
                inodeStatsCollected = collectInodeStats(
                    fs,
                    inodeStats,
                    directoryInodes,
                    totalInodes,
                    onError,
                    shouldCancel,
                    onProgress,
                    profileCounters
                );
            }
        }

        if (!inodeStatsCollected) {
            {
#ifdef KERYTHING_ENABLE_LOGGING
                ScopedAccumulatedTimer timer(profileTimings->close);
#endif
                ext2fs_close(fs);
            }

#ifdef KERYTHING_ENABLE_LOGGING
            logExt4ScanProfile(timings, counters);
#endif
            return false;
        }

        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedTimer timer("[Ext4ScannerEngine] inode stats sort");
            ScopedAccumulatedTimer profileTimer(timings.inodeStatsSort);
#endif

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

            if (!std::is_sorted(directoryInodes.begin(), directoryInodes.end())) {
                std::sort(directoryInodes.begin(), directoryInodes.end());
            }
        }

        const uint32_t inodesPerGroup = fs->super->s_inodes_per_group;

        InodeStatsLookup inodeStatsLookup;
        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedAccumulatedTimer timer(profileTimings->inodeStatsLookupBuild);
#endif
            inodeStatsLookup = buildInodeStatsLookup(
                inodeStats,
                totalInodes,
                inodesPerGroup
            );
        }

        if (profileCounters) {
            profileCounters->inodeStatsLookupBuckets = inodeStatsLookup.buckets.size();
            profileCounters->inodeStatsLookupBucketSpan = inodeStatsLookup.inodeSpanPerBucket;
        }

        Ext4StreamState stream{};
        stream.records.reserve(Ext4StreamState::kRecordsPerIpcChunk);
        stream.stringPool.reserve(Ext4StreamState::kMaxIpcBufferSizeBytes);

        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedTimer timer("[Ext4ScannerEngine] directory entry streaming");
            ScopedAccumulatedTimer directoryStreamingTimer(profileTimings->directoryStreaming);
#endif

            const FileStats* rootStats = findStatsByInode(inodeStatsLookup, EXT2_ROOT_INO);
            if (rootStats) {
#ifdef KERYTHING_ENABLE_LOGGING
                ScopedAccumulatedTimer rootTimer(profileTimings->rootRecordEmit);
#endif

                if (!stream.addRecord(EXT2_ROOT_INO,
                                      EXT2_ROOT_INO,
                                      std::string_view{},
                                      *rootStats,
                                      onFileRecordChunk,
                                      onStringPoolChunk,
                                      profileTimings,
                                      profileCounters)) {
                    {
#ifdef KERYTHING_ENABLE_LOGGING
                        ScopedAccumulatedTimer closeTimer(profileTimings->close);
#endif
                        ext2fs_close(fs);
                    }

#ifdef KERYTHING_ENABLE_LOGGING
                    logExt4ScanProfile(timings, counters);
#endif
                    return false;
                }

                if (!stream.flush(
                        onFileRecordChunk,
                        onStringPoolChunk,
                        profileTimings,
                        profileCounters
                    )) {
                    {
#ifdef KERYTHING_ENABLE_LOGGING
                        ScopedAccumulatedTimer closeTimer(profileTimings->close);
#endif
                        ext2fs_close(fs);
                    }

#ifdef KERYTHING_ENABLE_LOGGING
                    logExt4ScanProfile(timings, counters);
#endif
                    return false;
                }
            }

            if (onProgress) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Scanning directories"),
                    .unit = QStringLiteral("directories"),
                    .processed = 0,
                    .total = static_cast<quint64>(directoryInodes.size())
                });
            }

            const uint32_t workerCount =
                chooseDirectoryScanWorkerCount(directoryInodes.size(), options);

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "[Ext4ScannerEngine] directory scan workers="
                      << workerCount
                      << " directories="
                      << directoryInodes.size()
                      << " deviceIsRotational="
                      << (options.deviceIsRotational ? "true" : "false")
                      << "\n";
#endif

            if (workerCount <= 1) {
                DirCallbackContext ctx{
                    fs,
                    stream,
                    inodeStatsLookup,
                    onFileRecordChunk,
                    onStringPoolChunk,
                    shouldCancel,
                    profileTimings,
                    profileCounters
                };

                uint64_t directoriesScanned = 0;

                for (const uint32_t dirInode : directoryInodes) {
                    if ((directoriesScanned & (kDirectoryCancelCheckEvery - 1)) == 0) {
                        if (shouldCancel && shouldCancel()) {
                            {
#ifdef KERYTHING_ENABLE_LOGGING
                                ScopedAccumulatedTimer closeTimer(profileTimings->close);
#endif
                                ext2fs_close(fs);
                            }

#ifdef KERYTHING_ENABLE_LOGGING
                            logExt4ScanProfile(timings, counters);
#endif
                            return false;
                        }
                    }

                    {
#ifdef KERYTHING_ENABLE_LOGGING
                        ScopedAccumulatedTimer iterateTimer(profileTimings->dirIterateCalls);
#endif
                        retval = ext2fs_dir_iterate2(fs,
                                                     dirInode,
                                                     0,
                                                     nullptr,
                                                     dirCallback,
                                                     &ctx);
                    }

                    if (ctx.cancelled || ctx.failed) {
                        {
#ifdef KERYTHING_ENABLE_LOGGING
                            ScopedAccumulatedTimer closeTimer(profileTimings->close);
#endif
                            ext2fs_close(fs);
                        }

#ifdef KERYTHING_ENABLE_LOGGING
                        logExt4ScanProfile(timings, counters);
#endif
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
#ifdef KERYTHING_ENABLE_LOGGING
                    counters.directoriesScanned = directoriesScanned;
#endif

                    if (onProgress && ((directoriesScanned & (kProgressEvery - 1)) == 0)) {
                        onProgress(Protocol::ScanProgress{
                            .phase = QStringLiteral("Scanning directories"),
                            .unit = QStringLiteral("directories"),
                            .processed = directoriesScanned,
                            .total = static_cast<quint64>(directoryInodes.size())
                        });
                    }
                }
            } else {
                std::atomic_size_t nextDirectoryIndex{0};
                std::atomic_uint64_t directoriesScanned{0};
                std::atomic_bool sharedAbort{false};

                std::mutex emitMutex;
                std::mutex progressMutex;

                uint32_t globalStringPoolLength = stream.totalStringPoolLength;

                std::vector<Ext4ScanCounters> workerCounters(workerCount);
#ifdef KERYTHING_ENABLE_LOGGING
                std::vector<Ext4ScanTimings> workerTimings(workerCount);
#endif
                std::vector<std::thread> workers;
                workers.reserve(workerCount);

                for (uint32_t workerIdx = 0; workerIdx < workerCount; ++workerIdx) {
                    workers.emplace_back([&, workerIdx]() {
                        ext2_filsys workerFs = nullptr;
                        errcode_t workerOpenResult =
                            ext2fs_open(devicePathStd.c_str(), 0, 0, 0, unix_io_manager, &workerFs);

                        if (workerOpenResult) {
                            {
                                std::lock_guard lock(progressMutex);
                                reportError(
                                    onError,
                                    makeExt2Error("ext2fs_open failed for ext4 directory scan worker", workerOpenResult)
                                );
                            }

                            sharedAbort.store(true, std::memory_order_relaxed);
                            return;
                        }

                        Ext4StreamState workerStream{};
                        workerStream.records.reserve(Ext4StreamState::kRecordsPerIpcChunk);
                        workerStream.stringPool.reserve(Ext4StreamState::kMaxIpcBufferSizeBytes);

                        Ext4ScanCounters* workerCounter = &workerCounters[workerIdx];
#ifdef KERYTHING_ENABLE_LOGGING
                        Ext4ScanTimings* workerTiming = &workerTimings[workerIdx];
#else
                        Ext4ScanTimings* workerTiming = nullptr;
#endif

                        DirCallbackContext ctx{
                            workerFs,
                            workerStream,
                            inodeStatsLookup,
                            onFileRecordChunk,
                            onStringPoolChunk,
                            shouldCancel,
                            workerTiming,
                            workerCounter,
                            &emitMutex,
                            &globalStringPoolLength,
                            &sharedAbort
                        };

                        while (!sharedAbort.load(std::memory_order_relaxed)) {
                            const std::size_t begin =
                                nextDirectoryIndex.fetch_add(
                                    kParallelDirectoryChunkSize,
                                    std::memory_order_relaxed
                                );

                            if (begin >= directoryInodes.size()) {
                                break;
                            }

                            const std::size_t end = std::min<std::size_t>(
                                begin + kParallelDirectoryChunkSize,
                                directoryInodes.size()
                            );

                            for (std::size_t i = begin; i < end; ++i) {
                                if (sharedAbort.load(std::memory_order_relaxed)) {
                                    break;
                                }

                                if (((directoriesScanned.load(std::memory_order_relaxed) &
                                      (kDirectoryCancelCheckEvery - 1)) == 0) &&
                                    shouldCancel &&
                                    shouldCancel()) {
                                    sharedAbort.store(true, std::memory_order_relaxed);
                                    ctx.cancelled = true;
                                    break;
                                }

                                errcode_t iterateResult = 0;

                                {
#ifdef KERYTHING_ENABLE_LOGGING
                                    ScopedAccumulatedTimer iterateTimer(workerTiming->dirIterateCalls);
#endif
                                    iterateResult = ext2fs_dir_iterate2(workerFs,
                                                                       directoryInodes[i],
                                                                       0,
                                                                       nullptr,
                                                                       dirCallback,
                                                                       &ctx);
                                }

                                if (ctx.cancelled || ctx.failed) {
                                    sharedAbort.store(true, std::memory_order_relaxed);
                                    break;
                                }

                                if (iterateResult) {
                                    // Some directories may be unreadable/corrupt. Report it, but continue.
                                    std::lock_guard lock(progressMutex);
                                    std::cerr << "[Ext4ScannerEngine] ext2fs_dir_iterate2 failed for inode="
                                              << directoryInodes[i]
                                              << ": "
                                              << error_message(iterateResult)
                                              << "\n";
                                }

                                const uint64_t scanned =
                                    directoriesScanned.fetch_add(1, std::memory_order_relaxed) + 1;

                                ++workerCounter->directoriesScanned;

                                if (onProgress && ((scanned & (kProgressEvery - 1)) == 0)) {
                                    std::lock_guard lock(progressMutex);
                                    onProgress(Protocol::ScanProgress{
                                        .phase = QStringLiteral("Scanning directories"),
                                        .unit = QStringLiteral("directories"),
                                        .processed = scanned,
                                        .total = static_cast<quint64>(directoryInodes.size())
                                    });
                                }
                            }
                        }

                        if (!sharedAbort.load(std::memory_order_relaxed)) {
                            if (!flushWorkerStream(
                                    workerStream,
                                    emitMutex,
                                    globalStringPoolLength,
                                    onFileRecordChunk,
                                    onStringPoolChunk,
                                    workerTiming,
                                    workerCounter
                                )) {
                                sharedAbort.store(true, std::memory_order_relaxed);
                            }
                        }

                        ext2fs_close(workerFs);
                    });
                }

                for (std::thread& worker : workers) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }

#ifdef KERYTHING_ENABLE_LOGGING
                counters.directoriesScanned = 0;
#endif

                for (const Ext4ScanCounters& workerCounter : workerCounters) {
                    if (profileCounters) {
                        mergeCounters(*profileCounters, workerCounter);
                    }
                }

#ifdef KERYTHING_ENABLE_LOGGING
                for (const Ext4ScanTimings& workerTiming : workerTimings) {
                    mergeTimings(*profileTimings, workerTiming);
                }
#endif

                stream.totalStringPoolLength = globalStringPoolLength;

                if (sharedAbort.load(std::memory_order_relaxed)) {
                    {
#ifdef KERYTHING_ENABLE_LOGGING
                        ScopedAccumulatedTimer closeTimer(profileTimings->close);
#endif
                        ext2fs_close(fs);
                    }

#ifdef KERYTHING_ENABLE_LOGGING
                    logExt4ScanProfile(timings, counters);
#endif
                    return false;
                }
            }
        }

        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedAccumulatedTimer timer(profileTimings->finalFlush);
#endif
            if (!stream.flush(onFileRecordChunk, onStringPoolChunk, profileTimings, profileCounters)) {
                {
#ifdef KERYTHING_ENABLE_LOGGING
                    ScopedAccumulatedTimer closeTimer(profileTimings->close);
#endif
                    ext2fs_close(fs);
                }

#ifdef KERYTHING_ENABLE_LOGGING
                logExt4ScanProfile(timings, counters);
#endif
                return false;
            }
        }

        {
#ifdef KERYTHING_ENABLE_LOGGING
            ScopedAccumulatedTimer timer(profileTimings->close);
#endif
            ext2fs_close(fs);
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "[Ext4ScannerEngine] emitted stringPoolBytes="
                  << stream.totalStringPoolLength
                  << " inodeStatsCount="
                  << inodeStats.size()
                  << " directoryCount="
                  << directoryInodes.size()
                  << "\n";
#endif

        if (onProgress) {
            onProgress(Protocol::ScanProgress{
                .phase = QStringLiteral("Scanning directories"),
                .unit = QStringLiteral("directories"),
                .processed = static_cast<quint64>(directoryInodes.size()),
                .total = static_cast<quint64>(directoryInodes.size())
            });
        }

#ifdef KERYTHING_ENABLE_LOGGING
        logExt4ScanProfile(timings, counters);
#endif

        return true;
    }

} // namespace Ext4ScannerEngine