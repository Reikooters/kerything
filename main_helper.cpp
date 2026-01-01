// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <iostream>
#include "ScannerEngine.h"

/**
 * The Helper:
 * 1. Takes an NTFS device path as an argument.
 * 2. Scans the MFT.
 * 3. Dumps the results to stdout in binary format.
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        return 1;
    }

    // Turn off sync with stdio to speed up binary writes and prevent buffering issues
    std::ios_base::sync_with_stdio(false);

    // Use parseMFT from our engine
    auto db = ScannerEngine::parseMFT(argv[1]);
    if (!db) {
        return 1;
    }

    // 1. Write the number of records
    uint64_t recordCount = db->records.size();
    std::cout.write(reinterpret_cast<const char*>(&recordCount), sizeof(recordCount));

    // 2. Write the raw vector data (The FileRecord structs)
    std::cout.write(reinterpret_cast<const char*>(db->records.data()), static_cast<std::streamsize>(recordCount * sizeof(ScannerEngine::FileRecord)));

    // 3. Write the size of the string pool
    uint64_t poolSize = db->stringPool.size();
    std::cout.write(reinterpret_cast<const char*>(&poolSize), sizeof(poolSize));

    // 4. Write the string pool itself
    std::cout.write(db->stringPool.data(), static_cast<std::streamsize>(poolSize));

    // 5. Write the directory paths (Serialization)
    uint64_t dirCount = db->directoryPaths.size();
    std::cout.write(reinterpret_cast<const char*>(&dirCount), sizeof(dirCount));

    for (const auto& [idx, path] : db->directoryPaths) {
        std::cout.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
        uint32_t len = path.length();
        std::cout.write(reinterpret_cast<const char*>(&len), sizeof(len));
        std::cout.write(path.data(), len);
    }

    std::cout.flush();
    return 0;
}