// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_INDEXCONTROLLER_H
#define KERYTHING_INDEXCONTROLLER_H

#include <execution>
#include <iostream>
#include <QObject>
#include <shared_mutex>

#include "FileRecord.h"

class IndexController final : public QObject {
    Q_OBJECT

public:
    explicit IndexController(QObject* parent = nullptr);

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

    struct DeviceIndex {
        quint64 deviceId;
        QString fsType;
        quint64 generation = 0;
        bool isReady = false;

        // unix seconds; 0 means unknown
        qint64 lastIndexedTime = 0;

        // display metadata
        QString label;
        QString devicePath; // e.g. /dev/disk/by-partuuid/<partuuid> or for loopback devices: /dev/disk/by-uuid/<uuid>

        std::vector<FileRecord> fileRecords;
        std::vector<char> stringPool;
        std::unordered_map<uint64_t, uint32_t> fsIndexToRecordIdx;

        // Lowercase mirror of the string pool for sorting and searching
        std::vector<char> lowercaseStringPool;

        // A single sorted vector of all trigram-record pairs
        std::vector<TrigramEntry> flatIndex;

        /**
         * Resolves parent-child relationships between file records in the NTFS database.
         * This is called once after the MFT scan is completely finished
         *
         * This method converts temporary parent MFT indices into internal indices and updates
         * the database entries to reflect the hierarchical relationships. Any unresolved parent
         * pointers, such as those referencing the root directory, are marked accordingly.
         * Additionally, all temporary data structures used during the setup phase are freed
         * to optimize memory usage.
         */
        void resolveParentPointers() {
            std::cerr << "Resolving parent pointers..." << std::endl;

            // Convert parent MFT index to internal index
            for (size_t i = 0; i < fileRecords.size(); ++i) {
                auto it = fsIndexToRecordIdx.find(fileRecords[i].parentFsIndex);
                if (it != fsIndexToRecordIdx.end()) {
                    fileRecords[i].parentRecordIdx = it->second;
                } else {
                    // If parent isn't in our DB (like MFT Index 5's parent), mark as root
                    fileRecords[i].parentRecordIdx = 0xFFFFFFFF;
                }
            }
        }

        void buildLowercaseStringPool() {
            // Reserve memory once
            lowercaseStringPool.resize(stringPool.size());

            // Convert the string pool to lowercase in parallel and store it in lowercaseStringPool
            std::transform(std::execution::par_unseq, stringPool.begin(), stringPool.end(),
                      lowercaseStringPool.begin(),
                           [](unsigned char c) {
                               return (c >= 'A' && c <= 'Z') ? (c | 32) : c;
                           });
        }

        void sortByNameAscendingParallel() {
            if (fileRecords.empty()) {
                return;
            }

            std::cerr << "Pre-sorting records by name ascending (case-insensitive) in parallel..." << std::endl;

            auto compareCaseInsensitive = [&](const FileRecord& a, const FileRecord& b) {
                std::string_view s1(&lowercaseStringPool[a.nameOffset], a.nameLen);
                std::string_view s2(&lowercaseStringPool[b.nameOffset], b.nameLen);

                return std::lexicographical_compare(
                    s1.begin(), s1.end(),
                    s2.begin(), s2.end(),
                    [](char first, char second) {
                        return first < second;
                    }
                );
            };

            // NOTE: We cannot simply sort 'records' because parentRecordIdx
            // depends on the original vector indices. We must re-map them.

            // 1. Create an index mapping
            std::vector<uint32_t> p(fileRecords.size());
            std::iota(p.begin(), p.end(), 0);

            // 2. Sort the index mapping based on names
            std::sort(std::execution::par, p.begin(), p.end(), [&](uint32_t i, uint32_t j) {
                return compareCaseInsensitive(fileRecords[i], fileRecords[j]);
            });

            // 3. Create a reverse mapping to update parentRecordIdx
            std::vector<uint32_t> rev(p.size());
            for (uint32_t i = 0; i < p.size(); ++i) {
                rev[p[i]] = i;
            }

            // 4. Reorder records and update parent indices
            std::vector<FileRecord> newRecords(fileRecords.size());
            for (size_t i = 0; i < p.size(); ++i) {
                newRecords[i] = fileRecords[p[i]];
                if (newRecords[i].parentRecordIdx != 0xFFFFFFFF) {
                    newRecords[i].parentRecordIdx = rev[newRecords[i].parentRecordIdx];
                }
            }

            fileRecords = std::move(newRecords);
        }

