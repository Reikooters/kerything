// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <fstream>
#include <iostream>
#include "../Utils.h"
#include "NtfsScannerEngine.h"

namespace NtfsScannerEngine {
    std::vector<MftRun> mftRuns;

    FileInfo getFileInfo(MFT_RecordHeader* header, char* buffer, uint64_t index) {
        FileInfo info = {{}, 0, static_cast<bool>(header->flags & 0x02), false, 0, index};
        uint32_t attrOffset = header->firstAttributeOffset;
        uint32_t recordSize = header->usedSize; // Use the size reported by the header

        // Local struct to track names found in this record temporarily
        struct TempLink {
            std::string name;
            uint64_t parent;
            uint8_t namespaceType;
            uint64_t modTime;
            uint64_t dataSize; // Size cached in the filename attribute
        };
        std::vector<TempLink> allNames;

        uint64_t sizeFromData = 0;
        bool dataAttrFound = false;

        // Iterate through all attributes in the record until we hit the 0xFFFFFFFF end marker
        // Safety: Ensure attrOffset starts within the buffer
        while (attrOffset + sizeof(AttributeHeader) <= recordSize) {
            auto* attr = reinterpret_cast<AttributeHeader*>(buffer + attrOffset);

            // Validation: If attribute length is 0 or exceeds remaining buffer, it's corrupt.
            if (attr->length == 0 || (attrOffset + attr->length) > recordSize) {
                break;
            }

            // 0xFFFFFFFF is the end-of-attributes marker in NTFS
            if (attr->type == 0xFFFFFFFF) {
                break;
            }

            if (attr->type == 0x30) { // $FILE_NAME
                // Check if attribute is Resident.
                // Most $FILE_NAME attributes are resident. If not, we'd need to parse data runs
                // just to get a name, which is extremely rare and usually handled via extension records.
                if (attr->nonResident == 0) {
                    auto* res = reinterpret_cast<ResidentHeader*>(buffer + attrOffset + sizeof(AttributeHeader));
                    uint32_t nameDataOffset = attrOffset + res->dataOffset;

                    // Safety check: ensure the data offset and the FileNameAttribute struct fit
                    if (nameDataOffset + sizeof(FileNameAttribute) <= recordSize) {
                        auto* fn = reinterpret_cast<FileNameAttribute*>(buffer + nameDataOffset);

                        // Further safety: ensure the actual UTF-16 string fits in the record
                        // nameLength is in characters (2 bytes each)
                        if (nameDataOffset + offsetof(FileNameAttribute, name) + (fn->nameLength * 2) <= recordSize) {
                            // MFT references are 64-bit, but only the first 48 bits are the record index.
                            // The top 16 bits are the "Sequence Number" used for consistency checks.
                            allNames.push_back({
                                Utils::utf16ToUtf8(fn->name, fn->nameLength),
                                fn->parentDirectory & 0xFFFFFFFFFFFFULL,
                                fn->namespaceType,
                                fn->modificationTime,
                                fn->dataSize
                            });

                            // Check if this name attribute indicates a reparse point (Symlink/Junction)
                            if (fn->flags & FILE_ATTRIBUTE_REPARSE_POINT) {
                                if (fn->reparseValue == IO_REPARSE_TAG_SYMLINK ||
                                    fn->reparseValue == IO_REPARSE_TAG_MOUNT_POINT) {
                                    info.isSymlink = true;
                                }
                            }
                        }
                    }
                }
            }
            else if (attr->type == 0x80 && attr->nameLength == 0) { // $DATA (unnamed) (The actual file content)
                dataAttrFound = true;

                if (attr->nonResident == 0) {
                    // Resident: data is right here in the MFT record
                    sizeFromData = reinterpret_cast<ResidentHeader*>(buffer + attrOffset + sizeof(AttributeHeader))->dataLength;
                } else {
                    sizeFromData = reinterpret_cast<NonResidentHeader*>(buffer + attrOffset + sizeof(AttributeHeader))->dataSize;
                }
            }

            attrOffset += attr->length;
        }

        // NTFS Gotcha: Files can have multiple names (Hard Links or DOS 8.3 aliases).
        // We filter out the DOS names to avoid duplicates in the complete file list.

        // Process gathered names
        // Filter out DOS/POSIX names ONLY if a Win32 name exists for the same record.
        // Usually, a DOS name is a duplicate of a Win32 name in the same parent folder.
        uint64_t sizeFromFileName = 0;
        for (const auto& entry : allNames) {
            bool isDuplicateDosName = false;
            if (entry.namespaceType == 2) { // DOS Namespace
                for (const auto& other : allNames) {
                    if (other.namespaceType != 2 && other.parent == entry.parent) {
                        isDuplicateDosName = true;
                        break;
                    }
                }
            }

            if (!isDuplicateDosName) {
                info.links.push_back({entry.name, entry.parent});

                // Prefer metadata from Win32 or Win32/DOS combined namespaces
                // Namespace: 0=POSIX, 1=Win32, 2=DOS, 3=Win32&DOS
                if (info.modificationTime == 0 || entry.namespaceType == 1 || entry.namespaceType == 3) {
                    info.modificationTime = entry.modTime;
                    sizeFromFileName = entry.dataSize;
                }
            }
        }

        // If no DATA attribute was found (e.g. some directories or system files),
        // use the size cached in the filename attribute.
        info.size = dataAttrFound ? sizeFromData : sizeFromFileName;

        return info;
    }

