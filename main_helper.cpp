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

    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            std::cout << "kerything-scanner-helper v" << ScannerEngine::VERSION << std::endl;
            return 0;
        }
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

    std::cout.flush();
    return 0;
}