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
        quint64 deviceId;
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

signals:
    void deviceRemoved(quint64 deviceId);

private:
    quint64 nextDeviceId_ = 1;

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
