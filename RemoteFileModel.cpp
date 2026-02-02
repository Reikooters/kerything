// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <iostream>
#include <QDateTime>
#include <limits>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusVariant>
#include "RemoteFileModel.h"
#include "DbusIndexerClient.h"
#include "GuiUtils.h"

static constexpr quint32 kFlagIsDir = 1u << 0;

RemoteFileModel::RemoteFileModel(DbusIndexerClient* client, QObject* parent)
    : QAbstractTableModel(parent), m_client(client) {}

int RemoteFileModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    if (m_offline) return 0;

    // QAbstractItemView expects int; clamp for safety.
    const quint64 maxInt = static_cast<quint64>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(m_totalHits, maxInt));
}

int RemoteFileModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return 4; // Name, Path, Size, Date Modified
}

QVariant RemoteFileModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    switch (section) {
        case 0: return "Name";
        case 1: return "Path";
        case 2: return "Size";
        case 3: return "Date Modified";
        default: return {};
    }
}

QString RemoteFileModel::sortKeyForColumn(int column) const {
    switch (column) {
        case 0: return QStringLiteral("name");
        case 1: return QStringLiteral("path");
        case 2: return QStringLiteral("size");
        case 3: return QStringLiteral("mtime");
        default: return QStringLiteral("name");
    }
}

QString RemoteFileModel::sortDirForOrder(Qt::SortOrder order) const {
    return (order == Qt::DescendingOrder) ? QStringLiteral("desc") : QStringLiteral("asc");
}

void RemoteFileModel::clearAll() {
    beginResetModel();
    m_pages.clear();
    m_pagesLoading.clear();
    m_pagesFailed.clear();
    m_dirCache.clear();
    m_totalHits = 0;
    endResetModel();
}

void RemoteFileModel::setOffline(bool offline) {
    if (m_offline == offline) return;

    m_offline = offline;

    // When going offline: clear everything and present an empty table.
    // When coming back online: keep empty until caller decides to setQuery().
    clearAll();
}

void RemoteFileModel::invalidate() {
    if (m_offline) {
        clearAll();
        return;
    }

    beginResetModel();
    m_pages.clear();
    m_pagesLoading.clear();
    m_pagesFailed.clear();
    m_dirCache.clear();
    m_totalHits = 0;
    endResetModel();

    ensurePageLoaded(0);

    // Refresh view to reflect new rowCount()/data availability.
    beginResetModel();
    endResetModel();
}

void RemoteFileModel::setQuery(const QString& query) {
    m_query = query;

    // If offline, keep the table empty and don't attempt any calls.
    if (m_offline) {
        clearAll();
        return;
    }

    // Reset and load first page so rowCount becomes meaningful.
    beginResetModel();
    m_pages.clear();
    m_pagesLoading.clear();
    m_pagesFailed.clear();
    m_dirCache.clear();
    m_totalHits = 0;
    endResetModel();

    ensurePageLoaded(0);
    // After first fetch, rowCount changes -> reset to update view properly.
    beginResetModel();
    endResetModel();
}

void RemoteFileModel::setSort(int column, Qt::SortOrder order) {
    m_sortKey = sortKeyForColumn(column);
    m_sortDir = sortDirForOrder(order);

    // Changing sort invalidates cached pages (since order changes).
    setQuery(m_query);
}

void RemoteFileModel::sort(int column, Qt::SortOrder order) {
    setSort(column, order);
}

static QVariant unwrapDbusVariant(const QVariant& v) {
    if (v.canConvert<QDBusVariant>()) {
        return qvariant_cast<QDBusVariant>(v).variant();
    }
    return v;
}

static bool decodeDirPairFromDbusArgument(const QDBusArgument& in, quint32& dirIdOut, QString& pathOut) {
    const QDBusArgument a = in;

    QVariantList fields;
    fields.reserve(2);

    a.beginArray();
    while (!a.atEnd()) {
        QVariant elem;
        a >> elem;
        fields.push_back(unwrapDbusVariant(elem));
    }
    a.endArray();

    if (fields.size() != 2) {
        std::cerr << "Row array has unexpected field count (got " << fields.size() << ", expected 2)" << std::endl;
        return false;
        // if (errorOut) {
        //     *errorOut = QString("Row array has unexpected field count (got %1, expected 7)")
        //                     .arg(fields.size());
        // }
        // return false;
    }

    quint32 dirId = fields[0].toUInt();
    QString path = fields[1].toString();

    dirIdOut = dirId;
    pathOut = path;
    return true;
}

std::optional<RemoteFileModel::Row> RemoteFileModel::parseRow(const QVariant& input, QString* errorOut) {
    const QVariant v = unwrapDbusVariant(input);

    // Rows are arriving as QDBusArgument, but they are NOT a struct-of-7-basics.
    // Decode as an ARRAY of QVariant values.
    if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument a = qvariant_cast<QDBusArgument>(v);

        QVariantList fields;
        fields.reserve(7);

        a.beginArray();
        while (!a.atEnd()) {
            QVariant elem;
            a >> elem;
            fields.push_back(unwrapDbusVariant(elem));
        }
        a.endArray();

        if (fields.size() != 7) {
            if (errorOut) {
                *errorOut = QString("Row array has unexpected field count (got %1, expected 7)")
                                .arg(fields.size());
            }
            return std::nullopt;
        }

        Row r;
        r.entryId = fields[0].toULongLong();
        r.deviceId = fields[1].toString();
        r.name = fields[2].toString();
        r.dirId = fields[3].toUInt();
        r.size = fields[4].toULongLong();
        r.mtime = fields[5].toLongLong();
        r.flags = fields[6].toUInt();
        return r;
    }

    // Fallback (if it ever arrives as a plain QVariantList)
    const QVariantList list = v.toList();
    if (list.size() != 7) {
        if (errorOut) {
            *errorOut = QString("Row has unexpected field count (got %1, expected 7)")
                            .arg(list.size());
        }
        return std::nullopt;
    }

    Row r;
    r.entryId = unwrapDbusVariant(list[0]).toULongLong();
    r.deviceId = unwrapDbusVariant(list[1]).toString();
    r.name = unwrapDbusVariant(list[2]).toString();
    r.dirId = unwrapDbusVariant(list[3]).toUInt();
    r.size = unwrapDbusVariant(list[4]).toULongLong();
    r.mtime = unwrapDbusVariant(list[5]).toLongLong();
    r.flags = unwrapDbusVariant(list[6]).toUInt();
    return r;
}

