// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DEVICEPREFERENCECHANGE_H
#define KERYTHING_DEVICEPREFERENCECHANGE_H

#include <QString>

struct DevicePreferenceChange {
    QString deviceId;

    bool wasEnabled = false;
    bool enabled = false;

    bool wasScanWhenUnmounted = true;
    bool scanWhenUnmounted = true;

    bool wasShowOfflineResults = true;
    bool showOfflineResults = true;

    bool wasLiveUpdatesEnabled = true;
    bool liveUpdatesEnabled = true;
    bool refreshIndexBeforeEnablingLiveUpdates = false;

    bool wasSaveIndexToDisk = true;
    bool saveIndexToDisk = true;

    bool wasRefreshOnStartup = true;
    bool refreshOnStartup = true;

    [[nodiscard]] bool becameEnabled() const noexcept
    {
        return enabled && !wasEnabled;
    }

    [[nodiscard]] bool becameDisabled() const noexcept
    {
        return !enabled && wasEnabled;
    }

    [[nodiscard]] bool scanWhenUnmountedChanged() const noexcept
    {
        return scanWhenUnmounted != wasScanWhenUnmounted;
    }

    [[nodiscard]] bool showOfflineResultsChanged() const noexcept
    {
        return showOfflineResults != wasShowOfflineResults;
    }

    [[nodiscard]] bool liveUpdatesEnabledChanged() const noexcept
    {
        return liveUpdatesEnabled != wasLiveUpdatesEnabled;
    }

    [[nodiscard]] bool saveIndexToDiskChanged() const noexcept
    {
        return saveIndexToDisk != wasSaveIndexToDisk;
    }

    [[nodiscard]] bool refreshOnStartupChanged() const noexcept
    {
        return refreshOnStartup != wasRefreshOnStartup;
    }
};

#endif // KERYTHING_DEVICEPREFERENCECHANGE_H