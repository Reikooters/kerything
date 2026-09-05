// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <execution>
#include <algorithm>
#include <iterator>
#include <string>
#include <QBuffer>
#include <QIcon>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>
#include <QDir>
#include <QColor>
#include "FileModel.h"

#include "AppController.h"
#include "GuiUtils.h"
#include "SearchResultColumns.h"

#include "IndexController.h"
#include "FilesystemConstants.h"

namespace {
    constexpr uint8_t NoMountPoint = IndexController::RecordHandle::NoMountPoint;

    bool hasMountedPath(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle
    ) {
        return handle.mountPointIdx != NoMountPoint &&
               handle.mountPointIdx < static_cast<uint8_t>(deviceIndex.mountPoints.size());
    }

    QString displayVolumeName(const IndexController::DeviceIndex& deviceIndex) {
        QString label = deviceIndex.label.trimmed();
        if (!label.isEmpty() && label != QStringLiteral("TODO")) {
            return label;
        }

        QString deviceId = deviceIndex.deviceId.trimmed();
        if (!deviceId.isEmpty()) {
            static constexpr qsizetype MaxDeviceIdDisplayLength = 24;

            if (deviceId.size() <= MaxDeviceIdDisplayLength) {
                return deviceId;
            }

            return deviceId.left(MaxDeviceIdDisplayLength - 1) + QStringLiteral("…");
        }

        QString devNode = deviceIndex.devNode.trimmed();
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
        const QString relativePath = QDir::cleanPath(QString::fromStdString(filesystemPath));

        if (!hasMountedPath(deviceIndex, handle)) {
            const QString volumeName = displayVolumeName(deviceIndex);

            if (relativePath == QStringLiteral("/")) {
                return volumeName + QStringLiteral(":/");
            }

            return volumeName + QStringLiteral(":") + relativePath;
        }

        return deviceIndex.mountedPathForMountPointIndex(
            static_cast<int>(handle.mountPointIdx),
            filesystemPath
        );
    }

    bool isMountedBtrfsNamespaceRootRecord(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle
    ) {
        if (!deviceIndex.isBtrfsNamespaceRootRecord(handle.recordIdx)) {
            return false;
        }

        const int mountInfoIndex = deviceIndex.mountInfoIndexForMountPointIndex(
            static_cast<int>(handle.mountPointIdx)
        );

        if (mountInfoIndex < 0 ||
            mountInfoIndex >= static_cast<int>(deviceIndex.mounts.size())) {
            return false;
        }

        const FileRecordNamespace namespaceEntry =
            deviceIndex.namespaceForRecord(handle.recordIdx);

        const BlockDeviceMountInfo& mount =
            deviceIndex.mounts[static_cast<std::size_t>(mountInfoIndex)];

        return mount.btrfsRootId != 0 &&
               mount.btrfsRootId == namespaceEntry.fsNamespace;
    }

    QString mountedFullPathForHandle(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle,
        const FileRecord& record
    ) {
        if (!hasMountedPath(deviceIndex, handle)) {
            return {};
        }

        if (record.nameOffset + record.nameLen > deviceIndex.stringPool.size()) {
            return {};
        }

        if (deviceIndex.isFilesystemRootRecord(handle.recordIdx)) {
            return QDir::cleanPath(deviceIndex.mountPoints.at(handle.mountPointIdx));
        }

        if (isMountedBtrfsNamespaceRootRecord(deviceIndex, handle)) {
            return QDir::cleanPath(deviceIndex.mountPoints.at(handle.mountPointIdx));
        }

        const QString fileName = QString::fromUtf8(
            &deviceIndex.stringPool[record.nameOffset],
            record.nameLen
        );

        const std::string parentPath = deviceIndex.getFullPath(record.parentRecordIdx);
        const QString mountedParentPath = mountedPathForHandle(deviceIndex, handle, parentPath);

        return QDir::cleanPath(mountedParentPath + QStringLiteral("/") + fileName);
    }

    QString fileTypeText(const FileRecord& rec)
    {
        const bool isDirectory = (rec.flags & FileRecord_IsDir) != 0;
        const bool isSymlink = (rec.flags & FileRecord_IsSymlink) != 0;

        if (isDirectory && isSymlink) {
            return QStringLiteral("Folder symlink");
        }

        if (isDirectory) {
            return QStringLiteral("Folder");
        }

        if (isSymlink) {
            return QStringLiteral("File symlink");
        }

        return QStringLiteral("File");
    }

