// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexController.h"

#include <algorithm>
#include <iostream>
#include <shared_mutex>
#include <mutex>
#include <cctype>

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
            deviceIndex.mounted = !mountPoints.isEmpty();
            deviceIndex.isReady = false;
            deviceIndex.fileRecords.clear();
            deviceIndex.stringPool.clear();
            deviceIndex.deletedRecordBitmap.clear();
            deviceIndex.lowercaseStringPool.clear();
            deviceIndex.flatIndex.clear();
            deviceIndex.directoryFsIndexToRecordIdx.clear();
            deviceIndex.fsIndexToRecordIndices.clear();
            deviceIndex.generation++;
            deviceIndex.lastIndexedTime = 0;

#ifdef KERYTHING_ENABLE_LOGGING
            std::cout << "IndexController: Device already exists, so it was reset for devNode="
                      << devNode.toStdString()
                      << " indexId=" << existingIndexId
                      << "\n";
#endif
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
    deviceIndex->mounted = !mountPoints.isEmpty();

    indexByIndexId_.emplace(indexId, std::move(deviceIndex));
    indexIdByDevNode_[devNode] = indexId;
    indexIdByRequestId_[requestId] = indexId;

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Added device " << devNode.toStdString()
              << " deviceId=" << deviceId.toStdString()
              << " devNode=" << devNode.toStdString()
              << " fsType=" << fsType.toStdString()
              << " indexId=" << indexId
              << " requestId=" << requestId << "\n";
#endif

    return indexId;
}

void IndexController::removeDeviceByIndexId(quint64 indexId) {
    bool removed = false;

    {
        std::unique_lock lock(indexMutex_);
        removed = removeDeviceByIndexIdUnlocked(indexId);
    }

    if (removed) {
        Q_EMIT deviceRemoved(indexId);
    }
}

bool IndexController::removeDeviceByDeviceId(const QString& deviceId)
{
    if (deviceId.isEmpty()) {
        return false;
    }

    quint64 removedIndexId = 0;
    bool removed = false;

    {
        std::unique_lock lock(indexMutex_);

        for (const auto& [indexId, deviceIndex] : indexByIndexId_) {
            if (deviceIndex && deviceIndex->deviceId == deviceId) {
                removedIndexId = indexId;
                removed = removeDeviceByIndexIdUnlocked(indexId);
                break;
            }
        }
    }

    if (removed) {
        Q_EMIT deviceRemoved(removedIndexId);
    }

    return removed;
}