        void buildTrigramIndexParallel() {
            std::cerr << "Building Flat Trigram Index in parallel..." << std::endl;

            // 1. Calculate how many trigrams we'll have in total to avoid reallocations
            // (Roughly: sum of all filename lengths - 2)
            size_t totalTrigrams = 0;

            for (const auto& rec : fileRecords) {
                if (rec.nameLen >= 3) {
                    totalTrigrams += (rec.nameLen - 2);
                }
            }

            flatIndex.resize(totalTrigrams);

            // 2. Fill the flatIndex in parallel
            // We divide the records into chunks to give to each thread
            const size_t numRecords = fileRecords.size();
            std::vector<size_t> startOffsets(numRecords);

            // This part is serial but very fast (calculating where each record starts in flatIndex)
            size_t currentOffset = 0;
            for (size_t i = 0; i < numRecords; ++i) {
                startOffsets[i] = currentOffset;

                if (fileRecords[i].nameLen >= 3) {
                    currentOffset += (fileRecords[i].nameLen - 2);
                }
            }

            std::vector<uint32_t> workIndices(numRecords);
            std::iota(workIndices.begin(), workIndices.end(), 0);

            std::for_each(std::execution::par, workIndices.begin(), workIndices.end(), [&](uint32_t i) {
                const auto& rec = fileRecords[i];
                if (rec.nameLen < 3) {
                    return;
                }

                std::string_view name(&lowercaseStringPool[rec.nameOffset], rec.nameLen);
                size_t writePos = startOffsets[i];

                for (size_t j = 0; j <= name.length() - 3; ++j) {
                    uint32_t tri = (static_cast<uint32_t>(static_cast<unsigned char>(name[j])) << 16) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(name[j+1])) << 8) |
                                   (static_cast<uint32_t>(static_cast<unsigned char>(name[j+2])));

                    flatIndex[writePos++] = { tri, i };
                }
            });

            // 3. Sort the entire index in parallel
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
            // - A record that points to itself (some filesystems do this)
            while (current != 0xFFFFFFFF) {
                const auto& r = fileRecords[current];
                std::string_view name(&stringPool[r.nameOffset], r.nameLen);

                // Only count length if it's not a dot-entry and not blank
                if (name != oneDot && name != twoDots && !name.empty()) {
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
                const auto& r = fileRecords[idx];
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

    struct RecordHandle {
        quint64 deviceId;
        quint64 generation;
        uint32_t recordIdx;
    };

    const DeviceIndex* deviceIndex(quint64 deviceId) const;

    quint64 addDevice(const QString& devicePath, const QString& fsType, const QString& label, quint32 requestId);
    void removeDeviceByDeviceId(quint64 deviceId);
    bool removeDeviceByRequestId(quint32 requestId);
    void appendDeviceFileRecordsByRequestId(quint32 requestId, const std::vector<FileRecord>& records);
    void appendDeviceStringPoolByRequestId(quint32 requestId, QByteArrayView stringPool);
    bool removeRequestId(quint32 requestId);
    std::vector<RecordHandle> performTrigramSearch(const std::string& query);
    void resolveParentPointersByRequestId(quint32 requestId);
    void buildLowercaseStringPoolByRequestId(quint32 requestId);
    void sortByNameAscendingParallelByRequestId(quint32 requestId);
    void buildTrigramIndexParallelByRequestId(quint32 requestId);
    void setReadyState(quint32 requestId, bool isReady);

    template <typename Fn>
    auto withDeviceIndexRead(quint64 deviceId, Fn&& fn) const
        -> std::invoke_result_t<Fn, const DeviceIndex*> {
        std::shared_lock lock(indexMutex_);

        const auto it = indexByDeviceId_.find(deviceId);
        if (it == indexByDeviceId_.end()) {
            return std::invoke_result_t<Fn, const DeviceIndex*>{};
        }

        return std::forward<Fn>(fn)(it->second.get());
    }

    /**
     * @brief Case-insensitive substring helper.
     */
    static bool contains(std::string_view haystack, std::string_view needle);

Q_SIGNALS:
    void deviceRemoved(quint64 deviceId);

private:
    void removeDeviceByDeviceIdUnlocked(quint64 deviceId);

    quint64 nextDeviceId_ = 1;

    mutable std::shared_mutex indexMutex_;

    // deviceId -> in-memory index
    std::unordered_map<quint64, std::unique_ptr<DeviceIndex>> indexByDeviceId_;

    // devicePath -> in-memory index
    std::unordered_map<QString, quint64> deviceIdByDevicePath_;

    // requestId -> deviceId
    // To go from requestId -> in-memory index, use:
    // requestId -> deviceId -> in-memory index
    std::unordered_map<quint64, quint64> deviceIdByRequestId_;
};

#endif //KERYTHING_INDEXCONTROLLER_H