    QString fileSizeText(const FileRecord& rec)
    {
        if ((rec.flags & FileRecord_IsDir) != 0) {
            return QStringLiteral("—");
        }

        const QLocale locale;
        QString readableSize = locale.formattedDataSize(
            static_cast<qint64>(rec.size),
            1,
            QLocale::DataSizeIecFormat
        );

        if (rec.size < 1024) {
            return readableSize;
        }

        const QString byteCount = locale.toString(static_cast<qulonglong>(rec.size));

        return QStringLiteral("%1 (%2 bytes)").arg(readableSize, byteCount);
    }

    QString modifiedTimeText(const FileRecord& rec)
    {
        if (rec.modificationTime == 0) {
            return QStringLiteral("Unknown");
        }

        return QString::fromStdString(GuiUtils::uint64ToFormattedTime(rec.modificationTime));
    }

    QString tooltipRow(const QString& label, const QString& value)
    {
        return QStringLiteral(
            "<tr>"
            "<td style='padding-right: 1em; white-space: nowrap; font-weight: 600;'>%1</td>"
            "<td style='white-space: nowrap;'>%2</td>"
            "</tr>"
        ).arg(
            label.toHtmlEscaped(),
            value.toHtmlEscaped()
        );
    }

    QString warningIconDataUri()
    {
        static const QString dataUri = []() {
            const QIcon icon = QIcon::fromTheme(
                QStringLiteral("dialog-warning"),
                QIcon::fromTheme(QStringLiteral("emblem-warning"))
            );

            if (icon.isNull()) {
                return QString();
            }

            const QPixmap pixmap = icon.pixmap(16, 16);
            if (pixmap.isNull()) {
                return QString();
            }

            QByteArray bytes;
            QBuffer buffer(&bytes);

            if (!buffer.open(QIODevice::WriteOnly)) {
                return QString();
            }

            if (!pixmap.save(&buffer, "PNG")) {
                return QString();
            }

            return QStringLiteral("data:image/png;base64,%1")
                .arg(QString::fromLatin1(bytes.toBase64()));
        }();

        return dataUri;
    }

    QString resultToolTip(
        const IndexController::DeviceIndex& deviceIndex,
        const IndexController::RecordHandle& handle,
        const FileRecord& rec,
        const std::string& parentFilesystemPath,
        const QString& fileName
    ) {
        const QString indexedParentPath = QDir::cleanPath(QString::fromStdString(parentFilesystemPath));
        const QString displayParentPath = mountedPathForHandle(deviceIndex, handle, parentFilesystemPath);
        const bool isDirectory = (rec.flags & FileRecord_IsDir) != 0;
        const bool mounted = hasMountedPath(deviceIndex, handle);

        QString rows;
        rows += tooltipRow(QStringLiteral("File name"), fileName);
        rows += tooltipRow(QStringLiteral("Type"), fileTypeText(rec));

        if (!isDirectory) {
            rows += tooltipRow(QStringLiteral("Size"), fileSizeText(rec));
        }

        rows += tooltipRow(QStringLiteral("Modified"), modifiedTimeText(rec));

        if (mounted) {
            rows += tooltipRow(
                deviceIndex.isFilesystemRootRecord(handle.recordIdx)
                    ? QStringLiteral("Path")
                    : QStringLiteral("Parent path"),
                deviceIndex.isFilesystemRootRecord(handle.recordIdx)
                    ? mountedFullPathForHandle(deviceIndex, handle, rec)
                    : displayParentPath
            );
        } else {
            rows += tooltipRow(QStringLiteral("Indexed parent path"), indexedParentPath);
            rows += tooltipRow(QStringLiteral("Display parent path"), displayParentPath);
        }

        rows += tooltipRow(QStringLiteral("Device"), deviceIndex.deviceId);

        if (mounted) {
            rows += tooltipRow(QStringLiteral("Device node"), deviceIndex.devNode);
        } else {
            rows += tooltipRow(QStringLiteral("Last known node"), deviceIndex.devNode);
        }

        const QString warningIcon = warningIconDataUri();
        const QString warningIconHtml = warningIcon.isEmpty()
            ? QStringLiteral("⚠️ ")
            : QStringLiteral(
                "<img src=\"%1\" width=\"16\" height=\"16\" "
                "style=\"vertical-align: middle; margin-right: 0.35em;\">"
            ).arg(warningIcon);

        const QString warning = mounted
            ? QString()
            : QStringLiteral(
                "<div style='margin-top: 0.6em;'>"
                "%1<b>This result is from an unmounted device.</b><br>"
                "Mount the device to open this item or perform file actions."
                "</div>"
            ).arg(warningIconHtml);

        return QStringLiteral(
            "<qt>"
            "<div style='white-space: nowrap;'>"
            "<table cellspacing='0' cellpadding='0'>%1</table>"
            "%2"
            "</div>"
            "</qt>"
        ).arg(
            rows,
            warning
        );
    }

