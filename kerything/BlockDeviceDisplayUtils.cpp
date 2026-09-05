// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "BlockDeviceDisplayUtils.h"

#include <QStringList>

namespace BlockDeviceDisplayUtils {
    QString displayNameForBlockDevice(const BlockDevice& blockDevice)
    {
        const QString label = blockDevice.label.trimmed();
        if (!label.isEmpty()) {
            return label;
        }

        const QString mountPoint = blockDevice.primaryMountPoint.trimmed();
        if (!mountPoint.isEmpty()) {
            if (mountPoint == QStringLiteral("/")) {
                return QStringLiteral("Root filesystem");
            }

            const QStringList parts = mountPoint.split(QStringLiteral("/"), Qt::SkipEmptyParts);
            if (!parts.isEmpty()) {
                return parts.last();
            }

            return mountPoint;
        }

        const QString fsType = blockDevice.fsType.trimmed();
        if (!fsType.isEmpty()) {
            return fsType.toUpper() + QStringLiteral(" volume");
        }

        const QString devNode = blockDevice.devNode.trimmed();
        if (!devNode.isEmpty()) {
            return devNode;
        }

        return QStringLiteral("Unknown volume");
    }

    QString displayOrDash(const QString& value)
    {
        const QString trimmed = value.trimmed();
        return trimmed.isEmpty() ? QStringLiteral("—") : trimmed;
    }

    bool deviceLessThan(const BlockDevice& lhs, const BlockDevice& rhs)
    {
        if (lhs.mounted != rhs.mounted) {
            return lhs.mounted > rhs.mounted;
        }

        const QString lhsMountPoint = lhs.primaryMountPoint.trimmed();
        const QString rhsMountPoint = rhs.primaryMountPoint.trimmed();

        if (lhsMountPoint != rhsMountPoint) {
            if (lhsMountPoint == QStringLiteral("/")) {
                return true;
            }

            if (rhsMountPoint == QStringLiteral("/")) {
                return false;
            }

            if (lhsMountPoint.isEmpty() != rhsMountPoint.isEmpty()) {
                return !lhsMountPoint.isEmpty();
            }

            return QString::localeAwareCompare(lhsMountPoint, rhsMountPoint) < 0;
        }

        const QString lhsName = displayNameForBlockDevice(lhs);
        const QString rhsName = displayNameForBlockDevice(rhs);

        const int nameCompare = QString::localeAwareCompare(lhsName, rhsName);
        if (nameCompare != 0) {
            return nameCompare < 0;
        }

        return QString::localeAwareCompare(lhs.devNode, rhs.devNode) < 0;
    }

    bool isBtrfsDevice(const BlockDevice& blockDevice)
    {
        return blockDevice.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0;
    }

    qsizetype btrfsMountedSubvolumeCount(const BlockDevice& blockDevice)
    {
        if (!isBtrfsDevice(blockDevice)) {
            return 0;
        }

        qsizetype count = 0;

        for (const BlockDeviceMountInfo& mount : blockDevice.mounts) {
            if (mount.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) != 0) {
                continue;
            }

            if (mount.mountPoint.trimmed().isEmpty()) {
                continue;
            }

            if (mount.btrfsRootId == 0) {
                continue;
            }

            ++count;
        }

