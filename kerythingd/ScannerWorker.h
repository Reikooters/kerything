// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_SCANNERWORKER_H
#define KERYTHINGD_SCANNERWORKER_H

#include "ScannerHelper.h"
#include "ScanJob.h"
#include "FileRecord.h"

#include <QObject>
#include <memory>

class ScannerWorker final : public QObject {
    Q_OBJECT

public:
    explicit ScannerWorker(QObject* parent = nullptr);

public Q_SLOTS:
    void startScan(std::shared_ptr<ScanJob> job);

Q_SIGNALS:
    void scanStarted(
        quint32 requestId,
        const QString& deviceId,
        const QString& devNode,
        const QString& fsType,
        const QString& label,
        const QStringList& mountPoints,
        const QString& primaryMountPoint
    );
    void scanProgress(quint32 requestId, const Protocol::ScanProgress& progress);
    void scanFileRecordChunkReady(quint32 requestId, const std::vector<FileRecord>& fileRecordChunk);
    void scanStringPoolChunkReady(quint32 requestId, const std::vector<char>& stringPoolChunk);
    void scanCompleted(quint32 requestId, const QString& deviceId, const QString& devNode, const QString& fsType);
    void scanCancelled(quint32 requestId, const QString& deviceId);
    void scanError(quint32 requestId, const QString& errorText);

private:
    std::shared_ptr<ScanJob> currentJob_;
};

#endif // KERYTHINGD_SCANNERWORKER_H