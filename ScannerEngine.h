// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SCANNERENGINE_H
#define KERYTHING_SCANNERENGINE_H

#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <string_view>
#include <chrono>

// VITAL FIX: TBB and Qt both want 'emit'.
// We must undefine it before including <execution>
#ifdef emit
#define QT_EMIT_BACKUP emit
#undef emit
#endif

#include <execution>

#ifdef QT_EMIT_BACKUP
#define emit QT_EMIT_BACKUP
#undef QT_EMIT_BACKUP
#endif

#include "lib/utf8.h"

namespace ScannerEngine {
    inline constexpr std::string_view VERSION = "1.1.0";

    /**
     * NTFS structures must be byte-aligned because they represent raw data on disk.
     * Without #pragma pack(1), the compiler might add "padding" bytes between
     * struct members to align them with CPU registers (usually 4 or 8 bytes),
     * which would misalign our structs with the actual disk data.
     */
    #pragma pack(push, 1)

    /**
     * The Boot Sector is the very first sector (512 bytes) of the partition.
     * It contains geometry information needed to find the Master File Table (MFT).
     */
    struct NTFS_BootSector {
        uint8_t jump[3];
        char oemID[8];
        uint16_t bytesPerSector;
        uint8_t sectorsPerCluster;
        uint16_t reservedSectors;
        uint8_t fats; // NTFS: always 0
        uint16_t rootEntries; // NTFS: always 0
        uint16_t smallSectors; // NTFS: always 0
        uint8_t mediaDescriptor;
        uint16_t sectorsPerFat; // NTFS: always 0
        uint16_t sectorsPerTrack;
        uint16_t heads;
        uint32_t hiddenSectors;
        uint32_t largeSectors; // NTFS: always 0
        uint8_t physicalDriveNumber;
        uint8_t currentHead; // NTFS: used for chkdsk
        uint8_t extendedBootSignature;
        uint8_t reserved1;
        uint64_t totalSectors;
        uint64_t mftStartLcn; // Offset 0x30
        uint64_t mftMirrorStartLcn;
        int8_t clustersPerFileRecord; // Offset 0x40
        uint8_t reserved2[3];
        int8_t clustersPerIndexBlock;
        uint8_t reserved3[3];
        uint64_t volumeSerialNumber;
        uint32_t checksum;
        uint8_t bootstrapCode[426];
        uint16_t bootSignature; // 0xAA55 (Magic number for bootable partitions)
    };

    /**
     * Every file or directory on an NTFS volume has at least one MFT Record.
     * These records are typically 1024 bytes (fixed size).
     */
    struct MFT_RecordHeader {
        char signature[4]; // "FILE" or "BAAD" if corrupt
        uint16_t updateSequenceOffset;
        uint16_t updateSequenceSize;
        uint64_t logSequenceNumber;
        uint16_t sequenceNumber;
        uint16_t hardLinkCount;
        uint16_t firstAttributeOffset; // Points to the first attribute (e.g., $STANDARD_INFORMATION)
        uint16_t flags; // bit 0 (0x01): In Use, bit 1 (0x02): Directory
        uint32_t usedSize;
        uint32_t allocatedSize;
        uint64_t baseFileRecord;
        uint16_t nextAttributeID;
    };

    /**
     * NTFS uses an "Attribute" system. Everything (filename, data, security)
     * is an attribute following this generic header.
     */
    struct AttributeHeader {
        uint32_t type; // e.g., 0x30 for File Name
        uint32_t length;
        uint8_t nonResident;
        uint8_t nameLength;
        uint16_t nameOffset;
        uint16_t flags;
        uint16_t attributeID;
    };

    /**
     * $FILE_NAME Attribute (Type 0x30). Contains the name, parent directory
     * index, and cached size/dates. Note: Windows often creates multiple
     * $FILE_NAME attributes for one file (one for Win32 and one for DOS 8.3).
     */
    struct FileNameAttribute {
        uint64_t parentDirectory; // MFT index of the parent folder (bottom 48 bits)
        uint64_t creationTime;
        uint64_t modificationTime;
        uint64_t mftModificationTime;
        uint64_t accessTime;
        uint64_t allocatedSize;
        uint64_t dataSize; // Actual file size (cached here for speed)
        uint32_t flags;
        uint32_t reparseValue;
        uint8_t nameLength; // Length in characters
        uint8_t namespaceType;
        char16_t name[1]; // UTF-16 name starts here
    };

