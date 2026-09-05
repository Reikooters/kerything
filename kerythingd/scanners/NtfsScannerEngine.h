// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_NTFSSCANNERENGINE_H
#define KERYTHINGD_NTFSSCANNERENGINE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <string_view>

#include "Protocol.h"
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

    /*
     * BEGIN structs used for building FileRecords
     */
    struct TempFileLink {
        std::string name;
        uint64_t parent;
        uint8_t namespaceType;
        uint64_t modTime;
        uint64_t dataSize; // Size cached in the filename attribute
    };

    struct FileLink {
        std::string name;
        uint64_t parentIndex;
    };

    struct FileInfo {
        std::vector<FileLink> links;
        uint64_t size;
        bool isDir;
        bool isSymlink;
        uint64_t modificationTime;
        uint64_t mftIndex; // Used to track the record's location
    };

    struct ExtensionFileInfo {
        std::vector<TempFileLink> tempLinks;
        bool isDir;
        bool isSymlink;
        uint64_t mftIndex; // Used to track the record's location
        bool dataAttrFound;
        uint64_t sizeFromData = 0;
    };

    struct NtfsDatabase {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;
        uint32_t totalStringPoolLength = 0;

        // Map to store extension record file information, as we have to wait to process
        // these after we've finished reading the full disk so we have all details.
        // Extension records usually make up only a very small percentage of the disk.
        std::unordered_map<uint64_t, std::vector<ExtensionFileInfo>> extensionRecordFileInfos;

        // Average filename length estimate
        static constexpr uint32_t kFileNameLengthHeuristic = 20;

        // Target a 4MB buffer for IPC efficiency, on my machine: /proc/sys/net/core/wmem_max = 4194304
        // Need to subtract the header size from the buffer size, as the header needs to fit
        // within the 4MB buffer too. As well as the chunk type byte.
        static constexpr uint32_t kTargetIpcBufferSizeMB = 4;
        static constexpr uint32_t kMaxIpcBufferSizeBytes = (kTargetIpcBufferSizeMB * 1024 * 1024) - Protocol::HeaderSize - sizeof(Protocol::ScanIndexResultChunkType);

        // Calculate how many full records fit in that byte limit
        static constexpr uint32_t kRecordsPerIpcChunk = kMaxIpcBufferSizeBytes / sizeof(FileRecord);

        /**
         * Adds a file or directory record to the database.
         *
         * @param name The name of the file or directory.
         * @param mftIndex The MFT (Master File Table) index of the file or directory.
         * @param parentMftIndex The MFT index of the parent directory.
         * @param size The size of the file in bytes.
         * @param mod The last modification time of the file or directory, represented as a timestamp.
         * @param isDir A flag indicating whether the entry is a directory.
         * @param isSymlink A flag indicating whether the entry is a symbolic link.
         * @param onFileRecordChunk A callback function to handle file record chunk processing.
         * @param onStringPoolChunk A callback function to handle string pool chunk processing.
         */
        void add(
            std::string_view name,
            uint64_t mftIndex,
            uint64_t parentMftIndex,
            uint64_t size,
            uint64_t mod,
            bool isDir,
            bool isSymlink,
            const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
            const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk);
    };
    /*
     * END structs used for building FileRecords
     */

    /**
     * Scans an NTFS device to process its Master File Table (MFT), extracting metadata and file information.
     *
     * This method attempts to open and scan the specified NTFS device. It processes the MFT records
     * in chunks, handles errors, allows cancellation, and reports progress via provided callbacks.
     *
     * @param devicePath The path to the target device partition (e.g., "/dev/sdc2"). Must have proper permissions.
     * @param onFileRecordChunk Callback invoked for each processed chunk of MFT records.
     *                          Receives data about the processed files or records.
     * @param onStringPoolChunk Callback invoked for each processed chunk of the string pool which stores file names.
     *                          Receives data about the processed string pool entries.
     * @param onError Callback invoked when errors occur during the scan.
     * @param shouldCancel Callback consulted to check if the scanning process should be canceled.
     *                     The function terminates early if this callback returns true.
     * @param onProgress Callback for reporting scan progress, typically represented as a percentage
     *                   indicating how much of the MFT has been processed.
     * @return Returns true if the scan completes successfully; otherwise, returns false if an error occurs
     *         or if the device is not a valid NTFS partition.
     */
    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
                    const ScannerHelper::ErrorCallback& onError,
                    const ScannerHelper::CancelCallback& shouldCancel,
                    const ScannerHelper::ProgressCallback& onProgress);

    /**
     * The MFT itself is a file ($MFT) and can be fragmented.
     * This function decodes "Data Runs" (compressed byte streams) to find where the MFT fragments are
     * located physically on the disk.
     *
     * NTFS Data Runs describe the mapping between Virtual Cluster Numbers (VCNs) and Logical Cluster Numbers (LCNs),
     * and this method extracts these mappings into a list of runs for use in subsequent operations.
     *
     * @param buffer A pointer to the buffer containing the NTFS attribute data. This buffer should contain
     *               the raw attribute including the attribute header and the Data Runs.
     * @param attrOffset The offset, in bytes, within the buffer where the attribute starts. The Data Runs are
     *                   expected to begin at a specific offset relative to this base position.
     * @param mftRuns A reference to a vector where the parsed MFT runs will be stored. Each run represents
     *                a mapping from a Virtual Cluster Number (VCN) to a Logical Cluster Number (LCN).
     */
    void parseMftRuns(char* buffer, uint32_t attrOffset, std::vector<MftRun>& mftRuns);

    /**
     * NTFS Fixups (Update Sequence Array):
     * To detect partial writes, NTFS saves the last 2 bytes of every 512-byte
     * sector into an array and replaces them with a "sequence number".
     * Before reading, we must "fix" the sectors by putting the original bytes back.
     *
     * Applies the fixup procedure to a given MFT record buffer. The fixup process replaces
     * the last 2 bytes of each sector in the record with their correct values, based
     * on the update sequence array. This ensures integrity and consistency of the record.
     *
     * @param buffer Pointer to the buffer containing the MFT record data.
     * @param recordSize The size of the MFT record in bytes, used to calculate sector boundaries.
     */
    void applyFixups(char* buffer, uint32_t recordSize);

    /**
     * Processes a Master File Table (MFT) record, extracting metadata and attributes,
     * and determines whether the record can be finalized immediately or requires additional processing.
     *
     * @param header Pointer to the MFT record header, containing metadata and attribute references.
     * @param buffer Raw binary data representing the content of the MFT record.
     * @param mftxInex The index of the current MFT record being processed.
     * @param db Reference to the NtfsDatabase for storing file records and string pool.
     * @param onFileRecordChunk Callback function to handle file record chunks.
     * @param onStringPoolChunk Callback function to handle string pool chunks.
     */
    void processMftRecord(
        MFT_RecordHeader* header,
        char* buffer,
        uint64_t mftxInex,
        NtfsDatabase& db,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk);

    /**
     * Finalizes the processing of a file's metadata and adds it to the NtfsDatabase.
     *
     * @param info A reference to the FileInfo object containing the file's metadata.
     * @param allNames A vector of TempFileLink objects representing all names (links) associated with the file.
     * @param dataAttrFound A boolean indicating whether a DATA attribute was found for the file.
     * @param sizeFromData A 64-bit integer specifying the file size derived from the DATA attribute, if available.
     * @param db Reference to a NtfsDatabase where the finalized file information will be stored.
     * @param mftIndex A 64-bit integer representing the MFT index of the file.
     * @param onFileRecordChunk A callback function to handle file record chunk processing.
     * @param onStringPoolChunk A callback function to handle string pool chunk processing.
     */
    void finalizeAndAddFile(
        FileInfo& info,
        const std::vector<TempFileLink>& allNames,
        bool dataAttrFound,
        uint64_t sizeFromData,
        NtfsDatabase& db,
        uint64_t mftIndex,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk);

    /**
     * Processes base/extension MFT records that were deferred because the file
     * used an $ATTRIBUTE_LIST.
     *
     * Records belonging to the same base MFT index are merged, then emitted via
     * finalizeAndAddFile().
     */
    void processExtensionRecords(
        NtfsDatabase& db,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk);

    /**
     * Converts a UTF-16 string to a UTF-8 encoded string.
     *
     * This function takes a pointer to a UTF-16 encoded string and its length,
     * converts it to a UTF-8 encoded string, and returns the result. If the
     * input is null or its length is zero, it returns an empty string.
     *
     * @param utf16_ptr Pointer to the UTF-16 encoded string. Must not be null.
     * @param length The number of UTF-16 code units in the string.
     *
     * @return A UTF-8 encoded string. If the UTF-16 data is invalid,
     *         returns "Invalid UTF-16 Data".
     */
    std::string utf16ToUtf8(const char16_t* utf16_ptr, size_t length);

    /**
     * Converts an NTFS FILETIME value to Unix time in seconds.
     * NTFS FILETIME represents the number of 100-nanosecond intervals since
     * January 1, 1601 (UTC). Unix time represents the number of seconds since
     * January 1, 1970 (UTC).
     *
     * @param filetime100ns The NTFS FILETIME value in 100-nanosecond intervals.
     * @return The corresponding Unix time in seconds. If the NTFS FILETIME is
     *         before the Unix epoch, the method returns 0.
     */
    uint64_t ntfsFiletimeToUnixSeconds(uint64_t filetime100ns);

} // namespace NtfsScannerEngine

#endif //KERYTHINGD_NTFSSCANNERENGINE_H