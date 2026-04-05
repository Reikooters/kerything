// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "AppController.h"

#include "MainWindow.h"
#include "SingleInstanceServer.h"

#include <QApplication>

AppController::AppController(QApplication& app, QObject* parent)
    : QObject(parent),
      app_(app) {
    // Create the IPC/single-instance manager and make this controller its parent
    // so Qt cleans it up automatically with the controller.
    instanceServer_ = new SingleInstanceServer(QStringLiteral("kerything.single_instance_server"), this);

    // When another launch asks the primary instance to open a window, route that
    // request into application logic here.
    connect(instanceServer_, &SingleInstanceServer::requestOpenWindow,
            this, &AppController::onPrimaryRequestedOpenWindow);

    // Generic command hook for future extension of additional application features.
    connect(instanceServer_, &SingleInstanceServer::requestCommand,
            this, &AppController::onPrimaryRequestedCommand);
}

bool AppController::start() {
    // If this process is not primary, try to notify the primary instance and exit.
    if (!instanceServer_->isPrimary()) {
        if (instanceServer_->notifyPrimary()) {
            return false;
        }

        // If the primary instance cannot be reached, fall back to becoming primary.
    }

    // Primary instance opens its first window on startup.
    openNewWindow();
    return true;
}

void AppController::openNewWindow() {
    // Clean up any stale pointers before adding a new one.
    cleanupWindows();

    // Create a top-level window. The controller is passed in so the window can
    // request app-level actions, but the Qt parent remains null by design.
    auto* window = new MainWindow(this);
    windows_.append(window);

    // When the window is destroyed, remove its pointer from the controller list.
    connect(window, &QObject::destroyed, this, [this, window]() {
        windows_.removeAll(window);
    });

    // Show the new window and bring it to the front.
    window->show();
    window->raise();
    window->activateWindow();
}

void AppController::requestRefreshAllWindows() {
    // Remove dead entries first so iteration is safe and clean.
    cleanupWindows();

    // Refresh every live window. QPointer becomes null automatically if a window
    // has already been destroyed.
    for (auto& window : windows_) {
        if (window) {
            window->refresh();
        }
    }
}

void AppController::incrementSharedCounter() {
    // Placeholder for shared application state that all windows can observe.
    ++sharedCounter_;
}

int AppController::sharedCounter() const noexcept {
    return sharedCounter_;
}


void AppController::onPrimaryRequestedOpenWindow() {
    // IPC request from another process: just open a new window.
    openNewWindow();
}

void AppController::onPrimaryRequestedCommand(const QString& command) {
    // Very small command router for future extensibility.
    if (command == QStringLiteral("OPEN_WINDOW")) {
        openNewWindow();
    }
}

void AppController::cleanupWindows() {
    // Remove null QPointer entries from the list.
    for (auto it = windows_.begin(); it != windows_.end(); ) {
        if (it->isNull()) {
            it = windows_.erase(it);
        } else {
            ++it;
        }
    }
}