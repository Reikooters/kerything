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
}