    /**
     * Header for attributes stored directly inside the MFT record.
     */
    struct ResidentHeader {
        uint32_t dataLength;
        uint16_t dataOffset;
        uint8_t indexedFlag;
        uint8_t padding;
    };

    /**
     * Non-resident attributes (like big files or the MFT itself) are stored
     * in "Data Runs" (fragments) across the disk. This struct tracks them.
     */
    struct MftRun {
        uint64_t virtualClusterNumber; // Virtual Cluster Number
        uint64_t logicalClusterNumber; // Logical Cluster Number (Physical)
        uint64_t length; // Number of clusters
    };

    // Struct for storing our file records index
    struct FileRecord {
        uint64_t tempParentMft;   // Temporary storage for 48-bit MFT index
        uint32_t parentRecordIdx; // Index in the 'records' vector (NOT MFT index)
        uint64_t size;
        uint64_t modificationTime;
        uint32_t nameOffset; // Offset into the global string pool
        uint16_t nameLen;
        uint8_t isDir : 1;
        uint8_t reserved : 7;
    };

    #pragma pack(pop)

    struct TrigramEntry {
        uint32_t trigram;
        uint32_t recordIdx;

        // Sorting by trigram first, then recordIdx
        bool operator<(const TrigramEntry& other) const {
            if (trigram != other.trigram) return trigram < other.trigram;
            return recordIdx < other.recordIdx;
        }
    };

    struct SearchDatabase {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;

        // Final paths for DIRECTORIES only (mapped by internal record index)
        std::unordered_map<uint32_t, std::string> directoryPaths;

        // The "Frozen Index": A single sorted vector of all trigram-record pairs
        std::vector<TrigramEntry> flatIndex;

        // TEMPORARY (Only used during scan/setup)
        // We keep this here so add() can fill it, then we clear it in precalculatePaths()
        std::unordered_map<uint64_t, uint32_t> mftToRecordIdx;

