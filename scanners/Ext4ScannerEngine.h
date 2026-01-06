// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_EXT4SCANNERENGINE_H
#define KERYTHING_EXT4SCANNERENGINE_H

#include <optional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <string>
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
        uint8_t isSymlink : 1; // Unused in NTFS
        uint8_t reserved : 6;
    };

    #pragma pack(pop)

    struct Ext4Database {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;

        // TEMPORARY (Only used during scan/setup)
        // We keep this here so add() can fill it, then we clear it in resolveParentPointers()
        std::unordered_map<uint32_t, uint32_t> inodeToRecordIdx;
        // Temporary storage for inodes index
        std::vector<uint32_t> tempParentInodes;

        // We call this once after the inode scan is completely finished
        void resolveParentPointers();

        [[nodiscard]] std::string getFullPath(const uint32_t recordIdx) const;
    };

    std::optional<Ext4Database> parseInodes(const std::string& devicePath);

    int dirCallback(ext2_ino_t dir_ino, int entry_flags, ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *priv_data);
}

#endif //KERYTHING_EXT4SCANNERENGINE_H