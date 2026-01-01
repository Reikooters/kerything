// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <execution>
#include <algorithm>
#include <QIcon>
#include <string>
#include "FileModel.h"
#include "NtfsUtils.h"

FileModel::FileModel(QObject *parent) : QAbstractTableModel(parent) {}

void FileModel::setResults(std::vector<uint32_t> newResults, const ScannerEngine::SearchDatabase* db) {
    beginResetModel(); // Notify views that the entire model is being reset
    m_results = std::move(newResults);
    m_db = db;
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
                        // Look up the pre-computed directory path for the file's parent
                        auto itA = m_db->directoryPaths.find(a.parentRecordIdx);
                        auto itB = m_db->directoryPaths.find(b.parentRecordIdx);
                        static constexpr std::string_view rootPath = "/";
                        std::string_view pathA = (itA != m_db->directoryPaths.end()) ? std::string_view(itA->second) : rootPath;
                        std::string_view pathB = (itB != m_db->directoryPaths.end()) ? std::string_view(itB->second) : rootPath;
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
                        auto itA = m_db->directoryPaths.find(a.parentRecordIdx);
                        auto itB = m_db->directoryPaths.find(b.parentRecordIdx);

                        static constexpr std::string_view rootPath = "/";

                        std::string_view pathA = (itA != m_db->directoryPaths.end()) ? std::string_view(itA->second) : rootPath;
                        std::string_view pathB = (itB != m_db->directoryPaths.end()) ? std::string_view(itB->second) : rootPath;

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
        return rec.isDir ? QIcon::fromTheme("inode-directory") : QIcon::fromTheme("document-new");
    }

    switch (index.column()) {
        case 0: // Name: Extracted directly from the string pool
            return QString::fromUtf8(&m_db->stringPool[rec.nameOffset], rec.nameLen);
        case 1: // Path: Resolved from the directory map
        {
            auto it = m_db->directoryPaths.find(rec.parentRecordIdx);
            if (it != m_db->directoryPaths.end()) {
                return QString::fromStdString(it->second);
            }
            return QString("/");
        }
        case 2: // Size: Formatted according to the user's locale
            if (rec.isDir) return QString("<DIR>");

            // Format as bytes/KB/MB etc, with 2 decimal places
            //return QLocale().formattedDataSize(rec.size, 2, QLocale::DataSizeTraditionalFormat);

            // Formats the raw byte count with appropriate thousands separators
            return QLocale().toString(static_cast<qlonglong>(rec.size));
        case 3: // Date: Formatted using NTFS-specific logic
            return QString::fromStdString(NtfsUtils::ntfsTimeToStr(rec.modificationTime));
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