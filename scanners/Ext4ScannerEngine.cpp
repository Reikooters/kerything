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

    std::optional<Ext4Database> parseInodes(const std::string& devicePath) {
        ext2_filsys fs;
        errcode_t retval = ext2fs_open(devicePath.c_str(), 0, 0, 0, unix_io_manager, &fs);
        if (retval) {
            std::cerr << "Error: " << error_message(retval) << std::endl;
            return std::nullopt;
        }

        uint32_t max_inodes = fs->super->s_inodes_count;

        // Reserve a reasonable starting size for the map (1.5 million files)
        int initialCapacity = 1500000;

        Ext4Database db;
        db.records.reserve(initialCapacity);
        db.tempParentInodes.reserve(initialCapacity);
        db.inodeToRecordIdx.reserve(initialCapacity);
        db.inodeToFileStats.reserve(initialCapacity);

        // Pre-reserve string pool based on heuristic (average 20 chars per filename)
        db.stringPool.reserve(initialCapacity * 20);

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

        ScanContext ctx{db, max_inodes};

        // Crawl the directory tree to discover all names and structure
        // We start from the root inode (2) and let ext2fs_dir_iterate2 recurse
        // through directories.
        while (ext2fs_get_next_inode(scan, &ino, &inode) == 0 && ino != 0) {
            if (inode.i_links_count == 0) continue;

            FileStats stats{};
            stats.size = EXT2_I_SIZE(&inode);
            stats.modificationTime = inode.i_mtime;
            stats.isDir = LINUX_S_ISDIR(inode.i_mode);
            stats.isSymlink = LINUX_S_ISLNK(inode.i_mode);

            db.inodeToFileStats[ino] = stats;

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

        return db;
    }

    // We call this once after the scan is completely finished
    void Ext4Database::resolveParentPointers() {
        std::cerr << "Resolving parent pointers..." << std::endl;

        // Convert parent MFT index to internal index
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
            FileStats& stats = inodeToFileStats[it.first];
            record.size = stats.size;
            record.modificationTime = stats.modificationTime;
            record.isDir = stats.isDir;
            record.isSymlink = stats.isSymlink;
        }

        // Clean up remaining temporary data
        // The memory is freed, and the GUI never even sees it.
        inodeToRecordIdx.clear();
        inodeToFileStats.clear();
    }
}