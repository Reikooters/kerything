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
#include <limits>
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
    /*
     * Small RAII wrapper for file descriptors.
     *
     * The Btrfs tree-search ioctl is issued against any fd inside the mounted
     * filesystem. We open one mounted Btrfs root directory and reuse that fd for
     * all root/tree searches.
     */
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

    /*
     * Minimal /proc/self/mountinfo representation.
     *
     * For Btrfs, mountinfo is important because a single block device can expose
     * many mounted subvolumes. The mountinfo "root" field is the mounted filesystem
     * root, e.g. "/@", "/@home", "/@cache", while the mount point is the VFS path,
     * e.g. "/", "/home", "/var/cache".
     */
    struct MountInfoEntry {
        std::string root;
        std::string mountPoint;
        std::string fsType;
        std::string mountSource;
        std::string superOptions;
    };

    /*
     * One mounted Btrfs root/subvolume selected for scanning.
     *
     * rootId is the Btrfs subvolume/root id, commonly shown as "subvolid=..."
     * in mount options. Btrfs object ids/inode numbers are only unique within a
     * root, so Kerything must treat (rootId, inode) as the true filesystem identity.
     */
    struct MountedRoot {
        QString mountPoint;
        QString mountRoot;
        QString mountSource;

        quint64 rootId = 0;
        QString subvolPath;
    };

    /*
     * Metadata read from BTRFS_INODE_ITEM_KEY items.
     *
     * Directory indexes tell us that a name exists under a parent directory, but
     * the inode item carries file metadata such as size, mode, and timestamps.
     * The scanner first builds this root-local inode map, then joins directory
     * entries against it while emitting FileRecord objects.
     */
    struct InodeInfo {
        quint64 inode = 0;
        quint64 size = 0;
        qint64 modificationTime = 0;
        quint8 flags = 0;
        bool present = false;
    };

    /*
     * Buffered output state for the daemon -> GUI scan pipeline.
     *
     * Btrfs records are produced from several mounted roots. Instead of sending
     * one IPC message per file, records, namespace sidecars, and string-pool bytes
     * are accumulated into chunks sized to stay below the protocol message limit.
     */
    struct BtrfsStreamState {
        std::vector<FileRecord> records;
        std::vector<FileRecordNamespace> namespaces;
        std::vector<char> stringPool;

        uint32_t totalStringPoolLength = 0;

        static constexpr uint32_t kTargetIpcBufferSizeMB = 4;
        static constexpr uint32_t kMaxIpcBufferSizeBytes =
            (kTargetIpcBufferSizeMB * 1024 * 1024) -
            Protocol::HeaderSize -
            sizeof(Protocol::ScanIndexResultChunkType);

        static constexpr uint32_t kRecordsPerIpcChunk =
            kMaxIpcBufferSizeBytes /
            (sizeof(FileRecord) + sizeof(FileRecordNamespace));

        bool flush(
            const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
            const ScannerHelper::FileRecordNamespaceChunkCallback& onFileRecordNamespaceChunk,
            const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
        {
            /*
             * FileRecord and FileRecordNamespace chunks must remain aligned:
             * record N in the file-record chunk belongs to namespace sidecar N in
             * the namespace chunk. The GUI validates this after scan completion.
             */
            if (!records.empty()) {
                std::vector<FileRecord> fileRecordChunk = std::move(records);
                std::vector<FileRecordNamespace> namespaceChunk = std::move(namespaces);

                records.clear();
                namespaces.clear();

                records.reserve(kRecordsPerIpcChunk);
                namespaces.reserve(kRecordsPerIpcChunk);

                if (!onFileRecordChunk(fileRecordChunk)) {
                    std::cerr << "[BtrfsScannerEngine] scan aborted by file record receiver\n";
                    return false;
                }

                if (!onFileRecordNamespaceChunk(namespaceChunk)) {
                    std::cerr << "[BtrfsScannerEngine] scan aborted by namespace receiver\n";
                    return false;
                }
            }

            if (!stringPool.empty()) {
                std::vector<char> stringPoolChunk = std::move(stringPool);

                /*
                 * FileRecord::nameOffset is an absolute offset in the complete
                 * string pool, not an offset relative to the current IPC chunk.
                 * totalStringPoolLength tracks the number of string bytes already
                 * flushed so newly emitted records can point to the final combined
                 * pool after the GUI appends all chunks together.
                 */
                totalStringPoolLength += static_cast<uint32_t>(stringPoolChunk.size());

                stringPool.clear();
                stringPool.reserve(kMaxIpcBufferSizeBytes);

                if (!onStringPoolChunk(stringPoolChunk)) {
                    std::cerr << "[BtrfsScannerEngine] scan aborted by string pool receiver\n";
                    return false;
                }
            }

            return true;
        }

        bool addRecord(
            quint64 rootId,
            quint64 inode,
            quint64 parentRootId,
            quint64 parentInode,
            std::string_view name,
            quint64 size,
            qint64 modificationTime,
            quint8 flags,
            const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
            const ScannerHelper::FileRecordNamespaceChunkCallback& onFileRecordNamespaceChunk,
            const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk)
        {
            /*
             * FileRecord stores name length in a quint16. Extremely long names are
             * not expected on normal Linux filesystems, but skipping is safer than
             * truncating because truncation would create misleading search results.
             */
            if (name.size() > std::numeric_limits<quint16>::max()) {
                return true;
            }

            if (records.size() >= kRecordsPerIpcChunk ||
                stringPool.size() + name.size() >= kMaxIpcBufferSizeBytes) {
                if (!flush(
                        onFileRecordChunk,
                        onFileRecordNamespaceChunk,
                        onStringPoolChunk)) {
                    return false;
                }
            }

            FileRecord record{};
            record.fsIndex = inode;
            record.parentFsIndex = parentInode;
            record.parentRecordIdx = 0xFFFFFFFF;
            record.size = size;
            record.modificationTime = modificationTime;
            record.nameOffset = totalStringPoolLength + static_cast<uint32_t>(stringPool.size());
            record.nameLen = static_cast<quint16>(name.size());
            record.flags = flags;

            /*
             * Btrfs inode/object ids are root-local. Two subvolumes may both have
             * object id 12345, and they are different filesystem objects. The normal
             * FileRecord fields keep the object ids; this sidecar provides the Btrfs
             * root/subvolume namespace for the record and its parent.
             */
            FileRecordNamespace namespaceEntry{};
            namespaceEntry.fsNamespace = rootId;
            namespaceEntry.parentFsNamespace = parentRootId;

            records.push_back(record);
            namespaces.push_back(namespaceEntry);
            stringPool.insert(stringPool.end(), name.begin(), name.end());

            return true;
        }
    };

    /*
     * One directory entry found in a Btrfs root.
     *
     * rootId/parentInode identify the directory containing this name.
     * childRootId/childInode identify the object named by the entry.
     *
     * For ordinary files and directories, childRootId == rootId.
     * For a subvolume boundary, childRootId is the target subvolume root id and
     * childInode is BTRFS_FIRST_FREE_OBJECTID, the root directory object id inside
     * that target subvolume.
     */
    struct DirEntry {
        quint64 rootId = 0;
        quint64 parentInode = 0;
        quint64 childRootId = 0;
        quint64 childInode = 0;

        QString name;
        quint8 btrfsType = 0;
    };

    /*
     * Deduplication key for directory entries.
     *
     * The same directory item may be encountered through index/ref variations or
     * repeated scan continuation. Keep only one logical name -> object mapping per
     * parent/root to avoid duplicate FileRecords.
     */
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

    /*
     * All temporary data collected for one mounted Btrfs root.
     *
     * The scanner processes each mounted root independently because directory
     * entries and inode items live in root-specific trees. The final index then
     * combines them using namespace-aware FileRecord sidecars.
     */
    struct RootScanState {
        MountedRoot mountedRoot;

        std::unordered_map<quint64, InodeInfo> inodes;
        std::vector<DirEntry> entries;
        std::unordered_set<DirEntryKey, DirEntryKeyHash> seenEntries;

        /*
         * Debug/path-reconstruction indexes. These are not used as the final
         * Kerything parent pointers; final parent pointers are resolved later in
         * IndexController using (namespace, inode) keys.
         */
        std::unordered_map<quint64, std::vector<const DirEntry*>> entriesByChildInode;
        std::unordered_map<quint64, std::vector<const DirEntry*>> entriesByParentInode;
    };

    std::string decodeMountInfoField(const std::string& input)
    {
        /*
         * /proc/self/mountinfo escapes special characters using octal sequences,
         * e.g. a space appears as "\040". Decode those so comparisons against
         * QString mount points work for paths containing whitespace or other
         * escaped characters.
         */
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
        /*
         * mountinfo format is:
         *
         *   id parent major:minor root mountPoint mountOptions optional... -
         *   fsType mountSource superOptions
         *
         * Only a small subset is needed here:
         *   - root:        mounted filesystem root, e.g. "/@cache"
         *   - mountPoint:  visible VFS path, e.g. "/var/cache"
         *   - fsType:      must be "btrfs"
         *   - mountSource: block device or source
         *   - superOptions: contains "subvolid=..." and usually "subvol=..."
         */
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
        /*
         * Parse comma-separated mount option strings such as:
         *
         *   ro,ssd,space_cache=v2,subvolid=260,subvol=/@cache
         *
         * This intentionally handles only simple key=value options because that is
         * all we need for subvolid/subvol extraction.
         */
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
        /*
         * Device discovery and mountinfo may spell the same mount point differently
         * through symlinks or relative path components. Prefer canonical comparison
         * when both paths exist, but fall back to direct string comparison if either
         * path cannot be resolved.
         */
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
        /*
         * Convert the application's selected mount points into Btrfs roots.
         *
         * A single Btrfs filesystem can be mounted many times, once per subvolume.
         * For example:
         *
         *   /          -> subvolid=256 subvol=/@
         *   /home      -> subvolid=257 subvol=/@home
         *   /var/cache -> subvolid=260 subvol=/@cache
         *
         * Kerything indexes each selected mounted root separately and later exposes
         * records only through compatible mount points.
         */
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

        /*
         * Avoid scanning the exact same mounted root/mount-point pair twice if it
         * appears duplicated in the input. Multiple different mount points for the
         * same root id are intentionally not collapsed here because those may affect
         * result visibility elsewhere.
         */
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
        /*
         * Btrfs ioctl search results are byte-packed in a buffer. Do not cast the
         * data pointer directly to a struct pointer: it may be unaligned and that
         * would be undefined behaviour on some architectures. memcpy into a local
         * object is safe and lets the compiler optimize appropriately.
         */
        T value{};
        std::memcpy(&value, ptr, sizeof(T));
        return value;
    }

    qint64 btrfsTimeToUnixSeconds(const btrfs_timespec& time)
    {
        /*
         * Kerything stores modification time as Unix seconds. Btrfs also provides
         * nanoseconds, but the current FileRecord model does not store sub-second
         * precision.
         */
        return static_cast<qint64>(time.sec);
    }

    quint8 flagsFromMode(quint32 mode)
    {
        /*
         * Convert POSIX mode bits from the Btrfs inode item into Kerything's compact
         * FileRecord flag set. Other file types are currently left as plain files.
         */
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

    bool isSubvolumeBoundaryEntry(const DirEntry& entry) noexcept
    {
        return entry.childRootId != entry.rootId;
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
        /*
         * Thin wrapper around BTRFS_IOC_TREE_SEARCH_V2.
         *
         * treeId is the Btrfs root/subvolume id to search. The remaining bounds form
         * a key range. Btrfs keys are ordered lexicographically by:
         *
         *   (objectid, type, offset)
         *
         * The scanner uses this helper for two passes:
         *   1. BTRFS_INODE_ITEM_KEY: metadata by inode/objectid
         *   2. BTRFS_DIR_INDEX_KEY:  directory names by parent directory objectid
         *
         * Important: the ioctl continuation behaviour can still yield keys outside
         * the logical type range requested by the caller. Therefore each returned
         * header is filtered below before its payload is interpreted. Without that
         * guard, non-inode payloads such as extent data can be misread as inode
         * structs, producing nonsense sizes/timestamps.
         */
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

                /*
                 * Each returned item is:
                 *
                 *   btrfs_ioctl_search_header
                 *   payload bytes of length header.len
                 *
                 * The payload type is determined solely by header.type. Never parse
                 * itemPtr as a specific Btrfs struct until header.type/header.len have
                 * been validated by this helper and by the caller.
                 */
                const auto header = readUnaligned<btrfs_ioctl_search_header>(itemPtr);
                itemPtr += sizeof(btrfs_ioctl_search_header);

                /*
                 * Defensive type/range filter.
                 *
                 * This is intentionally silent: out-of-range keys are expected during
                 * broad object-id scans. The caller asked for a logical range, so only
                 * items inside that range may be passed to onItem.
                 */
                if (header.objectid < minObjectId ||
                    header.objectid > maxObjectId ||
                    header.type < minType ||
                    header.type > maxType) {
                    itemPtr += header.len;
                    lastHeader = header;
                    continue;
                }

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

            /*
             * Continue after the last key returned by the previous ioctl call.
             *
             * Because Btrfs keys are ordered as (objectid, type, offset), incrementing
             * offset is normally enough. If offset overflows, advance type; if type
             * moves beyond the caller's maximum type, wrap type back to the requested
             * minimum and advance objectid.
             */
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

            /*
             * The kernel overwrites nr_items with the number of items actually
             * returned. Reset it before the next ioctl call to request another full
             * batch.
             */
            key.nr_items = 4096;
        }
    }

    bool scanInodeItems(
        int fd,
        RootScanState& root,
        const ScannerHelper::CancelCallback& shouldCancel,
        QString* errorOut)
    {
        /*
         * Read all inode metadata items for this mounted Btrfs root.
         *
         * BTRFS_INODE_ITEM_KEY payloads are fixed-size btrfs_inode_item structs.
         * Their objectid is the inode/object id. Later, directory entries reference
         * these same object ids so metadata can be joined onto names.
         */
        return treeSearch(
            fd,
            root.mountedRoot.rootId,
            0,
            static_cast<quint64>(-1),
            BTRFS_INODE_ITEM_KEY,
            BTRFS_INODE_ITEM_KEY,
            [&root](const btrfs_ioctl_search_header& header, const char* data) {
                /*
                 * Be strict before parsing.
                 *
                 * This guard prevents the historical failure mode where non-inode
                 * items returned by BTRFS_IOC_TREE_SEARCH_V2 were interpreted as
                 * btrfs_inode_item payloads, producing huge bogus file sizes and
                 * invalid timestamps.
                 */
                if (header.type != BTRFS_INODE_ITEM_KEY ||
                    header.offset != 0 ||
                    header.len != sizeof(btrfs_inode_item)) {
#ifdef KERYTHING_ENABLE_LOGGING
                    std::cerr << "[BtrfsScannerEngine] skipping non-canonical inode item"
                              << " rootId=" << root.mountedRoot.rootId
                              << " objectid=" << header.objectid
                              << " type=" << header.type
                              << " expectedType=" << BTRFS_INODE_ITEM_KEY
                              << " offset=" << header.offset
                              << " len=" << header.len
                              << " sizeofInodeItem=" << sizeof(btrfs_inode_item)
                              << "\n";
#endif
                    return true;
                }

                const auto item = readUnaligned<btrfs_inode_item>(data);

                InodeInfo info;
                info.inode = header.objectid;
                info.size = item.size;
                info.modificationTime = btrfsTimeToUnixSeconds(item.mtime);
                info.flags = flagsFromMode(item.mode);
                info.present = true;

                /*
                 * Store by object id inside this root. The root id namespace is
                 * implicit because each RootScanState represents exactly one Btrfs
                 * root/subvolume.
                 */
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
        /*
         * Read directory index items for this root.
         *
         * BTRFS_DIR_INDEX_KEY items are keyed by the parent directory object id.
         * The payload contains one or more packed btrfs_dir_item records. Each
         * record is followed by its UTF-8 name bytes and optional data bytes.
         * These entries provide the name and parent relationship; inode metadata
         * is joined separately using the child object's object id.
         */
        return treeSearch(
            fd,
            root.mountedRoot.rootId,
            0,
            static_cast<quint64>(-1),
            BTRFS_DIR_INDEX_KEY,
            BTRFS_DIR_INDEX_KEY,
            [&root, &mountedRootIds, &options](const btrfs_ioctl_search_header& header, const char* data) {
                /*
                 * treeSearch already filters by type, but keep this local check so
                 * future refactors cannot accidentally parse the wrong payload type.
                 */
                if (header.type != BTRFS_DIR_INDEX_KEY) {
                    return true;
                }

                std::size_t offset = 0;

                while (offset < header.len) {
                    if (header.len - offset < sizeof(btrfs_dir_item)) {
#ifdef KERYTHING_ENABLE_LOGGING
                        std::cerr << "[BtrfsScannerEngine] skipping truncated packed dir item"
                                  << " rootId=" << root.mountedRoot.rootId
                                  << " parentObjectId=" << header.objectid
                                  << " offset=" << offset
                                  << " remaining=" << (header.len - offset)
                                  << " len=" << header.len
                                  << "\n";
#endif
                        break;
                    }

                    const auto item = readUnaligned<btrfs_dir_item>(data + offset);

                    const std::size_t nameOffset = offset + sizeof(btrfs_dir_item);
                    const std::size_t nameLength = item.name_len;
                    const std::size_t dataOffset = nameOffset + nameLength;
                    const std::size_t dataLength = item.data_len;
                    const std::size_t nextOffset = dataOffset + dataLength;

                    /*
                     * Each packed record is:
                     *
                     *   btrfs_dir_item
                     *   name bytes
                     *   optional data bytes
                     *
                     * Validate the whole record before using any variable-length
                     * fields. Include data_len in the stride even though normal
                     * directory entries usually have no data payload.
                     */
                    if (nameOffset > header.len ||
                        dataOffset > header.len ||
                        nextOffset > header.len ||
                        nextOffset <= offset) {
#ifdef KERYTHING_ENABLE_LOGGING
                        std::cerr << "[BtrfsScannerEngine] skipping malformed packed dir item"
                                  << " rootId=" << root.mountedRoot.rootId
                                  << " parentObjectId=" << header.objectid
                                  << " offset=" << offset
                                  << " nameLen=" << item.name_len
                                  << " dataLen=" << item.data_len
                                  << " len=" << header.len
                                  << "\n";
#endif
                        break;
                    }

                    offset = nextOffset;

                    if (item.name_len == 0) {
                        continue;
                    }

                    const quint64 childObjectId = item.location.objectid;
                    const quint8 type = item.type;

                    QString name = QString::fromUtf8(
                        data + nameOffset,
                        static_cast<int>(item.name_len)
                    );

                    if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
                        continue;
                    }

                    quint64 childRootId = root.mountedRoot.rootId;
                    quint64 childInode = childObjectId;

                    /*
                     * Subvolume boundary handling.
                     *
                     * Btrfs represents a subvolume as a directory-like entry in the
                     * parent root, but the entry's location points to a ROOT_ITEM rather
                     * than an inode item. The objectid is the child subvolume's root id.
                     *
                     * For Kerything, this means the visible directory name belongs in the
                     * parent root, while its contents live in a different root namespace.
                     */
                    const bool isSubvolumeBoundary =
                        item.location.type == BTRFS_ROOT_ITEM_KEY;

                    if (isSubvolumeBoundary) {
                        childRootId = childObjectId;
                        childInode = BTRFS_FIRST_FREE_OBJECTID;

                        /*
                         * Mounted-only policy.
                         *
                         * Do not silently descend into child subvolumes that are present
                         * in Btrfs metadata but not mounted/selected. If a child subvolume
                         * is mounted, it will be scanned as its own RootScanState and
                         * records will carry that child root id as their namespace.
                         */
                        if (options.skipUnmountedSubvolumeBoundaries &&
                            !mountedRootIds.contains(childRootId)) {
                            continue;
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

                    /*
                     * Avoid duplicate logical entries. Btrfs can expose both directory
                     * index/ref information, and future scanner changes may widen which
                     * item types are visited. Deduplicating here keeps output stable.
                     */
                    if (!root.seenEntries.insert(std::move(key)).second) {
                        continue;
                    }

                    root.entries.push_back(std::move(entry));
                }

                return true;
            },
            shouldCancel,
            errorOut
        );
    }

    void buildEntryIndexes(RootScanState& root)
    {
        /*
         * Build lightweight indexes used for debug path reconstruction.
         *
         * entriesByChildInode intentionally indexes only entries whose child object
         * is in the same root. Subvolume boundary entries point into another Btrfs
         * root namespace and cannot be reconstructed by walking this root's parent
         * chain alone.
         */
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
        /*
         * Debug-only path reconstruction from directory entries.
         *
         * The production index does not use this for final parent pointers. It is
         * only used by debug printing to make raw Btrfs object ids human-readable.
         */
        static constexpr int MaxDepth = 4096;

        if (inode == BTRFS_FIRST_FREE_OBJECTID) {
            return root.mountedRoot.mountPoint;
        }

        /*
         * Defensive recursion limit. A valid directory tree should not approach
         * this depth, but corrupted metadata or a scanner bug should not recurse
         * forever while printing diagnostics.
         */
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
        /*
         * Debug helper for a single directory entry.
         *
         * Subvolume boundary entries cannot be rendered as normal child paths here
         * because their child object lives in another root namespace. Make that
         * explicit in debug output rather than pretending the boundary is a regular
         * directory record.
         */
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
        /*
         * Diagnostic dump of the root-local directory entries after inode metadata
         * has been joined where possible. This is useful when comparing scanner
         * output with `find`, `stat`, Dolphin, or `btrfs inspect-internal`.
         */
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

            /*
             * Boundary entries are printed for diagnosis only. In the real scan,
             * mounted child subvolumes are represented by their own root record and
             * their own entries, with FileRecordNamespace preserving root identity.
             */
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

    const RootScanState* findRootStateById(
        const std::vector<RootScanState>& roots,
        quint64 rootId)
    {
        const auto it = std::find_if(
            roots.begin(),
            roots.end(),
            [rootId](const RootScanState& root) {
                return root.mountedRoot.rootId == rootId;
            }
        );

        if (it == roots.end()) {
            return nullptr;
        }

        return &*it;
    }
}

bool BtrfsScannerEngine::scanMountedFilesystem(
    const QString& devicePath,
    const QStringList& mountPoints,
    const ScannerHelper::FileRecordChunkCallback& onFileRecordChunk,
    const ScannerHelper::FileRecordNamespaceChunkCallback& onFileRecordNamespaceChunk,
    const ScannerHelper::StringPoolChunkCallback& onStringPoolChunk,
    const ScannerHelper::ErrorCallback& onError,
    const ScannerHelper::CancelCallback& shouldCancel,
    const ScannerHelper::ProgressCallback& onProgress)
{
    /*
     * Production Btrfs scan flow:
     *
     *   1. Resolve selected VFS mount points to mounted Btrfs roots/subvolumes.
     *   2. Open one Btrfs mount point to obtain an ioctl-capable fd.
     *   3. For each mounted root:
     *        a. scan inode items into root-local metadata map
     *        b. scan directory index items into root-local name/parent entries
     *   4. Stream synthetic root records and directory-entry records into the
     *      normal Kerything FileRecord pipeline.
     *
     * Btrfs-specific identity is preserved by FileRecordNamespace sidecars:
     *
     *   FileRecord::fsIndex         = Btrfs objectid/inode
     *   FileRecordNamespace::fsNamespace = Btrfs root/subvolume id
     */
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

    UniqueFd fd(::open(
        mountedRoots.front().mountPoint.toLocal8Bit().constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    ));

    /*
     * BTRFS_IOC_TREE_SEARCH_V2 works through an fd on the mounted Btrfs filesystem.
     * It does not need a separate fd per subvolume; the tree_id in the search key
     * selects which Btrfs root/subvolume tree to search.
     */
    if (!fd.valid()) {
        if (onError) {
            onError(errnoText("Failed to open Btrfs mount point"));
        }

        return false;
    }

    std::unordered_set<quint64> mountedRootIds;
    mountedRootIds.reserve(mountedRoots.size());

    /*
     * Fast lookup used when a directory entry crosses a subvolume boundary.
     * We only keep such boundary entries if the target root is also selected for
     * this scan.
     */
    for (const MountedRoot& root : mountedRoots) {
        mountedRootIds.insert(root.rootId);
    }

    DebugScanOptions options;
    options.printMountTable = false;
    options.printRecords = false;
    options.printSummary = false;

    /*
     * Phase 1 policy:
     * only cross into subvolume roots that are also mounted/selected for this scan.
     *
     * This avoids silently indexing hidden/unmounted subvolumes that the GUI cannot
     * yet expand to a stable visible mount path.
     */
    options.skipUnmountedSubvolumeBoundaries = true;

    std::vector<RootScanState> rootStates;
    rootStates.reserve(mountedRoots.size());

    if (onProgress) {
        onProgress(Protocol::ScanProgress{
            .phase = QStringLiteral("Reading Btrfs roots"),
            .unit = QStringLiteral("roots"),
            .processed = 0,
            .total = static_cast<quint64>(mountedRoots.size())
        });
    }

    quint64 rootsProcessed = 0;

    for (const MountedRoot& mountedRoot : mountedRoots) {
        if (shouldCancel && shouldCancel()) {
            return false;
        }

        /*
         * Scan this root/subvolume in isolation. Object ids read during these two
         * passes are meaningful only inside mountedRoot.rootId.
         */
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

        if (!scanDirectoryIndexItems(
                fd.fd,
                state,
                mountedRootIds,
                options,
                shouldCancel,
                &errorText)) {
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

        /*
         * Build debug/path helper indexes while state.entries is still stable.
         * These indexes store pointers into state.entries, so they must be rebuilt
         * after all entries have been pushed and before the state is moved into
         * rootStates.
         */
        buildEntryIndexes(state);
        rootStates.push_back(std::move(state));

        ++rootsProcessed;

        if (onProgress) {
            onProgress(Protocol::ScanProgress{
                .phase = QStringLiteral("Reading Btrfs roots"),
                .unit = QStringLiteral("roots"),
                .processed = rootsProcessed,
                .total = static_cast<quint64>(mountedRoots.size())
            });
        }
    }

    BtrfsStreamState stream;
    stream.records.reserve(BtrfsStreamState::kRecordsPerIpcChunk);
    stream.namespaces.reserve(BtrfsStreamState::kRecordsPerIpcChunk);
    stream.stringPool.reserve(BtrfsStreamState::kMaxIpcBufferSizeBytes);

    /*
     * Progress estimate: one emitted root record per mounted root, plus one record
     * per streamable directory entry. Subvolume boundary entries are excluded because
     * mounted child subvolumes are represented by their own synthetic root records.
     */
    std::size_t estimatedEntryCount = 0;
    for (const RootScanState& root : rootStates) {
        estimatedEntryCount += std::count_if(
            root.entries.begin(),
            root.entries.end(),
            [](const DirEntry& entry) {
                return !isSubvolumeBoundaryEntry(entry);
            }
        );
    }

    if (onProgress) {
        onProgress(Protocol::ScanProgress{
            .phase = QStringLiteral("Streaming Btrfs records"),
            .unit = QStringLiteral("records"),
            .processed = 0,
            .total = static_cast<quint64>(estimatedEntryCount + rootStates.size())
        });
    }

    quint64 recordsStreamed = 0;

    for (const RootScanState& root : rootStates) {
        if (shouldCancel && shouldCancel()) {
            return false;
        }

        quint64 rootSize = 0;
        qint64 rootModificationTime = 0;
        quint8 rootFlags = FileRecord_IsDir;

        /*
         * BTRFS_FIRST_FREE_OBJECTID is the conventional object id for a Btrfs root
         * directory. Use its inode item metadata for the synthetic namespace root
         * record when available.
         */
        const auto rootInodeIt = root.inodes.find(BTRFS_FIRST_FREE_OBJECTID);
        if (rootInodeIt != root.inodes.end()) {
            rootSize = rootInodeIt->second.size;
            rootModificationTime = rootInodeIt->second.modificationTime;
            rootFlags = rootInodeIt->second.flags | FileRecord_IsDir;
        }

        /*
         * Emit one synthetic/real root record per mounted Btrfs root.
         *
         * Empty name plus self-parent means this root expands as the top of the
         * indexed namespace. Path/mount expansion will become more precise in
         * Phase 2.
         */
        if (!stream.addRecord(
                root.mountedRoot.rootId,
                BTRFS_FIRST_FREE_OBJECTID,
                root.mountedRoot.rootId,
                BTRFS_FIRST_FREE_OBJECTID,
                std::string_view{},
                rootSize,
                rootModificationTime,
                rootFlags,
                onFileRecordChunk,
                onFileRecordNamespaceChunk,
                onStringPoolChunk)) {
            return false;
        }

        ++recordsStreamed;

        for (const DirEntry& entry : root.entries) {
            if (shouldCancel && shouldCancel()) {
                return false;
            }

            /*
             * Do not emit Btrfs subvolume boundary entries as normal search
             * results.
             *
             * A boundary entry is a directory-like name in one root whose location
             * points to another Btrfs root/subvolume. If that child subvolume is
             * mounted and selected, it is scanned separately and represented by its
             * own synthetic root record with an empty name.
             *
             * Emitting the boundary entry itself would create fake paths such as:
             *
             *   /mnt/cachyos/@
             *   /mnt/cachyos/home/@home
             *   /mnt/cachyos/var/cache/@cache
             *
             * which do not exist in the mounted VFS view.
             */
            if (isSubvolumeBoundaryEntry(entry)) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cerr << "[BtrfsScannerEngine] skipping subvolume boundary record"
                          << " parentRootId=" << entry.rootId
                          << " childRootId=" << entry.childRootId
                          << " parentInode=" << entry.parentInode
                          << " childInode=" << entry.childInode
                          << " name=" << entry.name.toStdString()
                          << "\n";
#endif
                continue;
            }

            const RootScanState* childRoot =
                findRootStateById(rootStates, entry.childRootId);

            const InodeInfo* inodeInfo = nullptr;
            if (childRoot) {
                const auto inodeIt = childRoot->inodes.find(entry.childInode);
                if (inodeIt != childRoot->inodes.end()) {
                    inodeInfo = &inodeIt->second;
                }
            }

            quint64 size = 0;
            qint64 modificationTime = 0;
            quint8 flags = 0;

            if (inodeInfo) {
                size = inodeInfo->size;
                modificationTime = inodeInfo->modificationTime;
                flags = inodeInfo->flags;
            }
            else {
                /*
                 * If inode metadata is unavailable, still emit a usable record from
                 * the directory entry. This can happen for boundary/stub cases or
                 * unusual metadata. Type information from btrfs_dir_item is enough to
                 * mark directories and symlinks.
                 */
                if (isDirectoryFromBtrfsDirType(entry.btrfsType)) {
                    flags |= FileRecord_IsDir;
                }

                if (isSymlinkFromBtrfsDirType(entry.btrfsType)) {
                    flags |= FileRecord_IsSymlink;
                }
            }

            /*
             * Store names as UTF-8 bytes in the shared string pool. QString is used
             * while scanning for convenience, but FileRecord stores byte offsets and
             * lengths into the pooled UTF-8 representation.
             */
            const QByteArray nameUtf8 = entry.name.toUtf8();
            if (nameUtf8.isEmpty()) {
                continue;
            }

            const std::string_view nameView(
                nameUtf8.constData(),
                static_cast<std::size_t>(nameUtf8.size())
            );

            if (!stream.addRecord(
                    entry.childRootId,
                    entry.childInode,
                    entry.rootId,
                    entry.parentInode,
                    nameView,
                    size,
                    modificationTime,
                    flags,
                    onFileRecordChunk,
                    onFileRecordNamespaceChunk,
                    onStringPoolChunk)) {
                return false;
            }

            ++recordsStreamed;

            if (onProgress && ((recordsStreamed & 4095ULL) == 0)) {
                onProgress(Protocol::ScanProgress{
                    .phase = QStringLiteral("Streaming Btrfs records"),
                    .unit = QStringLiteral("records"),
                    .processed = recordsStreamed,
                    .total = static_cast<quint64>(estimatedEntryCount + rootStates.size())
                });
            }
        }
    }

    if (!stream.flush(
            onFileRecordChunk,
            onFileRecordNamespaceChunk,
            onStringPoolChunk)) {
        return false;
    }

    if (onProgress) {
        onProgress(Protocol::ScanProgress{
            .phase = QStringLiteral("Streaming Btrfs records"),
            .unit = QStringLiteral("records"),
            .processed = recordsStreamed,
            .total = recordsStreamed
        });
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cerr << "[BtrfsScannerEngine] emitted records="
              << recordsStreamed
              << " stringPoolBytes="
              << stream.totalStringPoolLength
              << " roots="
              << rootStates.size()
              << "\n";
#endif

    return true;
}

bool BtrfsScannerEngine::debugScanMountedFilesystem(
    const QString& devicePath,
    const QStringList& mountPoints,
    const DebugScanOptions& options,
    const ScannerHelper::ErrorCallback& onError,
    const ScannerHelper::CancelCallback& shouldCancel)
{
    /*
     * Diagnostic variant of scanMountedFilesystem().
     *
     * This runs the same metadata discovery passes but does not stream FileRecord
     * chunks to the GUI. Instead it prints raw-ish scanner state so Btrfs behaviour
     * can be compared with external tools such as:
     *
     *   findmnt
     *   btrfs subvolume list
     *   find -xdev -inum ...
     *   stat
     *
     * Keep this path close to the production scan path so diagnostics reproduce
     * real scanner behaviour.
     */
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

    /*
     * Used by scanDirectoryIndexItems() to decide whether a subvolume boundary
     * entry points to a root that is included in this debug scan.
     */
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

    /*
     * Print sections are independent so tests can request just the mount table,
     * just records, or only summary counts.
     */
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