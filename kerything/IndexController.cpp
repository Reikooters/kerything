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

const IndexController::DeviceIndex* IndexController::deviceIndex(quint64 indexId) const {
    std::shared_lock lock(indexMutex_);

    const auto it = indexByIndexId_.find(indexId);
    if (it == indexByIndexId_.end()) {
        return nullptr;
    }

    return it->second.get();
}

quint64 IndexController::addDevice(
    const QString& deviceId,
    const QString& devNode,
    const QString& fsType,
    const QString& label,
    const QStringList& mountPoints,
    const QString& primaryMountPoint,
    quint32 requestId
) {
    std::unique_lock lock(indexMutex_);

    // Check whether devNode is valid
    if (devNode.isEmpty()) {
        std::cerr << "IndexController: Empty devNode provided for requestId=" << requestId << "\n";
        return 0;
    }

    // Check whether a DeviceIndex with the given devNode already exists.
    // If so, update the existing DeviceIndex with the new information and clear
    // the file records and string pool.
    const auto existingDevNodeIt = indexIdByDevNode_.find(devNode);
    if (existingDevNodeIt != indexIdByDevNode_.end()) {
        const quint64 existingIndexId = existingDevNodeIt->second;

        const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
        if (existingDeviceIndexIt != indexByIndexId_.end()) {
            indexIdByRequestId_[requestId] = existingIndexId;

            DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
            deviceIndex.fsType = fsType;
            deviceIndex.label = label;
            deviceIndex.deviceId = deviceId;
            deviceIndex.devNode = devNode;
            deviceIndex.mountPoints = mountPoints;
            deviceIndex.primaryMountPoint = primaryMountPoint;
            deviceIndex.fileRecords.clear();
            deviceIndex.stringPool.clear();
            deviceIndex.directoryFsIndexToRecordIdx.clear();
            deviceIndex.fsIndexToRecordIndices.clear();
            deviceIndex.generation++;
            deviceIndex.lastIndexedTime = 0;

            std::cout << "IndexController: Device already exists, so it was reset for devNode="
                      << devNode.toStdString()
                      << " indexId=" << existingIndexId
                      << "\n";
            return 0;
        }

        // Stale path mapping: remove it and fall through to create a new device
        indexIdByDevNode_.erase(existingDevNodeIt);
    }

    quint64 indexId = nextIndexId_++;

    std::unique_ptr<DeviceIndex> deviceIndex = std::make_unique<DeviceIndex>();
    deviceIndex->indexId = indexId;
    deviceIndex->fsType = fsType;
    deviceIndex->label = label;
    deviceIndex->deviceId = deviceId;
    deviceIndex->devNode = devNode;
    deviceIndex->mountPoints = mountPoints;
    deviceIndex->primaryMountPoint = primaryMountPoint;

    indexByIndexId_.emplace(indexId, std::move(deviceIndex));
    indexIdByDevNode_[devNode] = indexId;
    indexIdByRequestId_[requestId] = indexId;

    std::cout << "IndexController: Added device " << devNode.toStdString()
              << " deviceId=" << deviceId.toStdString()
              << " devNode=" << devNode.toStdString()
              << " fsType=" << fsType.toStdString()
              << " indexId=" << indexId
              << " requestId=" << requestId << "\n";

    return indexId;
}

void IndexController::removeDeviceByIndexId(quint64 indexId) {
    std::unique_lock lock(indexMutex_);
    removeDeviceByIndexIdUnlocked(indexId);
}

void IndexController::removeDeviceByIndexIdUnlocked(quint64 indexId) {
    // Look up the owning entry in the indexId -> DeviceIndex map.
    // We use find() instead of operator[] so we don't accidentally create
    // a new empty entry if the indexId does not exist.
    const auto deviceIt = indexByIndexId_.find(indexId);
    if (deviceIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: removeDeviceByIndexId: No device for indexId=" << indexId << "\n";
        return;
    }

    // Capture the device path before removing the DeviceIndex object.
    // We need this to clean up the reverse lookup map as well.
    const QString devicePath = deviceIt->second->devNode;

    // Remove the devicePath -> indexId mapping, but only if it still points
    // to the same device we are removing.
    const auto pathIt = indexIdByDevNode_.find(devicePath);
    if (pathIt != indexIdByDevNode_.end() && pathIt->second == indexId) {
        indexIdByDevNode_.erase(pathIt);
    }

    // Remove any requestId -> indexId entries that refer to this device.
    // This keeps the request lookup table from holding stale references after
    // a cancellation or failed scan.
    for (auto it = indexIdByRequestId_.begin(); it != indexIdByRequestId_.end(); ) {
        if (it->second == indexId) {
            it = indexIdByRequestId_.erase(it);
        } else {
            ++it;
        }
    }

    // Finally remove the owned DeviceIndex itself.
    indexByIndexId_.erase(deviceIt);

    std::cout << "IndexController: Removed device"
              << " devNode=" << devicePath.toStdString()
              << " indexId=" << indexId << "\n";

    Q_EMIT deviceRemoved(indexId);
}

