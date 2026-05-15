// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_BLOCKDEVICEHELPER_H
#define KERYTHINGD_BLOCKDEVICEHELPER_H

#include "BlockDevice.h"

#include <vector>

namespace BlockDeviceHelper {

    std::vector<BlockDevice> listKnownDevices();

} // namespace BlockDeviceHelper

#endif // KERYTHINGD_BLOCKDEVICEHELPER_H