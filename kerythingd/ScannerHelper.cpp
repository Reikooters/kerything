// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "ScannerHelper.h"

#include <expected>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <linux/limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QString>
#include <QDir>
#include <QFile>

#include "FileRecord.h"
#include "ScopedTimer.h"
#include "scanners/Ext4ScannerEngine.h"
#include "scanners/NtfsScannerEngine.h"

namespace ScannerHelper {

bool isAllowedFsType(const QString& fsType)
{
    return fsType == QStringLiteral("ntfs") || fsType == QStringLiteral("ext4");
}

std::expected<QString, QString> validateDevNode(const QString& inputPath)
{
    if (inputPath.isEmpty()) {
        return std::unexpected(QStringLiteral("empty device path"));
    }

    // Path must be absolute.
    if (!QDir::isAbsolutePath(inputPath)) {
        return std::unexpected(
            QStringLiteral("device path must be absolute (got: %1)").arg(inputPath));
    }

    // Only allow scanning devices under /dev.
    if (!inputPath.startsWith(QStringLiteral("/dev/"))) {
        return std::unexpected(
            QStringLiteral("device path must be under /dev (got: %1)").arg(inputPath));
    }

    // realpath() works on a native byte string.
    const QByteArray inputNative = QFile::encodeName(inputPath);

    // Resolve symlinks / relative components safely.
    // realpath() fails if the path doesn't exist.
    char* resolved = realpath(inputNative.constData(), nullptr);
    if (!resolved) {
        return std::unexpected(
            QStringLiteral("failed to resolve device path '%1': %2")
                .arg(inputPath, QString::fromLocal8Bit(std::strerror(errno))));
    }

    QString resolvedOut = QFile::decodeName(resolved);

    // Free the memory created by realpath()
    std::free(resolved);

    if (!resolvedOut.startsWith(QStringLiteral("/dev/"))) {
        return std::unexpected(
            QStringLiteral("resolved device path must still be under /dev (got: %1)")
                .arg(resolvedOut));
    }

    struct stat st {};
    if (stat(QFile::encodeName(resolvedOut).constData(), &st) != 0) {
        return std::unexpected(
            QStringLiteral("stat() failed for '%1': %2")
                .arg(resolvedOut, QString::fromLocal8Bit(std::strerror(errno))));
    }

    if (!S_ISBLK(st.st_mode)) {
        return std::unexpected(
            QStringLiteral("'%1' is not a block device").arg(resolvedOut));
    }

    // Reject world-writable device nodes (paranoia / sanity check)
    if ((st.st_mode & S_IWOTH) != 0) {
        return std::unexpected(
            QStringLiteral("refusing world-writable device node '%1'").arg(resolvedOut));
    }

    return resolvedOut;
}

bool scanDevice(const QString& devNode,
                const QString& fsType,
                const FileRecordChunkCallback& onFileRecordChunk,
                const StringPoolChunkCallback& onStringPoolChunk,
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

    const auto validated = validateDevNode(devNode);
    if (!validated) {
        if (onError) {
            onError(validated.error());
        }
        return false;
    }

    const QString& resolvedPath = *validated;

    if (fsType == "ntfs") {
        return NtfsScannerEngine::scanDevice(resolvedPath, onFileRecordChunk, onStringPoolChunk, onError, shouldCancel, onProgress);
    }
    if (fsType == "ext4") {
        {
            ScopedTimer totalTimer("total ext4 scan");

            return Ext4ScannerEngine::scanDevice(resolvedPath, onFileRecordChunk, onStringPoolChunk, onError, shouldCancel, onProgress);
        }
    }
    else {
        return false;
        //return scanDeviceOther(resolvedPath, fsType, onFileRecordChunk, onStringPoolChunk, onError, shouldCancel, onProgress);
    }

    // std::vector<FileRecord> chunk;
    // chunk.reserve(1024);
    //
    // quint64 filesSeen = 0;
    // quint64 filesEmitted = 0;
    //
    // auto flushChunk = [&]() -> bool {
    //     if (chunk.empty()) {
    //         return true;
    //     }
    //
    //     if (onChunk && !onChunk(chunk)) {
    //         return false;
    //     }
    //
    //     filesEmitted += chunk.size();
    //     chunk.clear();
    //
    //     if (onProgress) {
    //         onProgress(filesSeen, filesEmitted);
    //     }
    //
    //     return true;
    // };
    //
    // // TODO: Replace this with the real filesystem walking logic.
    // for (int i = 0; i < 50000; ++i) {
    //     if (shouldCancel && shouldCancel()) {
    //         if (onError) {
    //             onError(QStringLiteral("scan cancelled"));
    //         }
    //         return false;
    //     }
    //
    //     FileRecord rec;
    //     rec.path = QStringLiteral("%1/file_%2.txt")
    //                    .arg(resolvedPath)
    //                    .arg(i);
    //     rec.size = static_cast<quint64>(i * 100);
    //     rec.mtime = static_cast<quint64>(i * 1000);
    //
    //     chunk.push_back(std::move(rec));
    //     ++filesSeen;
    //
    //     if (onProgress && (filesSeen % 1024 == 0)) {
    //         onProgress(filesSeen, filesEmitted);
    //     }
    //
    //     if (chunk.size() >= 1024) {
    //         if (!flushChunk()) {
    //             if (onError) {
    //                 onError(QStringLiteral("scan aborted by receiver"));
    //             }
    //             return false;
    //         }
    //     }
    // }
    //
    // if (!flushChunk()) {
    //     if (onError) {
    //         onError(QStringLiteral("scan aborted by receiver"));
    //     }
    //     return false;
    // }
    //
    // return true;
}

} // namespace ScannerHelper