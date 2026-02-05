// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <iostream>
#include <chrono>
#include <QDateTime>
#include <QDir>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <limits>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusVariant>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include "RemoteFileModel.h"
#include "DbusIndexerClient.h"
#include "GuiUtils.h"

static constexpr quint32 kFlagIsDir = 1u << 0;

RemoteFileModel::RemoteFileModel(DbusIndexerClient* client, QObject* parent)
    : QAbstractTableModel(parent), m_client(client) {}

std::optional<quint64> RemoteFileModel::entryIdAtRow(int row) const {
    if (row < 0) return std::nullopt;
    if (m_offline) return std::nullopt;

    const quint32 pageIndex = static_cast<quint32>(row) / kPageSize;
    const quint32 inPage = static_cast<quint32>(row) % kPageSize;

    ensurePageLoaded(pageIndex);

    const auto pit = m_pages.find(pageIndex);
    if (pit == m_pages.end()) return std::nullopt;

    const auto& page = pit.value();
    if (inPage >= static_cast<quint32>(page.size())) return std::nullopt;

    return page[static_cast<int>(inPage)].entryId;
}

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
    m_pagesWanted.clear();
    m_inFlightPageLoads = 0;
    m_dispatchScheduled = false;
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

    ++m_querySerial;

    // Don't reset the whole model (causes visible flicker).
    // Mark current cached pages stale and clear only what is logically invalid.
    m_pagesLoading.clear();
    m_pagesFailed.clear();
    m_pagesWanted.clear();
    m_inFlightPageLoads = 0;
    m_dispatchScheduled = false;

    m_dirCache.clear(); // index changed -> paths may change
    m_searchStartNsBySerial.clear();

    // Keep showing whatever is currently on screen until new page 0 arrives.
    // Force a reload by invalidating the pages serial.
    m_pagesSerial = 0;

    ensurePageLoaded(0);
}

void RemoteFileModel::setQuery(const QString& query) {
    m_query = query;

    // If offline, keep the table empty and don't attempt any calls.
    if (m_offline) {
        clearAll();
        return;
    }

    ++m_querySerial;

    // Don't reset the model here either.
    m_pagesLoading.clear();
    m_pagesFailed.clear();
    m_pagesWanted.clear();
    m_inFlightPageLoads = 0;
    m_dispatchScheduled = false;

    //m_dirCache.clear(); // Don't clear directory cache between search queries
    m_searchStartNsBySerial.clear();

    // Mark cached pages as stale so ensurePageLoaded() will refetch.
    m_pagesSerial = 0;

    ensurePageLoaded(0);
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

QStringList RemoteFileModel::mimeTypes() const {
    return {QStringLiteral("text/uri-list")};
}

QMimeData* RemoteFileModel::mimeData(const QModelIndexList& indexes) const {
    auto* md = new QMimeData();
    if (m_offline || !m_client) return md;

    // Collect unique rows
    QSet<int> rows;
    for (const auto& idx : indexes) {
        if (!idx.isValid()) continue;
        rows.insert(idx.row());
    }

    QList<quint64> entryIds;
    entryIds.reserve(rows.size());
    for (int r : rows) {
        auto idOpt = entryIdAtRow(r);
        if (idOpt) entryIds.push_back(*idOpt);
    }

    if (entryIds.isEmpty()) return md;

    QString err;
    auto resolved = m_client->resolveEntries(entryIds, &err);
    if (!resolved) {
        // Best-effort: if we can't resolve, provide empty mime data
        return md;
    }

    QList<QUrl> urls;
    urls.reserve(resolved->size());

    for (const QVariant& v : *resolved) {
        const QVariantMap m = v.toMap();
        const bool mounted = m.value(QStringLiteral("mounted")).toBool();
        if (!mounted) continue;

        const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
        const QString internalPath = m.value(QStringLiteral("internalPath")).toString();
        if (mp.isEmpty() || internalPath.isEmpty()) continue;

        urls.push_back(QUrl::fromLocalFile(QDir::cleanPath(mp + internalPath)));
    }

    md->setUrls(urls);
    return md;
}

Qt::ItemFlags RemoteFileModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);

    // Allow dragging rows out of the view (Dolphin, desktop, etc.)
    if (index.isValid() && !m_offline) {
        f |= Qt::ItemIsDragEnabled;
    }
    return f;
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

bool RemoteFileModel::rowsEqual(const Row& a, const Row& b) const {
    return a.entryId == b.entryId &&
           a.deviceId == b.deviceId &&
           a.name == b.name &&
           a.dirId == b.dirId &&
           a.size == b.size &&
           a.mtime == b.mtime &&
           a.flags == b.flags;
}