    QIcon iconForFileName(const QString& fileName)
    {
        static QMimeDatabase mimeDatabase;

        const QMimeType mimeType = mimeDatabase.mimeTypeForFile(
            fileName,
            QMimeDatabase::MatchExtension
        );

        const QString iconName = mimeType.iconName();
        if (!iconName.isEmpty()) {
            const QIcon icon = QIcon::fromTheme(iconName);
            if (!icon.isNull()) {
                return icon;
            }
        }

        const QString genericIconName = mimeType.genericIconName();
        if (!genericIconName.isEmpty()) {
            const QIcon icon = QIcon::fromTheme(genericIconName);
            if (!icon.isNull()) {
                return icon;
            }
        }

        return QIcon::fromTheme(
            QStringLiteral("text-x-generic"),
            QIcon::fromTheme(QStringLiteral("document-new"))
        );
    }

    QString formatModelBytes(quint64 bytes)
    {
        static constexpr double KiB = 1024.0;
        static constexpr double MiB = KiB * 1024.0;
        static constexpr double GiB = MiB * 1024.0;

        if (bytes >= static_cast<quint64>(GiB)) {
            return QStringLiteral("%1 GiB").arg(bytes / GiB, 0, 'f', 2);
        }

        if (bytes >= static_cast<quint64>(MiB)) {
            return QStringLiteral("%1 MiB").arg(bytes / MiB, 0, 'f', 2);
        }

        if (bytes >= static_cast<quint64>(KiB)) {
            return QStringLiteral("%1 KiB").arg(bytes / KiB, 0, 'f', 2);
        }

        return QStringLiteral("%1 B").arg(bytes);
    }

    template <typename Vector>
    quint64 modelVectorCapacityBytes(const Vector& vector)
    {
        using ValueType = typename Vector::value_type;
        return static_cast<quint64>(vector.capacity()) * sizeof(ValueType);
    }

    QString formatModelBytesPerItem(quint64 bytes, std::size_t count)
    {
        if (count == 0) {
            return QStringLiteral("n/a");
        }

        const double bytesPerItem =
            static_cast<double>(bytes) / static_cast<double>(count);

        return QStringLiteral("%1 B").arg(bytesPerItem, 0, 'f', 2);
    }
}

FileModel::FileModel(AppController* controller, QObject *parent)
    : QAbstractTableModel(parent),
      controller_(controller) {}

void FileModel::setSearchHighlightTerms(
    QStringList terms,
    bool enabled,
    bool matchCase,
    bool matchWholeWord,
    bool useRegex
) {
    terms.removeAll(QString());

    for (QString& term : terms) {
        term = term.trimmed();
    }

    terms.removeDuplicates();

    if (searchHighlightTerms_ == terms &&
        searchHighlightTermsEnabled_ == enabled &&
        searchHighlightMatchCase_ == matchCase &&
        searchHighlightMatchWholeWord_ == matchWholeWord &&
        searchHighlightUseRegex_ == useRegex) {
        return;
    }

    searchHighlightTerms_ = std::move(terms);
    searchHighlightTermsEnabled_ = enabled;
    searchHighlightMatchCase_ = matchCase;
    searchHighlightMatchWholeWord_ = matchWholeWord;
    searchHighlightUseRegex_ = useRegex;

    if (rowCount() > 0) {
        Q_EMIT dataChanged(
            index(0, SearchResultColumn::Name),
            index(rowCount() - 1, SearchResultColumn::Name),
            {
                HighlightTermsRole,
                HighlightMatchCaseRole,
                HighlightMatchWholeWordRole,
                HighlightUseRegexRole
            }
        );
    }
}

