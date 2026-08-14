// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "Preferences.h"

#include <algorithm>

Preferences::Preferences()
    : settings_(QStringLiteral("Reikooters"), QStringLiteral("Kerything"))
{
    ensureDefaultSearchFilters();
}

bool Preferences::autoRefreshResultsForLiveUpdates() const
{
    return settings_.value(
        QStringLiteral("liveUpdates/autoRefreshResults"),
        true
    ).toBool();
}

void Preferences::setAutoRefreshResultsForLiveUpdates(bool enabled)
{
    settings_.setValue(QStringLiteral("liveUpdates/autoRefreshResults"), enabled);
    settings_.sync();
}

bool Preferences::hasAnyIndexedDevicePreferences() const
{
    return !deviceIds().isEmpty();
}

bool Preferences::isDeviceEnabled(const QString& deviceId) const
{
    if (deviceId.isEmpty()) {
        return false;
    }

    return loadDevicePreference(deviceId).enabled;
}

std::vector<IndexedDevicePreference> Preferences::indexedDevicePreferences() const
{
    const QStringList ids = deviceIds();

    std::vector<IndexedDevicePreference> out;
    out.reserve(static_cast<std::size_t>(ids.size()));

    for (const QString& id : ids) {
        out.push_back(loadDevicePreference(id));
    }

    return out;
}

std::optional<IndexedDevicePreference> Preferences::indexedDevicePreference(const QString& deviceId) const
{
    if (deviceId.isEmpty() || !deviceIds().contains(deviceId)) {
        return std::nullopt;
    }

    return loadDevicePreference(deviceId);
}

bool Preferences::initialDeviceSelectionCompleted() const
{
    return settings_.value(QStringLiteral("indexedDevices/initialSelectionCompleted"), false).toBool();
}

void Preferences::setInitialDeviceSelectionCompleted(bool completed)
{
    settings_.setValue(QStringLiteral("indexedDevices/initialSelectionCompleted"), completed);
    settings_.sync();
}

void Preferences::setDeviceEnabled(const BlockDevice& blockDevice, bool enabled)
{
    if (blockDevice.deviceId.isEmpty()) {
        return;
    }

    QStringList ids = deviceIds();
    if (!ids.contains(blockDevice.deviceId)) {
        ids << blockDevice.deviceId;
        ids.sort();
        setDeviceIds(ids);
    }

    IndexedDevicePreference preference = loadDevicePreference(blockDevice.deviceId);
    preference.deviceId = blockDevice.deviceId;
    preference.enabled = enabled;
    preference.displayName = displayNameForBlockDevice(blockDevice);
    preference.fsType = blockDevice.fsType;
    preference.uuid = blockDevice.uuid;
    preference.partuuid = blockDevice.partuuid;
    preference.lastKnownDevNode = blockDevice.devNode;
    preference.lastKnownPrimaryMountPoint = blockDevice.primaryMountPoint;
    preference.lastKnownMountPoints = blockDevice.mountPoints;
    preference.lastSeenAt = QDateTime::currentDateTimeUtc();

    if (!deviceSupportsUnmountedScanning(blockDevice)) {
        preference.scanWhenUnmounted = false;
    }

    saveDevicePreference(preference);
}

void Preferences::saveIndexedDevicePreference(const IndexedDevicePreference& preference)
{
    if (preference.deviceId.isEmpty()) {
        return;
    }

    QStringList ids = deviceIds();
    if (!ids.contains(preference.deviceId)) {
        ids << preference.deviceId;
        ids.sort();
        setDeviceIds(ids);
    }

    saveDevicePreference(preference);
}