bool IndexController::removeDeviceByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    // Resolve the request to the device it belongs to.
    const auto requestIt = indexIdByRequestId_.find(requestId);
    if (requestIt == indexIdByRequestId_.end()) {
        return false;
    }

    const quint64 indexId = requestIt->second;

    // Remove the request mapping first so we don't leave a stale in-flight request.
    indexIdByRequestId_.erase(requestIt);

    // Remove the associated device and all of its reverse mappings.
    removeDeviceByIndexIdUnlocked(indexId);

    return true;
}

void IndexController::appendDeviceFileRecordsByRequestId(const quint32 requestId, const std::vector<FileRecord> &records) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

    std::cout << "IndexController: Appending " << records.size()
              << " file records to device " << deviceIndex.devNode.toStdString() << "\n";

    // Get the count of how many file records were the index before appending
    const std::size_t fileRecordsCountBefore = deviceIndex.fileRecords.size();

    // Reserve space for the new records
    deviceIndex.fileRecords.reserve(deviceIndex.fileRecords.size() + records.size());

    // Insert the new records into the device index.
    // Parent pointers are resolved once after the full scan has completed.
    deviceIndex.fileRecords.insert(deviceIndex.fileRecords.end(), records.begin(), records.end());

    std::cout << "IndexController: The index now contains " << deviceIndex.fileRecords.size()
              << " file records for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
}

void IndexController::appendDeviceStringPoolByRequestId(const quint32 requestId, QByteArrayView stringPool) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;

    std::cout << "IndexController: Appending " << stringPool.size()
              << " string pool characters to device devNode=" << deviceIndex.devNode.toStdString() << "\n";

    // Reserve space for the new records
    deviceIndex.stringPool.reserve(deviceIndex.stringPool.size() + static_cast<size_t>(stringPool.size()));

    // Insert the new string pool data into the device index
    deviceIndex.stringPool.insert(deviceIndex.stringPool.end(), stringPool.begin(), stringPool.end());

    std::cout << "IndexController: The index now contains " << deviceIndex.stringPool.size()
              << " string pool characters for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
}

bool IndexController::removeRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    return indexIdByRequestId_.erase(requestId) > 0;
}

