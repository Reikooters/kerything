// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_BLOCKDEVICEDISPLAYUTILS_H
#define KERYTHING_BLOCKDEVICEDISPLAYUTILS_H

#include <QString>

#include "BlockDevice.h"

namespace BlockDeviceDisplayUtils {
    [[nodiscard]] QString displayNameForBlockDevice(const BlockDevice& blockDevice);
    [[nodiscard]] QString displayOrDash(const QString& value);
    [[nodiscard]] bool deviceLessThan(const BlockDevice& lhs, const BlockDevice& rhs);

    [[nodiscard]] bool isBtrfsDevice(const BlockDevice& blockDevice);
    [[nodiscard]] qsizetype btrfsMountedSubvolumeCount(const BlockDevice& blockDevice);
    [[nodiscard]] QString mountPointSummaryForBlockDevice(const BlockDevice& blockDevice);
    [[nodiscard]] QString mountPointToolTipForBlockDevice(const BlockDevice& blockDevice);
    [[nodiscard]] QString filesystemDisplayTextForBlockDevice(const BlockDevice& blockDevice);
    [[nodiscard]] QString filesystemToolTipForBlockDevice(const BlockDevice& blockDevice);
    [[nodiscard]] QString btrfsMountedSubvolumesTableHtml(const BlockDevice& blockDevice);
    [[nodiscard]] QString selectedDeviceDetailsHtml(const BlockDevice& blockDevice);
}

#endif // KERYTHING_BLOCKDEVICEDISPLAYUTILS_H