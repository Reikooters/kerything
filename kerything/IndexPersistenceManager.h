// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_INDEXPERSISTENCEMANAGER_H
#define KERYTHING_INDEXPERSISTENCEMANAGER_H

#include <QObject>
#include <QDateTime>
#include <QString>

#include "IndexController.h"

class IndexPersistenceManager final : public QObject {
    Q_OBJECT

public:
    struct SavedIndexInfo {
        bool available = false;
        qint64 sizeBytes = 0;
        QDateTime modifiedAt;
    };

    explicit IndexPersistenceManager(QObject* parent = nullptr);

    [[nodiscard]] bool saveCoreSnapshot(
        const IndexController::CoreSnapshot& snapshot,
        QString* errorText = nullptr
    ) const;

    [[nodiscard]] std::optional<IndexController::CoreSnapshot> loadCoreSnapshot(
        const QString& deviceId,
        QString* errorText = nullptr
    ) const;

    [[nodiscard]] bool removeSavedIndex(
        const QString& deviceId,
        QString* errorText = nullptr
    ) const;

    [[nodiscard]] bool hasSavedIndex(const QString& deviceId) const;
    [[nodiscard]] SavedIndexInfo savedIndexInfo(const QString& deviceId) const;

private:
    [[nodiscard]] QString indexDirectory() const;
    [[nodiscard]] QString pathForDeviceId(const QString& deviceId) const;
    [[nodiscard]] static QString fileSafeDeviceId(const QString& deviceId);
};

#endif // KERYTHING_INDEXPERSISTENCEMANAGER_H