        void buildTrigramIndexParallel() {
            std::cerr << "Building Flat Trigram Index in parallel..." << std::endl;

            // 1. Calculate how many trigrams we'll have in total to avoid reallocations
            // (Roughly: sum of all filename lengths - 2)
            size_t totalTrigrams = 0;
            for (const auto& rec : records) {
                if (rec.nameLen >= 3) totalTrigrams += (rec.nameLen - 2);
            }
            flatIndex.resize(totalTrigrams);

            // 2. Fill the flatIndex in parallel
            // We divide the records into chunks to give to each thread
            const size_t numRecords = records.size();
            std::vector<size_t> startOffsets(numRecords);

            // This part is serial but very fast (calculating where each record starts in flatIndex)
            size_t currentOffset = 0;
            for (size_t i = 0; i < numRecords; ++i) {
                startOffsets[i] = currentOffset;
                if (records[i].nameLen >= 3) currentOffset += (records[i].nameLen - 2);
            }

            std::vector<uint32_t> workIndices(numRecords);
            std::iota(workIndices.begin(), workIndices.end(), 0);

            std::for_each(std::execution::par, workIndices.begin(), workIndices.end(), [&](uint32_t i) {
                const auto& rec = records[i];
                if (rec.nameLen < 3) return;

                std::string_view name(&stringPool[rec.nameOffset], rec.nameLen);
                size_t writePos = startOffsets[i];

                for (size_t j = 0; j <= name.length() - 3; ++j) {
                    uint32_t tri = (static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(name[j]))) << 16) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(name[j+1]))) << 8) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(name[j+2]))));

                    flatIndex[writePos++] = { tri, i };
                }
            });

            // 3. Sort the entire index in parallel
            // This is the most CPU-intensive part!
            std::cerr << "Sorting " << flatIndex.size() << " trigrams..." << std::endl;
            std::sort(std::execution::par, flatIndex.begin(), flatIndex.end());

            // 4. Remove exact duplicates (same trigram in same file)
            std::cerr << "Removing duplicate trigrams..." << std::endl;
            auto last = std::unique(std::execution::par, flatIndex.begin(), flatIndex.end(), [](const auto& a, const auto& b) {
                return a.trigram == b.trigram && a.recordIdx == b.recordIdx;
            });
            flatIndex.erase(last, flatIndex.end());

            // 5. Reclaim memory used by the duplicates which were removed
            flatIndex.shrink_to_fit();
        }

        void add(std::string_view name, uint64_t mftIndex, uint64_t parentMftIndex, uint64_t size, uint64_t mod, bool isDir) {
            uint32_t currentIdx = static_cast<uint32_t>(records.size());

            FileRecord rec{};
            rec.tempParentMft = parentMftIndex;
            rec.size = size;
            rec.modificationTime = mod;
            rec.nameOffset = static_cast<uint32_t>(stringPool.size());
            rec.nameLen = static_cast<uint16_t>(name.length());
            rec.isDir = isDir;

            records.push_back(rec);
            stringPool.insert(stringPool.end(), name.begin(), name.end());

            // Fill the temporary map
            mftToRecordIdx[mftIndex] = currentIdx;
        }

        // We call this once after the MFT scan is completely finished
        void precalculatePaths() {
            std::cerr << "Fixing parent pointers..." << std::endl;

            // Step 1: Convert parent MFT index to internal index
            for (auto& rec : records) {
                auto it = mftToRecordIdx.find(rec.tempParentMft);
                if (it != mftToRecordIdx.end()) {
                    rec.parentRecordIdx = it->second;
                } else {
                    // If parent isn't in our DB (like MFT Index 5's parent), mark as root
                    rec.parentRecordIdx = 0xFFFFFFFF;
                }
            }

            // Reclaim the MFT map immediately
            mftToRecordIdx.clear();

            // Step 2: Iterative Path Calculation
            std::cerr << "Building directory strings..." << std::endl;
            for (uint32_t i = 0; i < records.size(); ++i) {
                // Only process directories that haven't been resolved yet
                if (records[i].isDir && !directoryPaths.contains(i)) {

                    std::vector<uint32_t> chain;
                    uint32_t current = i;

                    // 1. Identify the chain of parents that need resolving
                    // STOP if we hit:
                    // - A record we already resolved
                    // - The root marker (0xFFFFFFFF)
                    // - A record that points to itself (NTFS root often does this)
                    while (current != 0xFFFFFFFF && !directoryPaths.contains(current)) {
                        chain.push_back(current);
                        uint32_t next = records[current].parentRecordIdx;
                        if (next == current) break; // Self-reference safety
                        current = next;
                    }

                    // 2. Build paths from top to bottom
                    std::string base = (current != 0xFFFFFFFF && directoryPaths.contains(current))
                                       ? directoryPaths[current] : "";

                    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                        uint32_t idx = *it;
                        const auto& r = records[idx];
                        std::string_view name(&stringPool[r.nameOffset], r.nameLen);

                        // Filter out "." and ".." entries which NTFS sometimes includes
                        if (name == "." || name == "..") {
                            directoryPaths[idx] = base;
                            continue;
                        }

                        // Build the string
                        if (base == "/") base = ""; // Avoid "//"
                        base += "/" + std::string(name);
                        directoryPaths[idx] = base;
                    }
                }
            }
        }
    };

    struct Entry {
        std::string name;
        uint64_t parent;
        std::string cachedPath; // Store the resolved path here
    };

    struct FileLink {
        std::string name;
        uint64_t parentIndex;
    };

    struct FileInfo {
        std::vector<FileLink> links;
        uint64_t size;
        bool isDir;
        uint64_t modificationTime;
        uint64_t mftIndex; // Used to track the record's location
    };

    std::optional<SearchDatabase> parseMFT(const std::string& devicePath);
}


#endif //KERYTHING_SCANNERENGINE_H