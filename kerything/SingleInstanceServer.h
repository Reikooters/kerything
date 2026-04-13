// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SINGLEINSTANCESERVER_H
#define KERYTHING_SINGLEINSTANCESERVER_H

#include <QObject>
#include <QLocalServer>
#include <QString>

class SingleInstanceServer final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstanceServer(QString serverName, QObject* parent = nullptr);

    bool isPrimary() const noexcept;
    bool notifyPrimary(const QString& command = QStringLiteral("OPEN_WINDOW")) const;

Q_SIGNALS:
    void requestOpenWindow();
    void requestCommand(const QString& command);

private Q_SLOTS:
    void onNewConnection();

private:
    void removeStaleServerIfNeeded();

    QString serverName_;
    QLocalServer server_;
    bool primary_ = false;
};

#endif // KERYTHING_SINGLEINSTANCESERVER_H