        return count;
    }

    QString mountPointSummaryForBlockDevice(const BlockDevice& blockDevice)
    {
        if (!blockDevice.mounted) {
            return QStringLiteral("Not mounted");
        }

        if (isBtrfsDevice(blockDevice)) {
            const qsizetype subvolumeCount = btrfsMountedSubvolumeCount(blockDevice);

            if (subvolumeCount > 1) {
                return QStringLiteral("%1 mounted subvols").arg(subvolumeCount);
            }
        }

        const QString primaryMountPoint = blockDevice.primaryMountPoint.trimmed();
        return primaryMountPoint.isEmpty()
            ? QStringLiteral("Mounted")
            : primaryMountPoint;
    }

    QString mountPointToolTipForBlockDevice(const BlockDevice& blockDevice)
    {
        if (!blockDevice.mounted) {
            return QStringLiteral("This device is not currently mounted.");
        }

        if (!isBtrfsDevice(blockDevice)) {
            return blockDevice.mountPoints.isEmpty()
                ? QStringLiteral("This device is mounted, but no mount point details are available.")
                : blockDevice.mountPoints.join(QStringLiteral("\n"));
        }

        const QString tableHtml = btrfsMountedSubvolumesTableHtml(blockDevice);
        if (!tableHtml.isEmpty()) {
            return QStringLiteral(
                "<qt>"
                "<div style='white-space: nowrap;'>"
                "Mounted Btrfs subvolumes:"
                "%1"
                "</div>"
                "</qt>"
            ).arg(tableHtml);
        }

        return blockDevice.mountPoints.isEmpty()
            ? QStringLiteral("Mounted Btrfs filesystem. No subvolume details are available.")
            : blockDevice.mountPoints.join(QStringLiteral("\n"));
    }

    QString filesystemDisplayTextForBlockDevice(const BlockDevice& blockDevice)
    {
        return displayOrDash(blockDevice.fsType);
    }

    QString filesystemToolTipForBlockDevice(const BlockDevice& blockDevice)
    {
        if (!isBtrfsDevice(blockDevice)) {
            if (blockDevice.mounted && !blockDevice.mountedFsType.trimmed().isEmpty()) {
                return blockDevice.mountedFsType == blockDevice.fsType
                    ? QStringLiteral("Filesystem type: %1").arg(blockDevice.fsType)
                    : QStringLiteral("Detected filesystem: %1\nMounted as: %2")
                        .arg(blockDevice.fsType, blockDevice.mountedFsType);
            }

            return displayOrDash(blockDevice.fsType);
        }

        return QStringLiteral(
            "<qt>"
            "<div style='white-space: nowrap;'>"
            "Filesystem type: %1<br>"
            "Kerything indexes mounted Btrfs subvolumes for this filesystem."
            "%2"
            "</div>"
            "</qt>"
        ).arg(blockDevice.fsType, btrfsMountedSubvolumesTableHtml(blockDevice));
    }

    QString btrfsMountedSubvolumesTableHtml(const BlockDevice& blockDevice)
    {
        if (!isBtrfsDevice(blockDevice)) {
            return {};
        }

        QString html;

        html += QStringLiteral(
            "<table cellspacing='0' cellpadding='0' "
            "style='margin-top: 0.35em; border-collapse: collapse;'>"
            "<tr>"
            "<th align='left' style='padding-right: 1em;'>Mount point</th>"
            "<th align='right' style='padding-right: 1em;'>Subvol ID</th>"
            "<th align='left' style='padding-right: 1em;'>Subvolume</th>"
            "<th align='left'>Root</th>"
            "</tr>"
        );

        for (const BlockDeviceMountInfo& mount : blockDevice.mounts) {
            if (mount.fsType.trimmed().compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) != 0) {
                continue;
            }

            const QString mountPoint = mount.mountPoint.trimmed();
            if (mountPoint.isEmpty()) {
                continue;
            }

            const QString subvolId = mount.btrfsRootId == 0
                ? QStringLiteral("—")
                : QString::number(mount.btrfsRootId);

            const QString subvolPath = mount.subvolPath.trimmed().isEmpty()
                ? QStringLiteral("—")
                : mount.subvolPath.trimmed();

            const QString root = mount.root.trimmed().isEmpty()
                ? QStringLiteral("—")
                : mount.root.trimmed();

            html += QStringLiteral(
                "<tr>"
                "<td style='padding-right: 1em; white-space: nowrap;'>%1</td>"
                "<td align='right' style='padding-right: 1em; white-space: nowrap;'>%2</td>"
                "<td style='padding-right: 1em; white-space: nowrap;'>%3</td>"
                "<td style='white-space: nowrap;'>%4</td>"
                "</tr>"
            ).arg(
                mountPoint.toHtmlEscaped(),
                subvolId.toHtmlEscaped(),
                subvolPath.toHtmlEscaped(),
                root.toHtmlEscaped()
            );
        }

        html += QStringLiteral("</table>");
        return html;
    }

    QString selectedDeviceDetailsHtml(const BlockDevice& blockDevice)
    {
        QString html;

        html += QStringLiteral("<b>%1</b><br>").arg(
            displayNameForBlockDevice(blockDevice).toHtmlEscaped()
        );

        html += QStringLiteral("Device ID: %1<br>").arg(
            blockDevice.deviceId.toHtmlEscaped()
        );

        if (!blockDevice.devNode.trimmed().isEmpty()) {
            html += QStringLiteral("Device node: %1<br>").arg(
                blockDevice.devNode.trimmed().toHtmlEscaped()
            );
        }

        html += QStringLiteral("Filesystem: %1<br>").arg(
            filesystemDisplayTextForBlockDevice(blockDevice).toHtmlEscaped()
        );

        if (!blockDevice.mounted) {
            html += QStringLiteral("This device is not currently mounted.");
            return html;
        }

        if (!isBtrfsDevice(blockDevice)) {
            const QString mountPoint = blockDevice.primaryMountPoint.trimmed();
            html += mountPoint.isEmpty()
                ? QStringLiteral("This device is currently mounted.")
                : QStringLiteral("Currently mounted at <b>%1</b>.").arg(mountPoint.toHtmlEscaped());
            return html;
        }

        const qsizetype subvolumeCount = btrfsMountedSubvolumeCount(blockDevice);

        html += subvolumeCount == 1
            ? QStringLiteral("Kerything found <b>1 mounted Btrfs subvolume</b>:<br>")
            : QStringLiteral("Kerything found <b>%1 mounted Btrfs subvolumes</b>:").arg(subvolumeCount);

        html += btrfsMountedSubvolumesTableHtml(blockDevice);
        return html;
    }
}