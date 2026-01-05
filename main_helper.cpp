// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <iostream>
#include "scanners/NtfsScannerEngine.h"
#include "Version.h"

int scanNtfs(const std::string& devicePath) {
    // Use parseMft from the NTFS scanner engine
    std::optional<NtfsScannerEngine::NtfsDatabase> db = NtfsScannerEngine::parseMft(devicePath);
    if (!db) {
        return 1;
    }

    // 1. Write the number of records
    uint64_t recordCount = db->records.size();
    std::cout.write(reinterpret_cast<const char*>(&recordCount), sizeof(recordCount));

    // 2. Write the raw vector data (The FileRecord structs)
    std::cout.write(reinterpret_cast<const char*>(db->records.data()), static_cast<std::streamsize>(recordCount * sizeof(NtfsScannerEngine::FileRecord)));

    // 3. Write the size of the string pool
    uint64_t poolSize = db->stringPool.size();
    std::cout.write(reinterpret_cast<const char*>(&poolSize), sizeof(poolSize));

    // 4. Write the string pool itself
    std::cout.write(db->stringPool.data(), static_cast<std::streamsize>(poolSize));

    std::cout.flush();
    return 0;
}

int scanExt4(const std::string& devicePath) {
    // TODO
    return 0;
}

/**
 * The Helper:
 * 1. Takes a device path and file system type as arguments.
 * 2. Scans the specified partition.
 * 3. Dumps the results to stdout in binary format.
 */
int main(int argc, char* argv[]) {
    if (argc < 3) {
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            std::cout << "kerything-scanner-helper v" << Version::VERSION << std::endl;
            return 0;
        }
    }

    std::string devicePath = argv[1];
    std::string_view fsType = argv[2];

    // Turn off sync with stdio to speed up binary writes and prevent buffering issues
    std::ios_base::sync_with_stdio(false);

    std::cerr << "Scanning " << devicePath << " (" << fsType << ")" << std::endl;

    if (fsType == "ntfs") {
        return scanNtfs(devicePath);
    } else if (fsType == "ext4") {
        return scanExt4(devicePath);
    }

    return 1;
}
