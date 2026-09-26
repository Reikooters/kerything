// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DEVICECAPABILITIES_H
#define KERYTHING_DEVICECAPABILITIES_H

#include <QString>

#include "BlockDevice.h"

struct IndexedDevicePreference;

namespace DeviceCapabilities {
    [[nodiscard]] bool fsTypeSupportsUnmountedScanning(const QString& fsType);
    [[nodiscard]] bool deviceSupportsUnmountedScanning(const BlockDevice& blockDevice);
    [[nodiscard]] bool preferenceSupportsUnmountedScanning(const IndexedDevicePreference& preference);
    [[nodiscard]] bool deviceSupportsLiveUpdates(const BlockDevice& blockDevice);
    [[nodiscard]] bool preferenceSupportsLiveUpdates(const IndexedDevicePreference& preference);
}

#endif // KERYTHING_DEVICECAPABILITIES_H