void FileModel::trimSearchResultsOverCapacity()
{
    // Keep enough capacity for a broad search over the current index, not just
    // for the current query. During interactive typing, the current result count
    // can swing from millions to thousands and back again.
    static constexpr std::size_t RetainedResultSlackBytes = 4 * 1024 * 1024;
    static constexpr std::size_t MinExcessResultCapacityBytesToTrim = 4 * 1024 * 1024;

    const std::size_t resultSize = searchResults_.size();
    const std::size_t resultCapacity = searchResults_.capacity();

    const std::size_t retainedSlackHandles =
        RetainedResultSlackBytes / sizeof(IndexController::RecordHandle);

    const std::size_t minExcessHandlesToTrim =
        MinExcessResultCapacityBytesToTrim / sizeof(IndexController::RecordHandle);

    std::size_t indexResultCapacityFloor = 0;

    if (controller_ && controller_->indexController()) {
        indexResultCapacityFloor = controller_->indexController()->maxSearchResultCount();
    }

    if (resultSize == 0 && indexResultCapacityFloor == 0) {
        if (resultCapacity > 0) {
            std::vector<IndexController::RecordHandle>{}.swap(searchResults_);
        }

        return;
    }

    const std::size_t retainedCapacity =
        std::max(resultSize, indexResultCapacityFloor) + retainedSlackHandles;

    if (resultCapacity <= retainedCapacity + minExcessHandlesToTrim) {
        return;
    }

    std::vector<IndexController::RecordHandle> trimmed;
    trimmed.reserve(retainedCapacity);
    trimmed.assign(
        std::make_move_iterator(searchResults_.begin()),
        std::make_move_iterator(searchResults_.end())
    );

    searchResults_.swap(trimmed);
}

void FileModel::setSearchResults(std::vector<IndexController::RecordHandle> newResults)
{
    beginResetModel();
    searchResults_ = std::move(newResults);
    trimSearchResultsOverCapacity();
    endResetModel();
}

void FileModel::setSortedSearchResults(
    std::vector<IndexController::RecordHandle> newResults,
    int column,
    Qt::SortOrder order
) {
    if (controller_ && controller_->indexController() && !newResults.empty()) {
        newResults = controller_->indexController()->sortSearchResults(
            std::move(newResults),
            column,
            order,
            sortScratch_
        );
    }

    beginResetModel();
    searchResults_ = std::move(newResults);
    trimSearchResultsOverCapacity();
    endResetModel();
}

void FileModel::notifyRowsDataChanged(int firstRow, int lastRow)
{
    if (searchResults_.empty()) {
        return;
    }

    if (firstRow > lastRow) {
        return;
    }

    firstRow = std::max(firstRow, 0);
    lastRow = std::min(lastRow, rowCount() - 1);

    if (firstRow > lastRow) {
        return;
    }

    Q_EMIT dataChanged(
        index(firstRow, 0),
        index(lastRow, columnCount() - 1),
        {
            Qt::DisplayRole,
            Qt::DecorationRole,
            Qt::ToolTipRole,
            Qt::ForegroundRole
        }
    );
}

void FileModel::trimSortScratch()
{
    sortScratch_.clearAndRelease();
}

