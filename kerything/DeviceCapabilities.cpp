// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DeviceCapabilities.h"

#include "Preferences.h"

bool DeviceCapabilities::fsTypeSupportsUnmountedScanning(const QString& fsType)
{
    const QString normalized = fsType.trimmed().toLower();

    return normalized == QStringLiteral("ext4") ||
           normalized == QStringLiteral("ntfs") ||
           normalized == QStringLiteral("ntfs3");
}

bool DeviceCapabilities::deviceSupportsUnmountedScanning(const BlockDevice& blockDevice)
{
    return fsTypeSupportsUnmountedScanning(blockDevice.fsType);
}

bool DeviceCapabilities::preferenceSupportsUnmountedScanning(const IndexedDevicePreference& preference)
{
    return fsTypeSupportsUnmountedScanning(preference.fsType);
}

bool DeviceCapabilities::deviceSupportsLiveUpdates(const BlockDevice& blockDevice)
{
    /*
     * This answers whether the user may enable the live-update preference.
     * Actual watching still requires the device to be mounted and the daemon's
     * fanotify setup to succeed.
     */
    return !blockDevice.fsType.trimmed().isEmpty();
}

bool DeviceCapabilities::preferenceSupportsLiveUpdates(const IndexedDevicePreference& preference)
{
    return !preference.fsType.trimmed().isEmpty();
}