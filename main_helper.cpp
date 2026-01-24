// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <linux/limits.h>
#include <sys/stat.h>

#include "scanners/NtfsScannerEngine.h"
#include "scanners/Ext4ScannerEngine.h"
#include "Version.h"

static void printUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --version\n"
        << "  " << argv0 << " <devicePath> <fsType>\n"
        << "Where:\n"
        << "  <devicePath> is a block device path like /dev/sdXN or /dev/nvme0n1pN\n"
        << "  <fsType> is one of: ntfs, ext4\n";
}

static bool isAllowedFsType(std::string_view fsType) {
    return fsType == "ntfs" || fsType == "ext4";
}

static bool validateDevicePath(const std::string& inputPath, std::string& resolvedOut) {
    namespace fs = std::filesystem;

    if (inputPath.empty()) {
        std::cerr << "Error: empty device path.\n";
        return false;
    }

    fs::path p(inputPath);

    if (!p.is_absolute()) {
        std::cerr << "Error: device path must be absolute (got: " << inputPath << ").\n";
        return false;
    }

    // Only allow scanning devices under /dev.
    if (p.native().rfind("/dev/", 0) != 0) {
        std::cerr << "Error: device path must be under /dev (got: " << inputPath << ").\n";
        return false;
    }

    // Resolve symlinks / relative components safely.
    // realpath() fails if the path doesn't exist.
    char buf[PATH_MAX];
    if (!realpath(inputPath.c_str(), buf)) {
        std::cerr << "Error: failed to resolve device path '" << inputPath
                  << "': " << std::strerror(errno) << "\n";
        return false;
    }

    resolvedOut = buf;

    struct stat st {};
    if (stat(resolvedOut.c_str(), &st) != 0) {
        std::cerr << "Error: stat() failed for '" << resolvedOut
                  << "': " << std::strerror(errno) << "\n";
        return false;
    }

    if (!S_ISBLK(st.st_mode)) {
        std::cerr << "Error: '" << resolvedOut << "' is not a block device.\n";
        return false;
    }

    // Reject world-writable device nodes (paranoia / sanity check)
    if ((st.st_mode & S_IWOTH) != 0) {
        std::cerr << "Error: refusing world-writable device node '" << resolvedOut << "'.\n";
        return false;
    }

    return true;
}

static bool safeWriteAll(const char* data, std::streamsize n) {
    std::cout.write(data, n);
    return static_cast<bool>(std::cout);
}

struct ProgressReporter {
    using Clock = std::chrono::steady_clock;
    static constexpr auto kMinInterval = std::chrono::milliseconds(100);

    Clock::time_point nextEmit = Clock::now();
    uint8_t lastPct = 255;

    void operator()(uint64_t done, uint64_t total) {
        if (total == 0) {
            total = 1;
        }
        if (done > total) {
            done = total;
        }

        // Always emit completion immediately
        if (done == total) {
            if (lastPct == 100) {
                return;
            }

            lastPct = 100;
            std::cerr << "KERYTHING_PROGRESS 100\n";
            std::cerr.flush();
            return;
        }

        const auto now = Clock::now();
        if (now < nextEmit) {
            return;
        }
        nextEmit = now + kMinInterval;

        const uint64_t pct64 = (done * 100 + total / 2) / total;
        const uint8_t pct = static_cast<uint8_t>(pct64);

        if (pct == lastPct) {
            return;
        }
        lastPct = pct;

        std::cerr << "KERYTHING_PROGRESS " << static_cast<unsigned>(pct) << "\n";
        std::cerr.flush();
    }
};

