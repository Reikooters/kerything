// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_BTRFSSCANNERENGINE_H
#define KERYTHINGD_BTRFSSCANNERENGINE_H

#include <QString>
#include <QStringList>

#include "../ScannerHelper.h"

namespace BtrfsScannerEngine {

    struct DebugScanOptions {
        bool printMountTable = true;
        bool printRecords = true;
        bool printSummary = true;

        // Mounted-only policy:
        // If a directory entry points at another subvolume root, skip it unless
        // that root id is also one of the mounted roots selected for this scan.
        bool skipUnmountedSubvolumeBoundaries = true;
    };

    /**
     * Diagnostic Btrfs scanner using BTRFS_IOC_TREE_SEARCH_V2.
     *
     * This is intentionally not wired into the normal FileRecord pipeline yet.
     * It prints mounted-root mappings and metadata-derived directory entries to
     * stdout so test loop devices can be validated before index integration.
     *
     * @param devicePath Representative device path for logging only.
     * @param mountPoints Mount points associated with the Btrfs filesystem.
     * @param options Debug output and policy options.
     * @param onError Error callback.
     * @param shouldCancel Cancellation callback.
     * @return true if the diagnostic scan completed, false on error/cancel.
     */
    bool debugScanMountedFilesystem(
        const QString& devicePath,
        const QStringList& mountPoints,
        const DebugScanOptions& options,
        const ScannerHelper::ErrorCallback& onError,
        const ScannerHelper::CancelCallback& shouldCancel
    );

} // namespace BtrfsScannerEngine

#endif // KERYTHINGD_BTRFSSCANNERENGINE_H