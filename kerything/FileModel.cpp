// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <execution>
#include <algorithm>
#include <string>
#include <QIcon>
#include <QMimeData>
#include <QUrl>
#include <QDir>
#include <QColor>
#include "FileModel.h"

#include "AppController.h"
#include "GuiUtils.h"

#include "IndexController.h"

namespace {
    constexpr uint32_t NoMountPoint = 0xFFFFFFFF;

    bool hasMountedPath(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle
    ) {
        return handle.mountPointIdx != NoMountPoint &&
               handle.mountPointIdx < static_cast<uint32_t>(deviceIndex.mountPoints.size());
    }

    QString displayVolumeName(const IndexController::DeviceIndex& deviceIndex) {
        const QString label = deviceIndex.label.trimmed();
        if (!label.isEmpty() && label != QStringLiteral("TODO")) {
            return label;
        }

        const QString deviceId = deviceIndex.deviceId.trimmed();
        if (!deviceId.isEmpty()) {
            static constexpr qsizetype MaxDeviceIdDisplayLength = 24;

            if (deviceId.size() <= MaxDeviceIdDisplayLength) {
                return deviceId;
            }

            return deviceId.left(MaxDeviceIdDisplayLength - 1) + QStringLiteral("…");
        }

        const QString devNode = deviceIndex.devNode.trimmed();
        if (!devNode.isEmpty()) {
            return devNode;
        }

        return QStringLiteral("Unmounted volume");
    }

    QString mountedPathForHandle(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle,
        const std::string& filesystemPath
    ) {
        const QString relativePath = QString::fromStdString(filesystemPath);

        if (!hasMountedPath(deviceIndex, handle)) {
            return QDir::cleanPath(
                displayVolumeName(deviceIndex) + QStringLiteral(":") + relativePath
            );
        }

        const QString mountPoint = deviceIndex.mountPoints.at(static_cast<int>(handle.mountPointIdx));
        return QDir::cleanPath(mountPoint + QStringLiteral("/") + relativePath);
    }

    QString resultToolTip(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle,
        const std::string& filesystemPath
    ) {
        const QString relativePath = QDir::cleanPath(QString::fromStdString(filesystemPath));
        const QString displayPath = mountedPathForHandle(deviceIndex, handle, filesystemPath);

        if (hasMountedPath(deviceIndex, handle)) {
            return QStringLiteral(
                "Path:\n%1\n\n"
                "Device:\n%2\n\n"
                "Device node:\n%3"
            ).arg(
                displayPath,
                deviceIndex.deviceId,
                deviceIndex.devNode
            );
        }

        return QStringLiteral(
            "This result is from an unmounted device.\n"
            "Drag and drop is disabled until the device is mounted.\n\n"
            "Indexed path:\n%1\n\n"
            "Display path:\n%2\n\n"
            "Device:\n%3\n\n"
            "Last known device node:\n%4"
        ).arg(
            relativePath,
            displayPath,
            deviceIndex.deviceId,
            deviceIndex.devNode
        );
    }
}

FileModel::FileModel(AppController* controller, QObject *parent)
    : QAbstractTableModel(parent),
      controller_(controller) {}

void FileModel::setSearchResults(std::vector<IndexController::RecordHandle> newResults)
{
    beginResetModel(); // Notify views that the entire model is being reset
    searchResults_ = std::move(newResults);
    endResetModel();
}

// const IndexController::DeviceIndex* FileModel::resolveDeviceIndex(const IndexController::RecordHandle& handle) const {
//     if (!controller_->indexController()) {
//         return nullptr;
//     }
//
//     const IndexController::DeviceIndex* deviceIndex = controller_->indexController()->deviceIndex(handle.indexId);
//     if (!deviceIndex) {
//         return nullptr;
//     }
//
//     if (deviceIndex->generation != handle.generation) {
//         return nullptr;
//     }
//
//     return deviceIndex;
// }