void RemoteFileModel::ensurePageLoaded(quint32 pageIndex) const {
    if (m_offline) return;
    if (!m_client) return;

    if (m_pages.contains(pageIndex)) return;
    if (m_pagesLoading.contains(pageIndex)) return;

    m_pagesLoading.insert(pageIndex);

    const quint32 offset = pageIndex * kPageSize;
    const quint32 limit  = kPageSize;

    QString err;
    auto sr = m_client->search(m_query, m_deviceIds, m_sortKey, m_sortDir, offset, limit, {}, &err);
    if (!sr) {
        m_pagesLoading.remove(pageIndex);

        if (!m_pagesFailed.contains(pageIndex)) {
            m_pagesFailed.insert(pageIndex);

            const QString msg = err.trimmed().isEmpty()
                ? QStringLiteral("Failed to fetch results from daemon • live updates paused")
                : (QStringLiteral("Daemon error: ") + err);

            Q_EMIT const_cast<RemoteFileModel*>(this)->transientError(msg);
        }

        return;
    }

    m_totalHits = sr->totalHits;

    QVector<Row> parsed;
    parsed.reserve(sr->rows.size());

    // Collect dirIds per device to resolve in batches
    QHash<QString, QSet<quint32>> toResolve;

    for (const auto& item : sr->rows) {
        QString parseErr;
        auto rowOpt = parseRow(item, &parseErr);
        if (!rowOpt) {
            continue;
        }

        Row r = *rowOpt;
        parsed.push_back(r);

        if (!m_dirCache[r.deviceId].contains(r.dirId)) {
            toResolve[r.deviceId].insert(r.dirId);
        }
    }

    m_pages.insert(pageIndex, std::move(parsed));

    // Resolve directory paths per device (path pooling)
    for (auto it = toResolve.begin(); it != toResolve.end(); ++it) {
        const QString deviceId = it.key();
        const QList<quint32> dirIds = it.value().values();

        QString err2;
        auto pairs = m_client->resolveDirectories(deviceId, dirIds, &err2);
        if (!pairs) {
            // Not fatal; keep placeholders. Surface a one-shot message for this page.
            if (!m_pagesFailed.contains(pageIndex)) {
                m_pagesFailed.insert(pageIndex);

                const QString msg = err2.trimmed().isEmpty()
                    ? QStringLiteral("Failed to resolve paths from daemon • live updates paused")
                    : (QStringLiteral("Daemon error: ") + err2);

                Q_EMIT const_cast<RemoteFileModel*>(this)->transientError(msg);
            }
            continue;
        }

        for (const auto& p : *pairs) {
            const QVariant pv = unwrapDbusVariant(p);

            if (pv.canConvert<QDBusArgument>()) {
                quint32 dirId = 0;
                QString path;
                if (decodeDirPairFromDbusArgument(qvariant_cast<QDBusArgument>(pv), dirId, path)) {
                    m_dirCache[deviceId].insert(dirId, path);
                }
                continue;
            }

            // Fallback if it arrives as a list
            const QVariantList pair = pv.toList();
            if (pair.size() != 2) continue;

            const quint32 dirId = unwrapDbusVariant(pair[0]).toUInt();
            const QString path = unwrapDbusVariant(pair[1]).toString();
            m_dirCache[deviceId].insert(dirId, path);
        }
    }

    m_pagesLoading.remove(pageIndex);
}

QVariant RemoteFileModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || role != Qt::DisplayRole) return {};
    if (m_offline) return {};

    auto placeholder = []() -> QVariant { return QStringLiteral("…"); };

    const int row = index.row();
    if (row < 0) return placeholder();

    const quint32 pageIndex = static_cast<quint32>(row) / kPageSize;
    const quint32 inPage = static_cast<quint32>(row) % kPageSize;

    ensurePageLoaded(pageIndex);

    const auto pit = m_pages.find(pageIndex);
    if (pit == m_pages.end()) {
        return placeholder();
    }

    const auto& page = pit.value();
    if (inPage >= static_cast<quint32>(page.size())) {
        return placeholder();
    }

    const Row& r = page[static_cast<int>(inPage)];

    switch (index.column()) {
        case 0:
            return r.name;
        case 1: {
            const auto devIt = m_dirCache.find(r.deviceId);
            if (devIt == m_dirCache.end()) return placeholder();
            const auto pathIt = devIt->find(r.dirId);
            if (pathIt == devIt->end()) return placeholder();
            return *pathIt;
        }
        case 2:
            if (r.flags & kFlagIsDir) return QStringLiteral("<DIR>");
            return QLocale().toString(static_cast<qlonglong>(r.size));
        case 3: {
            if (r.mtime <= 0) return QStringLiteral("invalid-time");
            // const QDateTime dt = QDateTime::fromSecsSinceEpoch(r.mtime);
            // return QLocale().toString(dt, QLocale::ShortFormat);
            return QString::fromStdString(GuiUtils::uint64ToFormattedTime(r.mtime));
        }
        default:
            return placeholder();
    }
}