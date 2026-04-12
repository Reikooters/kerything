// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_INDEXCONTROLLER_H
#define KERYTHING_INDEXCONTROLLER_H

#include <QObject>

#include "FileRecord.h"

class IndexController final : public QObject {
    Q_OBJECT

public:
    explicit IndexController(QObject* parent = nullptr);

    struct DeviceIndex {
        QString fsType;
        quint64 generation = 0;

        // unix seconds; 0 means unknown
        qint64 lastIndexedTime = 0;

        // display metadata
        QString label;
        QString devicePath; // e.g. /dev/disk/by-partuuid/<partuuid> or for loopback devices: /dev/disk/by-uuid/<uuid>

        std::vector<FileRecord> fileRecords;
        std::vector<char> stringPool;
        std::unordered_map<uint64_t, uint32_t> fsIndexToRecordIdx;
    };

    void addDevice(const QString& devicePath, const QString& fsType, const QString& label, quint32 requestId);
    void appendDeviceFileRecords(quint32 requestId, const std::vector<FileRecord>& records);
    void appendDeviceStringPool(quint32 requestId, QByteArrayView stringPool);

    // TODO:
    //void removeDevice(const QString& devicePath);
    // void updateDevice(const QString& devicePath, const QString& fsType, const QString& label);
    // void updateDeviceGeneration(const QString& devicePath, quint64 generation);
    // void updateDeviceLastIndexedTime(const QString& devicePath, qint64 lastIndexedTime);
    // void updateDeviceRecords(const QString& devicePath, const std::vector<FileRecord>& records);
    // void updateDeviceStringPool(const QString& devicePath, const std::vector<char>& stringPool);
    // void updateDeviceFsIndexToRecordIdx(const QString& devicePath, const std::unordered_map<uint64_t, uint32_t>& fsIndexToRecordIdx);
    // void clear();

private:
    // devicePath -> in-memory index
    std::unordered_map<QString, DeviceIndex> indexByDevicePath_;

    // requestId -> devicePath
    std::unordered_map<quint64, QString> requestIdToDevicePath_;
};

#endif //KERYTHING_INDEXCONTROLLER_H
