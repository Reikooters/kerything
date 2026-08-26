// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "BtrfsScannerEngine.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <linux/btrfs.h>
#include <linux/btrfs_tree.h>

namespace {
    struct UniqueFd {
        int fd = -1;

        UniqueFd() = default;

        explicit UniqueFd(int value)
            : fd(value)
        {
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;

        UniqueFd(UniqueFd&& other) noexcept
            : fd(other.fd)
        {
            other.fd = -1;
        }

        UniqueFd& operator=(UniqueFd&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }

            reset();
            fd = other.fd;
            other.fd = -1;
            return *this;
        }

        ~UniqueFd()
        {
            reset();
        }

        void reset(int value = -1)
        {
            if (fd >= 0) {
                ::close(fd);
            }

            fd = value;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return fd >= 0;
        }
    };

    struct MountInfoEntry {
        std::string root;
        std::string mountPoint;
        std::string fsType;
        std::string mountSource;
        std::string superOptions;
    };

    struct MountedRoot {
        QString mountPoint;
        QString mountRoot;
        QString mountSource;

        quint64 rootId = 0;
        QString subvolPath;
    };

    struct InodeInfo {
        quint64 inode = 0;
        quint64 size = 0;
        qint64 modificationTime = 0;
        quint8 flags = 0;
        bool present = false;
    };

    struct DirEntry {
        quint64 rootId = 0;
        quint64 parentInode = 0;
        quint64 childRootId = 0;
        quint64 childInode = 0;

        QString name;
        quint8 btrfsType = 0;
    };

    struct DirEntryKey {
        quint64 rootId = 0;
        quint64 parentInode = 0;
        quint64 childRootId = 0;
        quint64 childInode = 0;
        QString name;

        bool operator==(const DirEntryKey& other) const noexcept
        {
            return rootId == other.rootId &&
                   parentInode == other.parentInode &&
                   childRootId == other.childRootId &&
                   childInode == other.childInode &&
                   name == other.name;
        }
    };

    struct DirEntryKeyHash {
        std::size_t operator()(const DirEntryKey& key) const noexcept
        {
            std::size_t seed = std::hash<quint64>{}(key.rootId);

            auto combine = [&seed](std::size_t value) {
                seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            };

            combine(std::hash<quint64>{}(key.parentInode));
            combine(std::hash<quint64>{}(key.childRootId));
            combine(std::hash<quint64>{}(key.childInode));
            combine(qHash(key.name));

            return seed;
        }
    };

    struct RootScanState {
        MountedRoot mountedRoot;

        std::unordered_map<quint64, InodeInfo> inodes;
        std::vector<DirEntry> entries;
        std::unordered_set<DirEntryKey, DirEntryKeyHash> seenEntries;

        std::unordered_map<quint64, std::vector<const DirEntry*>> entriesByChildInode;
        std::unordered_map<quint64, std::vector<const DirEntry*>> entriesByParentInode;
    };

