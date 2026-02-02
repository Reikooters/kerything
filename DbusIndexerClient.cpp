// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DbusIndexerClient.h"

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusError>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusVariant>

static QVariant unwrapDbusVariant(const QVariant& v) {
    if (v.canConvert<QDBusVariant>()) {
        return qvariant_cast<QDBusVariant>(v).variant();
    }
    return v;
}

static QVariantMap toVariantMapLoose(const QVariant& input) {
    const QVariant v = unwrapDbusVariant(input);

    if (v.metaType().id() == QMetaType::QVariantMap) {
        return v.toMap();
    }

    if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument a = qvariant_cast<QDBusArgument>(v);

        QVariantMap out;

        // D-Bus a{sv}: key=string, value=variant
        a.beginMap();
        while (!a.atEnd()) {
            QString key;
            QVariant value;
            a.beginMapEntry();
            a >> key >> value;
            a.endMapEntry();

            out.insert(key, unwrapDbusVariant(value));
        }
        a.endMap();

        return out;
    }

    return {};
}

static QVariantList toVariantListLoose(const QVariant& input) {
    QVariant v = unwrapDbusVariant(input);

    if (v.metaType().id() == QMetaType::QVariantList) {
        const QVariantList raw = v.toList();
        QVariantList out;
        out.reserve(raw.size());
        for (const auto& e : raw) out.push_back(unwrapDbusVariant(e));
        return out;
    }

    if (v.canConvert<QDBusArgument>()) {
        // NOTE: Use a non-const local copy for safe cursor operations.
        const QDBusArgument a = qvariant_cast<QDBusArgument>(v);

        QVariantList out;
        a.beginArray();
        while (!a.atEnd()) {
            QVariant elem;
            a >> elem;
            out.push_back(unwrapDbusVariant(elem));
        }
        a.endArray();
        return out;
    }

    return {};
}

DbusIndexerClient::DbusIndexerClient(QObject* parent)
    : QObject(parent)
    , m_service(QStringLiteral("net.reikooters.Kerything1"))
    , m_path(QStringLiteral("/net/reikooters/Kerything1"))
    , m_iface(QStringLiteral("net.reikooters.Kerything1.Indexer")) {}

bool DbusIndexerClient::isAvailable() const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    return iface.isValid();
}

std::optional<DbusIndexerClient::PingResult> DbusIndexerClient::ping(QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) *errorOut = iface.lastError().message();
        return std::nullopt;
    }

    QDBusMessage reply = iface.call(QStringLiteral("Ping"));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return std::nullopt;
    }

    const auto args = reply.arguments();
    if (args.size() < 2) {
        if (errorOut) *errorOut = QStringLiteral("Ping(): unexpected reply shape");
        return std::nullopt;
    }

    PingResult r;
    r.version = args[0].toString();
    r.apiVersion = args[1].toUInt();
    return r;
}

std::optional<QVariantList> DbusIndexerClient::listKnownDevices(QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) *errorOut = iface.lastError().message();
        return std::nullopt;
    }

    QDBusMessage reply = iface.call(QStringLiteral("ListKnownDevices"));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return std::nullopt;
    }

    const auto args = reply.arguments();
    if (args.size() < 1) {
        if (errorOut) *errorOut = QStringLiteral("ListKnownDevices(): unexpected reply shape");
        return std::nullopt;
    }

    const QVariantList rawList = toVariantListLoose(args[0]);

    // Normalize: ensure each element is a plain QVariantMap for consumers.
    QVariantList normalized;
    normalized.reserve(rawList.size());
    for (const QVariant& elem : rawList) {
        normalized.push_back(toVariantMapLoose(elem));
    }

    return normalized;
}