int scanNtfs(const std::string& devicePath) {
    ProgressReporter reporter;

    // Use parseMft from the NTFS scanner engine
    std::optional<NtfsScannerEngine::NtfsDatabase> db = NtfsScannerEngine::parseMft(devicePath, reporter);
    if (!db) {
        return 2;
    }

    // 1. Write the number of records
    uint64_t recordCount = db->records.size();
    if (!safeWriteAll(reinterpret_cast<const char*>(&recordCount), sizeof(recordCount))) {
        std::cerr << "Error: failed writing recordCount to stdout.\n";
        return 3;
    }

    // 2. Write the raw vector data (The FileRecord structs)
    const auto recordBytes = static_cast<std::streamsize>(recordCount * sizeof(NtfsScannerEngine::FileRecord));
    if (!safeWriteAll(reinterpret_cast<const char*>(db->records.data()), recordBytes)) {
        std::cerr << "Error: failed writing records to stdout.\n";
        return 3;
    }

    // 3. Write the size of the string pool
    uint64_t poolSize = db->stringPool.size();
    if (!safeWriteAll(reinterpret_cast<const char*>(&poolSize), sizeof(poolSize))) {
        std::cerr << "Error: failed writing poolSize to stdout.\n";
        return 3;
    }

    // 4. Write the string pool itself
    if (!safeWriteAll(db->stringPool.data(), static_cast<std::streamsize>(poolSize))) {
        std::cerr << "Error: failed writing stringPool to stdout.\n";
        return 3;
    }

    std::cout.flush();
    return std::cout ? 0 : 3;
}

int scanExt4(const std::string& devicePath) {
    ProgressReporter reporter;

    // Use parseInodes from the EXT4 scanner engine
    std::optional<Ext4ScannerEngine::Ext4Database> db = Ext4ScannerEngine::parseInodes(devicePath, reporter);
    if (!db) {
        return 2;
    }

    // 1. Write the number of records
    uint64_t recordCount = db->records.size();
    if (!safeWriteAll(reinterpret_cast<const char*>(&recordCount), sizeof(recordCount))) {
        std::cerr << "Error: failed writing recordCount to stdout.\n";
        return 3;
    }

    // 2. Write the raw vector data (The FileRecord structs)
    const auto recordBytes = static_cast<std::streamsize>(recordCount * sizeof(Ext4ScannerEngine::FileRecord));
    if (!safeWriteAll(reinterpret_cast<const char*>(db->records.data()), recordBytes)) {
        std::cerr << "Error: failed writing records to stdout.\n";
        return 3;
    }

    // 3. Write the size of the string pool
    uint64_t poolSize = db->stringPool.size();
    if (!safeWriteAll(reinterpret_cast<const char*>(&poolSize), sizeof(poolSize))) {
        std::cerr << "Error: failed writing poolSize to stdout.\n";
        return 3;
    }

    // 4. Write the string pool itself
    if (!safeWriteAll(db->stringPool.data(), static_cast<std::streamsize>(poolSize))) {
        std::cerr << "Error: failed writing stringPool to stdout.\n";
        return 3;
    }

    std::cout.flush();
    return std::cout ? 0 : 3;
}

/**
 * The Helper:
 * 1. Takes a device path and file system type as arguments.
 * 2. Scans the specified partition.
 * 3. Dumps the results to stdout in binary format.
 */
int main(int argc, char* argv[]) {
    // Allow "--version" without requiring other args
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "kerything-scanner-helper v" << Version::VERSION << std::endl;
        return 0;
    }

    if (argc != 3) {
        printUsage(argv[0]);
        return 64; // EX_USAGE
    }

    std::string devicePathInput = argv[1];
    std::string_view fsType = argv[2];

    if (!isAllowedFsType(fsType)) {
        std::cerr << "Error: unsupported fsType '" << fsType << "'.\n";
        printUsage(argv[0]);
        return 64; // EX_USAGE
    }

    std::string devicePath;
    if (!validateDevicePath(devicePathInput, devicePath)) {
        return 65; // EX_DATAERR-ish
    }

    // Turn off sync with stdio to speed up binary writes and prevent buffering issues
    std::ios_base::sync_with_stdio(false);

    std::cerr << "Scanning " << devicePath << " (" << fsType << ")\n";

    if (fsType == "ntfs") {
        return scanNtfs(devicePath);
    }
    if (fsType == "ext4") {
        return scanExt4(devicePath);
    }

    return 64;
}