    std::string decodeMountInfoField(const std::string& input)
    {
        std::string out;

        for (std::size_t i = 0; i < input.size(); ++i) {
            if (
                input[i] == '\\' &&
                i + 3 < input.size() &&
                input[i + 1] >= '0' && input[i + 1] <= '7' &&
                input[i + 2] >= '0' && input[i + 2] <= '7' &&
                input[i + 3] >= '0' && input[i + 3] <= '7'
            ) {
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

    std::vector<MountInfoEntry> readMountInfo()
    {
        std::ifstream file("/proc/self/mountinfo");
        std::vector<MountInfoEntry> entries;

        if (!file) {
            return entries;
        }

        std::string line;
        while (std::getline(file, line)) {
            const std::size_t separator = line.find(" - ");
            if (separator == std::string::npos) {
                continue;
            }

            const std::string left = line.substr(0, separator);
            const std::string right = line.substr(separator + 3);

            std::string id;
            std::string parent;
            std::string majorMinor;
            std::string root;
            std::string mountPoint;
            std::string mountOptions;

            {
                std::istringstream in(left);
                if (!(in >> id >> parent >> majorMinor >> root >> mountPoint >> mountOptions)) {
                    continue;
                }
            }

            std::string fsType;
            std::string mountSource;
            std::string superOptions;

            {
                std::istringstream in(right);
                if (!(in >> fsType >> mountSource >> superOptions)) {
                    continue;
                }
            }

            entries.push_back({
                decodeMountInfoField(root),
                decodeMountInfoField(mountPoint),
                decodeMountInfoField(fsType),
                decodeMountInfoField(mountSource),
                decodeMountInfoField(superOptions)
            });
        }

        return entries;
    }

    std::optional<std::string> optionValue(std::string_view options, std::string_view key)
    {
        std::size_t start = 0;

        while (start <= options.size()) {
            const std::size_t end = options.find(',', start);
            const std::string_view token = end == std::string_view::npos
                ? options.substr(start)
                : options.substr(start, end - start);

            const std::size_t equals = token.find('=');
            if (equals != std::string_view::npos) {
                const std::string_view tokenKey = token.substr(0, equals);
                const std::string_view tokenValue = token.substr(equals + 1);

                if (tokenKey == key) {
                    return std::string(tokenValue);
                }
            }

            if (end == std::string_view::npos) {
                break;
            }

            start = end + 1;
        }

        return std::nullopt;
    }

    std::optional<quint64> parseUnsigned(std::string_view text)
    {
        if (text.empty()) {
            return std::nullopt;
        }

        quint64 value = 0;

        for (const char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }

            const quint64 digit = static_cast<quint64>(c - '0');
            value = value * 10 + digit;
        }

        return value;
    }

    std::optional<quint64> parseSubvolIdFromSuperOptions(const std::string& superOptions)
    {
        const std::optional<std::string> value = optionValue(superOptions, "subvolid");
        if (!value) {
            return std::nullopt;
        }

        return parseUnsigned(*value);
    }

    QString parseSubvolPathFromSuperOptions(const std::string& superOptions)
    {
        const std::optional<std::string> value = optionValue(superOptions, "subvol");
        if (!value) {
            return {};
        }

        return QString::fromStdString(*value);
    }

    bool sameCanonicalPath(const QString& lhs, const QString& rhs)
    {
        namespace fs = std::filesystem;

        std::error_code lhsError;
        std::error_code rhsError;

        const fs::path lhsPath = fs::canonical(lhs.toStdString(), lhsError);
        const fs::path rhsPath = fs::canonical(rhs.toStdString(), rhsError);

        if (lhsError || rhsError) {
            return lhs == rhs;
        }

        return lhsPath == rhsPath;
    }

    std::vector<MountedRoot> mountedBtrfsRootsForMountPoints(const QStringList& mountPoints)
    {
        std::vector<MountedRoot> roots;
        const std::vector<MountInfoEntry> mountInfo = readMountInfo();

        for (const MountInfoEntry& entry : mountInfo) {
            if (entry.fsType != "btrfs") {
                continue;
            }

            const QString mountPoint = QString::fromStdString(entry.mountPoint);

            bool selected = mountPoints.isEmpty();
            for (const QString& requestedMountPoint : mountPoints) {
                if (sameCanonicalPath(mountPoint, requestedMountPoint)) {
                    selected = true;
                    break;
                }
            }

            if (!selected) {
                continue;
            }

            const std::optional<quint64> rootId =
                parseSubvolIdFromSuperOptions(entry.superOptions);

            if (!rootId || *rootId == 0) {
                continue;
            }

            MountedRoot root;
            root.mountPoint = mountPoint;
            root.mountRoot = QString::fromStdString(entry.root);
            root.mountSource = QString::fromStdString(entry.mountSource);
            root.rootId = *rootId;
            root.subvolPath = parseSubvolPathFromSuperOptions(entry.superOptions);

            roots.push_back(std::move(root));
        }

        std::sort(
            roots.begin(),
            roots.end(),
            [](const MountedRoot& lhs, const MountedRoot& rhs) {
                if (lhs.rootId != rhs.rootId) {
                    return lhs.rootId < rhs.rootId;
                }

                return lhs.mountPoint < rhs.mountPoint;
            }
        );

        roots.erase(
            std::unique(
                roots.begin(),
                roots.end(),
                [](const MountedRoot& lhs, const MountedRoot& rhs) {
                    return lhs.rootId == rhs.rootId &&
                           lhs.mountPoint == rhs.mountPoint;
                }
            ),
            roots.end()
        );

        return roots;
    }

    QString errnoText(const char* what)
    {
        return QStringLiteral("%1: %2")
            .arg(QString::fromUtf8(what), QString::fromUtf8(std::strerror(errno)));
    }

    template <typename T>
    T readUnaligned(const void* ptr)
    {
        T value{};
        std::memcpy(&value, ptr, sizeof(T));
        return value;
    }

    qint64 btrfsTimeToUnixSeconds(const btrfs_timespec& time)
    {
        return static_cast<qint64>(time.sec);
    }

    quint8 flagsFromMode(quint32 mode)
    {
        quint8 flags = 0;

        if (S_ISDIR(mode)) {
            flags |= 0x01;
        }

        if (S_ISLNK(mode)) {
            flags |= 0x02;
        }

        return flags;
    }

    bool isDirectoryFromBtrfsDirType(quint8 type)
    {
        return type == BTRFS_FT_DIR;
    }

    bool isSymlinkFromBtrfsDirType(quint8 type)
    {
        return type == BTRFS_FT_SYMLINK;
    }

    bool treeSearch(
        int fd,
        quint64 treeId,
        quint64 minObjectId,
        quint64 maxObjectId,
        quint32 minType,
        quint32 maxType,
        const std::function<bool(const btrfs_ioctl_search_header&, const char*)>& onItem,
        const ScannerHelper::CancelCallback& shouldCancel,
        QString* errorOut)
    {
        static constexpr std::size_t BufferSize = 1024 * 1024;

        std::vector<char> buffer(sizeof(btrfs_ioctl_search_args_v2) + BufferSize);
        auto* args = reinterpret_cast<btrfs_ioctl_search_args_v2*>(buffer.data());

        std::memset(args, 0, sizeof(*args));
        args->buf_size = BufferSize;

        btrfs_ioctl_search_key& key = args->key;
        key.tree_id = treeId;
        key.min_objectid = minObjectId;
        key.max_objectid = maxObjectId;
        key.min_type = minType;
        key.max_type = maxType;
        key.min_offset = 0;
        key.max_offset = static_cast<quint64>(-1);
        key.min_transid = 0;
        key.max_transid = static_cast<quint64>(-1);
        key.nr_items = 4096;

        while (true) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }

            const int rc = ::ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args);
            if (rc != 0) {
                if (errorOut) {
                    *errorOut = errnoText("BTRFS_IOC_TREE_SEARCH_V2 failed");
                }

                return false;
            }

            if (key.nr_items == 0) {
                return true;
            }

            char* itemPtr = reinterpret_cast<char*>(args->buf);
            btrfs_ioctl_search_header lastHeader{};

            for (quint32 i = 0; i < key.nr_items; ++i) {
                if (shouldCancel && shouldCancel()) {
                    return false;
                }

                const auto header = readUnaligned<btrfs_ioctl_search_header>(itemPtr);
                itemPtr += sizeof(btrfs_ioctl_search_header);

                if (!onItem(header, itemPtr)) {
                    return false;
                }

                itemPtr += header.len;
                lastHeader = header;
            }

            if (lastHeader.objectid == static_cast<quint64>(-1) &&
                lastHeader.type == static_cast<quint32>(-1) &&
                lastHeader.offset == static_cast<quint64>(-1)) {
                return true;
            }

            key.min_objectid = lastHeader.objectid;
            key.min_type = lastHeader.type;
            key.min_offset = lastHeader.offset + 1;

            if (key.min_offset == 0) {
                ++key.min_type;
                if (key.min_type > key.max_type) {
                    key.min_type = minType;
                    ++key.min_objectid;
                }
            }

            key.nr_items = 4096;
        }
    }

    bool scanInodeItems(
        int fd,
        RootScanState& root,
        const ScannerHelper::CancelCallback& shouldCancel,
        QString* errorOut)
    {
        return treeSearch(
            fd,
            root.mountedRoot.rootId,
            0,
            static_cast<quint64>(-1),
            BTRFS_INODE_ITEM_KEY,
            BTRFS_INODE_ITEM_KEY,
            [&root](const btrfs_ioctl_search_header& header, const char* data) {
                if (header.len < sizeof(btrfs_inode_item)) {
                    return true;
                }

                const auto item = readUnaligned<btrfs_inode_item>(data);

                InodeInfo info;
                info.inode = header.objectid;
                info.size = item.size;
                info.modificationTime = btrfsTimeToUnixSeconds(item.mtime);
                info.flags = flagsFromMode(item.mode);
                info.present = true;

                root.inodes[info.inode] = info;
                return true;
            },
            shouldCancel,
            errorOut
        );
    }

    bool scanDirectoryIndexItems(
        int fd,
        RootScanState& root,
        const std::unordered_set<quint64>& mountedRootIds,
        const BtrfsScannerEngine::DebugScanOptions& options,
        const ScannerHelper::CancelCallback& shouldCancel,
        QString* errorOut)
    {
        return treeSearch(
            fd,
            root.mountedRoot.rootId,
            0,
            static_cast<quint64>(-1),
            BTRFS_DIR_INDEX_KEY,
            BTRFS_DIR_INDEX_KEY,
            [&root, &mountedRootIds, &options](const btrfs_ioctl_search_header& header, const char* data) {
                if (header.len < sizeof(btrfs_dir_item)) {
                    return true;
                }

                const auto item = readUnaligned<btrfs_dir_item>(data);

                const quint64 childObjectId = item.location.objectid;
                const quint8 type = item.type;

                if (item.name_len == 0) {
                    return true;
                }

                const std::size_t nameOffset = sizeof(btrfs_dir_item);
                if (nameOffset + item.name_len > header.len) {
                    return true;
                }

                QString name = QString::fromUtf8(data + nameOffset, static_cast<int>(item.name_len));
                if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
                    return true;
                }

                quint64 childRootId = root.mountedRoot.rootId;
                quint64 childInode = childObjectId;

                // Btrfs encodes subvolume directory entries as type DIR with
                // location.type == BTRFS_ROOT_ITEM_KEY and location.objectid == root id.
                const bool isSubvolumeBoundary =
                    item.location.type == BTRFS_ROOT_ITEM_KEY;

                if (isSubvolumeBoundary) {
                    childRootId = childObjectId;
                    childInode = BTRFS_FIRST_FREE_OBJECTID;

                    if (options.skipUnmountedSubvolumeBoundaries &&
                        !mountedRootIds.contains(childRootId)) {
                        return true;
                    }
                }

                DirEntry entry;
                entry.rootId = root.mountedRoot.rootId;
                entry.parentInode = header.objectid;
                entry.childRootId = childRootId;
                entry.childInode = childInode;
                entry.name = std::move(name);
                entry.btrfsType = type;

                DirEntryKey key;
                key.rootId = entry.rootId;
                key.parentInode = entry.parentInode;
                key.childRootId = entry.childRootId;
                key.childInode = entry.childInode;
                key.name = entry.name;

                if (!root.seenEntries.insert(std::move(key)).second) {
                    return true;
                }

                root.entries.push_back(std::move(entry));
                return true;
            },
            shouldCancel,
            errorOut
        );
    }

    void buildEntryIndexes(RootScanState& root)
    {
        root.entriesByChildInode.clear();
        root.entriesByParentInode.clear();

        root.entriesByChildInode.reserve(root.entries.size());
        root.entriesByParentInode.reserve(root.entries.size());

        for (const DirEntry& entry : root.entries) {
            if (entry.childRootId == root.mountedRoot.rootId) {
                root.entriesByChildInode[entry.childInode].push_back(&entry);
            }

            root.entriesByParentInode[entry.parentInode].push_back(&entry);
        }
    }

    QString joinPath(const QString& parent, const QString& name)
    {
        if (parent.isEmpty() || parent == QStringLiteral("/")) {
            return QStringLiteral("/") + name;
        }

        return parent + QStringLiteral("/") + name;
    }

    QString reconstructPath(
        const RootScanState& root,
        quint64 inode,
        int depth = 0)
    {
        static constexpr int MaxDepth = 4096;

        if (inode == BTRFS_FIRST_FREE_OBJECTID) {
            return root.mountedRoot.mountPoint;
        }

        if (depth > MaxDepth) {
            return QStringLiteral("<path-depth-limit>");
        }

        const auto it = root.entriesByChildInode.find(inode);
        if (it == root.entriesByChildInode.end() || it->second.empty()) {
            return QStringLiteral("<unlinked-or-root>/%1").arg(inode);
        }

        const DirEntry* entry = it->second.front();
        if (!entry) {
            return QStringLiteral("<invalid-entry>");
        }

        const QString parentPath = reconstructPath(root, entry->parentInode, depth + 1);
        return joinPath(parentPath, entry->name);
    }

    QString reconstructEntryPath(
        const RootScanState& root,
        const DirEntry& entry)
    {
        if (entry.childRootId != root.mountedRoot.rootId) {
            return QStringLiteral("<mounted-subvolume-boundary rootId=%1 name=%2>")
                .arg(entry.childRootId)
                .arg(entry.name);
        }

        const QString parentPath = entry.parentInode == BTRFS_FIRST_FREE_OBJECTID
            ? root.mountedRoot.mountPoint
            : reconstructPath(root, entry.parentInode);

        return joinPath(parentPath, entry.name);
    }

    void printMountTable(const std::vector<RootScanState>& roots)
    {
        std::cout << "BTRFS mounted root table\n";

        for (const RootScanState& root : roots) {
            std::cout << "  rootId=" << root.mountedRoot.rootId
                      << " mountPoint=" << root.mountedRoot.mountPoint.toStdString()
                      << " mountRoot=" << root.mountedRoot.mountRoot.toStdString()
                      << " subvol=" << root.mountedRoot.subvolPath.toStdString()
                      << " source=" << root.mountedRoot.mountSource.toStdString()
                      << "\n";
        }
    }

    void printRecords(const RootScanState& root)
    {
        for (const DirEntry& entry : root.entries) {
            const auto inodeIt = root.inodes.find(entry.childInode);

            quint64 size = 0;
            qint64 mtime = 0;
            quint8 flags = 0;

            if (inodeIt != root.inodes.end()) {
                size = inodeIt->second.size;
                mtime = inodeIt->second.modificationTime;
                flags = inodeIt->second.flags;
            } else {
                if (isDirectoryFromBtrfsDirType(entry.btrfsType)) {
                    flags |= 0x01;
                }

                if (isSymlinkFromBtrfsDirType(entry.btrfsType)) {
                    flags |= 0x02;
                }
            }

            const QString path = reconstructEntryPath(root, entry);
            const bool isSubvolumeBoundary = entry.childRootId != entry.rootId;

            // Boundary entries are debug-only and must not become normal FileRecords.
            std::cout << (isSubvolumeBoundary ? "BTRFS_BOUNDARY" : "BTRFS_RECORD")
                      << " rootId=" << entry.rootId
                      << " inode=" << entry.childInode
                      << " parentRootId=" << entry.rootId
                      << " parentInode=" << entry.parentInode
                      << " childRootId=" << entry.childRootId
                      << " boundary=" << (isSubvolumeBoundary ? 1 : 0)
                      << " type=" << static_cast<unsigned>(entry.btrfsType)
                      << " flags=" << static_cast<unsigned>(flags)
                      << " size=" << size
                      << " mtime=" << mtime
                      << " name=\"" << entry.name.toStdString() << "\""
                      << " path=\"" << path.toStdString() << "\""
                      << "\n";
        }
    }

    void printSummary(const std::vector<RootScanState>& roots)
    {
        std::size_t totalInodes = 0;
        std::size_t totalEntries = 0;

        for (const RootScanState& root : roots) {
            totalInodes += root.inodes.size();
            totalEntries += root.entries.size();

            std::cout << "BTRFS_ROOT_SUMMARY"
                      << " rootId=" << root.mountedRoot.rootId
                      << " mountPoint=" << root.mountedRoot.mountPoint.toStdString()
                      << " inodes=" << root.inodes.size()
                      << " entries=" << root.entries.size()
                      << "\n";
        }

        std::cout << "BTRFS_SUMMARY"
                  << " roots=" << roots.size()
                  << " inodes=" << totalInodes
                  << " entries=" << totalEntries
                  << "\n";
    }
}