QString FileModel::memoryStatsText() const
{
    QString text;
    QTextStream out(&text);

    const quint64 searchResultsBytes = modelVectorCapacityBytes(searchResults_);
    const quint64 resultsOrderBytes = modelVectorCapacityBytes(sortScratch_.resultsOrder);
    const quint64 sortedResultsBytes = modelVectorCapacityBytes(sortScratch_.sortedResults);
    const quint64 numericKeysBytes = modelVectorCapacityBytes(sortScratch_.numericKeys);
    const quint64 sortScratchBytes =
        resultsOrderBytes +
        sortedResultsBytes +
        numericKeysBytes;
    const quint64 modelSubtotalBytes =
        searchResultsBytes +
        sortScratchBytes;

    std::size_t maxPossibleResults = 0;

    if (controller_ && controller_->indexController()) {
        maxPossibleResults = controller_->indexController()->maxSearchResultCount();
    }

    const std::size_t excessSearchResultCapacity =
        searchResults_.capacity() > searchResults_.size()
            ? searchResults_.capacity() - searchResults_.size()
            : 0;

    const quint64 excessSearchResultCapacityBytes =
        static_cast<quint64>(excessSearchResultCapacity) *
        sizeof(IndexController::RecordHandle);

    out << "FileModel:\n";
    out << "  max possible search result count from index: "
        << maxPossibleResults
        << '\n';

    out << "  searchResults size/capacity: "
        << searchResults_.size()
        << '/'
        << searchResults_.capacity()
        << " => "
        << formatModelBytes(searchResultsBytes)
        << '\n';

    out << "    excess searchResults capacity: "
        << excessSearchResultCapacity
        << " => "
        << formatModelBytes(excessSearchResultCapacityBytes)
        << '\n';

    out << "    searchResults bytes per current row: "
        << formatModelBytesPerItem(searchResultsBytes, searchResults_.size())
        << '\n';

    out << "  sortScratch.resultsOrder size/capacity: "
        << sortScratch_.resultsOrder.size()
        << '/'
        << sortScratch_.resultsOrder.capacity()
        << " => "
        << formatModelBytes(resultsOrderBytes)
        << '\n';

    out << "  sortScratch.sortedResults size/capacity: "
        << sortScratch_.sortedResults.size()
        << '/'
        << sortScratch_.sortedResults.capacity()
        << " => "
        << formatModelBytes(sortedResultsBytes)
        << '\n';

    out << "  sortScratch.numericKeys size/capacity: "
        << sortScratch_.numericKeys.size()
        << '/'
        << sortScratch_.numericKeys.capacity()
        << " => "
        << formatModelBytes(numericKeysBytes)
        << '\n';

    out << "  retained sort scratch total: "
        << formatModelBytes(sortScratchBytes)
        << '\n';

    out << "  releasable by trimSortScratch(): "
        << formatModelBytes(sortScratchBytes)
        << '\n';

    const quint64 searchResultsIf16ByteHandles =
        static_cast<quint64>(searchResults_.capacity()) * 16;
    const quint64 sortedResultsIf16ByteHandles =
        static_cast<quint64>(sortScratch_.sortedResults.capacity()) * 16;
    const quint64 searchResultsIf12ByteHandles =
        static_cast<quint64>(searchResults_.capacity()) * 12;
    const quint64 sortedResultsIf12ByteHandles =
        static_cast<quint64>(sortScratch_.sortedResults.capacity()) * 12;

    out << "  RecordHandle what-if estimates:\n";
    out << "    current sizeof(RecordHandle): "
        << sizeof(IndexController::RecordHandle)
        << " bytes\n";
    out << "    searchResults if handles were 16 bytes: "
        << formatModelBytes(searchResultsIf16ByteHandles)
        << " (current saving "
        << formatModelBytes(
            searchResultsIf16ByteHandles > searchResultsBytes
                ? searchResultsIf16ByteHandles - searchResultsBytes
                : 0
        )
        << ")\n";
    out << "    sortScratch.sortedResults if handles were 16 bytes: "
        << formatModelBytes(sortedResultsIf16ByteHandles)
        << " (current saving "
        << formatModelBytes(
            sortedResultsIf16ByteHandles > sortedResultsBytes
                ? sortedResultsIf16ByteHandles - sortedResultsBytes
                : 0
        )
        << ")\n";
    out << "    searchResults if handles were 12 bytes: "
        << formatModelBytes(searchResultsIf12ByteHandles)
        << " (current saving "
        << formatModelBytes(
            searchResultsIf12ByteHandles > searchResultsBytes
                ? searchResultsIf12ByteHandles - searchResultsBytes
                : 0
        )
        << ")\n";
    out << "    sortScratch.sortedResults if handles were 12 bytes: "
        << formatModelBytes(sortedResultsIf12ByteHandles)
        << " (current saving "
        << formatModelBytes(
            sortedResultsIf12ByteHandles > sortedResultsBytes
                ? sortedResultsIf12ByteHandles - sortedResultsBytes
                : 0
        )
        << ")\n";

    const quint64 temporaryFullSortGatherBufferBytes =
        static_cast<quint64>(searchResults_.size()) *
        sizeof(IndexController::RecordHandle);

    out << "  temporary full-result gather buffer used during sort: up to "
        << formatModelBytes(temporaryFullSortGatherBufferBytes)
        << '\n';

    out << "  rough accounted model subtotal: "
        << formatModelBytes(modelSubtotalBytes)
        << '\n';

    out << "  rough accounted model bytes per current row: "
        << formatModelBytesPerItem(modelSubtotalBytes, searchResults_.size())
        << '\n';

    return text;
}

