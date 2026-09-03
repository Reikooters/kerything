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
    void operationsReady(QString deviceId, QString mountPoint, std::vector<LiveUpdateOperation> operations);

private:
    struct WatchTarget {
        QString key;
        QString deviceId;
        QString mountPoint;
        QString fsType;
        quint64 fsNamespace = 0;
    };

    static QString watchKeyForDevice(const BlockDevice& device);
    static QString watchKeyForTarget(const WatchTarget& target);
    static bool isLiveUpdateEligible(const BlockDevice& device);
    static QString maskToString(quint64 mask);

    static std::vector<WatchTarget> watchTargetsForDevice(const BlockDevice& device);

    void startWatcherForDevice(const BlockDevice& device);
    void startWatcherForTarget(const WatchTarget& target);
    void removeWatcher(const QString& key);
    void logEventBatch(
        const QString& deviceId,
        const QString& mountPoint,
        const QString& fsType,
        const std::vector<LiveUpdateEvent>& events
    );

    QHash<QString, FanotifyWatcher*> watchersByKey_;
};

#endif // KERYTHINGD_LIVEUPDATEMANAGER_H