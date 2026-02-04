// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_REMOTEFILEMODEL_H
#define KERYTHING_REMOTEFILEMODEL_H

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

class DbusIndexerClient;

class RemoteFileModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit RemoteFileModel(DbusIndexerClient* client, QObject* parent = nullptr);

    void setQuery(const QString& query);
    void setSort(int column, Qt::SortOrder order);

    void setOffline(bool offline);
    void invalidate(); // clear cached pages/paths and reload page 0 for current query/sort

    [[nodiscard]] quint64 totalHits() const { return m_totalHits; }

    [[nodiscard]] std::optional<quint64> entryIdAtRow(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

signals:
    void transientError(const QString& message);

    // Emitted when the async search for the current query has produced totalHits (page 0 returned).
    void searchCompleted(quint64 totalHits, double elapsedSeconds);

private:
    struct Row {
        quint64 entryId = 0;
        QString deviceId;
        QString name;
        quint32 dirId = 0;
        quint64 size = 0;
        qint64 mtime = 0; // unix seconds
        quint32 flags = 0; // bitmask
    };

    void clearAll();
    void ensurePageLoaded(quint32 pageIndex) const;

    [[nodiscard]] QString sortKeyForColumn(int column) const;
    [[nodiscard]] QString sortDirForOrder(Qt::SortOrder order) const;

    static std::optional<Row> parseRow(const QVariant& v, QString* errorOut);

    DbusIndexerClient* m_client = nullptr;

    bool m_offline = false;

    QString m_query;
    QStringList m_deviceIds; // empty = all devices
    QString m_sortKey = QStringLiteral("name");
    QString m_sortDir = QStringLiteral("asc");

    static constexpr quint32 kPageSize = 256;

    mutable QHash<quint32, QVector<Row>> m_pages; // pageIndex -> rows
    mutable QSet<quint32> m_pagesLoading;         // avoid re-entrancy
    mutable QSet<quint32> m_pagesFailed;          // avoid spamming the same error

    // Path cache: deviceId -> (dirId -> pathString)
    mutable QHash<QString, QHash<quint32, QString>> m_dirCache;

    mutable quint64 m_totalHits = 0;

    // Timing for async searches (serial -> start time in nanoseconds since steady epoch)
    mutable QHash<quint64, qint64> m_searchStartNsBySerial;

    mutable quint64 m_querySerial = 0; // increments each setQuery/setSort/invalidate; used to drop stale async replies
};

#endif //KERYTHING_REMOTEFILEMODEL_H