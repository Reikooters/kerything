// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "GenericMountedScannerEngine.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <QFile>

#include "FileRecord.h"
#include "Protocol.h"
#include "ScopedTimer.h"

namespace GenericMountedScannerEngine {

namespace {

constexpr uint32_t kInvalidRecordIndex = 0xFFFFFFFF;
constexpr uint64_t kProgressEvery = 4096;
constexpr std::size_t kGetdentsBufferSize = 256 * 1024;

struct LinuxDirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct StreamState {
    std::vector<FileRecord> records;
    std::vector<char> stringPool;
    uint32_t totalStringPoolLength = 0;

    static constexpr uint32_t kTargetIpcBufferSizeMB = 4;
    static constexpr uint32_t kMaxIpcBufferSizeBytes =
        (kTargetIpcBufferSizeMB * 1024 * 1024) -
        Protocol::HeaderSize -
        sizeof(Protocol::ScanIndexResultChunkType);

    static constexpr uint32_t kRecordsPerIpcChunk =
        kMaxIpcBufferSizeBytes / sizeof(FileRecord);

    bool flush(
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk
    ) {
        if (!records.empty()) {
            std::vector<FileRecord> fileRecordChunk = std::move(records);
            records.clear();
            records.reserve(kRecordsPerIpcChunk);

            if (!onFileRecordChunk(fileRecordChunk)) {
                std::cerr << "[GenericMountedScannerEngine] scan aborted by file record receiver\n";
                return false;
            }
        }

        if (!stringPool.empty()) {
            std::vector<char> stringPoolChunk = std::move(stringPool);
            totalStringPoolLength += static_cast<uint32_t>(stringPoolChunk.size());

            stringPool.clear();
            stringPool.reserve(kMaxIpcBufferSizeBytes);

            if (!onStringPoolChunk(stringPoolChunk)) {
                std::cerr << "[GenericMountedScannerEngine] scan aborted by string pool receiver\n";
                return false;
            }
        }

        return true;
    }

    bool addRecord(
        uint64_t inode,
        uint64_t parentInode,
        std::string_view name,
        uint64_t size,
        uint64_t modificationTime,
        uint8_t flags,
        const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
        const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk
    ) {
        if (name.size() > std::numeric_limits<uint16_t>::max()) {
            return true;
        }

        if (totalStringPoolLength + stringPool.size() + name.size() >
            std::numeric_limits<uint32_t>::max()) {
            return true;
        }

        if (records.size() >= kRecordsPerIpcChunk ||
            stringPool.size() + name.size() >= kMaxIpcBufferSizeBytes) {
            if (!flush(onFileRecordChunk, onStringPoolChunk)) {
                return false;
            }
        }

        FileRecord record{};
        record.fsIndex = inode;
        record.parentFsIndex = parentInode;
        record.parentRecordIdx = kInvalidRecordIndex;
        record.size = size;
        record.modificationTime = modificationTime;
        record.nameOffset = totalStringPoolLength + static_cast<uint32_t>(stringPool.size());
        record.nameLen = static_cast<uint16_t>(name.size());
        record.flags = flags;

        records.push_back(record);
        stringPool.insert(stringPool.end(), name.begin(), name.end());

        return true;
    }
};

struct PendingDirectory {
    int parentFd = -1;
    std::string name;
    uint64_t inode = 0;
    std::string absolutePath;
};

void reportError(const ScannerHelper::ErrorCallback& onError, const QString& message)
{
    if (onError) {
        onError(message);
    }

    std::cerr << "[GenericMountedScannerEngine] "
              << message.toStdString()
              << "\n";
}

std::string decodeMountInfoField(const std::string& input)
{
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' &&
            i + 3 < input.size() &&
            input[i + 1] >= '0' && input[i + 1] <= '7' &&
            input[i + 2] >= '0' && input[i + 2] <= '7' &&
            input[i + 3] >= '0' && input[i + 3] <= '7') {
            const int value =
                (input[i + 1] - '0') * 64 +
                (input[i + 2] - '0') * 8 +
                (input[i + 3] - '0');

            out.push_back(static_cast<char>(value));
            i += 3;
        } else {
            out.push_back(input[i]);
        }
    }

    return out;
}

