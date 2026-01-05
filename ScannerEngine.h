// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SCANNERENGINE_H
#define KERYTHING_SCANNERENGINE_H

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <string_view>

// VITAL FIX: TBB and Qt both want 'emit'.
// We must undefine it before including <execution>
#ifdef emit
#define QT_EMIT_BACKUP emit
#undef emit
#endif

#include <execution>

#ifdef QT_EMIT_BACKUP
#define emit QT_EMIT_BACKUP
#undef QT_EMIT_BACKUP
#endif

namespace ScannerEngine {
    /**
     * We use #pragma pack(1) to ensure these structs are stored with NO padding.
     * This is critical because:
     * 1. The Helper process writes these structs as a raw binary blob to stdout.
     * 2. The GUI process reads that blob back into an array.
     *
     * Without this, different compilers, architectures (x86 vs ARM), or even
     * optimization levels could add "padding bytes" for CPU alignment, which
     * would corrupt the data transfer between the scanner and the GUI.
     */
    #pragma pack(push, 1)

    // Struct for storing our file records index
    struct FileRecord {
        uint32_t parentRecordIdx; // Index in the 'records' vector
        uint64_t size;
        uint64_t modificationTime;
        uint32_t nameOffset; // Offset into the global string pool
        uint16_t nameLen;
        uint8_t isDir : 1;
        uint8_t isSymlink : 1;
        uint8_t reserved : 6;
    };

    #pragma pack(pop)

    struct TrigramEntry {
        uint32_t trigram;
        uint32_t recordIdx;

        // Sorting by trigram first, then recordIdx
        bool operator<(const TrigramEntry& other) const {
            if (trigram != other.trigram) {
                return trigram < other.trigram;
            }

            return recordIdx < other.recordIdx;
        }
    };

    struct SearchDatabase {
        std::vector<FileRecord> records;
        std::vector<char> stringPool;

        // The "Frozen Index": A single sorted vector of all trigram-record pairs
        std::vector<TrigramEntry> flatIndex;

        void buildTrigramIndexParallel() {
            std::cerr << "Building Flat Trigram Index in parallel..." << std::endl;

            // 1. Calculate how many trigrams we'll have in total to avoid reallocations
            // (Roughly: sum of all filename lengths - 2)
            size_t totalTrigrams = 0;

            for (const auto& rec : records) {
                if (rec.nameLen >= 3) {
                    totalTrigrams += (rec.nameLen - 2);
                }
            }

            flatIndex.resize(totalTrigrams);

            // 2. Fill the flatIndex in parallel
            // We divide the records into chunks to give to each thread
            const size_t numRecords = records.size();
            std::vector<size_t> startOffsets(numRecords);

            // This part is serial but very fast (calculating where each record starts in flatIndex)
            size_t currentOffset = 0;
            for (size_t i = 0; i < numRecords; ++i) {
                startOffsets[i] = currentOffset;

                if (records[i].nameLen >= 3) {
                    currentOffset += (records[i].nameLen - 2);
                }
            }

            std::vector<uint32_t> workIndices(numRecords);
            std::iota(workIndices.begin(), workIndices.end(), 0);

            std::for_each(std::execution::par, workIndices.begin(), workIndices.end(), [&](uint32_t i) {
                const auto& rec = records[i];
                if (rec.nameLen < 3) {
                    return;
                }

                std::string_view name(&stringPool[rec.nameOffset], rec.nameLen);
                size_t writePos = startOffsets[i];

                for (size_t j = 0; j <= name.length() - 3; ++j) {
                    uint32_t tri = (static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(name[j]))) << 16) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(name[j+1]))) << 8) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(std::tolower(name[j+2]))));

                    flatIndex[writePos++] = { tri, i };
                }
            });

            // 3. Sort the entire index in parallel
            // This is the most CPU-intensive part!
            std::cerr << "Sorting " << flatIndex.size() << " trigrams..." << std::endl;
            std::sort(std::execution::par, flatIndex.begin(), flatIndex.end());

            // 4. Remove exact duplicates (same trigram in same file)
            std::cerr << "Removing duplicate trigrams..." << std::endl;
            auto last = std::unique(std::execution::par, flatIndex.begin(), flatIndex.end(), [](const auto& a, const auto& b) {
                return a.trigram == b.trigram && a.recordIdx == b.recordIdx;
            });
            flatIndex.erase(last, flatIndex.end());

            // 5. Reclaim memory used by the duplicates which were removed
            flatIndex.shrink_to_fit();
        }

        [[nodiscard]] std::string getFullPath(const uint32_t recordIdx) const {
            std::vector<uint32_t> chain;
            uint32_t current = recordIdx;
            size_t totalLength = 0;

            static constexpr std::string_view rootPath = "/";
            static constexpr std::string_view oneDot = ".";
            static constexpr std::string_view twoDots = "..";

            // 1. Identify the chain of parents that need resolving
            // STOP if we hit:
            // - The root marker (0xFFFFFFFF)
            // - A record that points to itself (NTFS root often does this)
            while (current != 0xFFFFFFFF) {
                const auto& r = records[current];
                std::string_view name(&stringPool[r.nameOffset], r.nameLen);

                // Only count length if it's not a dot-entry
                if (name != oneDot && name != twoDots) {
                    chain.push_back(current);
                    totalLength += 1; // For the "/" separator
                    totalLength += r.nameLen;
                }

                uint32_t next = r.parentRecordIdx;

                if (next == current) {
                    break; // Self-reference safety
                }

                current = next;
            }

            if (chain.empty()) {
                return std::string(rootPath);
            }

            // 2. Pre-allocate the exact size
            std::string base;
            base.reserve(totalLength);

            // 3. Build paths from top to bottom
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                uint32_t idx = *it;
                const auto& r = records[idx];
                std::string_view name(&stringPool[r.nameOffset], r.nameLen);

                // If the first element is the root "/", we don't want to double-up
                // but typically in MFT, the root is just an empty name or a specific index.
                // This check handles the edge case where the first entry is already "/"
                if (name == rootPath && base.empty()) {
                    base = rootPath;
                    continue;
                }

                // Build the string
                base += rootPath;
                base += name;
            }

            return base;
        }
    };
}

#endif //KERYTHING_SCANNERENGINE_H