void FileModel::sort(int column, Qt::SortOrder order) {
    if (!controller_ || !controller_->indexController() || searchResults_.empty()) {
        return;
    }

    beginResetModel();

    searchResults_ = controller_->indexController()->sortSearchResults(
        std::move(searchResults_),
        column,
        order,
        sortScratch_
    );

    trimSearchResultsOverCapacity();

    endResetModel();
}

int FileModel::rowCount(const QModelIndex &parent) const {
    return static_cast<int>(searchResults_.size());
}

int FileModel::columnCount(const QModelIndex &parent) const {
    Q_UNUSED(parent);
    return SearchResultColumn::Count;
}

QVariant FileModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
        case SearchResultColumn::Name: return "Name";
        case SearchResultColumn::Path: return "Path";
        case SearchResultColumn::Size: return "Size";
        case SearchResultColumn::DateModified: return "Date Modified";
        default: return {};
    }
}

QVariant FileModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) {
        return {};
    }

    if (index.row() < 0 || index.row() >= static_cast<int>(searchResults_.size())) {
        return {};
    }

    const bool supportedRole =
        role == Qt::DisplayRole ||
        role == Qt::ToolTipRole ||
        role == Qt::ForegroundRole ||
        role == Qt::DecorationRole ||
        role == HighlightTermsRole ||
        role == HighlightMatchCaseRole ||
        role == HighlightMatchWholeWordRole ||
        role == HighlightUseRegexRole;

    if (!supportedRole) {
        return {};
    }

    if (role == HighlightTermsRole) {
        if (!searchHighlightTermsEnabled_ ||
            index.column() != SearchResultColumn::Name ||
            searchHighlightTerms_.isEmpty()) {
            return {};
        }

        return searchHighlightTerms_;
    }

    if (role == HighlightMatchCaseRole) {
        if (!searchHighlightTermsEnabled_ ||
            index.column() != SearchResultColumn::Name ||
            searchHighlightTerms_.isEmpty()) {
            return {};
        }

        return searchHighlightMatchCase_;
    }

    if (role == HighlightMatchWholeWordRole) {
        if (!searchHighlightTermsEnabled_ ||
            index.column() != SearchResultColumn::Name ||
            searchHighlightTerms_.isEmpty()) {
            return {};
        }

        return searchHighlightMatchWholeWord_;
    }

    if (role == HighlightUseRegexRole) {
        if (!searchHighlightTermsEnabled_ ||
            index.column() != SearchResultColumn::Name ||
            searchHighlightTerms_.isEmpty()) {
            return {};
        }

        return searchHighlightUseRegex_;
    }

    if (!controller_ || !controller_->indexController()) {
        return {};
    }

    const auto handle = searchResults_[index.row()];

    return controller_->indexController()->withDeviceIndexRead(
        handle.indexId,
        [&](const IndexController::DeviceIndex* deviceIndex) -> QVariant {
            if (!deviceIndex) {
                return {};
            }

            if (static_cast<uint8_t>(deviceIndex->generation) != handle.generation) {
                return {};
            }

            if (!deviceIndex->isReady) {
                return {};
            }

            if (handle.recordIdx >= deviceIndex->fileRecords.size()) {
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
                const QString fileName = QString::fromUtf8(
                    &deviceIndex->stringPool[rec.nameOffset],
                    rec.nameLen
                );

                const std::string parentPath = deviceIndex->getFullPath(rec.parentRecordIdx);
                return resultToolTip(*deviceIndex, handle, rec, parentPath, fileName);
            }

            // DecorationRole provides the icon shown next to the filename/path.
            // This is used to differentiate Files vs Folders and mark unmounted results.
            if (role == Qt::DecorationRole) {
                if (index.column() == SearchResultColumn::Path && !mounted) {
                    return QIcon::fromTheme(
                        QStringLiteral("dialog-warning"),
                        QIcon::fromTheme(QStringLiteral("emblem-warning"))
                    );
                }

                if (index.column() != SearchResultColumn::Name) {
                    return {};
                }

                const QString fileName = QString::fromUtf8(
                    &deviceIndex->stringPool[rec.nameOffset],
                    rec.nameLen
                );

                // Using standard KDE/Freedesktop theme names for icons
                if ((rec.flags & FileRecord_IsDir) != 0) {
                    return (rec.flags & FileRecord_IsSymlink) != 0
                        ? QIcon::fromTheme("inode-directory-symlink", QIcon::fromTheme("folder-remote"))
                        : QIcon::fromTheme("inode-directory", QIcon::fromTheme("folder"));
                }

                if ((rec.flags & FileRecord_IsSymlink) != 0) {
                    const QIcon icon = QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"));
                    if (!icon.isNull()) {
                        return icon;
                    }
                }

                return iconForFileName(fileName);
            }

            switch (index.column()) {
                case SearchResultColumn::Name: // Name: Extracted directly from the string pool
                    return QString::fromUtf8(&deviceIndex->stringPool[rec.nameOffset], rec.nameLen);

                case SearchResultColumn::Path: { // Path: Resolved from the directory map and current mount/virtual volume
                    const std::string parentPath = deviceIndex->getFullPath(rec.parentRecordIdx);
                    return mountedPathForHandle(*deviceIndex, handle, parentPath);
                }

                case SearchResultColumn::Size: // Size: Formatted according to the user's locale
                    if ((rec.flags & FileRecord_IsDir) != 0) {
                        return QString("<DIR>");
                    }

                    // Format as bytes/KB/MB etc, with 2 decimal places
                    //return QLocale().formattedDataSize(rec.size, 2, QLocale::DataSizeTraditionalFormat);

                    // Formats the raw byte count with appropriate thousands separators
                    return QLocale().toString(static_cast<qlonglong>(rec.size));

                case SearchResultColumn::DateModified: // Date: Formatted from unix seconds to local time
                    return QString::fromStdString(GuiUtils::uint64ToFormattedTime(rec.modificationTime));

                default:
                    return {};
            }
        }
    );
}

