// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_BLOCKDEVICE_H
#define KERYTHING_BLOCKDEVICE_H

#include <QString>
#include <QStringList>

#pragma pack(push, 1)

struct BlockDeviceMountInfo {
    QString mountPoint;
    QString fsType;
    QString root;
    QString subvolPath;
    quint64 btrfsRootId = 0;
};

struct BlockDevice {
    QString deviceId;
    QString devNode;
    QString fsType;
    QString mountedFsType;
    QString uuid;
    QString partuuid;
    QString label;
    QString diskModel;
    bool mounted = false;
    QStringList mountPoints;
    QString primaryMountPoint;
    std::vector<BlockDeviceMountInfo> mounts;
};

#pragma pack(pop)

#endif // KERYTHING_BLOCKDEVICE_H
