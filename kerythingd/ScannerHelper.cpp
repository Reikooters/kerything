// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ScannerHelper.h"

#include <cstring>
#include <filesystem>
#include <linux/limits.h>
#include <sys/stat.h>

#include "FileRecord.h"

namespace ScannerHelper {

bool isAllowedFsType(const QString& fsType)
{
    return fsType == QStringLiteral("ntfs") || fsType == QStringLiteral("ext4");
}

std::expected<std::string, std::string> validateDevicePath(const std::string& inputPath)
{
    namespace fs = std::filesystem;

    if (inputPath.empty()) {
        return std::unexpected("empty device path");
    }

    const fs::path p(inputPath);

    if (!p.is_absolute()) {
        return std::unexpected("device path must be absolute (got: " + inputPath + ")");
    }

    // Only allow scanning devices under /dev.
    if (p.native().rfind("/dev/", 0) != 0) {
        return std::unexpected("device path must be under /dev (got: " + inputPath + ")");
    }

    // Resolve symlinks / relative components safely.
    // realpath() fails if the path doesn't exist.
    char buf[PATH_MAX];
    if (!realpath(inputPath.c_str(), buf)) {
        return std::unexpected(
            "failed to resolve device path '" + inputPath + "': " + std::strerror(errno));
    }

    std::string resolvedOut = buf;

    struct stat st {};
    if (stat(resolvedOut.c_str(), &st) != 0) {
        return std::unexpected(
            "stat() failed for '" + resolvedOut + "': " + std::strerror(errno));
    }

    if (!S_ISBLK(st.st_mode)) {
        return std::unexpected("'" + resolvedOut + "' is not a block device");
    }

    // Reject world-writable device nodes (paranoia / sanity check)
    if ((st.st_mode & S_IWOTH) != 0) {
        return std::unexpected("refusing world-writable device node '" + resolvedOut + "'");
    }

    return resolvedOut;
}

bool scanDevice(const QString& devicePath,
                const QString& fsType,
                const ChunkCallback& onChunk,
                const ErrorCallback& onError,
                const CancelCallback& shouldCancel,
                const ProgressCallback& onProgress)
{
    if (!isAllowedFsType(fsType)) {
        if (onError) {
            onError(QStringLiteral("unsupported fsType '%1'").arg(fsType));
        }
        return false;
    }

    const auto validated = validateDevicePath(devicePath.toStdString());
    if (!validated) {
        if (onError) {
            onError(QString::fromStdString(validated.error()));
        }
        return false;
    }

    const std::string& resolvedPath = *validated;

    std::vector<FileRecord> chunk;
    chunk.reserve(1024);

    quint64 filesSeen = 0;
    quint64 filesEmitted = 0;

    auto flushChunk = [&]() -> bool {
        if (chunk.empty()) {
            return true;
        }

        if (onChunk && !onChunk(chunk)) {
            return false;
        }

        filesEmitted += chunk.size();
        chunk.clear();

        if (onProgress) {
            onProgress(filesSeen, filesEmitted);
        }

        return true;
    };

    // TODO: Replace this with the real filesystem walking logic.
    for (int i = 0; i < 50000; ++i) {
        if (shouldCancel && shouldCancel()) {
            if (onError) {
                onError(QStringLiteral("scan cancelled"));
            }
            return false;
        }

        FileRecord rec;
        rec.path = QStringLiteral("%1/file_%2.txt")
                       .arg(QString::fromStdString(resolvedPath))
                       .arg(i);
        rec.size = static_cast<quint64>(i * 100);
        rec.mtime = static_cast<quint64>(i * 1000);

        chunk.push_back(std::move(rec));
        ++filesSeen;

        if (onProgress && (filesSeen % 1024 == 0)) {
            onProgress(filesSeen, filesEmitted);
        }

        if (chunk.size() >= 1024) {
            if (!flushChunk()) {
                if (onError) {
                    onError(QStringLiteral("scan aborted by receiver"));
                }
                return false;
            }
        }
    }

    if (!flushChunk()) {
        if (onError) {
            onError(QStringLiteral("scan aborted by receiver"));
        }
        return false;
    }

    return true;
}

} // namespace ScannerHelper