void Preferences::updateKnownDevices(const std::vector<BlockDevice>& blockDevices)
{
    QStringList ids = deviceIds();

    for (const BlockDevice& blockDevice : blockDevices) {
        if (blockDevice.deviceId.isEmpty()) {
            continue;
        }

        if (!ids.contains(blockDevice.deviceId)) {
            ids << blockDevice.deviceId;
        }

        IndexedDevicePreference preference = loadDevicePreference(blockDevice.deviceId);
        preference.deviceId = blockDevice.deviceId;

        if (preference.displayName.isEmpty()) {
            preference.displayName = displayNameForBlockDevice(blockDevice);
        }

        preference.fsType = blockDevice.fsType;
        preference.uuid = blockDevice.uuid;
        preference.partuuid = blockDevice.partuuid;
        preference.lastKnownDevNode = blockDevice.devNode;
        preference.lastKnownPrimaryMountPoint = blockDevice.primaryMountPoint;
        preference.lastKnownMountPoints = blockDevice.mountPoints;
        preference.lastSeenAt = QDateTime::currentDateTimeUtc();

        if (!deviceSupportsUnmountedScanning(blockDevice)) {
            preference.scanWhenUnmounted = false;
        }

        saveDevicePreference(preference);
    }

    ids.removeDuplicates();
    ids.sort();
    setDeviceIds(ids);
}

void Preferences::markDeviceIndexed(const QString& deviceId)
{
    if (deviceId.isEmpty() || !deviceIds().contains(deviceId)) {
        return;
    }

    IndexedDevicePreference preference = loadDevicePreference(deviceId);
    preference.lastIndexedAt = QDateTime::currentDateTimeUtc();
    saveDevicePreference(preference);
}

bool Preferences::fsTypeSupportsUnmountedScanning(const QString& fsType)
{
    const QString normalized = fsType.trimmed().toLower();

    return normalized == QStringLiteral("ext4") ||
           normalized == QStringLiteral("ntfs") ||
           normalized == QStringLiteral("ntfs3");
}

bool Preferences::deviceSupportsUnmountedScanning(const BlockDevice& blockDevice)
{
    return fsTypeSupportsUnmountedScanning(blockDevice.fsType);
}

bool Preferences::preferenceSupportsUnmountedScanning(const IndexedDevicePreference& preference)
{
    return fsTypeSupportsUnmountedScanning(preference.fsType);
}

bool Preferences::deviceSupportsLiveUpdates(const BlockDevice& blockDevice)
{
    /*
     * This answers whether the user may enable the live-update preference.
     * Actual watching still requires the device to be mounted and the daemon's
     * fanotify setup to succeed.
     */
    return !blockDevice.fsType.trimmed().isEmpty();
}

bool Preferences::preferenceSupportsLiveUpdates(const IndexedDevicePreference& preference)
{
    return !preference.fsType.trimmed().isEmpty();
}

QString Preferences::displayNameForBlockDevice(const BlockDevice& blockDevice)
{
    if (!blockDevice.label.trimmed().isEmpty()) {
        return blockDevice.label.trimmed();
    }

    if (!blockDevice.primaryMountPoint.trimmed().isEmpty()) {
        if (blockDevice.primaryMountPoint == QStringLiteral("/")) {
            return QStringLiteral("Root filesystem");
        }

        const QStringList parts = blockDevice.primaryMountPoint.split(
            QStringLiteral("/"),
            Qt::SkipEmptyParts
        );

        if (!parts.isEmpty()) {
            return parts.last();
        }

        return blockDevice.primaryMountPoint;
    }

    if (!blockDevice.fsType.trimmed().isEmpty()) {
        return blockDevice.fsType.toUpper() + QStringLiteral(" volume");
    }

    if (!blockDevice.devNode.trimmed().isEmpty()) {
        return blockDevice.devNode;
    }

    return QStringLiteral("Unknown volume");
}

QString Preferences::devicePreferenceKey(const QString& deviceId, const QString& key)
{
    return QStringLiteral("indexedDevices/devices/%1/%2").arg(deviceId, key);
}

QStringList Preferences::deviceIds() const
{
    return settings_.value(QStringLiteral("indexedDevices/deviceIds")).toStringList();
}

void Preferences::setDeviceIds(const QStringList& ids)
{
    settings_.setValue(QStringLiteral("indexedDevices/deviceIds"), ids);
    settings_.sync();
}