std::optional<QVariantList> DbusIndexerClient::listIndexedDevices(QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) *errorOut = iface.lastError().message();
        return std::nullopt;
    }

    QDBusMessage reply = iface.call(QStringLiteral("ListIndexedDevices"));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return std::nullopt;
    }

    const auto args = reply.arguments();
    if (args.size() < 1) {
        if (errorOut) *errorOut = QStringLiteral("ListIndexedDevices(): unexpected reply shape");
        return std::nullopt;
    }

    const QVariantList rawList = toVariantListLoose(args[0]);

    QVariantList normalized;
    normalized.reserve(rawList.size());
    for (const QVariant& elem : rawList) {
        normalized.push_back(toVariantMapLoose(elem));
    }

    return normalized;
}

std::optional<quint64> DbusIndexerClient::startIndex(const QString& deviceId, QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) *errorOut = iface.lastError().message();
        return std::nullopt;
    }

    QDBusMessage reply = iface.call(QStringLiteral("StartIndex"), deviceId);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return std::nullopt;
    }

    const auto args = reply.arguments();
    if (args.size() < 1) {
        if (errorOut) *errorOut = QStringLiteral("StartIndex(): unexpected reply shape");
        return std::nullopt;
    }

    return static_cast<quint64>(args[0].toULongLong());
}

bool DbusIndexerClient::cancelJob(quint64 jobId, QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) {
            const QDBusError e = iface.lastError();
            *errorOut = e.message().isEmpty()
                ? QStringLiteral("D-Bus interface not valid (service unavailable).")
                : e.name() + QStringLiteral(": ") + e.message();
        }
        return false;
    }

    QDBusMessage reply = iface.call(QStringLiteral("CancelJob"), jobId);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) {
            const QString name = reply.errorName();
            const QString msg  = reply.errorMessage();
            *errorOut = msg.isEmpty()
                ? (name.isEmpty() ? QStringLiteral("Unknown D-Bus error.") : name)
                : (name.isEmpty() ? msg : name + QStringLiteral(": ") + msg);
        }
        return false;
    }

    return true;
}

std::optional<DbusIndexerClient::SearchResult> DbusIndexerClient::search(const QString& query,
                                                                         const QStringList& deviceIds,
                                                                         const QString& sortKey,
                                                                         const QString& sortDir,
                                                                         quint32 offset,
                                                                         quint32 limit,
                                                                         const QVariantMap& options,
                                                                         QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) *errorOut = iface.lastError().message();
        return std::nullopt;
    }

    QDBusMessage reply = iface.call(QStringLiteral("Search"),
                                   query,
                                   deviceIds,
                                   sortKey,
                                   sortDir,
                                   offset,
                                   limit,
                                   options);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return std::nullopt;
    }

    const auto args = reply.arguments();
    if (args.size() < 2) {
        if (errorOut) *errorOut = QStringLiteral("Search(): unexpected reply shape");
        return std::nullopt;
    }

    SearchResult r;
    r.totalHits = args[0].toULongLong();
    r.rows = toVariantListLoose(args[1]);
    return r;
}

std::optional<QVariantList> DbusIndexerClient::resolveDirectories(const QString& deviceId,
                                                                  const QList<quint32>& dirIds,
                                                                  QString* errorOut) const {
    QDBusInterface iface(m_service, m_path, m_iface, QDBusConnection::systemBus());
    if (!iface.isValid()) {
        if (errorOut) *errorOut = iface.lastError().message();
        return std::nullopt;
    }

    // // QtDBus will marshal QList<quint32> to D-Bus "au"
    // QDBusMessage reply = iface.call(QStringLiteral("ResolveDirectories"),
    //                                deviceId,
    //                                QVariant::fromValue(dirIds));

    // Marshal as a QVariantList of basic integers (prototype-friendly).
    QVariantList ids;
    ids.reserve(dirIds.size());
    for (quint32 id : dirIds) {
        ids << QVariant::fromValue(id);
    }

    QDBusMessage reply = iface.call(QStringLiteral("ResolveDirectories"),
                                   deviceId,
                                   ids);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return std::nullopt;
    }

    const auto args = reply.arguments();
    if (args.size() < 1) {
        if (errorOut) *errorOut = QStringLiteral("ResolveDirectories(): unexpected reply shape");
        return std::nullopt;
    }

    return toVariantListLoose(args[0]);
}