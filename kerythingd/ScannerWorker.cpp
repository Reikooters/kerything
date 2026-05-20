// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ScannerWorker.h"

#include "FileRecord.h"
#include "ThrottledProgressReporter.h"

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

    Q_EMIT scanStarted(requestId, currentJob_->deviceId, currentJob_->devNode, currentJob_->fsType);

    // Wrap the Qt signal emission in a small throttler so fast scans do not
    // flood the UI with progress updates.
    //
    // The helper keeps the timing policy separate from the actual Qt signal.
    // That makes the scan code easier to read and keeps the throttling logic
    // reusable if we ever want to report progress somewhere else.
    auto progressReporter = ThrottledProgressReporter{
        [this, requestId](quint64 filesProcessed, quint64 filesTotal) {
            Q_EMIT scanProgress(requestId, filesProcessed, filesTotal);
        },
        std::chrono::milliseconds(100) // Maximum update rate: 10 times per second.
    };

    const bool ok = ScannerHelper::scanDevice(
        jobRef->devNode,
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
        [&progressReporter](quint64 filesProcessed, quint64 filesTotal) {
            // Forward raw progress into the throttler.
            // It will decide whether to emit now or skip this update.
            progressReporter(filesProcessed, filesTotal);
        }
    );

    if (jobRef->cancelled.load(std::memory_order_relaxed)) {
        Q_EMIT scanCancelled(requestId, currentJob_->deviceId);
    } else if (ok) {
        Q_EMIT scanCompleted(requestId, currentJob_->deviceId, currentJob_->devNode, currentJob_->fsType);
    }

    currentJob_.reset();
}