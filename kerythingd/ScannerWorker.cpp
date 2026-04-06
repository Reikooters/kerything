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

    emit scanStarted(currentJob_->requestId);

    const bool ok = ScannerHelper::scanDevice(
        currentJob_->devicePath,
        currentJob_->fsType,
        [this](const std::vector<FileRecord>& chunk) -> bool {
            emit scanChunkReady(currentJob_->requestId, chunk);
            return !currentJob_->cancelled.load(std::memory_order_relaxed);
        },
        [this](const QString& errorText) {
            emit scanError(currentJob_->requestId, errorText);
        },
        [this]() -> bool {
            return currentJob_->cancelled.load(std::memory_order_relaxed);
        },
        [this](quint64 filesSeen, quint64 filesEmitted) {
            emit scanProgress(currentJob_->requestId, filesSeen, filesEmitted);
        }
    );

    if (currentJob_->cancelled.load(std::memory_order_relaxed)) {
        emit scanCancelled(currentJob_->requestId);
    } else if (ok) {
        emit scanCompleted(currentJob_->requestId);
    }

    currentJob_.reset();
}