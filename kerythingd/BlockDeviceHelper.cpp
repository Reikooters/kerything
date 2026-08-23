#include "BlockDeviceHelper.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <QHash>
#include <QStringList>
#include <sstream>
#include <vector>

#include <blkid/blkid.h>

/**
 * Retrieves a specific value from the block device metadata for a given device node.
 *
 * @param devNode The device node path as a string (e.g., "/dev/sda1").
 * @param key The metadata key to retrieve (e.g., "TYPE", "UUID", "LABEL").
 * @return An optional QString containing the value associated with the given key if found,
 *         or an empty std::optional if the key cannot be retrieved or the probe fails.
 */
static std::optional<QString> blkidValueForDev(const std::string& devNode, const char* key) {
    blkid_probe pr = blkid_new_probe_from_filename(devNode.c_str());
    if (!pr) return std::nullopt;

    blkid_probe_enable_superblocks(pr, 1);
    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_UUID | BLKID_SUBLKS_LABEL);

    const int rc = blkid_do_safeprobe(pr);

    const char* data = nullptr;
    size_t len = 0;

    std::optional<QString> out;
    if (rc == 0 && blkid_probe_lookup_value(pr, key, &data, &len) == 0 && data && len > 1) {
        out = QString::fromUtf8(data, static_cast<int>(len) - 1);
    }

    blkid_free_probe(pr);
    return out;
}

/**
 * Selects the primary mount point from a list of available mount points.
 *
 * The method applies the following selection criteria:
 * 1) Prefers mount points under "/mnt", "/media" or "/run/media". Among these, the shortest path is chosen.
 * 2) If no mount points match the first criteria, the shortest path from the provided list is selected.
 *
 * @param mountPoints A list of mount points as strings.
 * @return A QString representing the selected primary mount point.
 *         Returns an empty QString if the input list is empty.
 */
static QString pickPrimaryMountPoint(const QStringList& mountPoints) {
    if (mountPoints.isEmpty()) return {};

    // 1) Prefer /mnt or /media
    QString best;
    for (const QString& mp : mountPoints) {
        if (mp.startsWith(QStringLiteral("/mnt/")) || mp == QStringLiteral("/mnt")) {
            if (best.isEmpty() || mp.size() < best.size()) best = mp;
        }
        if (mp.startsWith(QStringLiteral("/media/")) || mp == QStringLiteral("/media")) {
            if (best.isEmpty() || mp.size() < best.size()) best = mp;
        }
        if (mp.startsWith(QStringLiteral("/run/media/")) || mp == QStringLiteral("/run/media")) {
            if (best.isEmpty() || mp.size() < best.size()) best = mp;
        }
    }
    if (!best.isEmpty()) return best;

    // 2) Else shortest path
    best = mountPoints.first();
    for (const QString& mp : mountPoints) {
        if (mp.size() < best.size()) best = mp;
    }
    return best;
}

/**
 * Decodes an escaped field from the mount information, replacing
 * any octal escape sequences with their corresponding character values.
 *
 * @param input The input string containing the mount information field to decode.
 *              Octal escape sequences are expected to be in the format \XYZ,
 *              where X, Y, and Z are octal digits (0-7).
 * @return A decoded string in which all valid octal escape sequences are replaced
 *         with their corresponding character representations, and all other
 *         characters are copied as-is.
 */
static std::string decodeMountInfoField(const std::string& input)
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

struct MountInfoEntry {
    std::string root;
    std::string mountPoint;
    std::string fsType;
    std::string mountSource;
};

/**
 * Reads and parses the contents of /proc/self/mountinfo to retrieve a list of mounted file systems.
 *
 * This method extracts the mount root, mount point, filesystem type, and mount source from each line of the file.
 * Only entries that conform to the expected format are included in the output.
 *
 * @return A vector of MountInfoEntry structures, where each structure contains the mount root,
 *         mount point, filesystem type, and associated mount source. Returns an empty vector if the file cannot
 *         be read or if no valid entries are found.
 */
