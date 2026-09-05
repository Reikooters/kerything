// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <fstream>
#include <iostream>
#include <limits>
#include "../lib/utfcpp/utf8.h"

#include "NtfsScannerEngine.h"
#include "Protocol.h"

namespace NtfsScannerEngine {

    bool reportError(const ScannerHelper::ErrorCallback& onError, const QString& message) {
        std::cerr << message.toStdString() << "\n";

        if (onError) {
            onError(message);
        }

        return false;
    }

    bool readExact(std::ifstream& disk, char* data, std::streamsize size) {
        disk.read(data, size);
        return disk && disk.gcount() == size;
    }

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

    void applyFixups(char* buffer, uint32_t recordSize) {
        auto* header = reinterpret_cast<MFT_RecordHeader*>(buffer);

        if (recordSize < sizeof(MFT_RecordHeader)) {
            return;
        }

        if (header->updateSequenceSize == 0) {
            return;
        }

        const uint32_t updateSequenceBytes =
            static_cast<uint32_t>(header->updateSequenceSize) * sizeof(uint16_t);

        if (header->updateSequenceOffset > recordSize ||
            updateSequenceBytes > recordSize - header->updateSequenceOffset) {
            return;
        }

        // updateSequenceOffset is relative to the start of the record
        uint16_t* updateSequenceArray = reinterpret_cast<uint16_t*>(buffer + header->updateSequenceOffset);
        uint16_t sequenceNumber = updateSequenceArray[0];

        // Number of sectors (usually 2 for a 1024 byte record)
        // header->updateSequenceSize includes the sequence number itself, so we subtract 1
        int sectorCount = header->updateSequenceSize - 1;

        if (sectorCount <= 0) {
            return;
        }

        if (recordSize % static_cast<uint32_t>(sectorCount) != 0) {
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

    bool processMftRecord(
        MFT_RecordHeader* header,
        char* buffer,
        uint64_t mftIndex,
        NtfsDatabase& db,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
    {
        uint64_t baseIndex = mftIndex;
        bool isBaseRecord = true;

        if (header->baseFileRecord != 0) {
            // If the record has a reference to a base record, it implies that this
            // is an extension record. Get the base index.
            baseIndex = header->baseFileRecord & 0xFFFFFFFFFFFFULL;
            isBaseRecord = false;
        }

        FileInfo info = {{}, 0, static_cast<bool>(header->flags & 0x02), false, 0, baseIndex};
        uint32_t attrOffset = header->firstAttributeOffset;
        uint32_t recordSize = header->usedSize; // Use the size reported by the header

        std::vector<TempFileLink> allNames;

        uint64_t sizeFromData = 0;
        bool dataAttrFound = false;
        bool attributeListFound = false;

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

            if (attr->type == 0x20) { // $ATTRIBUTE_LIST
                // Presence of the $ATTRIBUTE_LIST header indicates that there are
                // extension records which are related to this record.
                attributeListFound = true;
            }
            else if (attr->type == 0x30) { // $FILE_NAME
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
                else {
                    std::cerr << "Non-resident $FILE_NAME attributes are not supported yet.\n";
                }
            }
            else if (attr->type == 0x80 && attr->nameLength == 0) { // $DATA (unnamed) (The actual file content)
                dataAttrFound = true;

                if (attr->nonResident == 0) {
                    // Resident: data is right here in the MFT record
                    sizeFromData = reinterpret_cast<ResidentHeader*>(buffer + attrOffset + sizeof(AttributeHeader))->dataLength;
                } else {
                    // Non-resident: data is stored elsewhere, but we can still get the file size from the header
                    sizeFromData = reinterpret_cast<NonResidentHeader*>(buffer + attrOffset + sizeof(AttributeHeader))->dataSize;
                }
            }

            attrOffset += attr->length;
        }

        // Check if record is a single record on its own, i.e. record is a base record
        // without has an attribute list (presence of an attribute list indicates that
        // there are extension records which are related to this record).
        //
        // If it's a single record on its own, we can add the file to the database now.
        // Otherwise, we need to wait until we have all records related to this file
        // to process them later.
        if (isBaseRecord && !attributeListFound) {
            // Record is on its own, we can add it to the database now.
            return finalizeAndAddFile(
                info,
                allNames,
                dataAttrFound,
                sizeFromData,
                db,
                mftIndex,
                onFileRecordChunk,
                onStringPoolChunk
            );
        }
        else {
            // Record is part of two or more records related to the file.
            // We need to wait until we have all records related to this file
            // before adding the file information to the database,
            // so that we can properly handle hard links and DOS aliases,
            // as well as capture missing file size or modification date.
            //
            // Note that almost all records in the MFT are going to be
            // base records on their own, so this logic isn't used much.
            // However, these files may either not appear in the index, or may
            // have missing file size or modification date without this step.
            //
            // On my two test drives, one had 0.4% and the other had 2.7%
            // of records which are related to multiple records.

            if (!dataAttrFound && allNames.empty()) {
                // Record doesn't contain any information we care about,
                // don't need to keep track of it for later
                return true;
            }

            db.extensionRecordFileInfos[baseIndex].emplace_back(
                std::move(allNames), // allNames is moved here
                info.isDir,
                info.isSymlink,
                mftIndex,
                dataAttrFound,
                sizeFromData
            );

            // Note for future me: 'allNames' is moved into the record - it must not be used afterward.
        }

        return true;
    }

    bool finalizeAndAddFile(
        FileInfo& info,
        const std::vector<TempFileLink>& allNames,
        bool dataAttrFound,
        uint64_t sizeFromData,
        NtfsDatabase& db,
        uint64_t mftIndex,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
    {
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
                    // Convert NTFS FILETIME -> unix seconds (so UI/daemon can treat all fs the same)
                    info.modificationTime = ntfsFiletimeToUnixSeconds(entry.modTime);
                    sizeFromFileName = entry.dataSize;
                }
            }
        }

        // If no DATA attribute was found (e.g. some directories or system files),
        // use the size cached in the filename attribute.
        info.size = dataAttrFound ? sizeFromData : sizeFromFileName;

        // Add file information to the database
        for (const auto& link : info.links) {
            if (link.name.empty()) {
                continue;
            }

            // System files starting with $ are usually hidden in Everything
            if (link.name[0] == '$' && mftIndex <= 38) {
                continue;
            }

            // Insert root directory as empty string to match EXT4 scanner behaviour
            if (link.name == ".") {
                if (!db.add(
                        "",
                        info.mftIndex,
                        link.parentIndex,
                        info.size,
                        info.modificationTime,
                        info.isDir,
                        info.isSymlink,
                        onFileRecordChunk,
                        onStringPoolChunk)) {
                    return false;
                }

                continue;
            }

            if (!db.add(
                    link.name,
                    mftIndex,
                    link.parentIndex,
                    info.size,
                    info.modificationTime,
                    info.isDir,
                    info.isSymlink,
                    onFileRecordChunk,
                    onStringPoolChunk)) {
                return false;
            }
        }

        return true;
    }

