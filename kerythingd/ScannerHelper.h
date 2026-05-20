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
#include "Protocol.h"

namespace ScannerHelper {

    using FileRecordChunkCallback = std::function<bool(const std::vector<FileRecord>&)>;
    using StringPoolChunkCallback = std::function<bool(const std::vector<char>&)>;
    using ErrorCallback = std::function<void(const QString&)>;
    using CancelCallback = std::function<bool()>;
    using ProgressCallback = std::function<void(quint64 filesProcessed, quint64 filesTotal)>;

    bool isAllowedFsType(const QString& fsType);

    std::expected<QString, QString> validateDevNode(const QString& inputPath);

    bool scanDevice(const QString& devNode,
                    const QString& fsType,
                    const FileRecordChunkCallback& onFileRecordChunk,
                    const StringPoolChunkCallback& onStringPoolChunk,
                    const ErrorCallback& onError,
                    const CancelCallback& shouldCancel,
                    const ProgressCallback& onProgress);

} // namespace ScannerHelper

#endif // KERYTHINGD_SCANNERHELPER_H