void IndexController::setReadyState(quint32 requestId, bool isReady) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: setReadyState: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: setReadyState: No device index for indexId=" << existingIndexId
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

    auto appendResult = [&results](const DeviceIndex& index, uint32_t recordIdx) {
        if (index.mountPoints.isEmpty()) {
            results.emplace_back(index.indexId, index.generation, recordIdx, 0xFFFFFFFF);
            return;
        }

        for (int mountPointIdx = 0; mountPointIdx < index.mountPoints.size(); ++mountPointIdx) {
            results.emplace_back(
                index.indexId,
                index.generation,
                recordIdx,
                static_cast<uint32_t>(mountPointIdx)
            );
        }
    };

    if (indexByIndexId_.empty()) {
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

        for (const auto& [indexId, indexPtr] : indexByIndexId_) {
            if (!indexPtr || !indexPtr->isReady) {
                continue;
            }

            const std::size_t mountMultiplier = indexPtr->mountPoints.isEmpty()
                ? 1
                : static_cast<std::size_t>(indexPtr->mountPoints.size());

            totalSize += indexPtr->fileRecords.size() * mountMultiplier;
        }

        results.reserve(totalSize);

        for (const auto& [indexId, indexPtr] : indexByIndexId_) {
            if (!indexPtr || !indexPtr->isReady) {
                continue;
            }

            const auto& index = *indexPtr;
            for (uint32_t i = 0; i < index.fileRecords.size(); ++i) {
                appendResult(index, i);
            }
        }

        return results;
    }

    for (const auto& [indexId, indexPtr] : indexByIndexId_) {
        if (!indexPtr || !indexPtr->isReady) {
            continue;
        }

        // 2. Candidate Filtering via Trigrams
        std::vector<uint32_t> candidates;
        bool firstKeyword = true;
        bool trigramsUsed = false; // Track if we actually used the index
        bool skipDevice = false;

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
                    // No matches for this trigram on this device.
                    skipDevice = true;
                    break;
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
                    skipDevice = true;
                    break;
                }
            }

            if (skipDevice) {
                break;
            }
        }

        if (skipDevice) {
            continue;
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

            appendResult(*indexPtr, recordIdx);
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

std::vector<IndexController::RecordHandle> IndexController::sortSearchResults(std::vector<RecordHandle> results, int column, Qt::SortOrder sortOrder) const {
    if (results.size() < 2) {
        return results;
    }

    auto handleLess = [](const RecordHandle& a, const RecordHandle& b) {
        if (a.indexId != b.indexId) {
            return a.indexId < b.indexId;
        }
        if (a.generation != b.generation) {
            return a.generation < b.generation;
        }
        if (a.recordIdx != b.recordIdx) {
            return a.recordIdx < b.recordIdx;
        }
        return a.mountPointIdx < b.mountPointIdx;
    };

    std::vector<size_t> resultsOrder(results.size());
    std::iota(resultsOrder.begin(), resultsOrder.end(), size_t{0});

    if (column == 0) { // Name column
        struct NameKey {
            std::string_view nameKey;
            RecordHandle handle{};
            bool valid = false;
        };

        std::vector<NameKey> keys(results.size());

        {
            std::shared_lock lock(indexMutex_);

            for (size_t i = 0; i < results.size(); ++i) {
                const auto& handle = results[i];
                auto& key = keys[i];
                key.handle = handle;

                const auto it = indexByIndexId_.find(handle.indexId);
                if (it == indexByIndexId_.end()) {
                    continue;
                }

                const auto* device = it->second.get();
                if (!device || !device->isReady || device->generation != handle.generation
                    || handle.recordIdx >= device->fileRecords.size()) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                if (record.nameOffset + record.nameLen <= device->lowercaseStringPool.size()) {
                    key.nameKey = std::string_view(
                        &device->lowercaseStringPool[record.nameOffset],
                        record.nameLen
                    );
                    key.valid = true;
                }
            }
        }

        auto lessByIndex = [&](size_t lhs, size_t rhs) {
            const auto& a = keys[lhs];
            const auto& b = keys[rhs];

            if (!a.valid || !b.valid) {
                return handleLess(a.handle, b.handle);
            }

            if (a.nameKey != b.nameKey) {
                return a.nameKey < b.nameKey;
            }

            return handleLess(a.handle, b.handle);
        };

        if (sortOrder == Qt::AscendingOrder) {
            std::sort(std::execution::par, resultsOrder.begin(), resultsOrder.end(), lessByIndex);
        } else {
            std::sort(std::execution::par, resultsOrder.begin(), resultsOrder.end(),
                      [&](size_t lhs, size_t rhs) {
                          return lessByIndex(rhs, lhs);
                      });
        }
    } else if (column == 1) { // Path column
        struct PathKey {
            std::string pathKey;
            RecordHandle handle{};
            bool valid = false;
        };

        std::vector<PathKey> keys(results.size());

        {
            std::shared_lock lock(indexMutex_);

            for (size_t i = 0; i < results.size(); ++i) {
                const auto& handle = results[i];
                auto& key = keys[i];
                key.handle = handle;

                const auto it = indexByIndexId_.find(handle.indexId);
                if (it == indexByIndexId_.end()) {
                    continue;
                }

                const auto* device = it->second.get();
                if (!device || !device->isReady || device->generation != handle.generation
                    || handle.recordIdx >= device->fileRecords.size()) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                key.pathKey = device->getFullPath(record.parentRecordIdx);

                if (handle.mountPointIdx != 0xFFFFFFFF &&
                    handle.mountPointIdx < static_cast<uint32_t>(device->mountPoints.size())) {
                    const QString mountPoint = device->mountPoints.at(static_cast<int>(handle.mountPointIdx));
                    key.pathKey = (mountPoint + QStringLiteral("/") + QString::fromStdString(key.pathKey))
                        .toStdString();
                }

                key.valid = true;
            }
        }

        auto lessByIndex = [&](size_t lhs, size_t rhs) {
            const auto& a = keys[lhs];
            const auto& b = keys[rhs];

            if (!a.valid || !b.valid) {
                return handleLess(a.handle, b.handle);
            }

            if (a.pathKey != b.pathKey) {
                return a.pathKey < b.pathKey;
            }

            return handleLess(a.handle, b.handle);
        };

        if (sortOrder == Qt::AscendingOrder) {
            std::sort(std::execution::par, resultsOrder.begin(), resultsOrder.end(), lessByIndex);
        } else {
            std::sort(std::execution::par, resultsOrder.begin(), resultsOrder.end(),
                      [&](size_t lhs, size_t rhs) {
                          return lessByIndex(rhs, lhs);
                      });
        }
    } else if (column == 2 || column == 3) { // Size or Modification time column
        std::vector<quint64> numericKeys(results.size());
        std::vector<RecordHandle> handles(results.size());

        {
            std::shared_lock lock(indexMutex_);

            for (size_t i = 0; i < results.size(); ++i) {
                const auto& handle = results[i];
                handles[i] = handle;

                const auto it = indexByIndexId_.find(handle.indexId);
                if (it == indexByIndexId_.end()) {
                    continue;
                }

                const auto* device = it->second.get();
                if (!device || !device->isReady || device->generation != handle.generation
                    || handle.recordIdx >= device->fileRecords.size()) {
                    continue;
                }

                const auto& record = device->fileRecords[handle.recordIdx];
                numericKeys[i] = (column == 2) ? record.size : record.modificationTime;
            }
        }

        auto lessByIndex = [&](size_t lhs, size_t rhs) {
            const auto aKey = numericKeys[lhs];
            const auto bKey = numericKeys[rhs];

            if (aKey != bKey) {
                return aKey < bKey;
            }

            return handleLess(handles[lhs], handles[rhs]);
        };

        // Sort using parallel execution policy to leverage multiple CPU cores via TBB
        if (sortOrder == Qt::AscendingOrder) {
            std::sort(std::execution::par, resultsOrder.begin(), resultsOrder.end(), lessByIndex);
        } else {
            std::sort(std::execution::par, resultsOrder.begin(), resultsOrder.end(),
                      [&](size_t lhs, size_t rhs) {
                          return lessByIndex(rhs, lhs);
                      });
        }
    } else {
        return results;
    }

    std::vector<RecordHandle> sorted;
    sorted.resize(results.size());

    for (size_t i = 0; i < resultsOrder.size(); ++i) {
        sorted[i] = std::move(results[resultsOrder[i]]);
    }

    return sorted;
}

void IndexController::resolveParentPointersByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: resolveParentPointersByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: resolveParentPointersByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.resolveParentPointers();
}

void IndexController::buildLowercaseStringPoolByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: buildLowercaseStringPoolByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: buildLowercaseStringPoolByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildLowercaseStringPool();
}

void IndexController::sortByNameAscendingParallelByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: sortByNameAscendingParallelByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: sortByNameAscendingParallelByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.sortByNameAscendingParallel();
}

void IndexController::buildTrigramIndexParallelByRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    const auto existingIndexIdIt = indexIdByRequestId_.find(requestId);
    if (existingIndexIdIt == indexIdByRequestId_.end()) {
        std::cerr << "IndexController: buildTrigramIndexParallelByRequestId: No device index for requestId=" << requestId << "\n";
        return;
    }

    const quint64 existingIndexId = existingIndexIdIt->second;

    const auto existingDeviceIndexIt = indexByIndexId_.find(existingIndexId);
    if (existingDeviceIndexIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: buildTrigramIndexParallelByRequestId: No device index for indexId=" << existingIndexId
                  << " requestId=" << requestId << "\n";
        return;
    }

    DeviceIndex& deviceIndex = *existingDeviceIndexIt->second;
    deviceIndex.buildTrigramIndexParallel();
}