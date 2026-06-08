// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_SCANJOB_H
#define KERYTHINGD_SCANJOB_H

#include <atomic>
#include <memory>
#include <QString>
#include <QThread>

class ClientConnection;

struct ScanJob {
    quint32 requestId = 0;
    QString deviceId;
    QString devNode;
    QString fsType;
    QStringList mountPoints;
    QString primaryMountPoint;
    std::atomic_bool cancelled{false};
};

#endif // KERYTHINGD_SCANJOB_H