const IndexController::DeviceIndex* FileModel::resolveDeviceIndex(const IndexController::RecordHandle& handle) const {
    if (!controller_->indexController()) {
        return nullptr;
    }

    return controller_->indexController()->withDeviceIndexRead(handle.indexId, [&](const IndexController::DeviceIndex* deviceIndex) -> const IndexController::DeviceIndex* {
        if (!deviceIndex) {
            return nullptr;
        }

        if (deviceIndex->generation != handle.generation) {
            return nullptr;
        }

        if (!deviceIndex->isReady) {
            return nullptr;
        }

        return deviceIndex;
    });
}

void FileModel::sort(int column, Qt::SortOrder order) {
    if (!controller_ || !controller_->indexController() || searchResults_.empty()) {
        return;
    }

    beginResetModel();

    searchResults_ = controller_->indexController()->sortSearchResults(
        std::move(searchResults_),
        column,
        order
    );

    endResetModel();
}

int FileModel::rowCount(const QModelIndex &parent) const {
    return static_cast<int>(searchResults_.size());
}

int FileModel::columnCount(const QModelIndex &parent) const {
    return 4; // Name, Path, Size, Modified
}

QVariant FileModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
        case 0: return "Name";
        case 1: return "Path";
        case 2: return "Size";
        case 3: return "Date Modified";
        default: return {};
    }
}

QVariant FileModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) {
        return {};
    }

    const bool supportedRole =
        role == Qt::DisplayRole ||
        role == Qt::ToolTipRole ||
        role == Qt::ForegroundRole ||
        role == Qt::DecorationRole;

    if (!supportedRole) {
        return {};
    }

    const auto& handle = searchResults_[index.row()];
    const auto* deviceIndex = resolveDeviceIndex(handle);
    if (!deviceIndex || handle.recordIdx >= deviceIndex->fileRecords.size()) {
        return {};
    }

    const FileRecord &rec = deviceIndex->fileRecords[handle.recordIdx];

    if (rec.nameOffset + rec.nameLen > deviceIndex->stringPool.size()) {
        return {};
    }

    const bool mounted = hasMountedPath(*deviceIndex, handle);

    if (role == Qt::ForegroundRole && !mounted) {
        return QColor(Qt::gray);
    }

    if (role == Qt::ToolTipRole) {
        const std::string parentPath = deviceIndex->getFullPath(rec.parentRecordIdx);
        return resultToolTip(*deviceIndex, handle, parentPath);
    }

    // DecorationRole provides the icon shown next to the filename/path.
    // This is used to differentiate Files vs Folders and mark unmounted results.
    if (role == Qt::DecorationRole) {
        if (index.column() == 1 && !mounted) {
            return QIcon::fromTheme(
                QStringLiteral("dialog-warning"),
                QIcon::fromTheme(QStringLiteral("emblem-warning"))
            );
        }

        if (index.column() != 0) {
            return {};
        }

        // Using standard KDE/Freedesktop theme names for icons
        if ((rec.flags & FileRecord_IsDir) != 0) {
            return (rec.flags & FileRecord_IsSymlink) != 0
                ? QIcon::fromTheme("inode-directory-symlink", QIcon::fromTheme("folder-remote"))
                : QIcon::fromTheme("inode-directory");
        }

        return (rec.flags & FileRecord_IsSymlink) != 0
            ? QIcon::fromTheme("emblem-symbolic-link")
            : QIcon::fromTheme("document-new");
    }

    switch (index.column()) {
        case 0: // Name: Extracted directly from the string pool
            return QString::fromUtf8(&deviceIndex->stringPool[rec.nameOffset], rec.nameLen);
        case 1: { // Path: Resolved from the directory map and current mount/virtual volume
            const std::string parentPath = deviceIndex->getFullPath(rec.parentRecordIdx);
            return mountedPathForHandle(*deviceIndex, handle, parentPath);
        }
        case 2: // Size: Formatted according to the user's locale
            if ((rec.flags & FileRecord_IsDir) != 0) {
                return QString("<DIR>");
            }

            // Format as bytes/KB/MB etc, with 2 decimal places
            //return QLocale().formattedDataSize(rec.size, 2, QLocale::DataSizeTraditionalFormat);

            // Formats the raw byte count with appropriate thousands separators
            return QLocale().toString(static_cast<qlonglong>(rec.size));
        case 3: // Date: Formatted from unix seconds to local time
            return QString::fromStdString(GuiUtils::uint64ToFormattedTime(rec.modificationTime));
        default:
            return {};
    }
}

