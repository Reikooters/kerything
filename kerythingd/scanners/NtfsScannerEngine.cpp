// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <fstream>
#include <iostream>
#include "NtfsScannerEngine.h"

namespace NtfsScannerEngine {

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

        std::cerr << "OK - WIP\n";

        return true;
    }

}