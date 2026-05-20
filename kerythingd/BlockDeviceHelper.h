// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_BLOCKDEVICEHELPER_H
#define KERYTHINGD_BLOCKDEVICEHELPER_H

#include <optional>
#include <vector>

#include "BlockDevice.h"

class BlockDeviceHelper {
public:
    static std::vector<BlockDevice> listKnownDevices();

    static std::optional<BlockDevice> findKnownDeviceById(const QString& deviceId);
};

#endif // KERYTHINGD_BLOCKDEVICEHELPER_H