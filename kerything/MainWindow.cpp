// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "MainWindow.h"

#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include "AppController.h"

MainWindow::MainWindow(AppController* controller, QWidget* parent)
    : QMainWindow(parent),
      controller_(controller) {
    setWindowTitle("Kerything");

    // Delete the QWidget object when the user closes the window.
    setAttribute(Qt::WA_DeleteOnClose);

    // Central widget that holds the window's main UI.
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);

    searchLine = new QLineEdit(centralWidget);
    searchLine->setPlaceholderText("Search files...");
    searchLine->setClearButtonEnabled(true);

    // Add magnifying glass icon to the search bar
    searchLine->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);

    // --- Burger Menu Setup ---
    auto *menu = new QMenu(this);

    auto *changePartitionAct = new QAction(QIcon::fromTheme("drive-harddisk"), "Change Partition", this);
    //connect(changePartitionAct, &QAction::triggered, this, &MainWindow::changePartition);
    changePartitionAct->setShortcut(QKeySequence::Open);
    menu->addAction(changePartitionAct);
    addAction(changePartitionAct); // Register with window for shortcuts

    auto *rescanPartitionAct = new QAction(QIcon::fromTheme("view-refresh"), "Rescan Partition", this);
    //connect(rescanPartitionAct, &QAction::triggered, this, &MainWindow::rescanPartition);
    rescanPartitionAct->setShortcut(QKeySequence::Refresh);
    menu->addAction(rescanPartitionAct);
    addAction(rescanPartitionAct); // Register with window for shortcuts

    menu->addSeparator();

    auto *aboutAct = new QAction(QIcon::fromTheme("kerything"), "About Kerything", this);
    //connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
    menu->addAction(aboutAct);

    auto *quitAct = new QAction(QIcon::fromTheme("application-exit"), "Quit", this);
    quitAct->setShortcut(QKeySequence::Quit);
    //connect(quitAct, &QAction::triggered, qApp, &QCoreApplication::quit);
    menu->addAction(quitAct);
    addAction(quitAct); // Register with window for shortcuts

    // Add the menu to a button inside the search line
    auto *menuAction = searchLine->addAction(QIcon::fromTheme("application-menu"), QLineEdit::TrailingPosition);
    connect(menuAction, &QAction::triggered, [this, menu, menuAction]() {
        // Show the menu just below the icon
        menu->exec(QCursor::pos());
    });
    // ---------------------

    layout->addWidget(searchLine);

    setCentralWidget(centralWidget);
    //refresh();
    resize(1200, 800);
}

void MainWindow::refresh() {
    // infoLabel_->setText(
    //     "This window belongs to the primary process.\n"
    //     "Launching the app again will ask this process to open another window."
    // );
    //
    // counterLabel_->setText(
    //     QString("Shared counter value: %1\nDaemon connection status: %2\nDaemon ready status: %3")
    //         .arg(controller_->sharedCounter())
    //         .arg(controller_->isDaemonConnected() ? "Connected" : "Disconnected")
    //         .arg(controller_->isDaemonReady() ? "Ready" : "Not Ready")
    //     );
}