IndexedDevicePreference Preferences::loadDevicePreference(const QString& deviceId) const
{
    IndexedDevicePreference preference;
    preference.deviceId = deviceId;

    preference.enabled = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("enabled")), false).toBool();
    preference.displayName = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("displayName"))).toString();
    preference.fsType = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("fsType"))).toString();
    preference.uuid = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("uuid"))).toString();
    preference.partuuid = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("partuuid"))).toString();
    preference.lastKnownDevNode = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("lastKnownDevNode"))).toString();
    preference.lastKnownPrimaryMountPoint = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("lastKnownPrimaryMountPoint"))).toString();
    preference.lastKnownMountPoints = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("lastKnownMountPoints"))).toStringList();
    preference.scanWhenUnmounted = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("scanWhenUnmounted")), true).toBool();
    preference.showOfflineResults = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("showOfflineResults")), true).toBool();
    preference.liveUpdatesEnabled = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("liveUpdatesEnabled")), true).toBool();
    preference.lastSeenAt = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("lastSeenAt"))).toDateTime();
    preference.lastIndexedAt = settings_.value(devicePreferenceKey(deviceId, QStringLiteral("lastIndexedAt"))).toDateTime();

    return preference;
}

void Preferences::saveDevicePreference(const IndexedDevicePreference& preference)
{
    if (preference.deviceId.isEmpty()) {
        return;
    }

    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("enabled")), preference.enabled);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("displayName")), preference.displayName);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("fsType")), preference.fsType);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("uuid")), preference.uuid);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("partuuid")), preference.partuuid);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("lastKnownDevNode")), preference.lastKnownDevNode);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("lastKnownPrimaryMountPoint")), preference.lastKnownPrimaryMountPoint);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("lastKnownMountPoints")), preference.lastKnownMountPoints);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("scanWhenUnmounted")), preference.scanWhenUnmounted);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("showOfflineResults")), preference.showOfflineResults);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("liveUpdatesEnabled")), preference.liveUpdatesEnabled);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("lastSeenAt")), preference.lastSeenAt);
    settings_.setValue(devicePreferenceKey(preference.deviceId, QStringLiteral("lastIndexedAt")), preference.lastIndexedAt);

    settings_.sync();
}

std::vector<SearchFilterPreference> Preferences::defaultSearchFilters()
{
    return {
        SearchFilterPreference{
            .id = QStringLiteral("audio"),
            .name = QStringLiteral("Audio"),
            .query = QStringLiteral("ext:aac;aif;aifc;aiff;au;flac;m3u;m3u8;m4a;m4b;mid;midi;mka;mp2;mp3;mpa;pls;ogg;opus;ra;rmi;voc;wav;wma;xspf"),
        },
        SearchFilterPreference{
            .id = QStringLiteral("images"),
            .name = QStringLiteral("Images"),
            .query = QStringLiteral("ext:ani;apng;avif;bmp;cur;gif;heic;heif;ico;jpe;jpeg;jpg;jxl;pcx;png;psd;svg;tga;tif;tiff;webp;wmf;xcf"),
        },
        SearchFilterPreference{
            .id = QStringLiteral("videos"),
            .name = QStringLiteral("Videos"),
            .query = QStringLiteral("ext:3g2;3gp;asf;avi;divx;f4v;flv;m2t;m2ts;m2v;m4v;mkv;mov;mp2v;mp4;mpe;mpeg;mpg;mpv;mts;ogm;ogv;qt;rm;rmvb;ts;vob;webm;wmv"),
        },
        SearchFilterPreference{
            .id = QStringLiteral("documents"),
            .name = QStringLiteral("Documents"),
            .query = QStringLiteral("ext:chm;csv;djvu;doc;docm;docx;dot;dotm;dotx;epub;fb2;htm;html;log;md;mht;mhtml;odg;odp;ods;odt;ott;pages;pdf;pot;potm;potx;pps;ppsm;ppsx;ppt;pptm;pptx;rst;rtf;tex;txt;wpd;wps;xls;xlsb;xlsm;xlsx;xlt;xltm;xltx;xml"),
        },
        SearchFilterPreference{
            .id = QStringLiteral("archives"),
            .name = QStringLiteral("Archives"),
            .query = QStringLiteral("ext:7z;appimage;bz2;cab;deb;gz;iso;jar;lz;lz4;lzma;pkg;rar;rpm;squashfs;tar;tbz2;tgz;tlz;txz;war;xar;xz;zip;zst"),
        },
        SearchFilterPreference{
            .id = QStringLiteral("code"),
            .name = QStringLiteral("Code"),
            .query = QStringLiteral("ext:asm;bash;c;cc;cpp;cs;css;cxx;dart;el;fish;go;gradle;groovy;h;hh;hpp;hxx;ini;ipynb;java;js;jsx;kt;kts;lua;m;mm;make;mk;php;pl;pm;py;rb;rs;scala;scss;sh;sql;svelte;swift;ts;tsx;vim;vue;xml;yaml;yml;zig"),
        },
    };
}