bool RemoteFileModel::pageEqual(const QVector<Row>& a, const QVector<Row>& b) const {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        if (!rowsEqual(a[i], b[i])) return false;
    }
    return true;
}

void RemoteFileModel::ensurePageLoaded(quint32 pageIndex) const {
    if (m_offline) return;
    if (!m_client) return;

    const bool cacheValidForQuery = (m_pagesSerial == m_querySerial);

    if (cacheValidForQuery && m_pages.contains(pageIndex)) return;
    if (m_pagesLoading.contains(pageIndex)) return;

    // Coalesce: record intent, let dispatcher decide when to actually fire requests.
    // Also prefetch neighbors for a more “Everything-like” feel during fast scroll/jumps.
    m_pagesWanted.insert(pageIndex);
    if (pageIndex > 0) m_pagesWanted.insert(pageIndex - 1);
    m_pagesWanted.insert(pageIndex + 1);

    m_lastWantedPage = pageIndex;
    scheduleDispatch();
}

void RemoteFileModel::scheduleDispatch() const {
    if (m_dispatchScheduled) return;

    auto* self = const_cast<RemoteFileModel*>(this);
    m_dispatchScheduled = true;

    QTimer::singleShot(0, self, [this, self]() {
        Q_UNUSED(self);
        m_dispatchScheduled = false;
        dispatchPendingLoads();
    });
}

void RemoteFileModel::dispatchPendingLoads() const {
    if (m_offline) return;
    if (!m_client) return;

    auto pickClosestWanted = [&]() -> quint32 {
        // Prefer the page nearest to the most recently requested page.
        quint32 best = *m_pagesWanted.constBegin();
        quint32 bestDist = (best > m_lastWantedPage) ? (best - m_lastWantedPage) : (m_lastWantedPage - best);

        for (auto it = m_pagesWanted.constBegin(); it != m_pagesWanted.constEnd(); ++it) {
            const quint32 p = *it;
            const quint32 d = (p > m_lastWantedPage) ? (p - m_lastWantedPage) : (m_lastWantedPage - p);
            if (d < bestDist) {
                bestDist = d;
                best = p;
                if (bestDist == 0) break;
            }
        }
        return best;
    };

    // Start up to N concurrent page loads.
    while (m_inFlightPageLoads < kMaxInFlightPageLoads && !m_pagesWanted.isEmpty()) {
        const quint32 pick = pickClosestWanted();
        m_pagesWanted.remove(pick);
        startLoadPage(pick);
    }
}

