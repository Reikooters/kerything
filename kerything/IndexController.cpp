// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexController.h"

#include <iostream>
#include <shared_mutex>
#include <mutex>

IndexController::IndexController(QObject* parent)
    : QObject(parent)
{
}

const IndexController::DeviceIndex* IndexController::deviceIndex(quint64 deviceId) const {
    std::shared_lock lock(indexMutex_);

    const auto it = indexByDeviceId_.find(deviceId);
    if (it == indexByDeviceId_.end()) {
        return nullptr;
    }

    return it->second.get();
}

quint64 IndexController::addDevice(const QString &devicePath, const QString &fsType, const QString &label, quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    // Check whether devicePath is valid
    if (devicePath.isEmpty()) {
        std::cerr << "IndexController: Empty device path provided for requestId=" << requestId << "\n";
        return 0;
    }

    // Check whether a DeviceIndex with the given devicePath already exists.
    // If so, update the existing DeviceIndex with the new information and clear
    // the file records and string pool.
    const auto existingPathIt = deviceIdByDevicePath_.find(devicePath);
    if (existingPathIt != deviceIdByDevicePath_.end()) {
        const quint64 existingDeviceId = existingPathIt->second;

        const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
        if (existingDeviceIndexIt != indexByDeviceId_.end()) {
            deviceIdByRequestId_[requestId] = existingDeviceId;

            DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
            deviceIndex.fsType = fsType;
            deviceIndex.label = label;
            deviceIndex.devicePath = devicePath;
            deviceIndex.fileRecords.clear();
            deviceIndex.stringPool.clear();
            deviceIndex.fsIndexToRecordIdx.clear();
            deviceIndex.generation++;
            deviceIndex.lastIndexedTime = 0;

            std::cout << "IndexController: Device already exists, so it was reset for path "
                      << devicePath.toStdString()
                      << " deviceId=" << existingDeviceId
                      << "\n";
            return 0;
        }

        // Stale path mapping: remove it and fall through to create a new device
        deviceIdByDevicePath_.erase(existingPathIt);
    }

    quint64 deviceId = nextDeviceId_++;

    std::unique_ptr<DeviceIndex> deviceIndex = std::make_unique<DeviceIndex>();
    deviceIndex->deviceId = deviceId;
    deviceIndex->fsType = fsType;
    deviceIndex->label = label;
    deviceIndex->devicePath = devicePath;

    indexByDeviceId_.emplace(deviceId, std::move(deviceIndex));
    deviceIdByDevicePath_[devicePath] = deviceId;
    deviceIdByRequestId_[requestId] = deviceId;

    std::cout << "IndexController: Added device " << devicePath.toStdString()
              << " fsType=" << fsType.toStdString()
              << " deviceId=" << deviceId
              << " requestId=" << requestId << "\n";

    return deviceId;
}

void IndexController::removeDeviceByDeviceId(quint64 deviceId) {
    std::unique_lock lock(indexMutex_);
    removeDeviceByDeviceIdUnlocked(deviceId);
}

void IndexController::removeDeviceByDeviceIdUnlocked(quint64 deviceId) {
    // Look up the owning entry in the deviceId -> DeviceIndex map.
    // We use find() instead of operator[] so we don't accidentally create
    // a new empty entry if the deviceId does not exist.
    const auto deviceIt = indexByDeviceId_.find(deviceId);
    if (deviceIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: removeDeviceByDeviceId: No device for deviceId=" << deviceId << "\n";
        return;
    }

    // Capture the device path before removing the DeviceIndex object.
    // We need this to clean up the reverse lookup map as well.
    const QString devicePath = deviceIt->second->devicePath;

    // Remove the devicePath -> deviceId mapping, but only if it still points
    // to the same device we are removing.
    const auto pathIt = deviceIdByDevicePath_.find(devicePath);
    if (pathIt != deviceIdByDevicePath_.end() && pathIt->second == deviceId) {
        deviceIdByDevicePath_.erase(pathIt);
    }

    // Remove any requestId -> deviceId entries that refer to this device.
    // This keeps the request lookup table from holding stale references after
    // a cancellation or failed scan.
    for (auto it = deviceIdByRequestId_.begin(); it != deviceIdByRequestId_.end(); ) {
        if (it->second == deviceId) {
            it = deviceIdByRequestId_.erase(it);
        } else {
            ++it;
        }
    }

    // Finally remove the owned DeviceIndex itself.
    indexByDeviceId_.erase(deviceIt);

    std::cout << "IndexController: Removed device "
              << devicePath.toStdString()
              << " deviceId=" << deviceId << "\n";

    Q_EMIT deviceRemoved(deviceId);
}

