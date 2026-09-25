// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_INDEXSUMMARY_H
#define KERYTHING_INDEXSUMMARY_H

struct IndexSummary {
    quint64 indexId = 0;
    QString deviceId;
    QString displayName;
    QString label;
    QString devNode;
    QString fsType;
    QString primaryMountPoint;
    QStringList mountPoints;
    qint64 lastIndexedTime = 0;
    qsizetype recordCount = 0;
    qsizetype deletedRecordCount = 0;
    bool ready = false;
    bool mounted = false;
    bool searchable = false;
    bool showOfflineResults = true;
};

#endif // KERYTHING_INDEXSUMMARY_H