void RemoteFileModel::startLoadPage(quint32 pageIndex) const {
    if (m_offline) return;
    if (!m_client) return;

    const bool cacheValidForQuery = (m_pagesSerial == m_querySerial);
    if (cacheValidForQuery && m_pages.contains(pageIndex)) return;
    if (m_pagesLoading.contains(pageIndex)) return;

    auto* self = const_cast<RemoteFileModel*>(this);

    m_pagesLoading.insert(pageIndex);
    ++m_inFlightPageLoads;

    const quint32 offset = pageIndex * kPageSize;
    const quint32 limit  = kPageSize;

    const quint64 serial = m_querySerial;

    if (pageIndex == 0 && !m_searchStartNsBySerial.contains(serial)) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const qint64 nowNs = static_cast<qint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
        );
        m_searchStartNsBySerial.insert(serial, nowNs);
    }

    auto* watcher = new QDBusPendingCallWatcher(
        m_client->searchAsync(m_query, m_deviceIds, m_sortKey, m_sortDir, offset, limit, {}),
        self
    );

    connect(watcher, &QDBusPendingCallWatcher::finished,
            self,
            [this, self, watcher, pageIndex, serial]() {
        watcher->deleteLater();

        // Always account for concurrency on completion.
        m_pagesLoading.remove(pageIndex);
        m_inFlightPageLoads = std::max(0, m_inFlightPageLoads - 1);

        // Drop stale replies (query changed while call was in-flight)
        if (serial != m_querySerial) {
            // Kick dispatcher in case newer requests are pending.
            scheduleDispatch();
            return;
        }

        // Search reply is (t, av) => <qulonglong, QVariantList>
        QDBusPendingReply<qulonglong, QVariantList> reply = *watcher;
        if (!reply.isValid()) {
            if (!m_pagesFailed.contains(pageIndex)) {
                m_pagesFailed.insert(pageIndex);
                Q_EMIT self->transientError(QStringLiteral("Daemon error: ") + reply.error().message());
            }
            scheduleDispatch();
            return;
        }

        const quint64 newTotalHits = static_cast<quint64>(reply.argumentAt<0>());
        const QVariantList rowsList = reply.argumentAt<1>();

        QVector<Row> parsed;
        parsed.reserve(rowsList.size());

        QHash<QString, QSet<quint32>> toResolve;

        for (const auto& item : rowsList) {
            QString parseErr;
            auto rowOpt = parseRow(item, &parseErr);
            if (!rowOpt) continue;

            Row r = *rowOpt;
            parsed.push_back(r);

            if (!m_dirCache[r.deviceId].contains(r.dirId)) {
                toResolve[r.deviceId].insert(r.dirId);
            }
        }

        // If this is the first accepted reply for this serial, switch cache ownership now.
        // (We kept showing old results until now to avoid flicker.)
        if (m_pagesSerial != serial) {
            m_pages.clear();
            m_pagesSerial = serial;
        }

        // Update row count changes using insert/remove rows (less repaint than layoutChanged()).
        if (pageIndex == 0) {
            const int oldCount = self->rowCount(); // uses updated m_totalHits

            m_totalHits = newTotalHits;

            const int newCount = self->rowCount();
            if (newCount > oldCount) {
                self->beginInsertRows(QModelIndex(), oldCount, newCount - 1);
                self->endInsertRows();
            } else if (newCount < oldCount) {
                self->beginRemoveRows(QModelIndex(), newCount, oldCount - 1);
                self->endRemoveRows();
            }
        }

        // Update cached page and notify only if it actually changed.
        const bool hadPage = m_pages.contains(pageIndex);
        const QVector<Row> oldPage = hadPage ? m_pages.value(pageIndex) : QVector<Row>{};

        m_pages.insert(pageIndex, parsed);

        const QVector<Row>& newPage = m_pages.value(pageIndex);
        const bool changed = !hadPage || !pageEqual(oldPage, newPage);

        if (changed) {
            const int startRow = static_cast<int>(pageIndex * kPageSize);
            const int count = newPage.size();
            if (count > 0) {
                const QModelIndex topLeft = self->index(startRow, 0);
                const QModelIndex bottomRight = self->index(startRow + count - 1, 3);
                Q_EMIT self->dataChanged(topLeft, bottomRight, {Qt::DisplayRole});
            }
        }

        // Emit Option B status update when page 0 arrives
        if (pageIndex == 0) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const qint64 nowNs = static_cast<qint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
            );

            double elapsed = 0.0;
            const auto it = m_searchStartNsBySerial.find(serial);
            if (it != m_searchStartNsBySerial.end()) {
                elapsed = static_cast<double>(nowNs - it.value()) / 1e9;
                m_searchStartNsBySerial.erase(it);
            }

            Q_EMIT self->searchCompleted(m_totalHits, elapsed);
        }

        // Continue dispatching pending loads ASAP (this is the key to smooth coalescing)
        scheduleDispatch();

        // Async resolve directories (path column fill-in)
        if (toResolve.isEmpty()) return;

        auto pending = std::make_shared<int>(toResolve.size());

        for (auto it = toResolve.begin(); it != toResolve.end(); ++it) {
            const QString deviceId = it.key();
            const QList<quint32> dirIds = it.value().values();

            auto* w2 = new QDBusPendingCallWatcher(
                m_client->resolveDirectoriesAsync(deviceId, dirIds),
                self
            );

            QObject::connect(w2, &QDBusPendingCallWatcher::finished,
                             self,
                             [this, self, w2, deviceId, pageIndex, serial, pending]() {
                w2->deleteLater();

                if (serial != m_querySerial) {
                    (*pending)--;
                    return;
                }

                // ResolveDirectories reply is (av) => <QVariantList>
                QDBusPendingReply<QVariantList> rep2 = *w2;
                if (rep2.isValid()) {
                    const QVariantList pairs = rep2.argumentAt<0>();

                    for (const auto& p : pairs) {
                        const QVariant pv = p.canConvert<QDBusVariant>()
                            ? qvariant_cast<QDBusVariant>(p).variant()
                            : p;

                        if (pv.canConvert<QDBusArgument>()) {
                            quint32 dirId = 0;
                            QString path;
                            if (decodeDirPairFromDbusArgument(qvariant_cast<QDBusArgument>(pv), dirId, path)) {
                                m_dirCache[deviceId].insert(dirId, path);
                            }
                        } else {
                            const QVariantList pair = pv.toList();
                            if (pair.size() == 2) {
                                const quint32 dirId = pair[0].toUInt();
                                const QString path = pair[1].toString();
                                m_dirCache[deviceId].insert(dirId, path);
                            }
                        }
                    }
                }

                (*pending)--;

                // Update only the Path column for this page
                if (*pending <= 0) {
                    const int startRow = static_cast<int>(pageIndex * kPageSize);
                    const auto pit = m_pages.find(pageIndex);
                    const int count = (pit == m_pages.end()) ? 0 : pit.value().size();
                    if (count > 0) {
                        const QModelIndex topLeft = self->index(startRow, 1);
                        const QModelIndex bottomRight = self->index(startRow + count - 1, 1);
                        Q_EMIT self->dataChanged(topLeft, bottomRight, {Qt::DisplayRole});
                    }
                }
            });
        }
    });
}

