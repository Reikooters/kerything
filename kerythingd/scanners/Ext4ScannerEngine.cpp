// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Ext4ScannerEngine.h"

namespace Ext4ScannerEngine {

    // Helper struct to pass multiple pieces of data to dirCallback
    struct ScanContext {
        Ext4Database& db;
        uint32_t maxInodes;
    };

    int dirCallback(ext2_ino_t dir_ino, int entry_flags, struct ext2_dir_entry *dirent,
                    int offset, int blocksize, char *buf, void *priv_data) {
        // Ignore invalid entries or empty inodes
        if (dirent->inode == 0) {
            return 0;
        }

        // dirent->name_len is sometimes encoded with file type info in modern EXT4,
        // so we mask it with 0xFF to get the actual length.
        uint16_t len = dirent->name_len & 0xFF;
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

        // Get scan context
        ScanContext *ctx = static_cast<ScanContext *>(priv_data);

        uint32_t max_inodes = ctx->maxInodes;
        if (dirent->inode > max_inodes) {
            return 0;
        }

        // Check if this inode already has a record (e.g. from another hard link)
        auto it = ctx->db.inodeToRecordIdx.find(dirent->inode);
        uint32_t recordIndex;

        if (it == ctx->db.inodeToRecordIdx.end()) {
            // "Birth" the record here because we have a name
            FileRecord newRecord{};
            newRecord.parentRecordIdx = 0xFFFFFFFF;

            ctx->db.records.push_back(newRecord);
            recordIndex = ctx->db.records.size() - 1;
            ctx->db.inodeToRecordIdx[dirent->inode] = recordIndex;

            // Keep the parallel vector in sync
            ctx->db.tempParentInodes.push_back(dir_ino);
        } else {
            recordIndex = it->second;
            // Update the parent just in case (though usually doesn't change for the same name)
            ctx->db.tempParentInodes[recordIndex] = dir_ino;
        }

        // Store the name in the string pool
        FileRecord& record = ctx->db.records[recordIndex];
        record.nameOffset = ctx->db.stringPool.size();
        record.nameLen = len;
        ctx->db.stringPool.insert(ctx->db.stringPool.end(), dirent->name, dirent->name + len);

        return 0;
    }

