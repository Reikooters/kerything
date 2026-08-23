// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ScannerWorker.h"

#include "FileRecord.h"
#include "ThrottledProgressReporter.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

ScannerWorker::ScannerWorker(QObject* parent)
    : QObject(parent)
{
}

void ScannerWorker::startScan(std::shared_ptr<ScanJob> job)
{
    if (!job) {
        return;
    }

    if (job->cancelled.load(std::memory_order_relaxed)) {
        Q_EMIT scanCancelled(job->requestId, job->deviceId);
        return;
    }

    currentJob_ = std::move(job);

    const quint32 requestId = currentJob_->requestId;
    const std::shared_ptr<ScanJob> jobRef = currentJob_;

    Q_EMIT scanStarted(
        jobRef->requestId,
        jobRef->deviceId,
        jobRef->devNode,
        jobRef->fsType,
        jobRef->label,
        jobRef->mountPoints,
        jobRef->primaryMountPoint
    );

    // Wrap the Qt signal emission in a small throttler so fast scans do not
    // flood the UI with progress updates.
    //
    // The helper keeps the timing policy separate from the actual Qt signal.
    // That makes the scan code easier to read and keeps the throttling logic
    // reusable if we ever want to report progress somewhere else.
    auto progressReporter = ThrottledProgressReporter{
        [this, requestId](Protocol::ScanProgress progress) {
            Q_EMIT scanProgress(requestId, std::move(progress));
        },
        std::chrono::milliseconds(100) // Maximum update rate: 10 times per second.
    };

    const bool ok = ScannerHelper::scanDevice(
        jobRef->devNode,
        jobRef->fsType,
        jobRef->primaryMountPoint,
        jobRef->mountPoints,
        [this, requestId, jobRef](const std::vector<FileRecord>& fileRecordChunk) -> bool {
            if (jobRef->cancelled.load(std::memory_order_relaxed)) {
                return false;
            }

            Q_EMIT scanFileRecordChunkReady(requestId, fileRecordChunk);

            return !jobRef->cancelled.load(std::memory_order_relaxed);
        },
        [this, requestId, jobRef](const std::vector<char>& stringPoolChunk) -> bool {
            if (jobRef->cancelled.load(std::memory_order_relaxed)) {
                return false;
            }

            Q_EMIT scanStringPoolChunkReady(requestId, stringPoolChunk);

            return !jobRef->cancelled.load(std::memory_order_relaxed);
        },
        [this, requestId](const QString& errorText) {
            Q_EMIT scanError(requestId, errorText);
        },
        [jobRef]() -> bool {
            return jobRef->cancelled.load(std::memory_order_relaxed);
        },
        [&progressReporter](const Protocol::ScanProgress& progress) {
            // Forward raw progress into the throttler.
            // It will decide whether to emit now or skip this update.
            progressReporter(progress);
        }
    );

    if (jobRef->cancelled.load(std::memory_order_relaxed)) {
        Q_EMIT scanCancelled(requestId, jobRef->deviceId);
    } else if (ok) {
        Q_EMIT scanCompleted(requestId, jobRef->deviceId, jobRef->devNode, jobRef->fsType);
    }

    currentJob_.reset();

#if defined(__GLIBC__)
    ::malloc_trim(0);
#endif
}