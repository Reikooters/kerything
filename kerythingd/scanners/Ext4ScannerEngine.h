// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_EXT4SCANNERENGINE_H
#define KERYTHINGD_EXT4SCANNERENGINE_H

#include <optional>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <string>
#include <functional>
#include <ext2fs/ext2fs.h>

#include "Protocol.h"
#include "../ScannerHelper.h"

namespace Ext4ScannerEngine {

    struct FileStats {
        uint64_t size;
        uint64_t modificationTime;
        uint8_t flags;            // bit 0 = isDir, bit 1 = isSymlink
        // uint8_t isDir : 1;
        // uint8_t isSymlink : 1;
        // uint8_t reserved : 6;
    };

    struct Ext4Database {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;
        uint32_t totalStringPoolLength = 0;

        // TEMPORARY (Only used during scan/setup)
        // We keep this here so add() can fill it, then we clear it in resolveParentPointers()
        std::unordered_map<uint32_t, uint32_t> inodeToRecordIdx;
        // Temporary storage for inodes index
        std::vector<uint32_t> tempParentInodes;
        // Temporary storage for inode stats
        std::unordered_map<uint32_t, FileStats> inodeToFileStats;

        // Average filename length estimate
        static constexpr uint32_t kFileNameLengthHeuristic = 20;

        // Target a 4MB buffer for IPC efficiency, on my machine: /proc/sys/net/core/wmem_max = 4194304
        // Need to subtract the header size from the buffer size, as the header needs to fit
        // within the 4MB buffer too. As well as the chunk type byte.
        static constexpr uint32_t kTargetIpcBufferSizeMB = 4;
        static constexpr uint32_t kMaxIpcBufferSizeBytes = (kTargetIpcBufferSizeMB * 1024 * 1024) - Protocol::HeaderSize - sizeof(Protocol::ScanIndexResultChunkType);

        // Calculate how many full records fit in that byte limit
        static constexpr uint32_t kRecordsPerIpcChunk = kMaxIpcBufferSizeBytes / sizeof(FileRecord);

        // We call these once after the inode scan is completely finished
        void resolveParentPointers();
        void populateStatsIntoRecords();
    };

    /**
     * Parses the inodes of the specified Ext4 filesystem and builds an internal database structure.
     *
     * This method attempts to open and scan the specified EXT4 device. It processes the inode records
     * in chunks, handles errors, allows cancellation, and reports progress via provided callbacks.
     *
     * @param devicePath The path to the target device partition (e.g., "/dev/sdc2"). Must have proper permissions.
     * @param onFileRecordChunk Callback invoked for each processed chunk of inode records.
     *                          Receives data about the processed files or records.
     * @param onStringPoolChunk Callback invoked for each processed chunk of the string pool which stores file names.
     *                          Receives data about the processed string pool entries.
     * @param onError Callback invoked when errors occur during the scan.
     * @param shouldCancel Callback consulted to check if the scanning process should be canceled.
     *                     The function terminates early if this callback returns true.
     * @param onProgress Callback for reporting scan progress, typically represented as a percentage
     *                   indicating how many of the inodes have been processed.
     * @return Returns true if the scan completes successfully; otherwise, returns false if an error occurs
     *         or if the device is not a valid EXT4 partition.
     */
    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                    const ScannerHelper::ErrorCallback& onError,
                    const ScannerHelper::CancelCallback& shouldCancel,
                    const ScannerHelper::ProgressCallback& onProgress);

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
} // namespace Ext4ScannerEngine

#endif //KERYTHINGD_EXT4SCANNERENGINE_H