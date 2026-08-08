// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PREFERENCES_H
#define KERYTHING_PREFERENCES_H

#include <QDateTime>
#include <QHash>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <vector>

#include "BlockDevice.h"

struct IndexedDevicePreference {
    QString deviceId;
    bool enabled = false;

    QString displayName;
    QString fsType;
    QString uuid;
    QString partuuid;
    QString lastKnownDevNode;
    QString lastKnownPrimaryMountPoint;
    QStringList lastKnownMountPoints;

    bool scanWhenUnmounted = true;
    bool showOfflineResults = true;
    bool liveUpdatesEnabled = true;

    QDateTime lastSeenAt;
    QDateTime lastIndexedAt;
};

struct SearchFilterPreference {
    QString id;
    QString name;
    QString query;
};

class Preferences final {
public:
    Preferences();

    // Devices
    [[nodiscard]] bool hasAnyIndexedDevicePreferences() const;
    [[nodiscard]] bool isDeviceEnabled(const QString& deviceId) const;
    [[nodiscard]] std::vector<IndexedDevicePreference> indexedDevicePreferences() const;
    [[nodiscard]] std::optional<IndexedDevicePreference> indexedDevicePreference(const QString& deviceId) const;
    [[nodiscard]] bool initialDeviceSelectionCompleted() const;
    void setInitialDeviceSelectionCompleted(bool completed);

    void setDeviceEnabled(const BlockDevice& blockDevice, bool enabled);
    void saveIndexedDevicePreference(const IndexedDevicePreference& preference);
    void updateKnownDevices(const std::vector<BlockDevice>& blockDevices);
    void markDeviceIndexed(const QString& deviceId);

    // Filters
    [[nodiscard]] std::vector<SearchFilterPreference> searchFilters() const;
    void saveSearchFilters(const std::vector<SearchFilterPreference>& filters);
    void restoreDefaultSearchFilters();
    static std::vector<SearchFilterPreference> defaultSearchFilters();

private:
    // Devices
    static QString displayNameForBlockDevice(const BlockDevice& blockDevice);
    static QString devicePreferenceKey(const QString& deviceId, const QString& key);

    [[nodiscard]] QStringList deviceIds() const;
    void setDeviceIds(const QStringList& ids);

    [[nodiscard]] IndexedDevicePreference loadDevicePreference(const QString& deviceId) const;
    void saveDevicePreference(const IndexedDevicePreference& preference);

    // Filters
    static QString searchFilterKey(const QString& filterId, const QString& key);

    void ensureDefaultSearchFilters();

    QSettings settings_;
};

#endif // KERYTHING_PREFERENCES_H