    bool scanDevice(const QString& devicePath,
                const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                const ScannerHelper::ErrorCallback& onError,
                const ScannerHelper::CancelCallback& shouldCancel,
                const ScannerHelper::ProgressCallback& onProgress) {
        ext2_filsys fs;
        errcode_t retval = ext2fs_open(devicePath.toStdString().c_str(), 0, 0, 0, unix_io_manager, &fs);
        if (retval) {
            onError(QStringLiteral("ext2fs_open failed: %1").arg(error_message(retval)));
            return false;
        }

        const uint32_t totalInodes = fs->super->s_inodes_count;
        const uint32_t freeInodes  = fs->super->s_free_inodes_count;
        const uint32_t inodesInUse = (freeInodes <= totalInodes) ? (totalInodes - freeInodes) : totalInodes;

        Ext4Database db{};
        db.records.reserve(Ext4Database::kRecordsPerIpcChunk);
        db.tempParentInodes.reserve(Ext4Database::kRecordsPerIpcChunk);
        db.inodeToRecordIdx.reserve(Ext4Database::kRecordsPerIpcChunk);
        db.inodeToFileStats.reserve(Ext4Database::kRecordsPerIpcChunk);

        db.stringPool.reserve(Ext4Database::kMaxIpcBufferSizeBytes);

        // totalStringPoolLength is not used yet. doesn't break anything right now,
        // because we only send the chunks at the very end of the scan. Will break nameOffset
        // in FileRecords when we switch to sending chunks as we process them unless we
        // switch to using this instead of `ctx->db.stringPool.size()`.
        db.totalStringPoolLength = 0;

        // Explicitly add the root entry first
        FileRecord rootRec{};
        rootRec.parentRecordIdx = 0xFFFFFFFF;
        db.records.push_back(rootRec);
        db.inodeToRecordIdx[EXT2_ROOT_INO] = 0;
        db.tempParentInodes.push_back(0);

        int bufferBlocks = 4096;

        ext2_inode_scan scan;
        ext2fs_open_inode_scan(fs, bufferBlocks, &scan);

        ext2_ino_t ino;
        ext2_inode inode;

        ScanContext ctx{db, totalInodes};

        uint64_t usedInodesSeen = 0;

        if (onProgress) {
            onProgress(0, inodesInUse);
        }

        // Crawl the directory tree to discover all names and structure
        // We start from the root inode (2) and let ext2fs_dir_iterate2 recurse
        // through directories.
        while (ext2fs_get_next_inode(scan, &ino, &inode) == 0 && ino != 0) {
            if (shouldCancel && shouldCancel()) {
                ext2fs_close_inode_scan(scan);
                ext2fs_close(fs);
                return false;
            }

            if (inode.i_links_count == 0) {
                continue;
            }

            ++usedInodesSeen;

            FileStats stats{};
            stats.size = EXT2_I_SIZE(&inode);
            stats.modificationTime = inode.i_mtime;

            if (LINUX_S_ISDIR(inode.i_mode)) {
                stats.flags |= FileRecord_IsDir;
            }

            if (LINUX_S_ISLNK(inode.i_mode)) {
                stats.flags |= FileRecord_IsSymlink;
            }

            db.inodeToFileStats[ino] = stats;

            // Notify progress
            static constexpr uint64_t kProgressEvery = 4096; // must be power of two
            if (onProgress && ((usedInodesSeen & (kProgressEvery - 1)) == 0)) {
                onProgress(usedInodesSeen, inodesInUse);
            }

            if (LINUX_S_ISDIR(inode.i_mode)) {
                // This will trigger dirCallback for every file inside this directory
                ext2fs_dir_iterate2(fs, ino, 0, nullptr, dirCallback, &ctx);
            }
        }

        // Close the scan
        ext2fs_close_inode_scan(scan);
        ext2fs_close(fs);

        // Resolve parent Inodes to parent Record Indices
        db.resolveParentPointers();

        // Populate stats into records
        db.populateStatsIntoRecords();

        // Flush remaining file records
        if (!db.records.empty()) {
            // Copy string pool chunk, so we can clear it after sending
            std::vector<FileRecord> fileRecordChunk = db.records;
            if (!onFileRecordChunk(fileRecordChunk)) {
                // TODO: scan aborted by receiver
                std::cerr << "scan aborted by receiver\n";
                return false;
            }
            db.records.clear();
        }

        // Flush remaining string pool records
        if (!db.stringPool.empty()) {
            // Copy string pool chunk, so we can clear it after sending
            std::vector<char> stringPoolChunk = db.stringPool;
            if (!onStringPoolChunk(stringPoolChunk)) {
                // TODO: scan aborted by receiver
                std::cerr << "scan aborted by receiver\n";
                return false;
            }
            db.stringPool.clear();
        }

        // Report completion
        if (onProgress) {
            onProgress(inodesInUse, inodesInUse);
        }

        // TODO FOR SHANE IN FUTURE:
        // Currently, the Ext4ScannerEngine builds both a vector of file stats,
        // and a vector of FileRecords which have the name only.
        //
        // At the end, `db.resolveParentPointers();` and `db.populateStatsIntoRecords();`
        // resolve the parent pointers and populate stats into the FileRecords respectively.
        // The implementations of these functions have not been copied into the project yet.
        //
        // Ideally we want this to work similar as possible to the NTFS implementation:
        // - Try to complete FileRecords early if possible
        // - Once we have filled a buffer of complete FileRecords, push them over the socket
        // - Then clear the buffer
        //
        // If it can't be done, then we will have to wait until the end to send all records
        // at once like we did previously.
        //
        // Furthermore, Ext4Database currently has an `add()` function in Ext4ScannerEngine.h
        // which is currently not used - it was simply copied from the NtfsDatabase
        // and should be either used if possible or just removed if not needed.

        return true;
    }

    // We call this once after the scan is completely finished
    void Ext4Database::resolveParentPointers() {
        std::cerr << "Resolving parent pointers..." << std::endl;

        // Convert parent inode index to internal index
        for (size_t i = 0; i < records.size(); ++i) {
            uint64_t parentInode = tempParentInodes[i]; // Look up from parallel vector

            // If parent inode is the root inode, mark as root
            if (parentInode == EXT2_ROOT_INO) {
                records[i].parentRecordIdx = 0xFFFFFFFF;
                continue;
            }

            auto it = inodeToRecordIdx.find(parentInode);
            if (it != inodeToRecordIdx.end()) {
                records[i].parentRecordIdx = it->second;
            } else {
                // If parent isn't in our DB, mark as root
                records[i].parentRecordIdx = 0xFFFFFFFF;
            }
        }

        // Clean up parent inodes temporary data
        tempParentInodes.clear();
        tempParentInodes.shrink_to_fit();
    }

    void Ext4Database::populateStatsIntoRecords() {
        std::cerr << "Populating stats into records..." << std::endl;

        for (auto& it : inodeToRecordIdx) {
            FileRecord& record = records[it.second];

            auto statsIt = inodeToFileStats.find(it.first);
            if (statsIt == inodeToFileStats.end()) {
                continue;
            }

            const FileStats& stats = statsIt->second;
            record.size = stats.size;
            record.modificationTime = stats.modificationTime;
            record.flags = stats.flags;
        }

        // Clean up remaining temporary data
        // The memory is freed, and the GUI never even sees it.
        inodeToRecordIdx.clear();
        inodeToFileStats.clear();
    }
}