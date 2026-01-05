// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_NTFSSCANNERENGINE_H
#define KERYTHING_NTFSSCANNERENGINE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <string_view>

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

namespace NtfsScannerEngine {

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
     * The NonResidentHeader structure is used for handling non-resident attributes
     * in NTFS. Non-resident attributes are stored outside the MFT record and
     * are described by their location on the volume through "Data Runs."
     *
     * Members:
     * - startVcn: The starting Virtual Cluster Number (VCN) for the non-resident attribute.
     * - endVcn: The ending Virtual Cluster Number (VCN), inclusive.
     * - mappingPairsOffset: The offset to the Mapping Pairs array, which describes
     *   the clusters where the data is stored on disk.
     * - compressionUnitSize: Denotes the size of the compression unit if the attribute
     *   is compressed. A value of zero indicates the attribute is not compressed.
     * - reserved: Reserved field for alignment or future use.
     * - allocatedSize: The total size allocated for the non-resident attribute, in bytes.
     * - dataSize: The logical size of the attribute data, in bytes. This represents the
     *   actual size of the file or data stored in the attribute.
     * - initializedSize: The size of the attribute data that has been initialized,
     *   in bytes. This is generally less than or equal to dataSize.
     */
    struct NonResidentHeader {
        uint64_t startVcn;
        uint64_t endVcn;
        uint16_t mappingPairsOffset;
        uint16_t compressionUnitSize;
        uint32_t reserved;
        uint64_t allocatedSize;
        uint64_t dataSize;       // Offset 48
        uint64_t initializedSize;
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
        uint32_t parentRecordIdx; // Index in the 'records' vector (NOT MFT index)
        uint64_t size;
        uint64_t modificationTime;
        uint32_t nameOffset; // Offset into the global string pool
        uint16_t nameLen;
        uint8_t isDir : 1;
        uint8_t isSymlink : 1; // Unused in NTFS
        uint8_t reserved : 6;
    };

    #pragma pack(pop)

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

    struct NtfsDatabase {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;

        // TEMPORARY (Only used during scan/setup)
        // We keep this here so add() can fill it, then we clear it in resolveParentPointers()
        std::unordered_map<uint64_t, uint32_t> mftToRecordIdx;
        // Temporary storage for 48-bit MFT index
        std::vector<uint64_t> tempParentMfts;

        void add(std::string_view name, uint64_t mftIndex, uint64_t parentMftIndex, uint64_t size, uint64_t mod, bool isDir);

        // We call this once after the MFT scan is completely finished
        void resolveParentPointers();
    };

    std::optional<NtfsDatabase> parseMft(const std::string& devicePath);

    FileInfo getFileInfo(MFT_RecordHeader* header, char* buffer, uint64_t index);

    /**
     * NTFS Fixups (Update Sequence Array):
     * To detect partial writes, NTFS saves the last 2 bytes of every 512-byte
     * sector into an array and replaces them with a "sequence number".
     * Before reading, we must "fix" the sectors by putting the original bytes back.
     */
    void applyFixups(char* buffer, uint32_t recordSize);

    /**
     * The MFT itself is a file ($MFT) and can be fragmented.
     * This function decodes "Data Runs" (compressed byte streams) to find
     * where the MFT fragments are located physically on the disk.
     */
    void parseMftRuns(char* buffer, uint32_t attrOffset);

    /**
     * Converts an MFT Index (e.g., File #1234) to a physical byte offset on the disk.
     * It uses the data runs we parsed earlier to account for MFT fragmentation.
     */
    uint64_t mftIndexToPhysicalOffset(uint64_t index, uint32_t recordSize, uint64_t bytesPerCluster);
}

#endif //KERYTHING_NTFSSCANNERENGINE_H