static std::vector<MountInfoEntry> readMountInfo() {
    std::ifstream f("/proc/self/mountinfo");
    std::vector<MountInfoEntry> out;
    if (!f) return out;

    std::string line;
    while (std::getline(f, line)) {
        // mountinfo format:
        //  id parent major:minor root mount_point opts ... - fstype mount_source superopts
        //
        // We need root, mount_point, fstype and mount_source.
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) continue;

        const std::string left = line.substr(0, sep);
        const std::string right = line.substr(sep + 3);

        // left: fields separated by spaces; root is field 4, mount_point is field 5.
        std::string id, parent, majmin, root, mountPoint;
        {
            std::istringstream iss(left);
            std::string opts;
            if (!(iss >> id >> parent >> majmin >> root >> mountPoint >> opts)) continue;
        }

        // right: fstype mount_source superopts...
        std::string fstype, mountSource;
        {
            std::istringstream iss(right);
            std::string superopts;
            if (!(iss >> fstype >> mountSource >> superopts)) continue;
        }

        out.push_back({
            decodeMountInfoField(root),
            decodeMountInfoField(mountPoint),
            decodeMountInfoField(fstype),
            decodeMountInfoField(mountSource)
        });
    }

    return out;
}

QString trimmedQStringFromFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) {
        return {};
    }

    std::string value;
    std::getline(in, value);

    return QString::fromStdString(value).trimmed();
}

QString diskNameForDevNode(const std::string& devNode)
{
    namespace fs = std::filesystem;

    const fs::path blockName = fs::path(devNode).filename();
    if (blockName.empty()) {
        return {};
    }

    std::error_code ec;
    fs::path sysBlockPath = fs::canonical(fs::path("/sys/class/block") / blockName, ec);
    if (ec) {
        return {};
    }

    /*
     * For partitions, /sys/class/block/<partition>/partition exists and the
     * parent directory is the whole disk.
     *
     * Examples:
     *   /sys/class/block/sda1     -> parent disk sda
     *   /sys/class/block/nvme0n1p1 -> parent disk nvme0n1
     *
     * For whole disks, use the node itself.
     */
    if (fs::exists(sysBlockPath / "partition")) {
        sysBlockPath = sysBlockPath.parent_path();
    }

    return QString::fromStdString(sysBlockPath.filename().string());
}

QString diskModelForDevNode(const std::string& devNode)
{
    const QString diskName = diskNameForDevNode(devNode);
    if (diskName.isEmpty()) {
        return {};
    }

    return trimmedQStringFromFile(
        std::filesystem::path("/sys/class/block") /
        diskName.toStdString() /
        "device/model"
    );
}

