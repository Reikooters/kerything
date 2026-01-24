// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_EXT4SCANNERENGINE_H
#define KERYTHING_EXT4SCANNERENGINE_H

#include <optional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <string>
#include <functional>
#include <ext2fs/ext2fs.h>

namespace Ext4ScannerEngine {

    /**
     * We use #pragma pack(1) to ensure these structs are stored with NO padding.
     * This is critical because:
     * 1. The Helper process writes these structs as a raw binary blob to stdout.
     * 2. The GUI process reads that blob back into an array.
     *
     * Without this, different compilers, architectures (x86 vs ARM), or even
     * optimization levels could add "padding bytes" for CPU alignment, which
     * would corrupt the data transfer between the scanner and the GUI.
     */
    #pragma pack(push, 1)

    // Struct for storing our file records index
    struct FileRecord {
        uint32_t parentRecordIdx; // Index in the 'records' vector (NOT inode index)
        uint64_t size;
        uint64_t modificationTime;
        uint32_t nameOffset; // Offset into the global string pool
        uint16_t nameLen;
        uint8_t isDir : 1;
        uint8_t isSymlink : 1;
        uint8_t reserved : 6;
    };

    #pragma pack(pop)

    struct FileStats {
        uint64_t size;
        uint64_t modificationTime;
        uint8_t isDir : 1;
        uint8_t isSymlink : 1;
        uint8_t reserved : 6;
    };

    struct Ext4Database {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;

        // TEMPORARY (Only used during scan/setup)
        // We keep this here so add() can fill it, then we clear it in resolveParentPointers()
        std::unordered_map<uint32_t, uint32_t> inodeToRecordIdx;
        // Temporary storage for inodes index
        std::vector<uint32_t> tempParentInodes;
        // Temporary storage for inode stats
        std::unordered_map<uint32_t, FileStats> inodeToFileStats;

        // We call these once after the inode scan is completely finished
        void resolveParentPointers();
        void populateStatsIntoRecords();
    };

    /**
     * Progress callback: called with (done, total) to report scan progress.
     */
    using ProgressCallback = std::function<void(uint64_t done, uint64_t total)>;

    /**
     * Parses the inodes of the specified Ext4 filesystem and builds an internal database structure.
     *
     * @param devicePath The file path to the device containing the Ext4 filesystem.
     * @param progressCb A callback function to report progress during inode scanning.
     *                   The callback takes two arguments: the number of inodes processed and the total number of inodes.
     *                   Can be null if progress reporting is not required.
     * @return An optional Ext4Database object containing the parsed data.
     *         Returns std::nullopt if there is an error opening or scanning the filesystem.
     */
    std::optional<Ext4Database> parseInodes(const std::string& devicePath, ProgressCallback progressCb = {});

    /**
     * Callback function invoked for each directory entry during a directory iteration in the Ext4 filesystem.
     * This function processes directory entries, updates the inode-to-record mappings, and populates file records.
     *
     * @param dir_ino The inode number of the directory being scanned.
     * @param entry_flags Flags providing additional information about the directory entry (e.g., error conditions, entry type).
     * @param dirent A pointer to the `ext2_dir_entry` structure that represents the directory entry.
     * @param offset The byte offset of the directory entry within the directory block.
     * @param blocksize The size of the directory block in bytes.
     * @param buf A pointer to the data buffer containing the raw directory block data.
     * @param priv_data A void pointer to user-defined private data (used to pass the scanning context, such as `ScanContext`).
     * @return Returns 0 on successful processing of the entry. Non-zero values may indicate processing errors.
     */
    int dirCallback(ext2_ino_t dir_ino, int entry_flags, ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *priv_data);
}

#endif //KERYTHING_EXT4SCANNERENGINE_H