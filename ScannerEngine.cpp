// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ScannerEngine.h"

namespace ScannerEngine {
    std::vector<MftRun> mftRuns;

    std::string utf16ToUtf8(const char16_t* utf16_ptr, size_t length)
    {
        if (!utf16_ptr || length == 0)
            return "";

        std::string out;
        out.reserve(length * 2); // Pre-allocate to avoid multiple reallocs
        try {
            utf8::utf16to8(utf16_ptr, utf16_ptr + length, std::back_inserter(out));
        } catch (const utf8::invalid_utf16& e) {
            return "Invalid UTF-16 Data";
        }
        return out;
    }

    FileInfo getFileInfo(MFT_RecordHeader* header, char* buffer, uint64_t index) {
        FileInfo info = {{}, 0, static_cast<bool>(header->flags & 0x02), 0, index};
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
            if (attr->length == 0 || (attrOffset + attr->length) > recordSize) break;
            if (attr->type == 0xFFFFFFFF) break;

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
                                utf16ToUtf8(fn->name, fn->nameLength),
                                fn->parentDirectory & 0xFFFFFFFFFFFFULL,
                                fn->namespaceType,
                                fn->modificationTime,
                                fn->dataSize
                            });
                        }
                    }
                }
            }
            else if (attr->type == 0x80 && attr->nameLength == 0) { // $DATA (unnamed) (The actual file content)
                dataAttrFound = true;
                if (attr->nonResident == 0) {
                    // Resident: data is right here in the MFT record
                    sizeFromData = reinterpret_cast<ResidentHeader*>(buffer + attrOffset + 16)->dataLength;
                } else {
                    // Non-resident: size is at offset 48 of the attribute header
                    sizeFromData = *reinterpret_cast<uint64_t*>(buffer + attrOffset + 48);
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

    /**
     * NTFS Fixups (Update Sequence Array):
     * To detect partial writes, NTFS saves the last 2 bytes of every 512-byte
     * sector into an array and replaces them with a "sequence number".
     * Before reading, we must "fix" the sectors by putting the original bytes back.
     */
    void applyFixups(char* buffer, uint32_t recordSize)
    {
        auto* header = reinterpret_cast<MFT_RecordHeader*>(buffer);

        // updateSequenceOffset is relative to the start of the record
        uint16_t* updateSequenceArray = reinterpret_cast<uint16_t*>(buffer + header->updateSequenceOffset);
        uint16_t sequenceNumber = updateSequenceArray[0];

        // Number of sectors (usually 2 for a 1024 byte record)
        // header->updateSequenceSize includes the sequence number itself, so we subtract 1
        int sectorCount = header->updateSequenceSize - 1;

        for (int i = 0; i < sectorCount; ++i) {
            // The last 2 bytes of every 512-byte sector
            uint16_t* sectorEnd = reinterpret_cast<uint16_t*>(buffer + ((i + 1) * 512) - 2);

            // Safety check: if it doesn't match, the record is corrupt or partially written
            if (*sectorEnd != sequenceNumber) {
                // In a real recovery tool, you'd log a warning here
                continue;
            }

            // Replace the sequence number with the actual data from the array
            *sectorEnd = updateSequenceArray[i + 1];
        }
    }

    /**
     * The MFT itself is a file ($MFT) and can be fragmented.
     * This function decodes "Data Runs" (compressed byte streams) to find
     * where the MFT fragments are located physically on the disk.
     */
    void parseMftRuns(char* buffer, uint32_t attrOffset, uint64_t bytesPerCluster)
    {
        auto* attr = reinterpret_cast<AttributeHeader*>(buffer + attrOffset);
        // Non-resident data attribute header is 64 bytes
        uint16_t runOffset = *reinterpret_cast<uint16_t*>(buffer + attrOffset + 32);
        uint8_t* runPos = reinterpret_cast<uint8_t*>(buffer + attrOffset + runOffset);

        uint64_t currentVcn = 0;
        int64_t currentLcn = 0;

        while (*runPos != 0) {
            uint8_t header = *runPos++;
            uint8_t lenSize = header & 0x0F; // How many bytes encode the length
            uint8_t offSize = (header >> 4) & 0x0F; // How many bytes encode the offset

            uint64_t runLen = 0;
            for (int i = 0; i < lenSize; ++i)
                runLen |= static_cast<uint64_t>(*runPos++) << (i * 8);

            int64_t runOff = 0;
            for (int i = 0; i < offSize; ++i)
                runOff |= static_cast<int64_t>(*runPos++) << (i * 8);

            // Sign extend the offset: NTFS offsets are relative and can be negative!
            if (offSize > 0 && (runOff & (1ULL << (offSize * 8 - 1)))) {
                for (int i = offSize; i < 8; ++i)
                    runOff |= (0xFFULL << (i * 8));
            }

            currentLcn += runOff;
            mftRuns.push_back({ currentVcn, (uint64_t)currentLcn, runLen });
            currentVcn += runLen;
        }
    }

    /**
     * Converts an MFT Index (e.g., File #1234) to a physical byte offset on the disk.
     * It uses the data runs we parsed earlier to account for MFT fragmentation.
     */
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

    std::optional<SearchDatabase> parseMFT(const std::string& devicePath)
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
        std::cerr << "Bytes per Sector:  " << boot.bytesPerSector << "\n";
        std::cerr << "Sectors per Clust: " << static_cast<int>(boot.sectorsPerCluster) << "\n";
        std::cerr << "MFT Start LCN:     " << boot.mftStartLcn << "\n";
        std::cerr << "MFT Offset (hex):  0x" << std::hex << mftOffset << std::dec << "\n";
        std::cerr << "Record Size:       " << recordSize << " bytes\n";
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
            if (attr->type == 0x80) { // Data Attribute
                parseMftRuns(buffer.data(), mftAttrOffset, bytesPerCluster);
                // Size is at offset 48 for non-resident attributes
                totalMftSize = *reinterpret_cast<uint64_t*>(buffer.data() + mftAttrOffset + 48);
                break;
            }
            if (attr->type == 0xFFFFFFFF || attr->length == 0)
                break;
            mftAttrOffset += attr->length;
        }

        uint64_t totalRecords = totalMftSize / recordSize;
        std::cerr << "MFT consists of " << mftRuns.size() << " fragments.\n";
        std::cerr << "Total MFT Records: " << totalRecords << "\n";

        // Batch processing buffer (e.g., 4MB)
        const size_t batchSizeInRecords = (4 * 1024 * 1024) / recordSize;
        std::vector<char> batchBuffer(batchSizeInRecords * recordSize);

        SearchDatabase db;
        db.records.reserve(totalRecords);
        db.stringPool.reserve(totalRecords * 20); // Average filename length estimate

        // Step 3: Single Pass.
        // We collect all valid records. Trigrams are built as we go.
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

                        db.add(link.name, recordIndex, link.parentIndex, info.size, info.modificationTime, info.isDir);
                    }
                }
            }
        }

        std::cerr << "MFT scan and index complete. " << db.records.size() << " entries indexed. Resolving parent pointers...\n";

        // Resolve parent pointers and clear the large MFT map
        db.resolveParentPointers();

        std::cerr << "Resolving parent pointers completed.\n";

        return SearchDatabase{std::move(db)};
    }
}