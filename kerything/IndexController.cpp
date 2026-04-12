// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexController.h"

#include <iostream>

IndexController::IndexController(QObject* parent)
    : QObject(parent)
{
}

const IndexController::DeviceIndex* IndexController::deviceIndex(quint64 deviceId) const {
    const auto it = indexByDeviceId_.find(deviceId);
    if (it == indexByDeviceId_.end()) {
        return nullptr;
    }

    return it->second.get();
}

quint64 IndexController::addDevice(const QString &devicePath, const QString &fsType, const QString &label, quint32 requestId) {
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

    emit deviceRemoved(deviceId);
}

bool IndexController::removeDeviceByRequestId(quint32 requestId) {
    // Resolve the request to the device it belongs to.
    const auto requestIt = deviceIdByRequestId_.find(requestId);
    if (requestIt == deviceIdByRequestId_.end()) {
        return false;
    }

    const quint64 deviceId = requestIt->second;

    // Remove the request mapping first so we don't leave a stale in-flight request.
    deviceIdByRequestId_.erase(requestIt);

    // Remove the associated device and all of its reverse mappings.
    removeDeviceByDeviceId(deviceId);

    return true;
}

void IndexController::appendDeviceFileRecordsByRequestId(const quint32 requestId, const std::vector<FileRecord> &records) {
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
    return deviceIdByRequestId_.erase(requestId) > 0;
}