std::optional<QUrl> FileModel::localUrlForRow(const int row) const {
    if (row < 0 || row >= static_cast<int>(searchResults_.size())) {
        return std::nullopt;
    }

    const auto& handle = searchResults_[row];
    const auto* deviceIndex = resolveDeviceIndex(handle);

    if (!deviceIndex || handle.recordIdx >= deviceIndex->fileRecords.size()) {
        return std::nullopt;
    }

    if (!hasMountedPath(*deviceIndex, handle)) {
        return std::nullopt;
    }

    const FileRecord& rec = deviceIndex->fileRecords[handle.recordIdx];

    if (rec.nameOffset + rec.nameLen > deviceIndex->stringPool.size()) {
        return std::nullopt;
    }

    const QString fileName = QString::fromUtf8(
        &deviceIndex->stringPool[rec.nameOffset],
        rec.nameLen
    );

    const std::string parentPath = deviceIndex->getFullPath(rec.parentRecordIdx);
    const QString mountedParentPath = mountedPathForHandle(*deviceIndex, handle, parentPath);
    const QString fullPath = QDir::cleanPath(mountedParentPath + QStringLiteral("/") + fileName);

    return QUrl::fromLocalFile(fullPath);
}

uint32_t FileModel::getRecordIndex(int row) const {
    if (row < 0 || row >= static_cast<int>(searchResults_.size())) {
        return 0;
    }

    return searchResults_[row].recordIdx;
}

Qt::ItemFlags FileModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags defaultFlags = QAbstractTableModel::flags(index);

    if (!index.isValid()) {
        return defaultFlags;
    }

    const auto& handle = searchResults_[index.row()];
    const auto* deviceIndex = resolveDeviceIndex(handle);

    if (!deviceIndex || !hasMountedPath(*deviceIndex, handle)) {
        return defaultFlags;
    }

    // Qt::ItemIsDragEnabled is required for QAbstractItemView to initiate a drag.
    // Only mounted results can be dragged because only those have real local paths.
    return Qt::ItemIsDragEnabled | defaultFlags;
}

QStringList FileModel::mimeTypes() const {
    return {"text/uri-list"};
}

QMimeData *FileModel::mimeData(const QModelIndexList &indexes) const {
    if (!controller_->indexController()) {
        return nullptr;
    }

    QList<QUrl> urls;

    // Since selection behavior is SelectRows, 'indexes' contains one entry per column
    // for every selected row. We only process column 0 to ensure each file is added once.
    for (const QModelIndex &index : indexes) {
        if (index.column() != 0) {
            continue;
        }

        const auto& handle = searchResults_[index.row()];
        const auto* deviceIndex = resolveDeviceIndex(handle);
        if (!deviceIndex || handle.recordIdx >= deviceIndex->fileRecords.size()) {
            continue;
        }

        if (!hasMountedPath(*deviceIndex, handle)) {
            continue;
        }

        const FileRecord& rec = deviceIndex->fileRecords[handle.recordIdx];

        if (rec.nameOffset + rec.nameLen > deviceIndex->stringPool.size()) {
            continue;
        }

        // Resolve file name from string pool
        QString fileName = QString::fromUtf8(&deviceIndex->stringPool[rec.nameOffset], rec.nameLen);

        // Resolve parent directory path
        const std::string parentPath = deviceIndex->getFullPath(rec.parentRecordIdx);
        const QString mountedParentPath = mountedPathForHandle(*deviceIndex, handle, parentPath);

        // Construct the absolute Linux path and wrap it in a QUrl
        QString fullPath = QDir::cleanPath(mountedParentPath + QStringLiteral("/") + fileName);
        urls.append(QUrl::fromLocalFile(fullPath));
    }

    if (urls.isEmpty()) {
        return nullptr;
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);

    return mimeData;
}