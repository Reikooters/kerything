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

class LiveUpdateManager final : public QObject {
    Q_OBJECT

public:
    explicit LiveUpdateManager(QObject* parent = nullptr);

    void setKnownDevices(const std::vector<BlockDevice>& devices);
    void stopAll();

    Q_SIGNALS:
        void deviceNeedsRescan(QString deviceId, QString reason);

private:
    static QString watchKeyForDevice(const BlockDevice& device);
    static bool isLiveUpdateEligible(const BlockDevice& device);

    void startWatcherForDevice(const BlockDevice& device);
    void removeWatcher(const QString& key);

    QHash<QString, FanotifyWatcher*> watchersByKey_;
};

#endif // KERYTHINGD_LIVEUPDATEMANAGER_H