bool IndexController::removeDeviceByIndexIdUnlocked(quint64 indexId) {
    // Look up the owning entry in the indexId -> DeviceIndex map.
    // We use find() instead of operator[] so we don't accidentally create
    // a new empty entry if the indexId does not exist.
    const auto deviceIt = indexByIndexId_.find(indexId);
    if (deviceIt == indexByIndexId_.end()) {
        std::cerr << "IndexController: removeDeviceByIndexId: No device for indexId=" << indexId << "\n";
        return false;
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

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Removed device"
              << " devNode=" << devicePath.toStdString()
              << " indexId=" << indexId << "\n";
#endif

    return true;
}

bool IndexController::removeDeviceByRequestId(quint32 requestId) {
    quint64 removedIndexId = 0;
    bool removed = false;

    {
        std::unique_lock lock(indexMutex_);

        // Resolve the request to the device it belongs to.
        const auto requestIt = indexIdByRequestId_.find(requestId);
        if (requestIt == indexIdByRequestId_.end()) {
            return false;
        }

        removedIndexId = requestIt->second;

        // Remove the request mapping first so we don't leave a stale in-flight request.
        indexIdByRequestId_.erase(requestIt);

        // Remove the associated device and all of its reverse mappings.
        removed = removeDeviceByIndexIdUnlocked(removedIndexId);
    }

    if (removed) {
        Q_EMIT deviceRemoved(removedIndexId);
    }

    return removed;
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

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Appending " << records.size()
              << " file records to device " << deviceIndex.devNode.toStdString() << "\n";
#endif

    // Get the count of how many file records were the index before appending
    const std::size_t fileRecordsCountBefore = deviceIndex.fileRecords.size();

    // Reserve space for the new records
    deviceIndex.fileRecords.reserve(deviceIndex.fileRecords.size() + records.size());

    // Insert the new records into the device index.
    // Parent pointers are resolved once after the full scan has completed.
    deviceIndex.fileRecords.insert(deviceIndex.fileRecords.end(), records.begin(), records.end());
    deviceIndex.deletedRecordBitmap.resize(deviceIndex.fileRecords.size(), 0);

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: The index now contains " << deviceIndex.fileRecords.size()
              << " file records for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif
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

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: Appending " << stringPool.size()
              << " string pool characters to device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif

    // Reserve space for the new records
    deviceIndex.stringPool.reserve(deviceIndex.stringPool.size() + static_cast<size_t>(stringPool.size()));

    // Insert the new string pool data into the device index
    deviceIndex.stringPool.insert(deviceIndex.stringPool.end(), stringPool.begin(), stringPool.end());

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: The index now contains " << deviceIndex.stringPool.size()
              << " string pool characters for device devNode=" << deviceIndex.devNode.toStdString() << "\n";
#endif
}

bool IndexController::removeRequestId(quint32 requestId) {
    std::unique_lock lock(indexMutex_);

    return indexIdByRequestId_.erase(requestId) > 0;
}

bool IndexController::updateDeviceRuntimeStateByDeviceId(
    const QString& deviceId,
    bool mounted,
    bool showOfflineResults,
    const QStringList& mountPoints,
    const QString& primaryMountPoint
) {
    if (deviceId.isEmpty()) {
        return false;
    }

    std::unique_lock lock(indexMutex_);

    for (const auto& [indexId, deviceIndex] : indexByIndexId_) {
        if (!deviceIndex || deviceIndex->deviceId != deviceId) {
            continue;
        }

        deviceIndex->mounted = mounted;
        deviceIndex->showOfflineResults = showOfflineResults;

        if (!mountPoints.isEmpty() || !primaryMountPoint.isEmpty() || !mounted) {
            deviceIndex->mountPoints = mountPoints;
            deviceIndex->primaryMountPoint = primaryMountPoint;
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "IndexController: Updated runtime state"
                  << " deviceId=" << deviceId.toStdString()
                  << " mounted=" << (mounted ? "true" : "false")
                  << " showOfflineResults=" << (showOfflineResults ? "true" : "false")
                  << " searchable=" << (deviceIndex->isSearchable() ? "true" : "false")
                  << "\n";
#endif

        return true;
    }

    return false;
}

IndexController::LiveUpdateApplyResult IndexController::applyLiveUpdateOperations(
    const QString& deviceId,
    const std::vector<LiveUpdateOperation>& operations)
{
    LiveUpdateApplyResult result;

    if (deviceId.isEmpty() || operations.empty()) {
        return result;
    }

    std::unique_lock lock(indexMutex_);

    DeviceIndex* targetIndex = nullptr;

    for (const auto& [indexId, deviceIndex] : indexByIndexId_) {
        if (deviceIndex && deviceIndex->deviceId == deviceId) {
            targetIndex = deviceIndex.get();
            break;
        }
    }

    if (!targetIndex || !targetIndex->isReady) {
        result.missingDevice = static_cast<qsizetype>(operations.size());
        return result;
    }

    std::vector<LiveUpdateOperation> pendingUpserts;
    pendingUpserts.reserve(operations.size());

    for (const LiveUpdateOperation& operation : operations) {
        if (operation.kind == LiveUpdateOperationKind::Upsert) {
            pendingUpserts.push_back(operation);
        }
    }

    std::vector<uint8_t> consumedUpserts(pendingUpserts.size(), 0);

    for (const LiveUpdateOperation& operation : operations) {
        if (operation.kind == LiveUpdateOperationKind::Upsert) {
            continue;
        }

        if (operation.kind == LiveUpdateOperationKind::MetadataChanged) {
            if (operation.inode == 0) {
                ++result.missingInode;
                continue;
            }

            const std::vector<uint32_t>* recordIndices =
                targetIndex->recordIndicesForFsIndex(operation.inode);

            if (!recordIndices || recordIndices->empty()) {
                ++result.missingInode;
                continue;
            }

            bool updatedAny = false;

            for (const uint32_t recordIdx : *recordIndices) {
                if (recordIdx >= targetIndex->fileRecords.size() ||
                    targetIndex->isDeletedRecord(recordIdx)) {
                    continue;
                }

                FileRecord& record = targetIndex->fileRecords[recordIdx];
                updateFileRecordMetadataFromLiveUpdateOperation(record, operation);
                updatedAny = true;
            }

            if (updatedAny) {
                ++result.metadataChanged;
            }
            else {
                ++result.missingInode;
            }

            continue;
        }

        if (operation.kind == LiveUpdateOperationKind::DeleteEntry) {
            if (operation.parentInode == 0 || operation.name.isEmpty()) {
                ++result.missingEntry;
                continue;
            }

            qsizetype deletedCount = 0;
            const QByteArray nameUtf8 = operation.name.toUtf8();
            const std::string_view operationName(
                nameUtf8.constData(),
                static_cast<std::size_t>(nameUtf8.size())
            );

            bool movedAny = false;

            for (uint32_t recordIdx = 0; recordIdx < targetIndex->fileRecords.size(); ++recordIdx) {
                if (targetIndex->isDeletedRecord(recordIdx)) {
                    continue;
                }

                const FileRecord& record = targetIndex->fileRecords[recordIdx];

                if (record.parentFsIndex != operation.parentInode) {
                    continue;
                }

                if (targetIndex->recordName(recordIdx) != operationName) {
                    continue;
                }

                std::size_t matchingUpsertIdx = pendingUpserts.size();

                for (std::size_t upsertIdx = 0; upsertIdx < pendingUpserts.size(); ++upsertIdx) {
                    if (consumedUpserts[upsertIdx] != 0) {
                        continue;
                    }

                    const LiveUpdateOperation& pendingUpsert = pendingUpserts[upsertIdx];

                    if (pendingUpsert.inode == record.fsIndex) {
                        matchingUpsertIdx = upsertIdx;
                        break;
                    }
                }

                if (matchingUpsertIdx != pendingUpserts.size()) {
                    const LiveUpdateOperation& pendingUpsert = pendingUpserts[matchingUpsertIdx];

                    if (updateRecordIdentityFromLiveUpdateOperation(
                            *targetIndex,
                            recordIdx,
                            pendingUpsert
                        )) {
                        consumedUpserts[matchingUpsertIdx] = 1;
                        movedAny = true;
                        ++result.upserted;
                        continue;
                    }
                }

                deletedCount += targetIndex->markDeletedRecordTree(recordIdx);
            }

            if (deletedCount > 0) {
                result.deleted += deletedCount;

                // Rebuild inode maps so deleted records no longer participate in
                // metadata updates, parent lookup, or future live-update matching.
                targetIndex->rebuildFsIndexMaps();
            }
            else if (!movedAny) {
                ++result.missingEntry;
            }

            continue;
        }

        if (operation.kind == LiveUpdateOperationKind::NeedsRescan) {
            ++result.needsRescan;
            continue;
        }

        ++result.unsupported;
    }

    while (!pendingUpserts.empty()) {
        bool madeProgress = false;
        std::vector<LiveUpdateOperation> stillPending;
        stillPending.reserve(pendingUpserts.size());

        for (std::size_t upsertIdx = 0; upsertIdx < pendingUpserts.size(); ++upsertIdx) {
            if (consumedUpserts[upsertIdx] != 0) {
                continue;
            }

            const LiveUpdateOperation& operation = pendingUpserts[upsertIdx];

            const UpsertApplyResult upsertResult =
                applyUpsertOperation(*targetIndex, operation);

            switch (upsertResult) {
                case UpsertApplyResult::Applied:
                    ++result.upserted;
                    madeProgress = true;
                    break;

                case UpsertApplyResult::MissingParent:
                    stillPending.push_back(operation);
                    break;

                case UpsertApplyResult::Invalid:
                    ++result.unsupported;
                    break;

                case UpsertApplyResult::NotUpsert:
                    ++result.unsupported;
                    break;
            }
        }

        if (!madeProgress) {
            result.missingParent += static_cast<qsizetype>(stillPending.size());
            break;
        }

        pendingUpserts = std::move(stillPending);
        consumedUpserts.assign(pendingUpserts.size(), 0);
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "IndexController: applied live update operations"
              << " deviceId=" << deviceId.toStdString()
              << " metadataChanged=" << result.metadataChanged
              << " upserted=" << result.upserted
              << " deleted=" << result.deleted
              << " needsRescan=" << result.needsRescan
              << " unsupported=" << result.unsupported
              << " missingDevice=" << result.missingDevice
              << " missingInode=" << result.missingInode
              << " missingParent=" << result.missingParent
              << " missingEntry=" << result.missingEntry
              << "\n";
#endif

    return result;
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

std::string IndexController::normalizeExtensionToken(std::string_view extension)
{
    while (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }

    std::string out;
    out.reserve(extension.size());

    for (const unsigned char c : extension) {
        if (std::isspace(c) || c == ';') {
            continue;
        }

        out.push_back(static_cast<char>(std::tolower(c)));
    }

    return out;
}

std::string_view IndexController::finalExtension(std::string_view lowercaseFileName)
{
    if (lowercaseFileName.empty()) {
        return {};
    }

    const std::size_t dotPos = lowercaseFileName.rfind('.');
    if (dotPos == std::string_view::npos) {
        return {};
    }

    // Treat ".bashrc" as extensionless.
    if (dotPos == 0) {
        return {};
    }

    // Treat "file." as extensionless.
    if (dotPos + 1 >= lowercaseFileName.size()) {
        return {};
    }

    return lowercaseFileName.substr(dotPos + 1);
}

bool IndexController::matchesExtensionFilter(
    std::string_view lowercaseFileName,
    const std::unordered_set<std::string>& extensions
) {
    if (extensions.empty()) {
        return true;
    }

    const std::string_view extension = finalExtension(lowercaseFileName);
    if (extension.empty()) {
        return false;
    }

    return extensions.contains(std::string(extension));
}

bool IndexController::matchesQueryRecordType(const FileRecord& record, const ParsedSearchQuery& query)
{
    if (query.foldersOnly && (record.flags & FileRecord_IsDir) == 0) {
        return false;
    }

    return true;
}

quint8 IndexController::fileRecordFlagsFromLiveUpdateOperation(
    const LiveUpdateOperation& operation)
{
    quint8 flags = 0;

    if (operation.isDirectory) {
        flags |= FileRecord_IsDir;
    }

    if (operation.isSymlink) {
        flags |= FileRecord_IsSymlink;
    }

    return flags;
}

void IndexController::updateFileRecordMetadataFromLiveUpdateOperation(
    FileRecord& record,
    const LiveUpdateOperation& operation)
{
    record.size = operation.size;
    record.modificationTime = operation.modificationTime;
    record.flags = fileRecordFlagsFromLiveUpdateOperation(operation);
}

void IndexController::appendTrigramsForRecord(DeviceIndex& deviceIndex, uint32_t recordIdx)
{
    if (recordIdx >= deviceIndex.fileRecords.size()) {
        return;
    }

    const FileRecord& record = deviceIndex.fileRecords[recordIdx];

    if (record.nameOffset + record.nameLen > deviceIndex.lowercaseStringPool.size()) {
        return;
    }

    if (record.nameLen < 3) {
        return;
    }

    const std::string_view name(
        &deviceIndex.lowercaseStringPool[record.nameOffset],
        record.nameLen
    );

    std::unordered_set<uint32_t> uniqueTrigrams;
    uniqueTrigrams.reserve(record.nameLen);

    for (std::size_t i = 0; i <= name.size() - 3; ++i) {
        const uint32_t trigram =
            (static_cast<uint32_t>(static_cast<unsigned char>(name[i])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(name[i + 1])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(name[i + 2]));

        uniqueTrigrams.insert(trigram);
    }

    deviceIndex.flatIndex.reserve(deviceIndex.flatIndex.size() + uniqueTrigrams.size());

    for (const uint32_t trigram : uniqueTrigrams) {
        deviceIndex.flatIndex.push_back({
            trigram,
            recordIdx
        });
    }

    std::sort(std::execution::par, deviceIndex.flatIndex.begin(), deviceIndex.flatIndex.end());
}

bool IndexController::appendRecordFromLiveUpdateOperation(
    DeviceIndex& deviceIndex,
    const LiveUpdateOperation& operation)
{
    if (operation.inode == 0 || operation.name.isEmpty()) {
        return false;
    }

    if (operation.parentInode == 0) {
        return false;
    }

    const QByteArray nameUtf8 = operation.name.toUtf8();
    if (nameUtf8.isEmpty()) {
        return false;
    }

    const uint32_t recordIdx = static_cast<uint32_t>(deviceIndex.fileRecords.size());
    const uint32_t nameOffset = static_cast<uint32_t>(deviceIndex.stringPool.size());
    const uint32_t nameLen = static_cast<uint32_t>(nameUtf8.size());

    FileRecord record{};
    record.fsIndex = operation.inode;
    record.parentFsIndex = operation.parentInode;
    record.parentRecordIdx = 0xFFFFFFFF;
    record.size = operation.size;
    record.modificationTime = operation.modificationTime;
    record.nameOffset = nameOffset;
    record.nameLen = nameLen;
    record.flags = fileRecordFlagsFromLiveUpdateOperation(operation);

    if (operation.inode != operation.parentInode) {
        const auto parentIt = deviceIndex.directoryFsIndexToRecordIdx.find(operation.parentInode);
        if (parentIt != deviceIndex.directoryFsIndexToRecordIdx.end()) {
            record.parentRecordIdx = parentIt->second;
        }
    }

    deviceIndex.stringPool.insert(
        deviceIndex.stringPool.end(),
        nameUtf8.begin(),
        nameUtf8.end()
    );

    deviceIndex.lowercaseStringPool.reserve(
        deviceIndex.lowercaseStringPool.size() + static_cast<std::size_t>(nameUtf8.size())
    );

    for (const unsigned char c : nameUtf8) {
        deviceIndex.lowercaseStringPool.push_back(
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 32) : static_cast<char>(c)
        );
    }

    deviceIndex.fileRecords.push_back(record);
    deviceIndex.deletedRecordBitmap.push_back(0);

    deviceIndex.fsIndexToRecordIndices[record.fsIndex].push_back(recordIdx);

    if ((record.flags & FileRecord_IsDir) != 0) {
        deviceIndex.directoryFsIndexToRecordIdx[record.fsIndex] = recordIdx;
    }

    appendTrigramsForRecord(deviceIndex, recordIdx);

    return true;
}

bool IndexController::updateRecordIdentityFromLiveUpdateOperation(
    DeviceIndex& deviceIndex,
    uint32_t recordIdx,
    const LiveUpdateOperation& operation)
{
    if (recordIdx >= deviceIndex.fileRecords.size()) {
        return false;
    }

    if (operation.inode == 0 || operation.parentInode == 0 || operation.name.isEmpty()) {
        return false;
    }

    FileRecord& record = deviceIndex.fileRecords[recordIdx];

    if (record.fsIndex != operation.inode) {
        return false;
    }

    uint32_t parentRecordIdx = 0xFFFFFFFF;

    if (operation.parentInode != operation.inode) {
        const auto parentIt = deviceIndex.directoryFsIndexToRecordIdx.find(operation.parentInode);
        if (parentIt == deviceIndex.directoryFsIndexToRecordIdx.end()) {
            return false;
        }

        parentRecordIdx = parentIt->second;
    }

    const QByteArray nameUtf8 = operation.name.toUtf8();
    if (nameUtf8.isEmpty()) {
        return false;
    }

    const uint32_t nameOffset = static_cast<uint32_t>(deviceIndex.stringPool.size());
    const uint32_t nameLen = static_cast<uint32_t>(nameUtf8.size());

    deviceIndex.stringPool.insert(
        deviceIndex.stringPool.end(),
        nameUtf8.begin(),
        nameUtf8.end()
    );

    deviceIndex.lowercaseStringPool.reserve(
        deviceIndex.lowercaseStringPool.size() + static_cast<std::size_t>(nameUtf8.size())
    );

    for (const unsigned char c : nameUtf8) {
        deviceIndex.lowercaseStringPool.push_back(
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 32) : static_cast<char>(c)
        );
    }

    record.parentFsIndex = operation.parentInode;
    record.parentRecordIdx = parentRecordIdx;
    record.nameOffset = nameOffset;
    record.nameLen = nameLen;

    updateFileRecordMetadataFromLiveUpdateOperation(record, operation);
    appendTrigramsForRecord(deviceIndex, recordIdx);

    return true;
}

IndexController::UpsertApplyResult IndexController::applyUpsertOperation(
    DeviceIndex& deviceIndex,
    const LiveUpdateOperation& operation)
{
    if (operation.kind != LiveUpdateOperationKind::Upsert) {
        return UpsertApplyResult::NotUpsert;
    }

    if (operation.inode == 0 || operation.parentInode == 0 || operation.name.isEmpty()) {
        return UpsertApplyResult::Invalid;
    }

    const std::vector<uint32_t>* recordIndices =
        deviceIndex.recordIndicesForFsIndex(operation.inode);

    if (recordIndices && !recordIndices->empty()) {
        bool updatedAny = false;

        for (const uint32_t recordIdx : *recordIndices) {
            if (recordIdx >= deviceIndex.fileRecords.size() ||
                deviceIndex.isDeletedRecord(recordIdx)) {
                continue;
            }

            FileRecord& record = deviceIndex.fileRecords[recordIdx];
            updateFileRecordMetadataFromLiveUpdateOperation(record, operation);
            updatedAny = true;
        }

        if (updatedAny) {
            return UpsertApplyResult::Applied;
        }
    }

    /*
     * New non-root entries need their parent directory to exist in the index
     * before we append them, otherwise parentRecordIdx cannot be resolved.
     */
    if (operation.parentInode != operation.inode &&
        !deviceIndex.directoryFsIndexToRecordIdx.contains(operation.parentInode)) {
        return UpsertApplyResult::MissingParent;
    }

    if (!appendRecordFromLiveUpdateOperation(deviceIndex, operation)) {
        return UpsertApplyResult::Invalid;
    }

    return UpsertApplyResult::Applied;
}

IndexController::ParsedSearchQuery IndexController::parseSearchQuery(std::string_view query)
{
    ParsedSearchQuery parsed;

    std::string lowercaseQuery(query);
    std::transform(
        lowercaseQuery.begin(),
        lowercaseQuery.end(),
        lowercaseQuery.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    auto consumeExtensionList = [&parsed](std::string_view extensionList) {
        std::size_t start = 0;

        while (start <= extensionList.size()) {
            const std::size_t end = extensionList.find(';', start);
            const std::string_view token = end == std::string_view::npos
                ? extensionList.substr(start)
                : extensionList.substr(start, end - start);

            std::string normalized = normalizeExtensionToken(token);
            if (!normalized.empty()) {
                parsed.extensions.insert(std::move(normalized));
            }

            if (end == std::string_view::npos) {
                break;
            }

            start = end + 1;
        }
    };

    std::size_t start = 0;
    while (start < lowercaseQuery.size()) {
        while (start < lowercaseQuery.size() &&
               std::isspace(static_cast<unsigned char>(lowercaseQuery[start]))) {
            ++start;
        }

        if (start >= lowercaseQuery.size()) {
            break;
        }

        std::size_t end = start;
        while (end < lowercaseQuery.size() &&
               !std::isspace(static_cast<unsigned char>(lowercaseQuery[end]))) {
            ++end;
        }

        const std::string_view token(lowercaseQuery.data() + start, end - start);

        if (token.starts_with("ext:")) {
            consumeExtensionList(token.substr(4));
        } else if (token.starts_with("extension:")) {
            consumeExtensionList(token.substr(10));
        } else if (token == "folder:" || token == "folders:" || token == "type:folder" || token == "type:folders") {
            parsed.foldersOnly = true;
        } else {
            parsed.keywords.emplace_back(token);
        }

        start = end;
    }

    return parsed;
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

    const ParsedSearchQuery parsedQuery = parseSearchQuery(query);
    const std::vector<std::string>& keywords = parsedQuery.keywords;
    const std::unordered_set<std::string>& extensionFilter = parsedQuery.extensions;

    // IF EMPTY: Return everything matching non-trigram filters.
    if (keywords.empty()) {
        std::size_t totalSize = 0;

        for (const auto& [indexId, indexPtr] : indexByIndexId_) {
            if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
                continue;
            }

            const std::size_t mountMultiplier = indexPtr->mountPoints.isEmpty()
                ? 1
                : static_cast<std::size_t>(indexPtr->mountPoints.size());

            totalSize += indexPtr->fileRecords.size() * mountMultiplier;
        }

        results.reserve(totalSize);

        for (const auto& [indexId, indexPtr] : indexByIndexId_) {
            if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
                continue;
            }

            const auto& index = *indexPtr;
            for (uint32_t i = 0; i < index.fileRecords.size(); ++i) {
                if (index.isDeletedRecord(i)) {
                    continue;
                }

                const FileRecord& rec = index.fileRecords[i];

                if (rec.nameOffset + rec.nameLen > index.lowercaseStringPool.size()) {
                    continue;
                }

                const std::string_view name(
                    &index.lowercaseStringPool[rec.nameOffset],
                    rec.nameLen
                );

                if (!matchesQueryRecordType(rec, parsedQuery)) {
                    continue;
                }

                if (!matchesExtensionFilter(name, extensionFilter)) {
                    continue;
                }

                appendResult(index, i);
            }
        }

        return results;
    }

    for (const auto& [indexId, indexPtr] : indexByIndexId_) {
        if (!indexPtr || !indexPtr->isReady || !indexPtr->isSearchable()) {
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
            if (indexPtr->isDeletedRecord(recordIdx)) {
                return;
            }

            const auto& rec = indexPtr->fileRecords[recordIdx];

            if (rec.nameOffset + rec.nameLen > indexPtr->lowercaseStringPool.size()) {
                return;
            }

            std::string_view name(&indexPtr->lowercaseStringPool[rec.nameOffset], rec.nameLen);

            if (!matchesQueryRecordType(rec, parsedQuery)) {
                return;
            }

            if (!matchesExtensionFilter(name, extensionFilter)) {
                return;
            }

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
                    || handle.recordIdx >= device->fileRecords.size()
                    || device->isDeletedRecord(handle.recordIdx)) {
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
                    || handle.recordIdx >= device->fileRecords.size()
                    || device->isDeletedRecord(handle.recordIdx)) {
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
                    || handle.recordIdx >= device->fileRecords.size()
                    || device->isDeletedRecord(handle.recordIdx)) {
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