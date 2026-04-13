// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_APPCONTROLLER_H
#define KERYTHING_APPCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QList>

#include "DaemonClient.h"
#include "IndexController.h"

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
    [[nodiscard]] bool isDaemonConnected() const noexcept;
    [[nodiscard]] bool isDaemonReady() const noexcept;
    IndexController* indexController() const noexcept;

private Q_SLOTS:
    void onPrimaryRequestedOpenWindow();
    void onPrimaryRequestedCommand(const QString& command);

private:
    void cleanupWindows();

    QApplication& app_;
    SingleInstanceServer* instanceServer_ = nullptr;
    DaemonClient* daemonClient_ = nullptr;
    IndexController* indexController_ = nullptr;
    QList<QPointer<MainWindow>> windows_;
};

#endif // KERYTHING_APPCONTROLLER_H