QString Preferences::searchFilterKey(const QString& filterId, const QString& key)
{
    return QStringLiteral("searchFilters/filters/%1/%2").arg(filterId, key);
}

void Preferences::ensureDefaultSearchFilters()
{
    const QStringList ids = settings_.value(QStringLiteral("searchFilters/filterIds")).toStringList();

    if (!ids.isEmpty()) {
        return;
    }

    saveSearchFilters(defaultSearchFilters());
}

std::vector<SearchFilterPreference> Preferences::searchFilters() const
{
    const QStringList ids = settings_.value(QStringLiteral("searchFilters/filterIds")).toStringList();

    std::vector<SearchFilterPreference> out;
    out.reserve(static_cast<std::size_t>(ids.size()));

    for (const QString& id : ids) {
        if (id.trimmed().isEmpty()) {
            continue;
        }

        SearchFilterPreference filter;
        filter.id = id;
        filter.name = settings_.value(searchFilterKey(id, QStringLiteral("name"))).toString();
        filter.query = settings_.value(searchFilterKey(id, QStringLiteral("query"))).toString();

        if (!filter.name.trimmed().isEmpty() && !filter.query.trimmed().isEmpty()) {
            out.push_back(std::move(filter));
        }
    }

    return out;
}

void Preferences::saveSearchFilters(const std::vector<SearchFilterPreference>& filters)
{
    const QStringList oldIds = settings_.value(QStringLiteral("searchFilters/filterIds")).toStringList();

    for (const QString& oldId : oldIds) {
        settings_.remove(QStringLiteral("searchFilters/filters/%1").arg(oldId));
    }

    QStringList ids;

    for (const SearchFilterPreference& filter : filters) {
        const QString id = filter.id.trimmed();
        const QString name = filter.name.trimmed();
        const QString query = filter.query.trimmed();

        if (id.isEmpty() || name.isEmpty() || query.isEmpty()) {
            continue;
        }

        if (ids.contains(id)) {
            continue;
        }

        ids << id;
        settings_.setValue(searchFilterKey(id, QStringLiteral("name")), name);
        settings_.setValue(searchFilterKey(id, QStringLiteral("query")), query);
    }

    settings_.setValue(QStringLiteral("searchFilters/filterIds"), ids);
    settings_.sync();
}

void Preferences::restoreDefaultSearchFilters()
{
    std::vector<SearchFilterPreference> filters = searchFilters();
    const std::vector<SearchFilterPreference> defaults = defaultSearchFilters();

    for (const SearchFilterPreference& defaultFilter : defaults) {
        auto existing = std::ranges::find_if(
            filters,
            [&](const SearchFilterPreference& filter) {
                return filter.id == defaultFilter.id;
            }
        );

        if (existing == filters.end()) {
            filters.push_back(defaultFilter);
        } else {
            *existing = defaultFilter;
        }
    }

    saveSearchFilters(filters);
}