    bool processExtensionRecords(
        NtfsDatabase& db,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
    {
        for (const auto& [baseMftIndex, extensionRecords] : db.extensionRecordFileInfos) {
            // Combine the data from all records that belong to the same base
            // MFT record, then finalize the file once.
            FileInfo info{};
            info.mftIndex = baseMftIndex;

            std::vector<TempFileLink> allNames;
            bool dataAttrFound = false;
            uint64_t sizeFromData = 0;

            for (const ExtensionFileInfo& fileInfo : extensionRecords) {
                info.isDir |= fileInfo.isDir;
                info.isSymlink |= fileInfo.isSymlink;

                if (fileInfo.dataAttrFound) {
                    dataAttrFound = true;
                    sizeFromData = std::max(sizeFromData, fileInfo.sizeFromData);
                }

                allNames.insert(
                    allNames.end(),
                    fileInfo.tempLinks.begin(),
                    fileInfo.tempLinks.end()
                );
            }

            if (!finalizeAndAddFile(
                    info,
                    allNames,
                    dataAttrFound,
                    sizeFromData,
                    db,
                    baseMftIndex,
                    onFileRecordChunk,
                    onStringPoolChunk)) {
                return false;
            }
        }

        db.extensionRecordFileInfos.clear();
        return true;
    }

    std::string utf16ToUtf8(const char16_t* utf16_ptr, size_t length) {
        if (!utf16_ptr || length == 0) {
            return "";
        }

        std::string out;
        out.reserve(length * 2); // Pre-allocate to avoid multiple reallocs

        try {
            utf8::utf16to8(utf16_ptr, utf16_ptr + length, std::back_inserter(out));
        } catch (const utf8::invalid_utf16& e) {
            return "Invalid UTF-16 Data";
        }

        return out;
    }

    uint64_t ntfsFiletimeToUnixSeconds(uint64_t filetime100ns) {
        // NTFS FILETIME: 100ns intervals since 1601-01-01 (UTC)
        // Unix time: seconds since 1970-01-01 (UTC)
        static constexpr uint64_t kUnixEpochInFiletime100ns = 116444736000000000ULL; // 1970-01-01 in FILETIME units
        static constexpr uint64_t kTicksPerSecond = 10000000ULL;

        if (filetime100ns < kUnixEpochInFiletime100ns) {
            return 0;
        }

        return (filetime100ns - kUnixEpochInFiletime100ns) / kTicksPerSecond;
    }

    bool scanDevice(const QString& devicePath,
                    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
                    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
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
        if (!readExact(
                disk,
                reinterpret_cast<char*>(&boot),
                static_cast<std::streamsize>(sizeof(boot)))) {
            return reportError(
                onError,
                QStringLiteral("Failed to read NTFS boot sector from %1").arg(devicePath)
            );
        }

        if (std::string(boot.oemID, 8) != "NTFS    ") {
            return reportError(
                onError,
                QStringLiteral("%1 does not appear to be a valid NTFS partition").arg(devicePath)
            );
        }

        uint64_t bytesPerCluster = static_cast<uint64_t>(boot.bytesPerSector) * boot.sectorsPerCluster;
        uint64_t mftOffset = boot.mftStartLcn * bytesPerCluster;

        if (boot.bytesPerSector == 0 ||
            boot.sectorsPerCluster == 0 ||
            bytesPerCluster == 0) {
            return reportError(
                onError,
                QStringLiteral("Invalid NTFS cluster geometry for %1").arg(devicePath)
            );
        }

        // Determine Record Size: Usually 1024 bytes.
        // If clustersPerFileRecord is negative, size is 2^(abs(value)).
        int32_t clustersPerFileRecord = boot.clustersPerFileRecord;
        uint32_t mftRecordSize = (clustersPerFileRecord > 0) ? (clustersPerFileRecord * bytesPerCluster) : (1 << (-clustersPerFileRecord));

        if (mftRecordSize < sizeof(MFT_RecordHeader) ||
            mftRecordSize > 1024 * 1024 ||
            (8 * 1024 * 1024) / mftRecordSize == 0) {
            return reportError(
                onError,
                QStringLiteral("Invalid NTFS MFT record size %1 for %2")
                    .arg(mftRecordSize)
                    .arg(devicePath)
            );
        }

        std::cerr << "--- NTFS Volume Info ---" << "\n";
        std::cerr << "Bytes per Sector:    " << boot.bytesPerSector << "\n";
        std::cerr << "Sectors per Cluster: " << static_cast<int>(boot.sectorsPerCluster) << "\n";
        std::cerr << "MFT Start LCN:       " << boot.mftStartLcn << "\n";
        std::cerr << "MFT Offset (hex):    0x" << std::hex << mftOffset << std::dec << "\n";
        std::cerr << "Record Size:         " << mftRecordSize << " bytes\n";
        std::cerr << "------------------------" << "\n";

        if (mftOffset == 0 || mftRecordSize == 0) {
            std::cerr << "Invalid MFT parameters calculated. Struct alignment might be wrong.\n";
            return false;
        }

        std::vector<char> buffer(mftRecordSize);

        std::vector<MftRun> mftRuns;

        // Step 2: Read MFT Record 0 (The MFT's own entry) to find all fragments of the MFT.
        disk.seekg(mftOffset);
        if (!disk ||
            !readExact(
                disk,
                buffer.data(),
                static_cast<std::streamsize>(mftRecordSize))) {
            return reportError(
                onError,
                QStringLiteral("Failed to read NTFS MFT record 0 from %1").arg(devicePath)
            );
        }

        auto* mftHeader = reinterpret_cast<MFT_RecordHeader*>(buffer.data());

        if (std::string_view(mftHeader->signature, 4) != "FILE") {
            return reportError(
                onError,
                QStringLiteral("Invalid NTFS MFT record 0 signature for %1").arg(devicePath)
            );
        }

        applyFixups(buffer.data(), mftRecordSize);

        if (mftHeader->firstAttributeOffset >= mftRecordSize ||
            mftHeader->usedSize < mftHeader->firstAttributeOffset ||
            mftHeader->usedSize > mftRecordSize) {
            return reportError(
                onError,
                QStringLiteral("Invalid NTFS MFT record 0 attribute layout for %1").arg(devicePath)
            );
        }

        uint32_t mftAttrOffset = mftHeader->firstAttributeOffset;
        uint64_t totalMftSize = 0;
        while (mftAttrOffset + sizeof(AttributeHeader) <= mftHeader->usedSize) {
            auto* attr = reinterpret_cast<AttributeHeader*>(buffer.data() + mftAttrOffset);

            // 0xFFFFFFFF is the end-of-attributes marker in NTFS
            if (attr->type == 0xFFFFFFFF) {
                break;
            }

            if (attr->length == 0 ||
                mftAttrOffset + attr->length > mftHeader->usedSize) {
                return reportError(
                    onError,
                    QStringLiteral("Invalid NTFS MFT record 0 attribute length for %1").arg(devicePath)
                );
            }

            if (attr->type == 0x80) { // $DATA Attribute
                if (attr->nonResident == 0 ||
                    mftAttrOffset + sizeof(AttributeHeader) + sizeof(NonResidentHeader) > mftHeader->usedSize) {
                    return reportError(
                        onError,
                        QStringLiteral("Invalid NTFS $MFT data attribute for %1").arg(devicePath)
                    );
                }

                parseMftRuns(buffer.data(), mftAttrOffset, mftRuns);

                auto* nonResident = reinterpret_cast<NonResidentHeader*>(
                    buffer.data() + mftAttrOffset + sizeof(AttributeHeader));

                totalMftSize = nonResident->dataSize;
                break;
            }

            mftAttrOffset += attr->length;
        }

        if (mftRuns.empty() || totalMftSize == 0) {
            return reportError(
                onError,
                QStringLiteral("Could not locate NTFS MFT data runs for %1").arg(devicePath)
            );
        }

        uint64_t totalRecords = totalMftSize / mftRecordSize;
        std::cerr << "MFT consists of " << mftRuns.size() << " fragments.\n";
        std::cerr << "Total MFT Records: " << totalRecords << "\n";

        // Batch processing buffer for scanning the MFT records (8MB)
        const size_t mftScanBatchSizeInRecords = (8 * 1024 * 1024) / mftRecordSize;
        std::vector<char> batchBuffer(mftScanBatchSizeInRecords * mftRecordSize);

        NtfsDatabase db{};
        db.records.reserve(NtfsDatabase::kRecordsPerIpcChunk);
        db.stringPool.reserve(NtfsDatabase::kTargetIpcBufferSizeMB);
        db.totalStringPoolLength = 0;

        uint64_t scannedRecords = 0;

        if (onProgress) {
            onProgress(Protocol::ScanProgress{
                .phase = QStringLiteral("Reading MFT"),
                .unit = QStringLiteral("records"),
                .processed = 0,
                .total = totalRecords
            });
        }

        // Step 3: Collect all valid records.
        for (const auto& run : mftRuns) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }

            uint64_t runOffset = run.logicalClusterNumber * bytesPerCluster;
            uint64_t recordsInRun = (run.length * bytesPerCluster) / mftRecordSize;

            // Starting index for this specific fragment/run
            uint64_t runStartIndex = (run.virtualClusterNumber * bytesPerCluster) / mftRecordSize;

            for (uint64_t r = 0; r < recordsInRun; r += mftScanBatchSizeInRecords) {
                if (shouldCancel && shouldCancel()) {
                    return false;
                }

                uint64_t toRead = std::min(mftScanBatchSizeInRecords, recordsInRun - r);
                disk.seekg(runOffset + (r * mftRecordSize));

                const uint64_t bytesToRead = toRead * mftRecordSize;
                if (!disk ||
                    bytesToRead > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
                    !readExact(
                        disk,
                        batchBuffer.data(),
                        static_cast<std::streamsize>(bytesToRead))) {
                    return reportError(
                        onError,
                        QStringLiteral("Failed to read NTFS MFT records from %1").arg(devicePath)
                    );
                }

                for (uint64_t i = 0; i < toRead; ++i) {
                    ++scannedRecords;

                    if (shouldCancel && shouldCancel()) {
                        return false;
                    }

                    // Notify progress
                    static constexpr uint64_t kProgressEvery = 4096; // must be power of two
                    if (onProgress && ((scannedRecords & (kProgressEvery - 1)) == 0)) {
                        onProgress(Protocol::ScanProgress{
                            .phase = QStringLiteral("Reading MFT"),
                            .unit = QStringLiteral("records"),
                            .processed = scannedRecords,
                            .total = totalRecords
                        });
                    }

                    char* mftRecordPtr = batchBuffer.data() + (i * mftRecordSize);
                    auto* mftRecordHeader = reinterpret_cast<MFT_RecordHeader*>(mftRecordPtr);

                    // Check signature and 'In Use' flag
                    if (std::string_view(mftRecordHeader->signature, 4) != "FILE" || !(mftRecordHeader->flags & 0x01)) {
                        continue;
                    }

                    applyFixups(mftRecordPtr, mftRecordSize);
                    uint64_t mftRecordIndex = runStartIndex + r + i;

                    if (!processMftRecord(
                            mftRecordHeader,
                            mftRecordPtr,
                            mftRecordIndex,
                            db,
                            onFileRecordChunk,
                            onStringPoolChunk)) {
                        if (onError) {
                            onError(QStringLiteral("NTFS scan was aborted while streaming file records"));
                        }

                        return false;
                    }
                }
            }
        }

