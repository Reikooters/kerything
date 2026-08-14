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

#include <fcntl.h>

#include <QString>
#include <QStringList>
#include <QDir>
#include <QFile>

#include "FileRecord.h"
#include "ScopedTimer.h"
#include "scanners/Ext4ScannerEngine.h"
#include "scanners/GenericMountedScannerEngine.h"
#include "scanners/NtfsScannerEngine.h"

namespace ScannerHelper {

namespace {
    QStringList orderedMountPoints(const QString& primaryMountPoint, const QStringList& mountPoints)
    {
        QStringList ordered;

        const QString primary = primaryMountPoint.trimmed();
        if (!primary.isEmpty()) {
            ordered << primary;
        }

        for (const QString& mountPoint : mountPoints) {
            const QString trimmed = mountPoint.trimmed();
            if (!trimmed.isEmpty()) {
                ordered << trimmed;
            }
        }

        ordered.removeDuplicates();
        return ordered;
    }

    bool syncMountedFilesystem(
        const QString& primaryMountPoint,
        const QStringList& mountPoints,
        const ErrorCallback& onError)
    {
        const QStringList candidates = orderedMountPoints(primaryMountPoint, mountPoints);

        if (candidates.isEmpty()) {
            return true;
        }

        for (const QString& mountPoint : candidates) {
            const QByteArray encodedMountPoint = QFile::encodeName(mountPoint);
            const int fd = ::open(
                encodedMountPoint.constData(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC
            );

            if (fd < 0) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cerr << "[ScannerHelper] failed to open mount point for syncfs: "
                          << mountPoint.toStdString()
                          << ": "
                          << std::strerror(errno)
                          << "\n";
#endif
                continue;
            }

            if (::syncfs(fd) != 0) {
                const QString errorText = QStringLiteral("syncfs failed for mount point %1: %2")
                    .arg(mountPoint, QString::fromLocal8Bit(std::strerror(errno)));

                ::close(fd);

#ifdef KERYTHING_ENABLE_LOGGING
                std::cerr << "[ScannerHelper] "
                          << errorText.toStdString()
                          << "\n";
#endif

                if (onError) {
                    onError(errorText);
                }

                return false;
            }

#ifdef KERYTHING_ENABLE_LOGGING
            std::cerr << "[ScannerHelper] synced mounted filesystem before raw scan using mount point: "
                      << mountPoint.toStdString()
                      << "\n";
#endif

            ::close(fd);
            return true;
        }

        const QString errorText = QStringLiteral("could not open any mount point for syncfs before raw scan");

#ifdef KERYTHING_ENABLE_LOGGING
        std::cerr << "[ScannerHelper] "
                  << errorText.toStdString()
                  << "\n";
#endif

        if (onError) {
            onError(errorText);
        }

        return false;
    }
}

bool isAllowedFsType(const QString& fsType)
{
    const QString normalized = fsType.trimmed().toLower();

    /*
     * EXT4 and NTFS have specialized low-level scanners.
     *
     * Other filesystem types can still be scanned when mounted through the
     * generic VFS scanner. Filtering of unsuitable device types should happen
     * in BlockDeviceHelper where mount/device metadata is available.
     */
    return !normalized.isEmpty();
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
                const QString& primaryMountPoint,
                const QStringList& mountPoints,
                const FileRecordChunkCallback& onFileRecordChunk,
                const StringPoolChunkCallback& onStringPoolChunk,
                const ErrorCallback& onError,
                const CancelCallback& shouldCancel,
                const ProgressCallback& onProgress)
{
    const QString normalizedFsType = fsType.trimmed().toLower();

    if (!isAllowedFsType(normalizedFsType)) {
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

    if (normalizedFsType == QStringLiteral("ntfs") ||
        normalizedFsType == QStringLiteral("ntfs3")) {
        syncMountedFilesystem(primaryMountPoint, mountPoints, onError);

        return NtfsScannerEngine::scanDevice(
            resolvedPath,
            onFileRecordChunk,
            onStringPoolChunk,
            onError,
            shouldCancel,
            onProgress
        );
    }

    if (normalizedFsType == QStringLiteral("ext4")) {
        syncMountedFilesystem(primaryMountPoint, mountPoints, onError);

        return Ext4ScannerEngine::scanDevice(
            resolvedPath,
            onFileRecordChunk,
            onStringPoolChunk,
            onError,
            shouldCancel,
            onProgress
        );
    }

    if (primaryMountPoint.trimmed().isEmpty()) {
        if (onError) {
            onError(
                QStringLiteral("filesystem type '%1' can only be scanned while mounted")
                    .arg(fsType)
            );
        }

        return false;
    }

    return GenericMountedScannerEngine::scanMountedDevice(
        primaryMountPoint,
        onFileRecordChunk,
        onStringPoolChunk,
        onError,
        shouldCancel,
        onProgress
    );
}

} // namespace ScannerHelper