std::string normalizePath(std::string path)
{
    while (path.size() > 1 && path.ends_with('/')) {
        path.pop_back();
    }

    return path.empty() ? std::string("/") : std::move(path);
}

bool isPathBelowOrEqual(std::string_view path, std::string_view root)
{
    if (path == root) {
        return true;
    }

    if (root == "/") {
        return path.starts_with("/");
    }

    return path.size() > root.size() &&
           path.starts_with(root) &&
           path[root.size()] == '/';
}

std::unordered_set<std::string> nestedMountPointsForRoot(const std::string& rootMountPoint)
{
    std::unordered_set<std::string> nestedMountPoints;

    std::ifstream in("/proc/self/mountinfo");
    if (!in) {
        return nestedMountPoints;
    }

    const std::string normalizedRoot = normalizePath(rootMountPoint);

    std::string line;
    while (std::getline(in, line)) {
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) {
            continue;
        }

        const std::string left = line.substr(0, sep);

        std::istringstream iss(left);
        std::string id;
        std::string parent;
        std::string majorMinor;
        std::string mountRoot;
        std::string mountPoint;
        std::string options;

        if (!(iss >> id >> parent >> majorMinor >> mountRoot >> mountPoint >> options)) {
            continue;
        }

        const std::string decodedMountPoint = normalizePath(decodeMountInfoField(mountPoint));

        if (decodedMountPoint == normalizedRoot) {
            continue;
        }

        if (isPathBelowOrEqual(decodedMountPoint, normalizedRoot)) {
            nestedMountPoints.insert(decodedMountPoint);
        }
    }

    return nestedMountPoints;
}

uint8_t flagsFromMode(mode_t mode)
{
    uint8_t flags = 0;

    if (S_ISDIR(mode)) {
        flags |= FileRecord_IsDir;
    }

    if (S_ISLNK(mode)) {
        flags |= FileRecord_IsSymlink;
    }

    return flags;
}

uint64_t mtimeSecondsFromStat(const struct stat& st)
{
#if defined(__APPLE__)
    return static_cast<uint64_t>(st.st_mtimespec.tv_sec);
#else
    return static_cast<uint64_t>(st.st_mtim.tv_sec);
#endif
}

bool shouldSkipName(const char* name)
{
    return std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0;
}

std::string childPath(const std::string& parent, std::string_view name)
{
    if (parent == "/") {
        std::string out;
        out.reserve(1 + name.size());
        out.push_back('/');
        out.append(name);
        return out;
    }

    std::string out;
    out.reserve(parent.size() + 1 + name.size());
    out.append(parent);
    out.push_back('/');
    out.append(name);
    return out;
}

} // namespace