        // Now that the whole MFT has been scanned, process records that were
        // deferred because they used $ATTRIBUTE_LIST / extension records.
        //
        // This must happen before the final chunk flush, otherwise directories
        // or files represented by extension records can be absent from the index,
        // causing their children to resolve as mount-root entries.
        if (!db.extensionRecordFileInfos.empty()) {
            if (!processExtensionRecords(
                    db,
                    onFileRecordChunk,
                    onStringPoolChunk)) {
                if (onError) {
                    onError(QStringLiteral("NTFS scan was aborted while streaming extension records"));
                }

                return false;
            }
        }

        // Flush remaining records
        if (!db.flush(onFileRecordChunk, onStringPoolChunk)) {
            if (onError) {
                onError(QStringLiteral("NTFS scan was aborted while flushing final records"));
            }

            return false;
        }

        // Report completion
        if (onProgress) {
            onProgress(Protocol::ScanProgress{
                .phase = QStringLiteral("Reading MFT"),
                .unit = QStringLiteral("records"),
                .processed = scannedRecords,
                .total = totalRecords
            });
        }

        return true;
    }

    bool NtfsDatabase::add(
        std::string_view name,
        uint64_t mftIndex,
        uint64_t parentMftIndex,
        uint64_t size,
        uint64_t mod,
        bool isDir,
        bool isSymlink,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
    {
        if (name.size() > std::numeric_limits<uint16_t>::max()) {
            return true;
        }

        if (stringPool.size() + name.length() > kMaxIpcBufferSizeBytes) {
            if (!flush(onFileRecordChunk, onStringPoolChunk)) {
                return false;
            }
        }

        FileRecord rec{};
        rec.fsIndex = mftIndex;
        rec.parentFsIndex = parentMftIndex;
        rec.parentRecordIdx = 0; // Initialize to 0, will be updated later in GUI code
        rec.size = size;
        rec.modificationTime = mod;
        rec.nameOffset = totalStringPoolLength;
        rec.nameLen = static_cast<uint16_t>(name.length());
        rec.flags = 0;

        if (isDir) {
            rec.flags |= FileRecord_IsDir;
        }

        if (isSymlink) {
            rec.flags |= FileRecord_IsSymlink;
        }

        totalStringPoolLength += name.length();

        records.push_back(rec);
        stringPool.insert(stringPool.end(), name.begin(), name.end());

        if (records.size() >= kRecordsPerIpcChunk) {
            if (!flush(onFileRecordChunk, onStringPoolChunk)) {
                return false;
            }
        }

        return true;
    }

    bool NtfsDatabase::flush(
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
    {
        if (!records.empty()) {
            std::vector<FileRecord> fileRecordChunk = std::move(records);

            records.clear();
            records.reserve(kRecordsPerIpcChunk);

            if (!onFileRecordChunk(fileRecordChunk)) {
                std::cerr << "NTFS scan aborted by file record receiver\n";
                return false;
            }
        }

        if (!stringPool.empty()) {
            std::vector<char> stringPoolChunk = std::move(stringPool);

            stringPool.clear();
            stringPool.reserve(kMaxIpcBufferSizeBytes);

            if (!onStringPoolChunk(stringPoolChunk)) {
                std::cerr << "NTFS scan aborted by string pool receiver\n";
                return false;
            }
        }

        return true;
    }
}