    void applyFixups(char* buffer, uint32_t recordSize)
    {
        auto* header = reinterpret_cast<MFT_RecordHeader*>(buffer);

        // updateSequenceOffset is relative to the start of the record
        uint16_t* updateSequenceArray = reinterpret_cast<uint16_t*>(buffer + header->updateSequenceOffset);
        uint16_t sequenceNumber = updateSequenceArray[0];

        // Number of sectors (usually 2 for a 1024 byte record)
        // header->updateSequenceSize includes the sequence number itself, so we subtract 1
        int sectorCount = header->updateSequenceSize - 1;

        if (sectorCount <= 0) {
            return;
        }

        // Use recordSize to determine the actual bytes per sector for this record
        uint32_t bytesPerSector = recordSize / sectorCount;

        for (int i = 0; i < sectorCount; ++i) {
            // The last 2 bytes of every sector
            uint32_t offset = ((i + 1) * bytesPerSector) - 2;

            // Safety check to prevent buffer overflow if MFT header is corrupt
            if (offset + 2 > recordSize) {
                break;
            }

            uint16_t* sectorEnd = reinterpret_cast<uint16_t*>(buffer + offset);

            // Safety check: if it doesn't match, the record is corrupt or partially written
            if (*sectorEnd != sequenceNumber) {
                // In a real recovery tool, you'd log a warning here
                continue;
            }

            // Replace the sequence number with the actual data from the array
            *sectorEnd = updateSequenceArray[i + 1];
        }
    }

