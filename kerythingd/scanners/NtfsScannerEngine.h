// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_NTFSSCANNERENGINE_H
#define KERYTHINGD_NTFSSCANNERENGINE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <string_view>
#include <functional>

#include "../ScannerHelper.h"

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

    #pragma pack(pop)

    // Constants for Reparse Points
    static constexpr uint32_t FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400;
    static constexpr uint32_t IO_REPARSE_TAG_SYMLINK = 0xA000000C;
    static constexpr uint32_t IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003; // Junctions

    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::ChunkCallback& onChunk,
                    const ScannerHelper::ErrorCallback& onError,
                    const ScannerHelper::CancelCallback& shouldCancel,
                    const ScannerHelper::ProgressCallback& onProgress);

} // namespace NtfsScannerEngine

#endif //KERYTHINGD_NTFSSCANNERENGINE_H