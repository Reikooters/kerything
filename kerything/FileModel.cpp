// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <execution>
#include <algorithm>
#include <string>
#include <QIcon>
#include <QMimeData>
#include <QUrl>
#include <QDir>
#include "FileModel.h"

#include "AppController.h"
#include "GuiUtils.h"

#include "IndexController.h"

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
//     const IndexController::DeviceIndex* deviceIndex = controller_->indexController()->deviceIndex(handle.deviceId);
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

    return controller_->indexController()->withDeviceIndexRead(handle.deviceId, [&](const IndexController::DeviceIndex* deviceIndex) -> const IndexController::DeviceIndex* {
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

// void FileModel::sort(int column, Qt::SortOrder order) {
//     if (!controller_->indexController() || searchResults_.empty()) {
//         return;
//     }
//
//     beginResetModel();
//
//     // Helper for case-insensitive comparison
//     auto compareCaseInsensitive = [](std::string_view s1, std::string_view s2) {
//         return std::lexicographical_compare(
//             s1.begin(), s1.end(),
//             s2.begin(), s2.end(),
//             [](char a, char b) {
//                 return std::tolower(static_cast<unsigned char>(a)) <
//                        std::tolower(static_cast<unsigned char>(b));
//             }
//         );
//     };
//
//     auto compare = [&](const IndexController::RecordHandle& aHandle, const IndexController::RecordHandle& bHandle) {
//         const auto* aDevice = resolveDeviceIndex(aHandle);
//         const auto* bDevice = resolveDeviceIndex(bHandle);
//
//         if (!aDevice || !bDevice) {
//             return false;
//         }
//
//         const auto& a = aDevice->fileRecords[aHandle.recordIdx];
//         const auto& b = bDevice->fileRecords[bHandle.recordIdx];
//
//         if (order == Qt::AscendingOrder) {
//             // To ensure strict weak ordering and avoid crashes, we must handle
//             // the 'ascending vs descending' logic carefully.
//             auto isLess = [&]() {
//                 switch (column) {
//                     case 0: // Name
//                         return compareCaseInsensitive(
//                             std::string_view(&aDevice->stringPool[a.nameOffset], a.nameLen),
//                             std::string_view(&bDevice->stringPool[b.nameOffset], b.nameLen)
//                         );
//                     case 1: // Path
//                         return compareCaseInsensitive(
//                             aDevice->getFullPath(a.parentRecordIdx),
//                             bDevice->getFullPath(b.parentRecordIdx)
//                         );
//                     case 2: // Size
//                         return a.size < b.size;
//                     case 3: // Date
//                         return a.modificationTime < b.modificationTime;
//                     default:
//                         return false;
//                 }
//             };
//             return isLess();
//         } else {
//             // For descending, we check if B < A
//             // This preserves strict weak ordering and prevents segfaults
//             auto isGreater = [&]() {
//                 switch (column) {
//                     case 0: // Name
//                         return compareCaseInsensitive(
//                             std::string_view(&bDevice->stringPool[b.nameOffset], b.nameLen),
//                             std::string_view(&aDevice->stringPool[a.nameOffset], a.nameLen)
//                         );
//                     case 1: // Path
//                         return compareCaseInsensitive(
//                             bDevice->getFullPath(b.parentRecordIdx),
//                             aDevice->getFullPath(a.parentRecordIdx)
//                         );
//                     case 2: // Size
//                         return b.size < a.size;
//                     case 3: // Date
//                         return b.modificationTime < a.modificationTime;
//                     default:
//                         return false;
//                 }
//             };
//             return isGreater();
//         }
//     };
//
//     // Sort using parallel execution policy to leverage multiple CPU cores via TBB
//     std::sort(std::execution::par, searchResults_.begin(), searchResults_.end(), compare);
//
//     endResetModel();
// }

void FileModel::sort(int column, Qt::SortOrder order) {
    if (!controller_ || !controller_->indexController() || searchResults_.empty()) {
        return;
    }

    beginResetModel();

    struct SortEntry {
        IndexController::RecordHandle handle;
        std::string textKey;    // used for Name / Path
        quint64 numericKey = 0; // used for Size / Date
    };

    std::vector<SortEntry> entries;
    entries.reserve(searchResults_.size());

    auto toLowerCopy = [](std::string_view s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (unsigned char ch : s) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    };

    auto handleLess = [](const IndexController::RecordHandle& a, const IndexController::RecordHandle& b) {
        if (a.deviceId != b.deviceId) {
            return a.deviceId < b.deviceId;
        }
        if (a.generation != b.generation) {
            return a.generation < b.generation;
        }
        return a.recordIdx < b.recordIdx;
    };

    for (const auto& handle : searchResults_) {
        SortEntry entry;
        entry.handle = handle;

        const auto* device = resolveDeviceIndex(handle);
        if (device && handle.recordIdx < device->fileRecords.size()) {
            const auto& record = device->fileRecords[handle.recordIdx];

            switch (column) {
                case 0:
                    entry.textKey = toLowerCopy(std::string_view(
                        &device->stringPool[record.nameOffset],
                        record.nameLen
                    ));
                    break;
                case 1:
                    entry.textKey = device->getFullPath(handle.recordIdx);
                    break;
                case 2:
                    entry.numericKey = record.size;
                    break;
                case 3:
                    entry.numericKey = record.modificationTime;
                    break;
                default:
                    break;
            }
        }

        entries.push_back(std::move(entry));
    }

    auto less = [&](const SortEntry& a, const SortEntry& b) {
        bool result = false;

        switch (column) {
            case 0:
            case 1:
                result = a.textKey < b.textKey;
                break;
            case 2:
            case 3:
                result = a.numericKey < b.numericKey;
                break;
            default:
                result = false;
                break;
        }

        if (result) {
            return true;
        }

        if (column == 0 || column == 1) {
            if (a.textKey == b.textKey) {
                return handleLess(a.handle, b.handle);
            }
        } else {
            if (a.numericKey == b.numericKey) {
                return handleLess(a.handle, b.handle);
            }
        }

        return false;
    };

    if (order == Qt::AscendingOrder) {
        std::sort(std::execution::par, entries.begin(), entries.end(), less);
    } else {
        std::sort(std::execution::par, entries.begin(), entries.end(),
                  [&](const SortEntry& a, const SortEntry& b) {
                      return less(b, a);
                  });
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        searchResults_[i] = std::move(entries[i].handle);
    }

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
    if (!index.isValid()
        || (!(role == Qt::DecorationRole && index.column() == 0) && role != Qt::DisplayRole)) {
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

    // DecorationRole provides the icon shown next to the filename
    // This is used to differentiate Files vs Folders
    if (role == Qt::DecorationRole) {
        // index.column() is always 0 in this case, as we checked earlier.

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
        case 1: // Path: Resolved from the directory map
            return QString::fromStdString(deviceIndex->getFullPath(rec.parentRecordIdx));
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

uint32_t FileModel::getRecordIndex(int row) const {
    if (row < 0 || row >= static_cast<int>(searchResults_.size())) {
        return 0;
    }

    return searchResults_[row].recordIdx;
}

Qt::ItemFlags FileModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags defaultFlags = QAbstractTableModel::flags(index);

    if (index.isValid()) {
        // Qt::ItemIsDragEnabled is required for QAbstractItemView to initiate a drag
        return Qt::ItemIsDragEnabled | defaultFlags;
    }

    return defaultFlags;
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

        const auto &rec = deviceIndex->fileRecords[handle.recordIdx];

        if (rec.nameOffset + rec.nameLen > deviceIndex->stringPool.size()) {
            continue;
        }

        // Resolve file name from string pool
        QString fileName = QString::fromUtf8(&deviceIndex->stringPool[rec.nameOffset], rec.nameLen);

        // Resolve parent directory path
        QString internalPath = QString::fromStdString(deviceIndex->getFullPath(rec.parentRecordIdx));

        // Construct the absolute Linux path and wrap it in a QUrl
        // QString fullPath = QDir::cleanPath(mountPath_ + "/" + internalPath + "/" + fileName);
        QString fullPath = QDir::cleanPath(internalPath + "/" + fileName);
        urls.append(QUrl::fromLocalFile(fullPath));
    }

    if (urls.isEmpty()) {
        return nullptr;
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);

    return mimeData;
}