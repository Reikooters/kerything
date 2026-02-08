// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_KERYTHINGD_WATCHMANAGER_H
#define KERYTHING_KERYTHINGD_WATCHMANAGER_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <unordered_map>

class QSocketNotifier;
class IndexerService;

class WatchManager final : public QObject {
    Q_OBJECT
public:
    struct Status {
        QString state; // "watching" | "notMounted" | "error"
        QString error; // empty if OK
    };

    explicit WatchManager(IndexerService* svc, QObject* parent = nullptr);
    ~WatchManager() override;

    void refreshWatchesForUid(quint32 uid);
    [[nodiscard]] Status statusFor(quint32 uid, const QString& deviceId) const;

private:
    struct Key {
        quint32 uid = 0;
        QString deviceId;
        bool operator==(const Key& o) const noexcept { return uid == o.uid && deviceId == o.deviceId; }
    };

    struct KeyHash {
        size_t operator()(const Key& k) const noexcept;
    };

    struct Entry {
        QString mountPoint;
        int fanFd = -1;
        QSocketNotifier* notifier = nullptr;

        Status status{QStringLiteral("error"), QStringLiteral("Not initialized")};

        // Coalesce events into one rescan attempt
        QTimer* quietTimer = nullptr;
        bool dirty = false;
    };

    void stopEntry(Entry& e);
    void ensureEntryWatching(const Key& k, Entry& e, const QString& mountPoint);
    void onFanotifyReadable(const Key& k);

    IndexerService* m_svc = nullptr;

    static constexpr int kQuietMs = 2000;
    std::unordered_map<Key, Entry, KeyHash> m_entries;
};

#endif //KERYTHING_KERYTHINGD_WATCHMANAGER_H