bool scanMountedDevice(
    const QString& primaryMountPoint,
    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
    const ScannerHelper::ErrorCallback& onError,
    const ScannerHelper::CancelCallback& shouldCancel,
    const ScannerHelper::ProgressCallback& onProgress
) {
    ScopedTimer totalTimer("[GenericMountedScannerEngine] total mounted scan");

    if (primaryMountPoint.trimmed().isEmpty()) {
        reportError(onError, QStringLiteral("generic mounted scanner requires a mount point"));
        return false;
    }

    const QByteArray encodedMountPoint = QFile::encodeName(primaryMountPoint);
    const std::string rootPath = normalizePath(encodedMountPoint.constData());

    struct stat rootStat {};
    if (::lstat(rootPath.c_str(), &rootStat) != 0) {
        reportError(
            onError,
            QStringLiteral("lstat failed for mount point %1: %2")
                .arg(primaryMountPoint, QString::fromLocal8Bit(std::strerror(errno)))
        );
        return false;
    }

    if (!S_ISDIR(rootStat.st_mode)) {
        reportError(onError, QStringLiteral("mount point is not a directory: %1").arg(primaryMountPoint));
        return false;
    }

    const dev_t rootDev = rootStat.st_dev;
    const uint64_t rootInode = static_cast<uint64_t>(rootStat.st_ino);
    const auto nestedMountPoints = nestedMountPointsForRoot(rootPath);

    int rootFd = ::open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (rootFd < 0) {
        reportError(
            onError,
            QStringLiteral("failed to open mount point %1: %2")
                .arg(primaryMountPoint, QString::fromLocal8Bit(std::strerror(errno)))
        );
        return false;
    }

    StreamState stream;
    stream.records.reserve(StreamState::kRecordsPerIpcChunk);
    stream.stringPool.reserve(StreamState::kMaxIpcBufferSizeBytes);

    if (!stream.addRecord(
            rootInode,
            rootInode,
            std::string_view{},
            0,
            mtimeSecondsFromStat(rootStat),
            FileRecord_IsDir,
            onFileRecordChunk,
            onStringPoolChunk)) {
        ::close(rootFd);
        return false;
    }

    std::vector<PendingDirectory> stack;
    stack.push_back(PendingDirectory{
        .parentFd = AT_FDCWD,
        .name = rootPath,
        .inode = rootInode,
        .absolutePath = rootPath
    });

    uint64_t processed = 0;

    std::vector<char> buffer(kGetdentsBufferSize);

    while (!stack.empty()) {
        if (shouldCancel && shouldCancel()) {
            ::close(rootFd);
            return false;
        }

        PendingDirectory current = std::move(stack.back());
        stack.pop_back();

        int dirFd = -1;
        if (current.absolutePath == rootPath) {
            dirFd = ::dup(rootFd);
        } else {
            dirFd = ::openat(
                current.parentFd,
                current.name.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
            );
        }

        if (current.parentFd >= 0 && current.parentFd != AT_FDCWD) {
            ::close(current.parentFd);
        }

        if (dirFd < 0) {
            continue;
        }

        while (true) {
            if (shouldCancel && shouldCancel()) {
                ::close(dirFd);
                ::close(rootFd);
                return false;
            }

            const int nread = static_cast<int>(
                ::syscall(SYS_getdents64, dirFd, buffer.data(), buffer.size())
            );

            if (nread < 0) {
                break;
            }

            if (nread == 0) {
                break;
            }

            for (int pos = 0; pos < nread; ) {
                const auto* entry = reinterpret_cast<const LinuxDirent64*>(buffer.data() + pos);
                pos += entry->d_reclen;

                const char* name = entry->d_name;
                if (!name || shouldSkipName(name)) {
                    continue;
                }

                struct stat st {};
                if (::fstatat(dirFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    continue;
                }

                const bool isDirectory = S_ISDIR(st.st_mode);
                const std::string absoluteChildPath = childPath(current.absolutePath, name);

                /*
                 * Skip entries that belong to another mounted filesystem.
                 *
                 * st_dev catches normal filesystem boundaries such as /proc, /dev,
                 * /sys, and another disk mounted under this tree.
                 *
                 * mountinfo catches bind mounts, including bind mounts that may
                 * have the same st_dev as the root filesystem.
                 */
                if (isDirectory) {
                    if (st.st_dev != rootDev) {
                        continue;
                    }

                    if (nestedMountPoints.contains(normalizePath(absoluteChildPath))) {
                        continue;
                    }
                }

                const std::string_view nameView(name);
                const uint8_t flags = flagsFromMode(st.st_mode);

                if (!stream.addRecord(
                        static_cast<uint64_t>(st.st_ino),
                        current.inode,
                        nameView,
                        S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0,
                        mtimeSecondsFromStat(st),
                        flags,
                        onFileRecordChunk,
                        onStringPoolChunk)) {
                    ::close(dirFd);
                    ::close(rootFd);
                    return false;
                }

                ++processed;

                if (onProgress && ((processed & (kProgressEvery - 1)) == 0)) {
                    onProgress(processed, 0);
                }

                if (!isDirectory) {
                    continue;
                }

                const int childParentFd = ::dup(dirFd);
                if (childParentFd < 0) {
                    continue;
                }

                stack.push_back(PendingDirectory{
                    .parentFd = childParentFd,
                    .name = name,
                    .inode = static_cast<uint64_t>(st.st_ino),
                    .absolutePath = absoluteChildPath
                });
            }
        }

        ::close(dirFd);
    }

    ::close(rootFd);

    if (!stream.flush(onFileRecordChunk, onStringPoolChunk)) {
        return false;
    }

    if (onProgress) {
        onProgress(processed, processed);
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "[GenericMountedScannerEngine] emitted stringPoolBytes="
              << stream.totalStringPoolLength
              << " recordsProcessed="
              << processed
              << "\n";
#endif

    return true;
}

} // namespace GenericMountedScannerEngine