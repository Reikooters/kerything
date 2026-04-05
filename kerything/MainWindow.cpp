// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "MainWindow.h"

#include <QLabel>
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
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    // Text shown to explain the current app state.
    infoLabel_ = new QLabel(this);
    infoLabel_->setWordWrap(true);

    // Displays shared app state, such as the counter.
    counterLabel_ = new QLabel(this);

    // Actions exposed in the demo UI.
    auto* openWindowButton = new QPushButton("Open another window", this);
    auto* incrementButton = new QPushButton("Increment shared counter", this);

    layout->addWidget(infoLabel_);
    layout->addWidget(counterLabel_);
    layout->addWidget(openWindowButton);
    layout->addWidget(incrementButton);
    layout->addStretch();

    setCentralWidget(central);

    // Ask the controller to open a new window.
    connect(openWindowButton, &QPushButton::clicked, this, [this]() {
        if (controller_) {
            controller_->openNewWindow();
        }
    });

    // Update shared application state and then refresh all windows so they show
    // the new value.
    connect(incrementButton, &QPushButton::clicked, this, [this]() {
        if (controller_) {
            controller_->incrementSharedCounter();
            controller_->requestRefreshAllWindows();
        }
    });

    refresh();
    resize(450, 220);
}

void MainWindow::refresh() {
    infoLabel_->setText(
        "This window belongs to the primary process.\n"
        "Launching the app again will ask this process to open another window."
    );

    counterLabel_->setText(
        QString("Shared counter value: %1").arg(controller_->sharedCounter())
    );
}