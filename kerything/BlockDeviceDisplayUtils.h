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
}

#endif // KERYTHING_BLOCKDEVICEDISPLAYUTILS_H