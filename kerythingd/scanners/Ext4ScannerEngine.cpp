// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Ext4ScannerEngine.h"

#include <algorithm>
#include <string_view>

#include "ScopedTimer.h"

namespace Ext4ScannerEngine {

    namespace {

        constexpr uint32_t kInvalidRecordIndex = 0xFFFFFFFF;
        constexpr uint64_t kProgressEvery = 4096; // must be power of two

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

        [[nodiscard]] bool collectInodeStats(ext2_filsys fs,
                                             std::vector<InodeStatsEntry>& inodeStats,
                                             std::vector<uint32_t>& directoryInodes,
                                             uint64_t inodesInUse,
                                             const ScannerHelper::ErrorCallback& onError,
                                             const ScannerHelper::CancelCallback& shouldCancel,
                                             const ScannerHelper::ProgressCallback& onProgress) {
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
                onProgress(0, inodesInUse);
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
                    onProgress(usedInodesSeen, inodesInUse);
                }

                if (shouldCancel && shouldCancel()) {
                    ext2fs_close_inode_scan(scan);
                    return false;
                }
            }

            ext2fs_close_inode_scan(scan);

            if (onProgress) {
                onProgress(inodesInUse, inodesInUse);
            }

            return true;
        }

    } // namespace

    struct DirCallbackContext {
        ext2_filsys fs = nullptr;
        Ext4StreamState& stream;
        const std::vector<InodeStatsEntry>& inodeStats;
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk;
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk;
        const ScannerHelper::CancelCallback& shouldCancel;
        bool cancelled = false;
        bool failed = false;
    };

    bool Ext4StreamState::flush(const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                                const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk) {
        if (!records.empty()) {
            std::vector<FileRecord> fileRecordChunk = std::move(records);
            records.clear();
            records.reserve(kRecordsPerIpcChunk);

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
                                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk) {
        if (name.size() > std::numeric_limits<uint16_t>::max()) {
            return true;
        }

        if (records.size() >= kRecordsPerIpcChunk ||
            stringPool.size() + name.size() >= kMaxIpcBufferSizeBytes) {
            if (!flush(onFileRecordChunk, onStringPoolChunk)) {
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

        if (ctx->shouldCancel && ctx->shouldCancel()) {
            ctx->cancelled = true;
            return 1;
        }

        // Ignore invalid entries or empty inodes
        if (dirent->inode == 0) {
            return 0;
        }

        // dirent->name_len is sometimes encoded with file type info in modern EXT4,
        // so we mask it with 0xFF to get the actual length.
        const uint16_t len = dirent->name_len & 0xFF;
        if (len == 0) {
            return 0;
        }

        // Ignore '.' and '..'
        if (len == 1 && dirent->name[0] == '.') {
            return 0;
        }
        if (len == 2 && dirent->name[0] == '.' && dirent->name[1] == '.') {
            return 0;
        }

        const auto* stats = findStatsByInode(ctx->inodeStats, dirent->inode);
        if (!stats) {
            return 0;
        }

        const std::string_view name(dirent->name, len);

        if (!ctx->stream.addRecord(static_cast<uint32_t>(dirent->inode),
                                   static_cast<uint32_t>(dir_ino),
                                   name,
                                   *stats,
                                   ctx->onFileRecordChunk,
                                   ctx->onStringPoolChunk)) {
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

        ext2_filsys fs = nullptr;
        const std::string devicePathStd = devicePath.toStdString();

        errcode_t retval = ext2fs_open(devicePathStd.c_str(), 0, 0, 0, unix_io_manager, &fs);
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

        inodeStats.reserve(static_cast<size_t>(std::min<uint32_t>(inodesInUse, 1'000'000)));
        directoryInodes.reserve(65536);

        if (!collectInodeStats(fs,
                               inodeStats,
                               directoryInodes,
                               inodesInUse,
                               onError,
                               shouldCancel,
                               onProgress)) {
            ext2fs_close(fs);
            return false;
        }

        {
            ScopedTimer timer("[Ext4ScannerEngine] inode stats sort");

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

        Ext4StreamState stream{};
        stream.records.reserve(Ext4StreamState::kRecordsPerIpcChunk);
        stream.stringPool.reserve(Ext4StreamState::kMaxIpcBufferSizeBytes);

        {
            ScopedTimer timer("[Ext4ScannerEngine] directory entry streaming");

            const FileStats* rootStats = findStatsByInode(inodeStats, EXT2_ROOT_INO);
            if (rootStats) {
                if (!stream.addRecord(EXT2_ROOT_INO,
                                      EXT2_ROOT_INO,
                                      std::string_view{},
                                      *rootStats,
                                      onFileRecordChunk,
                                      onStringPoolChunk)) {
                    ext2fs_close(fs);
                    return false;
                }
            }

            DirCallbackContext ctx{
                fs,
                stream,
                inodeStats,
                onFileRecordChunk,
                onStringPoolChunk,
                shouldCancel
            };

            uint64_t directoriesScanned = 0;

            for (const uint32_t dirInode : directoryInodes) {
                if (shouldCancel && shouldCancel()) {
                    ext2fs_close(fs);
                    return false;
                }

                retval = ext2fs_dir_iterate2(fs,
                                             dirInode,
                                             0,
                                             nullptr,
                                             dirCallback,
                                             &ctx);

                if (ctx.cancelled) {
                    ext2fs_close(fs);
                    return false;
                }

                if (ctx.failed) {
                    ext2fs_close(fs);
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

                if (onProgress && ((directoriesScanned & (kProgressEvery - 1)) == 0)) {
                    onProgress(directoriesScanned, directoryInodes.size());
                }
            }
        }

        if (!stream.flush(onFileRecordChunk, onStringPoolChunk)) {
            ext2fs_close(fs);
            return false;
        }

        ext2fs_close(fs);

        std::cerr << "[Ext4ScannerEngine] emitted stringPoolBytes="
                  << stream.totalStringPoolLength
                  << " inodeStatsCount="
                  << inodeStats.size()
                  << " directoryCount="
                  << directoryInodes.size()
                  << "\n";

        if (onProgress) {
            onProgress(directoryInodes.size(), directoryInodes.size());
        }

        return true;
    }

} // namespace Ext4ScannerEngine