std::vector<BlockDevice> BlockDeviceHelper::listKnownDevices()
{
    namespace fs = std::filesystem;

    // Read mountinfo once, then match by resolved /dev node path
    const auto mountInfo = readMountInfo();

    struct Candidate {
        QString deviceId;     // what we expose on D-Bus
        std::string devNode;  // canonical /dev path
        QString partuuid;
        QString uuid;
    };

    // Collect candidates keyed by canonical devNode so we de-dup between by-partuuid/by-uuid.
    std::unordered_map<std::string, Candidate> byDevNode;

    auto addSymlinkDir = [&](const fs::path& dir,
                             const QString& idPrefix,
                             bool isPartuuid) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return;
        }

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_symlink()) {
                continue;
            }

            const std::string name = entry.path().filename().string();

            std::error_code ec;
            const fs::path resolved = fs::canonical(entry.path(), ec);
            if (ec) {
                continue;
            }

            const std::string devNode = resolved.string();

            Candidate c;
            c.deviceId = idPrefix + QString::fromStdString(name);
            c.devNode = devNode;

            if (isPartuuid) {
                c.partuuid = QString::fromStdString(name);
            }
            else {
                c.uuid = QString::fromStdString(name);
            }

            auto it = byDevNode.find(devNode);
            if (it == byDevNode.end()) {
                byDevNode.emplace(devNode, c);
            } else {
                // Prefer partuuid IDs when we have both.
                const bool existingIsPartuuid = it->second.deviceId.startsWith(QStringLiteral("partuuid:"));
                const bool newIsPartuuid = c.deviceId.startsWith(QStringLiteral("partuuid:"));

                if (!existingIsPartuuid && newIsPartuuid) {
                    it->second = c;
                } else {
                    // Merge extra metadata if missing.
                    if (it->second.partuuid.isEmpty() && !c.partuuid.isEmpty()) {
                        it->second.partuuid = c.partuuid;
                    }

                    if (it->second.uuid.isEmpty() && !c.uuid.isEmpty()) {
                        it->second.uuid = c.uuid;
                    }
                }
            }
        }
    };

    // 1) Prefer partuuid (physical partitions).
    addSymlinkDir(fs::path("/dev/disk/by-partuuid"), QStringLiteral("partuuid:"), true);

    // 2) Also accept filesystem UUIDs (covers loop devices with mkfs.*).
    addSymlinkDir(fs::path("/dev/disk/by-uuid"), QStringLiteral("uuid:"), false);

    std::vector<BlockDevice> devicesOut;

    for (const auto& kv : byDevNode) {
        const Candidate& cand = kv.second;
        const std::string& devNode = cand.devNode;

        const auto fsTypeOpt = blkidValueForDev(devNode, "TYPE");
        if (!fsTypeOpt) {
            continue;
        }

        QString fsType = fsTypeOpt->toLower();

        /*
         * Do not restrict the device list to only filesystems with specialized
         * raw scanners. EXT4 and NTFS still use their optimized engines, while
         * other mounted block-device filesystems can be indexed through the
         * generic mounted scanner.
         *
         * Pseudo filesystems such as procfs/sysfs/devtmpfs normally do not show
         * up here because we enumerate real /dev/disk/by-* block devices and
         * require blkid filesystem metadata.
         */
        if (fsType.isEmpty()) {
#ifdef KERYTHING_ENABLE_LOGGING
            std::cout << "Skipping block device with empty filesystem type devNode="
                      << devNode
                      << "\n";
#endif
            continue;
        }

        const auto probedUuid = blkidValueForDev(devNode, "UUID");
        const auto label = blkidValueForDev(devNode, "LABEL");

        QStringList mountPoints;
        QHash<QString, QString> mountFsTypeByMountPoint;

        for (const auto& mi : mountInfo) {
            /*
             * Ignore bind mounts of subdirectories. systemd service hardening
             * options such as ProtectSystem= and ProtectHome= create private
             * bind mounts like /usr, /etc, /home and /root backed by the same
             * block device as /. Treating those as device mount points causes
             * every indexed result to appear once per bind mount.
             *
             * For ordinary full filesystem mounts, the mountinfo "root" field
             * is "/".
             *
             * Btrfs subvolumes are different: mountinfo stores the mounted
             * subvolume path in the "root" field, for example "/@", "/@home",
             * or "/@var/log". Those are real btrfs mount points, so accept
             * non-root mountinfo roots when both the probed block device and
             * the mounted filesystem are btrfs.
             */
            const QString mountedFsType = QString::fromStdString(mi.fsType).toLower();
            const bool isBtrfsSubvolumeMount =
                fsType == QStringLiteral("btrfs") &&
                mountedFsType == QStringLiteral("btrfs");

            if (mi.root != "/" && !isBtrfsSubvolumeMount) {
                continue;
            }

            if (mi.mountSource.rfind("/dev/", 0) != 0) {
                continue;
            }

            std::error_code ec;
            const fs::path srcResolved = fs::canonical(mi.mountSource, ec);
            if (ec) {
                continue;
            }

            if (srcResolved.string() == devNode) {
                const QString mountPoint = QString::fromStdString(mi.mountPoint);
                mountPoints << mountPoint;
                mountFsTypeByMountPoint.insert(
                    mountPoint,
                    mountedFsType
                );
            }
        }

        mountPoints.removeDuplicates();
        std::sort(mountPoints.begin(), mountPoints.end());

        const QString primaryMountPoint = pickPrimaryMountPoint(mountPoints);

        BlockDevice dev;
        dev.deviceId = cand.deviceId;
        dev.devNode = QString::fromStdString(devNode);
        dev.fsType = fsType;
        dev.mountedFsType = mountFsTypeByMountPoint.value(primaryMountPoint);
        dev.uuid = probedUuid ? probedUuid->toLower() : cand.uuid.toLower();
        dev.partuuid = cand.partuuid.toLower();
        dev.label = label.value_or(QString());
        dev.diskModel = diskModelForDevNode(devNode);
        dev.mounted = !mountPoints.isEmpty();
        dev.mountPoints = mountPoints;
        dev.primaryMountPoint = primaryMountPoint;

        devicesOut.push_back(std::move(dev));
    }

    return devicesOut;
}

std::optional<BlockDevice> BlockDeviceHelper::findKnownDeviceById(const QString& deviceId)
{
    if (deviceId.isEmpty()) {
        return std::nullopt;
    }

    const auto devices = listKnownDevices();

    for (const BlockDevice& device : devices) {
        if (device.deviceId == deviceId) {
            return device;
        }
    }

    return std::nullopt;
}