    void parseMftRuns(char* buffer, uint32_t attrOffset)
    {
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

    uint64_t mftIndexToPhysicalOffset(uint64_t index, uint32_t recordSize, uint64_t bytesPerCluster)
    {
        uint64_t virtualClusterNumber = (index * recordSize) / bytesPerCluster;
        uint64_t virtualClusterNumberOffset = (index * recordSize) % bytesPerCluster;

        for (const auto& run : mftRuns) {
            if (virtualClusterNumber >= run.virtualClusterNumber && virtualClusterNumber < (run.virtualClusterNumber + run.length)) {
                return (run.logicalClusterNumber + (virtualClusterNumber - run.virtualClusterNumber)) * bytesPerCluster + virtualClusterNumberOffset;
            }
        }
        return 0;
    }

    std::optional<NtfsDatabase> parseMft(const std::string& devicePath)
    {
        // Opening a disk device requires 'root' privileges on Linux.
        std::ifstream disk(devicePath, std::ios::binary);
        if (!disk) {
            std::perror("Error opening device");
            std::cerr << "Make sure to use sudo and the correct partition (e.g., /dev/sdc2).\n";
            return std::nullopt;
        }

        NTFS_BootSector boot;

        // Step 1: Read the boot sector to find the start of the MFT
        disk.read(reinterpret_cast<char*>(&boot), sizeof(boot));

        if (std::string(boot.oemID, 8) != "NTFS    ") {
            std::cerr << "Error: " << devicePath << " does not appear to be a valid NTFS partition.\n";
            std::cerr << "OEM ID found: [" << std::string(boot.oemID, 8) << "]\n";
            return std::nullopt;
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
            return std::nullopt;
        }

        std::vector<char> buffer(recordSize);

        // Step 2: Read MFT Record 0 (The MFT's own entry) to find all fragments of the MFT.
        disk.seekg(mftOffset);
        disk.read(buffer.data(), recordSize);
        auto* mftHeader = reinterpret_cast<MFT_RecordHeader*>(buffer.data());

        uint32_t mftAttrOffset = mftHeader->firstAttributeOffset;
        uint64_t totalMftSize = 0;
        while (mftAttrOffset + 16 <= mftHeader->usedSize) {
            auto* attr = reinterpret_cast<AttributeHeader*>(buffer.data() + mftAttrOffset);

            if (attr->type == 0x80) { // $DATA Attribute
                parseMftRuns(buffer.data(), mftAttrOffset);

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

        // Batch processing buffer (e.g., 4MB)
        const size_t batchSizeInRecords = (4 * 1024 * 1024) / recordSize;
        std::vector<char> batchBuffer(batchSizeInRecords * recordSize);

        NtfsDatabase db;
        db.records.reserve(totalRecords);
        db.tempParentMfts.reserve(totalRecords);
        db.mftToRecordIdx.reserve(totalRecords);
        db.stringPool.reserve(totalRecords * 20); // Average filename length estimate

        // Step 3: Single Pass.
        // We collect all valid records.
        for (const auto& run : mftRuns) {
            uint64_t runOffset = run.logicalClusterNumber * bytesPerCluster;
            uint64_t recordsInRun = (run.length * bytesPerCluster) / recordSize;

            // Starting index for this specific fragment/run
            uint64_t runStartIndex = (run.virtualClusterNumber * bytesPerCluster) / recordSize;

            for (uint64_t r = 0; r < recordsInRun; r += batchSizeInRecords) {
                uint64_t toRead = std::min(batchSizeInRecords, recordsInRun - r);
                disk.seekg(runOffset + (r * recordSize));
                disk.read(batchBuffer.data(), toRead * recordSize);

                for (uint64_t i = 0; i < toRead; ++i) {
                    char* recordPtr = batchBuffer.data() + (i * recordSize);
                    auto* header = reinterpret_cast<MFT_RecordHeader*>(recordPtr);

                    if (std::string_view(header->signature, 4) != "FILE" || !(header->flags & 0x01) || header->baseFileRecord != 0)
                        continue;

                    applyFixups(recordPtr, recordSize);
                    uint64_t recordIndex = runStartIndex + r + i;
                    FileInfo info = getFileInfo(header, recordPtr, recordIndex);

                    for (const auto& link : info.links) {
                        // System files starting with $ are usually hidden in Everything
                        if (link.name[0] == '$' && recordIndex < 16) continue;

                        db.add(link.name, recordIndex, link.parentIndex, info.size, info.modificationTime, info.isDir, info.isSymlink);
                    }
                }
            }
        }

        std::cerr << "MFT scan and index complete. " << db.records.size() << " entries indexed. Resolving parent pointers...\n";

        // Resolve parent pointers and clear the large MFT map
        db.resolveParentPointers();

        std::cerr << "Resolving parent pointers completed.\n";

        return db;
    }

    void NtfsDatabase::add(std::string_view name, uint64_t mftIndex, uint64_t parentMftIndex, uint64_t size, uint64_t mod, bool isDir, bool isSymlink) {
        uint32_t currentIdx = static_cast<uint32_t>(records.size());

        FileRecord rec{};
        rec.size = size;
        rec.modificationTime = mod;
        rec.nameOffset = static_cast<uint32_t>(stringPool.size());
        rec.nameLen = static_cast<uint16_t>(name.length());
        rec.isDir = isDir;
        rec.isSymlink = isSymlink;
        rec.isSymlink = false; // unused

        records.push_back(rec);
        tempParentMfts.push_back(parentMftIndex); // Store metadata in parallel vector
        stringPool.insert(stringPool.end(), name.begin(), name.end());

        // Fill the temporary map
        mftToRecordIdx[mftIndex] = currentIdx;
    }

    // We call this once after the MFT scan is completely finished
    void NtfsDatabase::resolveParentPointers() {
        std::cerr << "Resolving parent pointers..." << std::endl;

        // Convert parent MFT index to internal index
        for (size_t i = 0; i < records.size(); ++i) {
            uint64_t parentMft = tempParentMfts[i]; // Look up from parallel vector

            auto it = mftToRecordIdx.find(parentMft);
            if (it != mftToRecordIdx.end()) {
                records[i].parentRecordIdx = it->second;
            } else {
                // If parent isn't in our DB (like MFT Index 5's parent), mark as root
                records[i].parentRecordIdx = 0xFFFFFFFF;
            }
        }

        // Cleanup all temporary data.
        // The memory is freed, and the GUI never even sees it.
        mftToRecordIdx.clear();
        tempParentMfts.clear();
        tempParentMfts.shrink_to_fit();
    }
}