std::optional<QUrl> FileModel::localUrlForRow(const int row) const {
    if (row < 0 || row >= static_cast<int>(searchResults_.size())) {
        return std::nullopt;
    }

    if (!controller_ || !controller_->indexController()) {
        return std::nullopt;
    }

    const auto handle = searchResults_[row];

    return controller_->indexController()->withDeviceIndexRead(
        handle.indexId,
        [&](const IndexController::DeviceIndex* deviceIndex) -> std::optional<QUrl> {
            if (!deviceIndex) {
                return std::nullopt;
            }

            if (static_cast<uint8_t>(deviceIndex->generation) != handle.generation) {
                return std::nullopt;
            }

            if (!deviceIndex->isReady) {
                return std::nullopt;
            }

            if (handle.recordIdx >= deviceIndex->fileRecords.size()) {
                return std::nullopt;
            }

            if (!hasMountedPath(*deviceIndex, handle)) {
                return std::nullopt;
            }

            const FileRecord& rec = deviceIndex->fileRecords[handle.recordIdx];

            const QString fullPath = mountedFullPathForHandle(
                *deviceIndex,
                handle,
                rec
            );

            if (fullPath.isEmpty()) {
                return std::nullopt;
            }

            return QUrl::fromLocalFile(fullPath);
        }
    );
}

bool FileModel::isMountedRow(const int row) const
{
    if (row < 0 || row >= static_cast<int>(searchResults_.size())) {
        return false;
    }

    if (!controller_ || !controller_->indexController()) {
        return false;
    }

    const auto handle = searchResults_[row];

    return controller_->indexController()->withDeviceIndexRead(
        handle.indexId,
        [&](const IndexController::DeviceIndex* deviceIndex) -> bool {
            if (!deviceIndex) {
                return false;
            }

            if (static_cast<uint8_t>(deviceIndex->generation) != handle.generation) {
                return false;
            }

            if (!deviceIndex->isReady) {
                return false;
            }

            if (handle.recordIdx >= deviceIndex->fileRecords.size()) {
                return false;
            }

            return hasMountedPath(*deviceIndex, handle);
        }
    );
}

