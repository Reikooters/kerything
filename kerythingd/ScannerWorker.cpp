// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ScannerWorker.h"

#include "FileRecord.h"

ScannerWorker::ScannerWorker(QObject* parent)
    : QObject(parent)
{
}

void ScannerWorker::startScan(std::shared_ptr<ScanJob> job)
{
    if (!job) {
        return;
    }

    currentJob_ = std::move(job);

    const quint32 requestId = currentJob_->requestId;
    const std::shared_ptr<ScanJob> jobRef = currentJob_;

    Q_EMIT scanStarted(requestId, currentJob_->devicePath, currentJob_->fsType);

    const bool ok = ScannerHelper::scanDevice(
        jobRef->devicePath,
        jobRef->fsType,
        [this, requestId, jobRef](const std::vector<FileRecord>& fileRecordChunk) -> bool {
            Q_EMIT scanFileRecordChunkReady(requestId, fileRecordChunk);
            return !jobRef->cancelled.load(std::memory_order_relaxed);
        },
        [this, requestId, jobRef](const std::vector<char>& stringPoolChunk) -> bool {
            Q_EMIT scanStringPoolChunkReady(requestId, stringPoolChunk);
            return !jobRef->cancelled.load(std::memory_order_relaxed);
        },
        [this, requestId](const QString& errorText) {
            Q_EMIT scanError(requestId, errorText);
        },
        [jobRef]() -> bool {
            return jobRef->cancelled.load(std::memory_order_relaxed);
        },
        [this, requestId](quint64 filesSeen, quint64 filesEmitted) {
            Q_EMIT scanProgress(requestId, filesSeen, filesEmitted);
        }
    );

    if (jobRef->cancelled.load(std::memory_order_relaxed)) {
        Q_EMIT scanCancelled(requestId);
    } else if (ok) {
        Q_EMIT scanCompleted(requestId);
    }

    currentJob_.reset();
}