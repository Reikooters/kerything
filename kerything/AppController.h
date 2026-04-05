// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_APPCONTROLLER_H
#define KERYTHING_APPCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QList>

class QApplication;
class MainWindow;
class SingleInstanceServer;

class AppController final : public QObject {
    Q_OBJECT

public:
    explicit AppController(QApplication& app, QObject* parent = nullptr);

    bool start();
    void openNewWindow();
    void requestRefreshAllWindows();
    void incrementSharedCounter();
    [[nodiscard]] int sharedCounter() const noexcept;

private slots:
    void onPrimaryRequestedOpenWindow();
    void onPrimaryRequestedCommand(const QString& command);

private:
    void cleanupWindows();

    QApplication& app_;
    SingleInstanceServer* instanceServer_ = nullptr;
    QList<QPointer<MainWindow>> windows_;
    int sharedCounter_ = 0;
};

#endif // KERYTHING_APPCONTROLLER_H