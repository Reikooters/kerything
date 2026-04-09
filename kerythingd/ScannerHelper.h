// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_SCANNERHELPER_H
#define KERYTHINGD_SCANNERHELPER_H

#include <expected>
#include <functional>
#include <string>
#include <vector>
#include <QString>

#include "FileRecord.h"

namespace ScannerHelper {

    using ChunkCallback = std::function<bool(const std::vector<FileRecord>&, const std::vector<char>&)>;
    using ErrorCallback = std::function<void(const QString&)>;
    using CancelCallback = std::function<bool()>;
    using ProgressCallback = std::function<void(quint64 filesSeen, quint64 filesEmitted)>;

    bool isAllowedFsType(const QString& fsType);

    std::expected<QString, QString> validateDevicePath(const QString& inputPath);

    bool scanDevice(const QString& devicePath,
                    const QString& fsType,
                    const ChunkCallback& onChunk,
                    const ErrorCallback& onError,
                    const CancelCallback& shouldCancel,
                    const ProgressCallback& onProgress);

} // namespace ScannerHelper

#endif // KERYTHINGD_SCANNERHELPER_H