bool IndexController::removeDeviceByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    // Resolve the request to the device it belongs to.
    const auto requestIt = deviceIdByRequestId_.find(requestId);
    if (requestIt == deviceIdByRequestId_.end()) {
        return false;
    }

    const quint64 deviceId = requestIt->second;

    // Remove the request mapping first so we don't leave a stale in-flight request.
    deviceIdByRequestId_.erase(requestIt);

    // Remove the associated device and all of its reverse mappings.
    removeDeviceByDeviceIdUnlocked(deviceId);

    return true;
}

void IndexController::appendDeviceFileRecordsByRequestId(const quint32 requestId, const std::vector<FileRecord> &records) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

    std::cout << "IndexController: Appending " << records.size()
              << " file records to device " << deviceIndex.devicePath.toStdString() << "\n";

    // Get the count of how many file records were the index before appending
    const std::size_t fileRecordsCountBefore = deviceIndex.fileRecords.size();

    // Reserve space for the new records
    deviceIndex.fileRecords.reserve(deviceIndex.fileRecords.size() + records.size());

    // Insert the new records into the device index
    deviceIndex.fileRecords.insert(deviceIndex.fileRecords.end(), records.begin(), records.end());

    std::cout << "IndexController: The index now contains " << deviceIndex.fileRecords.size()
              << " file records for device " << deviceIndex.devicePath.toStdString() << "\n";

    // Update the fsIndex to record index mapping
    for (int i = 0; i < records.size(); ++i) {
        deviceIndex.fsIndexToRecordIdx[records[i].fsIndex] = fileRecordsCountBefore + i;
    }
}

void IndexController::appendDeviceStringPoolByRequestId(const quint32 requestId, QByteArrayView stringPool) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

    std::cout << "IndexController: Appending " << stringPool.size()
              << " string pool characters to device " << deviceIndex.devicePath.toStdString() << "\n";

    // Reserve space for the new records
    deviceIndex.stringPool.reserve(deviceIndex.stringPool.size() + static_cast<size_t>(stringPool.size()));

    // Insert the new string pool data into the device index
    deviceIndex.stringPool.insert(deviceIndex.stringPool.end(), stringPool.begin(), stringPool.end());

    std::cout << "IndexController: The index now contains " << deviceIndex.stringPool.size()
              << " string pool characters for device " << deviceIndex.devicePath.toStdString() << "\n";
}

bool IndexController::removeRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    return deviceIdByRequestId_.erase(requestId) > 0;
}

void IndexController::setReadyState(quint32 requestId, bool isReady) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: setReadyState: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: setReadyState: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.isReady = isReady;
}

bool IndexController::contains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }

    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return ch1 == ch2;
        }
    );

    return it != haystack.end();
}

