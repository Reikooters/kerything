// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_GENERICMOUNTEDSCANNERENGINE_H
#define KERYTHINGD_GENERICMOUNTEDSCANNERENGINE_H

#include <QString>

#include "../ScannerHelper.h"

namespace GenericMountedScannerEngine {

    /**
     * Scans a mounted filesystem through generic Linux VFS APIs.
     *
     * This scanner is intended for filesystems that do not have a specialized
     * low-level scanner. It requires a mount point and will not scan unmounted
     * devices.
     *
     * Safety/per-device isolation:
     * - Traversal starts at primaryMountPoint.
     * - Directory descent is limited to the root mount's st_dev.
     * - Nested mount points from /proc/self/mountinfo are skipped.
     * - Symlinks are indexed but not followed.
     */
    bool scanMountedDevice(
        const QString& primaryMountPoint,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
        const ScannerHelper::ErrorCallback& onError,
        const ScannerHelper::CancelCallback& shouldCancel,
        const ScannerHelper::ProgressCallback& onProgress
    );

} // namespace GenericMountedScannerEngine

#endif // KERYTHINGD_GENERICMOUNTEDSCANNERENGINE_H