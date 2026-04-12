// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "AppController.h"

#include <iostream>
#include <QApplication>

#include "DaemonClient.h"
#include "MainWindow.h"
#include "SingleInstanceServer.h"
#include "FileRecord.h"

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

    // Create index controller
    indexController_ = new IndexController(this);

    // Start the daemon client
    daemonClient_ = new DaemonClient(indexController_, this);

    connect(daemonClient_, &DaemonClient::daemonAvailable,
            this, [this]() {
                // Update UI: daemon is connected, waiting for "ready" signal
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::daemonReady,
            this, [this]() {
                // Update UI: daemon is ready
                requestRefreshAllWindows();

                // TODO: demo only - send scan request on ready
                const QByteArray payload = Protocol::makeScanDevicePayload(QStringLiteral("/dev/disk/by-partuuid/89a0622c-75e2-427c-9766-b2fd2a2e69d8"), QStringLiteral("ntfs"));

                quint32 requestId;
                daemonClient_->sendRequest(Protocol::MessageType::ScanDevice, payload, &requestId);

                std::cout << "GUI: Demo ScanDevice request sent requestId=" << requestId << "\n";
            });

    connect(daemonClient_, &DaemonClient::daemonUnavailable,
            this, [this]() {
                // Update UI: backend not available
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::connectedChanged,
            this, [this](bool connected) {
                // Use this for a status bar / indicator
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanStarted,
            this, [this](quint32 requestId, const QString& devicePath, const QString& fsType) {
                std::cout << "GUI: scan started requestId=" << requestId
                          << " devicePath=" << devicePath.toStdString()
                          << " fsType=" << fsType.toStdString() << "\n";
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanProgress,
            this, [this](quint32 requestId, quint64 seen, quint64 emitted) {
                std::cout << "GUI: scan progress requestId=" << requestId
                          << " seen=" << seen
                          << " emitted=" << emitted << "\n";
            });

    connect(daemonClient_, &DaemonClient::scanChunkReceived,
            this, [this](quint32 requestId, const std::vector<FileRecord>& chunk) {
                std::cout << "GUI: received scan chunk requestId=" << requestId
                          << " size=" << chunk.size() << "\n";

                // TODO: Apply chunk to the in-memory index here.
                // index.addChunk(chunk);

                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanCompleted,
            this, [this](quint32 requestId) {
                std::cout << "GUI: scan completed requestId=" << requestId << "\n";
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanCancelled,
            this, [this](quint32 requestId) {
                std::cout << "GUI: scan cancelled requestId=" << requestId << "\n";
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanFailed,
            this, [this](quint32 requestId, const QString& errorText) {
                std::cerr << "GUI: scan failed requestId=" << requestId
                          << " " << errorText.toStdString() << "\n";
            });

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

bool AppController::isDaemonConnected() const noexcept {
    if (!daemonClient_) {
        return false;
    }

    return daemonClient_->isConnected();
}

bool AppController::isDaemonReady() const noexcept {
    if (!daemonClient_) {
        return false;
    }

    return daemonClient_->isReady();
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