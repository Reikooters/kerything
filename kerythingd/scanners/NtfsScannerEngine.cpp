// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <fstream>
#include <iostream>
#include "NtfsScannerEngine.h"

namespace NtfsScannerEngine {

    void parseMftRuns(char* buffer, uint32_t attrOffset, std::vector<MftRun>& mftRuns) {
        auto* attr = reinterpret_cast<AttributeHeader*>(buffer + attrOffset);

        // The "Mapping Pairs" (Data Runs) offset is at byte 32 of a non-resident attribute header
        uint16_t runOffset = *reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(attr) + 32);
        uint8_t* runPos = reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(attr) + runOffset);

        uint64_t currentVcn = 0;
        int64_t currentLcn = 0;

        while (*runPos != 0) {
            uint8_t header = *runPos++;
            uint8_t lenSize = header & 0x0F; // How many bytes encode the length
            uint8_t offSize = (header >> 4) & 0x0F; // How many bytes encode the offset

            uint64_t runLen = 0;
            for (int i = 0; i < lenSize; ++i) {
                runLen |= static_cast<uint64_t>(*runPos++) << (i * 8);
            }

            int64_t runOff = 0;
            for (int i = 0; i < offSize; ++i) {
                runOff |= static_cast<int64_t>(*runPos++) << (i * 8);
            }

            // Sign extend the offset: NTFS offsets are relative and can be negative!
            if (offSize > 0 && (runOff & (1ULL << (offSize * 8 - 1)))) {
                for (int i = offSize; i < 8; ++i) {
                    runOff |= (0xFFULL << (i * 8));
                }
            }

            currentLcn += runOff;
            mftRuns.push_back({ currentVcn, (uint64_t)currentLcn, runLen });
            currentVcn += runLen;
        }
    }

    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::ChunkCallback& onChunk,
                    const ScannerHelper::ErrorCallback& onError,
                    const ScannerHelper::CancelCallback& shouldCancel,
                    const ScannerHelper::ProgressCallback& onProgress)
    {
        // Opening a disk device requires 'root' privileges on Linux.
        std::ifstream disk(devicePath.toStdString(), std::ios::binary);
        if (!disk) {
            std::perror("Error opening device");
            std::cerr << "Make sure to use sudo and the correct partition (e.g., /dev/sdc2).\n";
            return false;
        }

        NTFS_BootSector boot;

        // Step 1: Read the boot sector to find the start of the MFT
        disk.read(reinterpret_cast<char*>(&boot), sizeof(boot));

        if (std::string(boot.oemID, 8) != "NTFS    ") {
            std::cerr << "Error: " << devicePath.toStdString() << " does not appear to be a valid NTFS partition.\n";
            std::cerr << "OEM ID found: [" << std::string(boot.oemID, 8) << "]\n";
            return false;
        }

        uint64_t bytesPerCluster = static_cast<uint64_t>(boot.bytesPerSector) * boot.sectorsPerCluster;
        uint64_t mftOffset = boot.mftStartLcn * bytesPerCluster;

        // Determine Record Size: Usually 1024 bytes.
        // If clustersPerFileRecord is negative, size is 2^(abs(value)).
        int32_t clustersPerFileRecord = boot.clustersPerFileRecord;
        uint32_t recordSize = (clustersPerFileRecord > 0) ? (clustersPerFileRecord * bytesPerCluster) : (1 << (-clustersPerFileRecord));

        std::cerr << "--- NTFS Volume Info ---" << "\n";
        std::cerr << "Bytes per Sector:    " << boot.bytesPerSector << "\n";
        std::cerr << "Sectors per Cluster: " << static_cast<int>(boot.sectorsPerCluster) << "\n";
        std::cerr << "MFT Start LCN:       " << boot.mftStartLcn << "\n";
        std::cerr << "MFT Offset (hex):    0x" << std::hex << mftOffset << std::dec << "\n";
        std::cerr << "Record Size:         " << recordSize << " bytes\n";
        std::cerr << "------------------------" << "\n";

        if (mftOffset == 0 || recordSize == 0) {
            std::cerr << "Invalid MFT parameters calculated. Struct alignment might be wrong.\n";
            return false;
        }

        std::vector<char> buffer(recordSize);

        std::vector<MftRun> mftRuns;

        // Step 2: Read MFT Record 0 (The MFT's own entry) to find all fragments of the MFT.
        disk.seekg(mftOffset);
        disk.read(buffer.data(), recordSize);
        auto* mftHeader = reinterpret_cast<MFT_RecordHeader*>(buffer.data());

        uint32_t mftAttrOffset = mftHeader->firstAttributeOffset;
        uint64_t totalMftSize = 0;
        while (mftAttrOffset + 16 <= mftHeader->usedSize) {
            auto* attr = reinterpret_cast<AttributeHeader*>(buffer.data() + mftAttrOffset);

            if (attr->type == 0x80) { // $DATA Attribute
                parseMftRuns(buffer.data(), mftAttrOffset, mftRuns);

                if (attr->nonResident) {
                    auto* nonResident = reinterpret_cast<NonResidentHeader*>(
                        buffer.data() + mftAttrOffset + sizeof(AttributeHeader));
                    totalMftSize = nonResident->dataSize;
                }
                break;
            }

            // 0xFFFFFFFF is the end-of-attributes marker in NTFS
            if (attr->type == 0xFFFFFFFF || attr->length == 0) {
                break;
            }

            mftAttrOffset += attr->length;
        }

        uint64_t totalRecords = totalMftSize / recordSize;
        std::cerr << "MFT consists of " << mftRuns.size() << " fragments.\n";
        std::cerr << "Total MFT Records: " << totalRecords << "\n";

        // Batch processing buffer (4MB)
        const size_t batchSizeInRecords = (4 * 1024 * 1024) / recordSize;
        std::vector<char> batchBuffer(batchSizeInRecords * recordSize);

        std::unordered_map<uint64_t, std::vector<ExtensionFileInfo>> extensionRecordFileInfos;

        static constexpr uint32_t kFileNameLengthHeuristic = 20; // Average filename length estimate

        // Target a 4MB buffer for IPC efficiency
        static constexpr uint32_t kTargetBufferSizeMB = 4;
        static constexpr uint32_t kMaxBufferSizeBytes = kTargetBufferSizeMB * 1024 * 1024;

        // Calculate how many full records fit in that byte limit
        static constexpr uint32_t kSizePerRecord = sizeof(FileRecord) + (sizeof(char) * kFileNameLengthHeuristic);
        static constexpr uint32_t kRecordsPerChunk = kMaxBufferSizeBytes / (sizeof(quint32) + sizeof(quint32) + kSizePerRecord);

        std::vector<FileRecord> records;
        std::vector<char> stringPool;

        records.reserve(kRecordsPerChunk);
        stringPool.reserve(kRecordsPerChunk * kFileNameLengthHeuristic);

        uint64_t scannedRecords = 0;

        // Step 3: Collect all valid records.

        // TODO:
        // - Implement file processing
        // - Add calls to onProgress()
        // - Add checks for cancellation requested and stop if requested

        std::cerr << "OK - WIP\n";

        return true;
    }

}