// void RemoteFileModel::ensurePageLoaded(quint32 pageIndex) const {
//     if (m_offline) return;
//     if (!m_client) return;
//
//     const bool cacheValidForQuery = (m_pagesSerial == m_querySerial);
//
//     if (cacheValidForQuery && m_pages.contains(pageIndex)) return;
//     if (m_pagesLoading.contains(pageIndex)) return;
//
//     auto* self = const_cast<RemoteFileModel*>(this);
//
//     m_pagesLoading.insert(pageIndex);
//
//     const quint32 offset = pageIndex * kPageSize;
//     const quint32 limit  = kPageSize;
//
//     const quint64 serial = m_querySerial;
//
//     // Start timing only once per serial (page 0 is what we care about for status)
//     if (pageIndex == 0 && !m_searchStartNsBySerial.contains(serial)) {
//         const auto now = std::chrono::steady_clock::now().time_since_epoch();
//         const qint64 nowNs = static_cast<qint64>(
//             std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
//         );
//         m_searchStartNsBySerial.insert(serial, nowNs);
//     }
//
//     auto* watcher = new QDBusPendingCallWatcher(
//         m_client->searchAsync(m_query, m_deviceIds, m_sortKey, m_sortDir, offset, limit, {}),
//         self
//     );
//
//     QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
//                      self,
//                      [this, self, watcher, pageIndex, serial]() {
//         watcher->deleteLater();
//
//         // Drop stale replies (query changed while call was in-flight)
//         if (serial != m_querySerial) {
//             m_pagesLoading.remove(pageIndex);
//             return;
//         }
//
//         // Search reply is (t, av) => <qulonglong, QVariantList>
//         QDBusPendingReply<qulonglong, QVariantList> reply = *watcher;
//         if (!reply.isValid()) {
//             m_pagesLoading.remove(pageIndex);
//
//             if (!m_pagesFailed.contains(pageIndex)) {
//                 m_pagesFailed.insert(pageIndex);
//                 Q_EMIT self->transientError(QStringLiteral("Daemon error: ") + reply.error().message());
//             }
//             return;
//         }
//
//         const quint64 newTotalHits = static_cast<quint64>(reply.argumentAt<0>());
//         const QVariantList rowsList = reply.argumentAt<1>();
//
//         QVector<Row> parsed;
//         parsed.reserve(rowsList.size());
//
//         QHash<QString, QSet<quint32>> toResolve;
//
//         for (const auto& item : rowsList) {
//             QString parseErr;
//             auto rowOpt = parseRow(item, &parseErr);
//             if (!rowOpt) continue;
//
//             Row r = *rowOpt;
//             parsed.push_back(r);
//
//             if (!m_dirCache[r.deviceId].contains(r.dirId)) {
//                 toResolve[r.deviceId].insert(r.dirId);
//             }
//         }
//
//         // If this is the first accepted reply for this serial, switch cache ownership now.
//         // (We kept showing old results until now to avoid flicker.)
//         if (m_pagesSerial != serial) {
//             m_pages.clear();
//             m_pagesSerial = serial;
//         }
//
//         // Update row count changes using insert/remove rows (less repaint than layoutChanged()).
//         if (pageIndex == 0) {
//             const int oldCount = self->rowCount();
//             const quint64 oldTotalHits = m_totalHits;
//
//             m_totalHits = newTotalHits;
//
//             const int newCount = self->rowCount(); // uses updated m_totalHits
//
//             if (newCount > oldCount) {
//                 self->beginInsertRows(QModelIndex(), oldCount, newCount - 1);
//                 self->endInsertRows();
//             } else if (newCount < oldCount) {
//                 self->beginRemoveRows(QModelIndex(), newCount, oldCount - 1);
//                 self->endRemoveRows();
//             }
//
//             Q_UNUSED(oldTotalHits);
//         }
//
//         // Update cached page and notify only if it actually changed.
//         const bool hadPage = m_pages.contains(pageIndex);
//         const QVector<Row> oldPage = hadPage ? m_pages.value(pageIndex) : QVector<Row>{};
//
//         m_pages.insert(pageIndex, parsed);
//         m_pagesLoading.remove(pageIndex);
//
//         const QVector<Row>& newPage = m_pages.value(pageIndex);
//         const bool changed = !hadPage || !pageEqual(oldPage, newPage);
//
//         if (changed) {
//             const int startRow = static_cast<int>(pageIndex * kPageSize);
//             const int count = newPage.size();
//             if (count > 0) {
//                 const QModelIndex topLeft = self->index(startRow, 0);
//                 const QModelIndex bottomRight = self->index(startRow + count - 1, 3);
//                 Q_EMIT self->dataChanged(topLeft, bottomRight, {Qt::DisplayRole});
//             }
//         }
//
//         // Emit Option B status update when page 0 arrives
//         if (pageIndex == 0) {
//             const auto now = std::chrono::steady_clock::now().time_since_epoch();
//             const qint64 nowNs = static_cast<qint64>(
//                 std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
//             );
//
//             double elapsed = 0.0;
//             const auto it = m_searchStartNsBySerial.find(serial);
//             if (it != m_searchStartNsBySerial.end()) {
//                 elapsed = static_cast<double>(nowNs - it.value()) / 1e9;
//                 m_searchStartNsBySerial.erase(it);
//             }
//
//             Q_EMIT self->searchCompleted(m_totalHits, elapsed);
//         }
//
//         // Async resolve directories (path column fill-in)
//         if (toResolve.isEmpty()) return;
//
//         auto pending = std::make_shared<int>(toResolve.size());
//
//         for (auto it = toResolve.begin(); it != toResolve.end(); ++it) {
//             const QString deviceId = it.key();
//             const QList<quint32> dirIds = it.value().values();
//
//             auto* w2 = new QDBusPendingCallWatcher(
//                 m_client->resolveDirectoriesAsync(deviceId, dirIds),
//                 self
//             );
//
//             QObject::connect(w2, &QDBusPendingCallWatcher::finished,
//                              self,
//                              [this, self, w2, deviceId, pageIndex, serial, pending]() {
//                 w2->deleteLater();
//
//                 if (serial != m_querySerial) {
//                     (*pending)--;
//                     return;
//                 }
//
//                 // ResolveDirectories reply is (av) => <QVariantList>
//                 QDBusPendingReply<QVariantList> rep2 = *w2;
//                 if (rep2.isValid()) {
//                     const QVariantList pairs = rep2.argumentAt<0>();
//
//                     for (const auto& p : pairs) {
//                         const QVariant pv = p.canConvert<QDBusVariant>()
//                             ? qvariant_cast<QDBusVariant>(p).variant()
//                             : p;
//
//                         if (pv.canConvert<QDBusArgument>()) {
//                             quint32 dirId = 0;
//                             QString path;
//                             if (decodeDirPairFromDbusArgument(qvariant_cast<QDBusArgument>(pv), dirId, path)) {
//                                 m_dirCache[deviceId].insert(dirId, path);
//                             }
//                         } else {
//                             const QVariantList pair = pv.toList();
//                             if (pair.size() == 2) {
//                                 const quint32 dirId = pair[0].toUInt();
//                                 const QString path = pair[1].toString();
//                                 m_dirCache[deviceId].insert(dirId, path);
//                             }
//                         }
//                     }
//                 }
//
//                 (*pending)--;
//
//                 // Update only the Path column for this page
//                 if (*pending <= 0) {
//                     const int startRow = static_cast<int>(pageIndex * kPageSize);
//                     const auto pit = m_pages.find(pageIndex);
//                     const int count = (pit == m_pages.end()) ? 0 : pit.value().size();
//                     if (count > 0) {
//                         const QModelIndex topLeft = self->index(startRow, 1);
//                         const QModelIndex bottomRight = self->index(startRow + count - 1, 1);
//                         Q_EMIT self->dataChanged(topLeft, bottomRight, {Qt::DisplayRole});
//                     }
//                 }
//             });
//         }
//     });
// }

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
            if (r.mtime < 0) return QStringLiteral("invalid-time");
            // const QDateTime dt = QDateTime::fromSecsSinceEpoch(r.mtime);
            // return QLocale().toString(dt, QLocale::ShortFormat);
            return QString::fromStdString(GuiUtils::uint64ToFormattedTime(r.mtime));
        }
        default:
            return placeholder();
    }
}