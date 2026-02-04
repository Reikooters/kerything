// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DBUSINDEXERCLIENT_H
#define KERYTHING_DBUSINDEXERCLIENT_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

class DbusIndexerClient final : public QObject {
    Q_OBJECT

public:
    struct PingResult {
        QString version;
        quint32 apiVersion = 0;
    };

    struct SearchResult {
        quint64 totalHits = 0;
        QVariantList rows; // each row is QVariantList of 7 fields
    };

    explicit DbusIndexerClient(QObject* parent = nullptr);

    [[nodiscard]] bool isAvailable() const;

    std::optional<PingResult> ping(QString* errorOut = nullptr) const;

    std::optional<QVariantList> listKnownDevices(QString* errorOut = nullptr) const;

    // list devices currently indexed in daemon memory
    std::optional<QVariantList> listIndexedDevices(QString* errorOut = nullptr) const;

    std::optional<quint64> startIndex(const QString& deviceId, QString* errorOut = nullptr) const;
    bool cancelJob(quint64 jobId, QString* errorOut = nullptr) const;

    std::optional<SearchResult> search(const QString& query,
                                       const QStringList& deviceIds,
                                       const QString& sortKey,
                                       const QString& sortDir,
                                       quint32 offset,
                                       quint32 limit,
                                       const QVariantMap& options = {},
                                       QString* errorOut = nullptr) const;

    std::optional<QVariantList> resolveDirectories(const QString& deviceId,
                                                   const QList<quint32>& dirIds,
                                                   QString* errorOut = nullptr) const;

    std::optional<QVariantList> resolveEntries(const QList<quint64>& entryIds,
                                               QString* errorOut = nullptr) const;

    bool forgetIndex(const QString& deviceId, QString* errorOut = nullptr) const;

private:
    QString m_service;
    QString m_path;
    QString m_iface;
};

#endif //KERYTHING_DBUSINDEXERCLIENT_H