#include "BlockDeviceHelper.h"

#include <filesystem>
#include <fstream>
#include <iostream>
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
 * 1) Prefers mount points under "/mnt" or "/media". Among these, the shortest path is chosen.
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
    std::string mountPoint;
    std::string mountSource;
};

/**
 * Reads and parses the contents of /proc/self/mountinfo to retrieve a list of mounted file systems.
 *
 * This method extracts the mount point and the corresponding mount source from each line of the file.
 * Only entries that conform to the expected format are included in the output.
 *
 * @return A vector of MountInfoEntry structures, where each structure contains the mount point and
 *         the associated mount source. Returns an empty vector if the file cannot be read or if
 *         no valid entries are found.
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
        // We need mount_point and mount_source.
        const auto sep = line.find(" - ");
        if (sep == std::string::npos) continue;

        const std::string left = line.substr(0, sep);
        const std::string right = line.substr(sep + 3);

        // left: fields separated by spaces; mount_point is field 5 (1-based)
        // We'll parse first 6 tokens: id, parent, maj:min, root, mount_point, opts
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

        out.push_back({decodeMountInfoField(mountPoint), decodeMountInfoField(mountSource)});
    }

    return out;
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

        if (fsType != QStringLiteral("ext4") &&
            fsType != QStringLiteral("ntfs") &&
            fsType != QStringLiteral("ntfs3")) {
            continue;
        }

        const auto probedUuid = blkidValueForDev(devNode, "UUID");
        const auto label = blkidValueForDev(devNode, "LABEL");

        QStringList mountPoints;

        for (const auto& mi : mountInfo) {
            if (mi.mountSource.rfind("/dev/", 0) != 0) {
                continue;
            }

            std::error_code ec;
            const fs::path srcResolved = fs::canonical(mi.mountSource, ec);
            if (ec) {
                continue;
            }

            if (srcResolved.string() == devNode) {
                mountPoints << QString::fromStdString(mi.mountPoint);
            }
        }

        mountPoints.removeDuplicates();
        std::sort(mountPoints.begin(), mountPoints.end());

        BlockDevice dev;
        dev.deviceId = cand.deviceId;
        dev.devNode = QString::fromStdString(devNode);
        dev.fsType = fsType;
        dev.uuid = probedUuid ? probedUuid->toLower() : cand.uuid.toLower();
        dev.partuuid = cand.partuuid.toLower();
        dev.label = label.value_or(QString());
        dev.mounted = !mountPoints.isEmpty();
        dev.mountPoints = mountPoints;
        dev.primaryMountPoint = pickPrimaryMountPoint(mountPoints);

        devicesOut.push_back(std::move(dev));
    }

    return devicesOut;
}
