// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_MAINWINDOW_H
#define KERYTHING_MAINWINDOW_H

#include <QMainWindow>

class QLabel;
class QPushButton;
class AppController;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(AppController* controller, QWidget* parent = nullptr);

    void refresh();

private:
    AppController* controller_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QLabel* counterLabel_ = nullptr;
};

#endif // KERYTHING_MAINWINDOW_H