qsizetype FileModel::mountedRowCount(const QModelIndexList& rows) const
{
    qsizetype count = 0;

    for (const QModelIndex& index : rows) {
        if (index.isValid() && isMountedRow(index.row())) {
            ++count;
        }
    }

    return count;
}

std::optional<IndexController::RecordHandle> FileModel::recordHandleForRow(const int row) const
{
    if (row < 0 || row >= static_cast<int>(searchResults_.size())) {
        return std::nullopt;
    }

    return searchResults_[row];
}

int FileModel::rowForRecordHandle(const IndexController::RecordHandle& handle) const
{
    const auto sameHandle = [&handle](const IndexController::RecordHandle& candidate) {
        return candidate.indexId == handle.indexId &&
               candidate.generation == handle.generation &&
               candidate.recordIdx == handle.recordIdx &&
               candidate.mountPointIdx == handle.mountPointIdx;
    };

    const auto it = std::find_if(searchResults_.cbegin(), searchResults_.cend(), sameHandle);

    if (it == searchResults_.cend()) {
        return -1;
    }

    return static_cast<int>(std::distance(searchResults_.cbegin(), it));
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

    if (index.row() < 0 || index.row() >= static_cast<int>(searchResults_.size())) {
        return defaultFlags;
    }

    if (!controller_ || !controller_->indexController()) {
        return defaultFlags;
    }

    const auto handle = searchResults_[index.row()];

    const bool mounted = controller_->indexController()->withDeviceIndexRead(
        handle.indexId,
        [&](const IndexController::DeviceIndex* deviceIndex) -> bool {
            if (!deviceIndex) {
                return false;
            }

            if (static_cast<uint8_t>(deviceIndex->generation) != handle.generation) {
                return false;
            }

            if (!deviceIndex->isReady) {
                return false;
            }

            if (handle.recordIdx >= deviceIndex->fileRecords.size()) {
                return false;
            }

            return hasMountedPath(*deviceIndex, handle);
        }
    );

    if (!mounted) {
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
    if (!controller_ || !controller_->indexController()) {
        return nullptr;
    }

    QList<QUrl> urls;

    // Since selection behavior is SelectRows, 'indexes' contains one entry per column
    // for every selected row. We only process column 0 to ensure each file is added once.
    for (const QModelIndex &index : indexes) {
        if (!index.isValid()) {
            continue;
        }

        if (index.column() != SearchResultColumn::Name) {
            continue;
        }

        if (index.row() < 0 || index.row() >= static_cast<int>(searchResults_.size())) {
            continue;
        }

        const auto handle = searchResults_[index.row()];

        const std::optional<QUrl> url =
            controller_->indexController()->withDeviceIndexRead(
                handle.indexId,
                [&](const IndexController::DeviceIndex* deviceIndex) -> std::optional<QUrl> {
                    if (!deviceIndex) {
                        return std::nullopt;
                    }

                    if (static_cast<uint8_t>(deviceIndex->generation) != handle.generation) {
                        return std::nullopt;
                    }

                    if (!deviceIndex->isReady) {
                        return std::nullopt;
                    }

                    if (handle.recordIdx >= deviceIndex->fileRecords.size()) {
                        return std::nullopt;
                    }

                    if (!hasMountedPath(*deviceIndex, handle)) {
                        return std::nullopt;
                    }

                    const FileRecord& rec = deviceIndex->fileRecords[handle.recordIdx];

                    const QString fullPath = mountedFullPathForHandle(
                        *deviceIndex,
                        handle,
                        rec
                    );

                    if (fullPath.isEmpty()) {
                        return std::nullopt;
                    }

                    return QUrl::fromLocalFile(fullPath);
                }
            );

        if (url) {
            urls.append(*url);
        }
    }

    if (urls.isEmpty()) {
        return nullptr;
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);

    return mimeData;
}