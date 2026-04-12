// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexController.h"

#include <iostream>

IndexController::IndexController(QObject* parent)
    : QObject(parent)
{
}

void IndexController::addDevice(const QString &devicePath, const QString &fsType, const QString &label, quint32 requestId) {
    // TODO: Clear the DeviceIndex if devicePath already exists and maybe increment generation

    indexByDevicePath_[devicePath] = DeviceIndex {
        .fsType = fsType,
        .label = label,
        .devicePath = devicePath,
    };

    requestIdToDevicePath_[requestId] = devicePath;

    std::cout << "IndexController: Added device " << devicePath.toStdString()
              << " requestId=" << requestId << "\n";
}

void IndexController::appendDeviceFileRecords(const quint32 requestId, const std::vector<FileRecord> &records) {
    const auto requestIdIt = requestIdToDevicePath_.find(requestId);
    if (requestIdIt == requestIdToDevicePath_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    auto devicePath = requestIdIt->second;

    const auto deviceIndexIt = indexByDevicePath_.find(devicePath);
    if (deviceIndexIt == indexByDevicePath_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for devicePath=" << devicePath.toStdString()
                  << " (requestId=" << requestId << ")\n";
        return;
    }

    DeviceIndex& deviceIndex = deviceIndexIt->second;

    std::cout << "IndexController: Appending " << records.size() << " file records to device " << devicePath.toStdString() << "\n";

    // Get the count of how many file records were the index before appending
    const std::size_t fileRecordsCountBefore = deviceIndex.fileRecords.size();

    // Reserve space for the new records
    deviceIndex.fileRecords.reserve(deviceIndex.fileRecords.size() + records.size());

    // Insert the new records into the device index
    deviceIndex.fileRecords.insert(deviceIndex.fileRecords.end(), records.begin(), records.end());

    std::cout << "IndexController: The index now contains " << deviceIndex.fileRecords.size() << " file records for device " << devicePath.toStdString() << "\n";

    // Update the fsIndex to record index mapping
    for (int i = 0; i < records.size(); ++i) {
        deviceIndex.fsIndexToRecordIdx[records[i].fsIndex] = fileRecordsCountBefore + i;
    }
}

void IndexController::appendDeviceStringPool(const quint32 requestId, QByteArrayView stringPool) {
    const auto requestIdIt = requestIdToDevicePath_.find(requestId);
    if (requestIdIt == requestIdToDevicePath_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for requestId=" << requestId << "\n";
        return;
    }

    auto devicePath = requestIdIt->second;

    const auto deviceIndexIt = indexByDevicePath_.find(devicePath);
    if (deviceIndexIt == indexByDevicePath_.end()) {
        std::cerr << "IndexController: appendDeviceFileRecords: No device index for devicePath=" << devicePath.toStdString()
                  << " (requestId=" << requestId << ")\n";
        return;
    }

    DeviceIndex& deviceIndex = deviceIndexIt->second;

    std::cout << "IndexController: Appending " << stringPool.size() << " string pool characters to device " << devicePath.toStdString() << "\n";

    // Reserve space for the new records
    deviceIndex.stringPool.reserve(deviceIndex.stringPool.size() + static_cast<size_t>(stringPool.size()));

    // Insert the new string pool data into the device index
    deviceIndex.stringPool.insert(deviceIndex.stringPool.end(), stringPool.begin(), stringPool.end());

    std::cout << "IndexController: The index now contains " << deviceIndex.stringPool.size() << " string pool characters for device " << devicePath.toStdString() << "\n";
}
