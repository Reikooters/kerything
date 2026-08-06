// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_LIVEUPDATEMANAGER_H
#define KERYTHINGD_LIVEUPDATEMANAGER_H

#include <QHash>
#include <QObject>
#include <QString>

#include <vector>

#include "BlockDevice.h"
#include "FanotifyWatcher.h"
#include "LiveUpdateEvent.h"

struct LiveUpdateStatusSnapshot {
    QString deviceId;
    LiveUpdateStatus status = LiveUpdateStatus::NotWatching;
    QString reason;
};

class LiveUpdateManager final : public QObject {
    Q_OBJECT

public:
    explicit LiveUpdateManager(QObject* parent = nullptr);

    void setKnownDevices(const std::vector<BlockDevice>& devices);
    void stopAll();
    std::vector<LiveUpdateStatusSnapshot> currentStatusSnapshots() const;

Q_SIGNALS:
    void liveUpdateStatusChanged(QString deviceId, LiveUpdateStatus status, QString reason);
    void deviceNeedsRescan(QString deviceId, QString reason);
    void eventsReady(QString deviceId, QString mountPoint, std::vector<LiveUpdateEvent> events);

private:
    static QString watchKeyForDevice(const BlockDevice& device);
    static bool isLiveUpdateEligible(const BlockDevice& device);
    static QString maskToString(quint64 mask);

    void startWatcherForDevice(const BlockDevice& device);
    void removeWatcher(const QString& key);
    void logEventBatch(
        const QString& deviceId,
        const QString& mountPoint,
        const std::vector<LiveUpdateEvent>& events
    );

    QHash<QString, FanotifyWatcher*> watchersByKey_;
};

#endif // KERYTHINGD_LIVEUPDATEMANAGER_H