bool BtrfsScannerEngine::debugScanMountedFilesystem(
    const QString& devicePath,
    const QStringList& mountPoints,
    const DebugScanOptions& options,
    const ScannerHelper::ErrorCallback& onError,
    const ScannerHelper::CancelCallback& shouldCancel)
{
    if (shouldCancel && shouldCancel()) {
        return false;
    }

    const std::vector<MountedRoot> mountedRoots =
        mountedBtrfsRootsForMountPoints(mountPoints);

    if (mountedRoots.empty()) {
        if (onError) {
            onError(QStringLiteral("No mounted Btrfs subvolumes found for %1").arg(devicePath));
        }

        return false;
    }

    UniqueFd fd(::open(mountedRoots.front().mountPoint.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!fd.valid()) {
        if (onError) {
            onError(errnoText("Failed to open Btrfs mount point"));
        }

        return false;
    }

    std::unordered_set<quint64> mountedRootIds;
    mountedRootIds.reserve(mountedRoots.size());

    for (const MountedRoot& root : mountedRoots) {
        mountedRootIds.insert(root.rootId);
    }

    std::vector<RootScanState> rootStates;
    rootStates.reserve(mountedRoots.size());

    for (const MountedRoot& mountedRoot : mountedRoots) {
        if (shouldCancel && shouldCancel()) {
            return false;
        }

        RootScanState state;
        state.mountedRoot = mountedRoot;

        QString errorText;
        if (!scanInodeItems(fd.fd, state, shouldCancel, &errorText)) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }

            if (onError) {
                onError(QStringLiteral("Btrfs inode scan failed for rootId=%1: %2")
                    .arg(state.mountedRoot.rootId)
                    .arg(errorText));
            }

            return false;
        }

        if (!scanDirectoryIndexItems(fd.fd, state, mountedRootIds, options, shouldCancel, &errorText)) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }

            if (onError) {
                onError(QStringLiteral("Btrfs directory scan failed for rootId=%1: %2")
                    .arg(state.mountedRoot.rootId)
                    .arg(errorText));
            }

            return false;
        }

        buildEntryIndexes(state);
        rootStates.push_back(std::move(state));
    }

    if (options.printMountTable) {
        printMountTable(rootStates);
    }

    if (options.printRecords) {
        for (const RootScanState& root : rootStates) {
            printRecords(root);
        }
    }

    if (options.printSummary) {
        printSummary(rootStates);
    }

    return true;
}