std::vector<IndexController::RecordHandle> IndexController::performTrigramSearch(const std::string& query) {
    std::shared_lock lock(indexMutex_);

    // 1. Tokenize query: "valley dragonforce" -> ["valley", "dragonforce"]
    // 2. For each word >= 3 chars, get candidate IDs from trigramIndex
    // 3. Intersect the ID lists (Candidate Filtering)
    // 4. For remaining candidates, do a case-insensitive sub-string check (Refinement)

    std::vector<RecordHandle> results;

    if (indexByDeviceId_.empty()) {
        return results;
    }

    // Convert query to lowercase
    std::string lowercaseQuery = query;
    std::transform(lowercaseQuery.begin(), lowercaseQuery.end(), lowercaseQuery.begin(),
           [](unsigned char c) { return std::tolower(c); });

    // 1. Tokenize query by spaces
    std::vector<std::string> keywords;
    size_t start = 0, end = 0;

    while ((end = lowercaseQuery.find(' ', start)) != std::string::npos) {
        if (end != start) {
            keywords.push_back(lowercaseQuery.substr(start, end - start));
        }
        start = end + 1;
    }

    if (start < lowercaseQuery.length()) {
        keywords.push_back(lowercaseQuery.substr(start));
    }

    // IF EMPTY: Return everything
    if (keywords.empty()) {
        std::size_t totalSize = 0;

        for (const auto& [deviceId, indexPtr] : indexByDeviceId_) {
            if (!indexPtr || !indexPtr->isReady) {
                continue;
            }

            totalSize += indexPtr->fileRecords.size();
        }

        results.reserve(totalSize);

        for (const auto& [deviceId, indexPtr] : indexByDeviceId_) {
            if (!indexPtr || !indexPtr->isReady) {
                continue;
            }

            const auto& index = *indexPtr;
            for (uint32_t i = 0; i < index.fileRecords.size(); ++i) {
                results.emplace_back(index.deviceId, index.generation, i);
            }
        }

        return results;
    }

    for (const auto& [deviceId, indexPtr] : indexByDeviceId_) {
        if (!indexPtr || !indexPtr->isReady) {
            continue;
        }

        // 2. Candidate Filtering via Trigrams
        std::vector<uint32_t> candidates;
        bool firstKeyword = true;
        bool trigramsUsed = false; // Track if we actually used the index

        for (const auto& kw : keywords) {
            if (kw.length() < 3) {
                // Skip short words for now, as they won't be in our trigram index
                continue;
            }

            // Generate all trigrams for this keyword and intersect them
            for (size_t i = 0; i <= kw.length() - 3; ++i) {
                trigramsUsed = true;
                uint32_t tri = (static_cast<uint32_t>(std::tolower(kw[i])) << 16) |
                               (static_cast<uint32_t>(std::tolower(kw[i+1])) << 8) |
                               (static_cast<uint32_t>(std::tolower(kw[i+2])));

                // Binary search for the trigram in the flat index
                auto range = std::equal_range(indexPtr->flatIndex.begin(), indexPtr->flatIndex.end(),
                                              TrigramEntry{tri, 0},
                                              [](const auto& a, const auto& b) { return a.trigram < b.trigram; });

                if (range.first == range.second) {
                    // No matches for this trigram
                    return results;
                }

                // 3. Intersect candidates (Candidate Filtering)
                if (firstKeyword) {
                    // First trigram: populate candidates directly from the range
                    candidates.reserve(std::distance(range.first, range.second));

                    for (auto it = range.first; it != range.second; ++it) {
                        candidates.push_back(it->recordIdx);
                    }

                    firstKeyword = false;
                } else {
                    // Subsequent trigrams: intersect existing candidates with the range
                    std::vector<uint32_t> nextCandidates;
                    nextCandidates.reserve(std::min(candidates.size(), static_cast<size_t>(std::distance(range.first, range.second))));

                    // Custom intersection that works between a vector<uint32_t> and a range of TrigramEntry
                    auto candIt = candidates.begin();
                    auto rangeIt = range.first;

                    while (candIt != candidates.end() && rangeIt != range.second) {
                        if (*candIt < rangeIt->recordIdx) {
                            ++candIt;
                        } else if (rangeIt->recordIdx < *candIt) {
                            ++rangeIt;
                        } else {
                            nextCandidates.push_back(*candIt);
                            ++candIt;
                            ++rangeIt;
                        }
                    }

                    candidates = std::move(nextCandidates);
                }

                if (candidates.empty()) {
                    return results;
                }
            }
        }

        // 4. Refinement Phase
        // If no trigrams were used (all keywords < 3 chars), we scan everything.
        // Otherwise, we only scan the filtered candidates.
        auto resultCallback = [&](uint32_t recordIdx) {
            const auto& rec = indexPtr->fileRecords[recordIdx];
            std::string_view name(&indexPtr->lowercaseStringPool[rec.nameOffset], rec.nameLen);

            for (const auto& kw : keywords) {
                if (!contains(name, kw)) {
                    return;
                }
            }

            results.emplace_back(indexPtr->deviceId, indexPtr->generation, recordIdx);
        };

        if (!trigramsUsed) {
            // Fallback: Linear scan of all records (All keywords were too short)
            for (uint32_t i = 0; i < static_cast<uint32_t>(indexPtr->fileRecords.size()); ++i) {
                resultCallback(i);
            }
        } else {
            // High-speed scan of candidates
            for (uint32_t idx : candidates) {
                resultCallback(idx);
            }
        }
    }

    return results;
}

void IndexController::resolveParentPointersByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: resolveParentPointersByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: resolveParentPointersByRequestId: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.resolveParentPointers();
}

void IndexController::buildLowercaseStringPoolByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: buildLowercaseStringPoolByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: buildLowercaseStringPoolByRequestId: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildLowercaseStringPool();
}

void IndexController::sortByNameAscendingParallelByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: sortByNameAscendingParallelByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: sortByNameAscendingParallelByRequestId: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.sortByNameAscendingParallel();
}

void IndexController::buildTrigramIndexParallelByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingDeviceIdIt = deviceIdByRequestId_.find(requestId);
    if (existingDeviceIdIt == deviceIdByRequestId_.end()) {
        std::cerr << "IndexController: buildTrigramIndexParallelByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingDeviceId = existingDeviceIdIt->second;

    const auto existingDeviceIndexIt = indexByDeviceId_.find(existingDeviceId);
    if (existingDeviceIndexIt == indexByDeviceId_.end()) {
        std::cerr << "IndexController: buildTrigramIndexParallelByRequestId: No device index for deviceId=" << existingDeviceId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildTrigramIndexParallel();
}