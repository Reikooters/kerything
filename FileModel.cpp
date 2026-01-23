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
#include "GuiUtils.h"

FileModel::FileModel(QObject *parent) : QAbstractTableModel(parent) {}

void FileModel::setResults(std::vector<uint32_t> newResults, const ScannerEngine::SearchDatabase* db, QString mountPath, QString fsType) {
    beginResetModel(); // Notify views that the entire model is being reset
    m_results = std::move(newResults);
    m_db = db;
    m_mountPath = std::move(mountPath);
    m_fsType = std::move(fsType);
    endResetModel();
}

void FileModel::sort(int column, Qt::SortOrder order) {
    if (!m_db || m_results.empty()) {
        return;
    }

    beginResetModel();

    // Use parallel execution policy to leverage multiple CPU cores via TBB
    auto policy = std::execution::par;

    // Helper for case-insensitive comparison
    auto compareCaseInsensitive = [](std::string_view s1, std::string_view s2) {
        return std::lexicographical_compare(
            s1.begin(), s1.end(),
            s2.begin(), s2.end(),
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) <
                       std::tolower(static_cast<unsigned char>(b));
            }
        );
    };

    auto compare = [&](uint32_t aIdx, uint32_t bIdx) {
        const auto& a = m_db->records[aIdx];
        const auto& b = m_db->records[bIdx];

        if (order == Qt::AscendingOrder) {
            // To ensure strict weak ordering and avoid crashes, we must handle
            // the 'ascending vs descending' logic carefully.
            auto isLess = [&]() {
                switch (column) {
                    case 0: // Name
                        return compareCaseInsensitive(
                            std::string_view(&m_db->stringPool[a.nameOffset], a.nameLen),
                            std::string_view(&m_db->stringPool[b.nameOffset], b.nameLen)
                        );
                    case 1: // Path
                    {
                        std::string pathA = m_db->getFullPath(a.parentRecordIdx);
                        std::string pathB = m_db->getFullPath(b.parentRecordIdx);
                        return compareCaseInsensitive(pathA, pathB);
                    }
                    case 2: // Size
                        return a.size < b.size;
                    case 3: // Date
                        return a.modificationTime < b.modificationTime;
                    default:
                        return false;
                }
            };
            return isLess();
        } else {
            // For descending, we check if B < A
            // This preserves strict weak ordering and prevents segfaults
            auto isGreater = [&]() {
                switch (column) {
                    case 0: // Name
                        return compareCaseInsensitive(
                            std::string_view(&m_db->stringPool[b.nameOffset], b.nameLen),
                            std::string_view(&m_db->stringPool[a.nameOffset], a.nameLen)
                        );
                    case 1: // Path
                    {
                        std::string pathA = m_db->getFullPath(a.parentRecordIdx);
                        std::string pathB = m_db->getFullPath(b.parentRecordIdx);
                        return compareCaseInsensitive(pathB, pathA);
                    }
                    case 2: // Size
                        return b.size < a.size;
                    case 3: // Date
                        return b.modificationTime < a.modificationTime;
                    default:
                        return false;
                }
            };
            return isGreater();
        }
    };

    std::sort(policy, m_results.begin(), m_results.end(), compare);

    endResetModel();
}

int FileModel::rowCount(const QModelIndex &parent) const {
    return static_cast<int>(m_results.size());
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
        || (!(role == Qt::DecorationRole && index.column() == 0) && role != Qt::DisplayRole)
        || !m_db) {
        return {};
    }

    uint32_t recordIdx = m_results[index.row()];
    const ScannerEngine::FileRecord &rec = m_db->records[recordIdx];

    // DecorationRole provides the icon shown next to the filename
    // This is used to differentiate Files vs Folders
    if (role == Qt::DecorationRole) {
        // index.column() is always 0 in this case, as we checked earlier.

        // Using standard KDE/Freedesktop theme names for icons
        if (rec.isDir) {
            return rec.isSymlink ? QIcon::fromTheme("inode-directory-symlink", QIcon::fromTheme("folder-remote")) : QIcon::fromTheme("inode-directory");
        }

        return rec.isSymlink ? QIcon::fromTheme("emblem-symbolic-link") : QIcon::fromTheme("document-new");
    }

    switch (index.column()) {
        case 0: // Name: Extracted directly from the string pool
            return QString::fromUtf8(&m_db->stringPool[rec.nameOffset], rec.nameLen);
        case 1: // Path: Resolved from the directory map
        {
            return QString::fromStdString(m_db->getFullPath(rec.parentRecordIdx));
        }
        case 2: // Size: Formatted according to the user's locale
            if (rec.isDir) return QString("<DIR>");

            // Format as bytes/KB/MB etc, with 2 decimal places
            //return QLocale().formattedDataSize(rec.size, 2, QLocale::DataSizeTraditionalFormat);

            // Formats the raw byte count with appropriate thousands separators
            return QLocale().toString(static_cast<qlonglong>(rec.size));
        case 3: // Date: Formatted using NTFS-specific logic
        {
            if (m_fsType == "ntfs") {
                return QString::fromStdString(GuiUtils::ntfsTimeToLocalStr(rec.modificationTime));
            }
            if (m_fsType == "ext4") {
                return QString::fromStdString(GuiUtils::uint64ToFormattedTime(rec.modificationTime));
            }
            return QString::fromStdString(std::to_string(rec.modificationTime));
        }
        default:
            return {};
    }
}

uint32_t FileModel::getRecordIndex(int row) const {
    if (row < 0 || row >= static_cast<int>(m_results.size())) {
        return 0;
    }

    return m_results[row];
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
    if (!m_db || m_mountPath.isEmpty()) {
        return nullptr;
    }

    QList<QUrl> urls;

    // Since selection behavior is SelectRows, 'indexes' contains one entry per column
    // for every selected row. We only process column 0 to ensure each file is added once.
    for (const QModelIndex &index : indexes) {
        if (index.column() == 0) {
            uint32_t recordIdx = m_results[index.row()];
            const auto &rec = m_db->records[recordIdx];

            // Resolve file name from string pool
            QString fileName = QString::fromUtf8(&m_db->stringPool[rec.nameOffset], rec.nameLen);

            // Resolve parent directory path
            QString internalPath = QString::fromStdString(m_db->getFullPath(rec.parentRecordIdx));

            // Construct the absolute Linux path and wrap it in a QUrl
            QString fullPath = QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName);
            urls.append(QUrl::fromLocalFile(fullPath));
        }
    }

    if (urls.isEmpty()) {
        return nullptr;
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);

    return mimeData;
}