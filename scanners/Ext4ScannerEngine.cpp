// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Ext4ScannerEngine.h"

namespace Ext4ScannerEngine {

    // Helper struct to pass multiple pieces of data to dirCallback
    struct ScanContext {
        Ext4Database& db;
        uint32_t maxInodes;
    };

    [[nodiscard]] std::string Ext4Database::getFullPath(const uint32_t recordIdx) const {
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
            const auto& r = records[current];
            std::string_view name(&stringPool[r.nameOffset], r.nameLen);

            // Only count length if it's not a dot-entry
            if (name != oneDot && name != twoDots) {
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
            const auto& r = records[idx];
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

        // Store the name in the string pool
        uint32_t current_offset = ctx->db.stringPool.size();
        ctx->db.stringPool.insert(ctx->db.stringPool.end(), dirent->name, dirent->name + len);

        // Access the FileRecord via the ScanContext
        FileRecord* record;
        int recordIndex;

        auto it = ctx->db.inodeToRecordIdx.find(dirent->inode);
        if (it == ctx->db.inodeToRecordIdx.end()) {
            FileRecord newRecord{};
            record = &ctx->db.records.emplace_back(newRecord);
            ctx->db.tempParentInodes.push_back(dir_ino);
            recordIndex = ctx->db.records.size() - 1;
            ctx->db.inodeToRecordIdx[dirent->inode] = recordIndex;
        }
        else {
            record = &ctx->db.records[it->second];
            recordIndex = it->second;

            // Set parent inode for the FileRecord to be the directory inode
            ctx->db.tempParentInodes[recordIndex] = dir_ino;
        }

        // Update the FileRecord to specify the name offset and length in the string pool
        record->nameOffset = current_offset;
        record->nameLen = len;

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

        // Pre-reserve string pool based on heuristic (average 20 chars per filename)
        db.stringPool.reserve(initialCapacity * 20);

        int bufferBlocks = 512;

        ext2_inode_scan scan;
        ext2fs_open_inode_scan(fs, bufferBlocks, &scan);

        ext2_ino_t ino;
        ext2_inode inode;

        ScanContext ctx{db, max_inodes};

        while (ext2fs_get_next_inode(scan, &ino, &inode) == 0 && ino != 0) {
            // Ignore files that have no links (deleted but still in use)
            if (inode.i_links_count == 0) {
                continue;
            }

            FileRecord* record;

            auto it = db.inodeToRecordIdx.find(ino);
            if (it == db.inodeToRecordIdx.end()) {
                FileRecord newRecord{};
                record = &db.records.emplace_back(newRecord);
                db.inodeToRecordIdx[ino] = db.records.size() - 1;

                // Add temporary placeholder for the file's parent inode, will be updated later in dirCallback
                db.tempParentInodes.push_back(0);
            }
            else {
                record = &db.records[it->second];
            }

            record->size = EXT2_I_SIZE(&inode); // Note: For symlinks, EXT2_I_SIZE(&inode) will be the length of the path it points to.
            record->modificationTime = inode.i_mtime;

            bool isDir = LINUX_S_ISDIR(inode.i_mode);

            if (isDir) {
                record->isDir = 1;
            }

            bool isSymlink = LINUX_S_ISLNK(inode.i_mode);

            if (isSymlink) {
                record->isSymlink = 1;
            }

            // If it's a directory, iterate its entries to find child names
            if (record->isDir) {
                ext2fs_dir_iterate2(fs, ino, 0, nullptr, dirCallback, &ctx);
            }

            // NOTE TO SELF: Don't use `record` anymore here as the pointer might have been
            // invalidated in dirCallback()
        }

        // Resolve parent Inodes to parent Record Indices
        db.resolveParentPointers();

        // // Now output the results
        // std::cout << "\nInode\tType\tParent\tSize\t\tName\n";
        // std::cout << "------------------------------------------------------------\n";
        // for (int i = 0; i < db.records.size(); i++) {
        //     FileRecord& record = db.records[i];
        //
        //     // Skip entries that don't have a name
        //     if (record.nameLen == 0) {
        //         continue;
        //     }
        //
        //     std::string type = record.isDir ? "DIR" : "FILE";
        //
        //     if (record.isSymlink) {
        //         type.append("_SYMLINK");
        //     }
        //
        //     // Reconstruct the name from the arena
        //     std::string display_name;
        //     std::string parent_path;
        //
        //     display_name.assign(db.stringPool.data() + record.nameOffset, record.nameLen);
        //     parent_path = db.getFullPath(record.parentRecordIdx);
        //
        //     // Ensure parent_path ends with / if it's not empty
        //     if (!parent_path.empty() && parent_path.back() != '/') {
        //         parent_path += "/";
        //     }
        //
        //     std::cout << type << "\t"
        //               << record.size << "\t\t"
        //               << parent_path << "\t"
        //               << display_name << "\n";
        // }

        ext2fs_close_inode_scan(scan);
        ext2fs_close(fs);

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

        // Cleanup all temporary data.
        // The memory is freed, and the GUI never even sees it.
        inodeToRecordIdx.clear();
        tempParentInodes.clear();
        tempParentInodes.shrink_to_fit();
    }
}