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

/**
 * Starts the main application controller.
 *
 * This function initializes and manages the main components of the application, handling interactions
 * with the primary instance, creating services, and establishing connections for UI and backend updates.
 *
 * @return True if the application starts successfully, otherwise false if it's intended to exit due to
 *         being a secondary instance or other issues.
 *
 * Behavior:
 * - If the current process is not the primary instance, it tries to notify the primary instance. If
 *   successful, the function exits with false. If the notification fails, the process attempts to
 *   become the primary instance.
 * - Initializes the `IndexController` for managing device indexing and file-related operations.
 * - Establishes connections with `IndexController` to handle device removal and refresh the UI.
 * - Initializes the `DaemonClient` for handling backend operations and connects signals for:
 *     - Backend availability/unavailability updates.
 *     - Backend readiness and known device requests.
 *     - File scan initiation, progress, and completion events.
 * - Handles scan lifecycle events:
 *     - Tracks scan progress and updates the UI with status messages.
 *     - Logs file record and string pool data chunks received from the backend.
 *     - On scan completion, performs post-scan operations such as building indexes, updating preferences,
 *       and marking devices as indexed.
 *
 * Notes:
 * - UI refresh methods (`requestRefreshAllWindows`, `requestWindowStatusMessage`) are invoked to update
 *   visual components.
 * - Multiple connections and lambda functions are used to organize responses to backend signals for
 *   processing and indexing devices, as well as resolving scanning operations.
 * - Temporary demo code and sample request payloads are commented out but illustrate intended use cases
 *   for scanning devices.
 */
bool AppController::start() {
    // If this process is not primary, try to notify the primary instance and exit.
    if (!instanceServer_->isPrimary()) {
        if (instanceServer_->notifyPrimary()) {
            return false;
        }

        // If the primary instance cannot be reached, fall back to becoming primary.
    }

    // Create index controller
    indexController_ = new IndexController(this);

    connect(indexController_, &IndexController::deviceRemoved,
        this, [this](quint64 indexId) {
            Q_UNUSED(indexId);
            requestRefreshAllWindows();
        });

    // Start the daemon client
    daemonClient_ = new DaemonClient(this, this);

    connect(daemonClient_, &DaemonClient::daemonAvailable,
            this, [this]() {
                // Update UI: daemon is connected, waiting for "ready" signal
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::daemonReady,
            this, [this]() {
                // Update UI: daemon is ready
                requestRefreshAllWindows();
                requestKnownDevices();
            });

    connect(daemonClient_, &DaemonClient::daemonUnavailable,
            this, [this]() {
                // Any in-flight daemon requests are no longer valid after disconnect.
                for (const quint32 requestId : scanRequestDeviceIds_.keys()) {
                    indexController_->removeDeviceByRequestId(requestId);
                }

                scanRequestDeviceIds_.clear();
                activeScanDeviceIds_.clear();

                // Update UI: backend not available
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::connectedChanged,
            this, [this](bool connected) {
                // Use this for a status bar / indicator
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanStarted,
            this, [this](quint32 requestId, const QString& deviceId, const QString& devNode, const QString& fsType) {
                const bool deviceIdMatches = validateScanDeviceId(requestId, deviceId, "scanStarted");

                if (!deviceIdMatches) {
                    std::cerr << "GUI: continuing scanStarted handling despite deviceId mismatch requestId="
                              << requestId
                              << "\n";
                }

                std::cout << "GUI: scan started requestId=" << requestId
                          << " deviceId=" << deviceId.toStdString()
                          << " devNode=" << devNode.toStdString()
                          << " fsType=" << fsType.toStdString() << "\n";

                indexController_->addDevice(deviceId, devNode, fsType, "TODO", requestId);

                // requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanProgress,
            this, [this](quint32 requestId, quint64 processed, quint64 total) {
                std::cout << "GUI: scan progress requestId=" << requestId
                          << " processed=" << processed
                          << " total=" << total << "\n";

                // Calculate percentage progress with rounding
                const quint64 pct64 = total > 0
                    ? (processed * 100 + total / 2) / total
                    : 0;

                const quint8 pct = static_cast<quint8>(pct64);

                requestWindowStatusMessage(
                    QStringLiteral("RequestId: %1, Processed: %2/%3 (%4%)")
                        .arg(requestId)
                        .arg(processed)
                        .arg(total)
                        .arg(pct)
                , processed < total ? 0 : 3000);
            });

    connect(daemonClient_, &DaemonClient::scanFileRecordChunkReceived,
            this, [this](quint32 requestId, const std::vector<FileRecord>& chunk) {
                std::cout << "GUI: received scan file record chunk requestId=" << requestId
                          << " size=" << chunk.size() << "\n";

                indexController_->appendDeviceFileRecordsByRequestId(requestId, chunk);

                // requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanStringPoolChunkReceived,
            this, [this](quint32 requestId, QByteArrayView chunk) {
                std::cout << "GUI: received scan file record chunk requestId=" << requestId
                          << " size=" << chunk.size() << "\n";

                indexController_->appendDeviceStringPoolByRequestId(requestId, chunk);

                // requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanCompleted,
            this, [this](quint32 requestId, const QString& deviceId, const QString& devNode, const QString& fsType) {
                const bool deviceIdMatches = validateScanDeviceId(requestId, deviceId, "scanCompleted");

                std::cout << "GUI: scan completed successfully requestId=" << requestId
                          << " deviceId=" << deviceId.toStdString()
                          << " devNode=" << devNode.toStdString()
                          << " fsType=" << fsType.toStdString()
                          << "\n";

                // Build lowercase string pool
                indexController_->buildLowercaseStringPoolByRequestId(requestId);

                // Sort by name ascending
                indexController_->sortByNameAscendingParallelByRequestId(requestId);

                // Resolve parent pointers
                indexController_->resolveParentPointersByRequestId(requestId);

                // Build trigram index
                indexController_->buildTrigramIndexParallelByRequestId(requestId);

                // Mark ready
                indexController_->setReadyState(requestId, true);

                // Mark device indexed in preferences so we persist the lastIndexedAt
                if (deviceIdMatches) {
                    preferences_.markDeviceIndexed(deviceId);
                }

                takeTrackedScanDeviceId(requestId, deviceId);

                // Clean up the requestId as the scan has completed successfully
                indexController_->removeRequestId(requestId);

                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanCancelled,
            this, [this](quint32 requestId, const QString& deviceId) {
                const bool deviceIdMatches = validateScanDeviceId(requestId, deviceId, "scanCancelled");

                if (!deviceIdMatches) {
                    std::cerr << "GUI: continuing scanCancelled cleanup despite deviceId mismatch requestId="
                              << requestId
                              << "\n";
                }

                takeTrackedScanDeviceId(requestId, deviceId);

                // Clean up the requestId and any device associated with it,
                // as the scan was cancelled.
                indexController_->removeDeviceByRequestId(requestId);

                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanFailed,
            this, [this](quint32 requestId, const QString& errorText) {
                const QString deviceId = takeTrackedScanDeviceId(requestId);

                std::cerr << "GUI: scan failed requestId=" << requestId
                          << " deviceId=" << deviceId.toStdString()
                          << " " << errorText.toStdString()
                          << "\n";

                // Clean up the requestId and any device associated with it,
                // as the scan failed.
                indexController_->removeDeviceByRequestId(requestId);

                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::knownDevices,
        this, [this](quint32 requestId, const std::vector<BlockDevice>& blockDevices) {
            std::cout << "GUI: received known devices requestId=" << requestId
                          << " size=" << blockDevices.size() << "\n";

            preferences_.updateKnownDevices(blockDevices);

            // Temporary first-run behavior until the device picker exists:
            // enable mounted devices once, then persist that choice.
            if (!preferences_.initialDeviceSelectionCompleted()) {
                for (const BlockDevice& blockDevice : blockDevices) {
                    if (blockDevice.mounted) {
                        preferences_.setDeviceEnabled(blockDevice, true);
                    }
                }

                preferences_.setInitialDeviceSelectionCompleted(true);
            }

            for (const auto& blockDevice : blockDevices) {
                std::cout << "GUI: received known device devNode=" << blockDevice.devNode.toStdString()
                          << " fsType=" << blockDevice.fsType.toStdString()
                          << " label=" << blockDevice.label.toStdString()
                          << " uuid=" << blockDevice.uuid.toStdString()
                          << " partuuid=" << blockDevice.partuuid.toStdString()
                          << " deviceId=" << blockDevice.deviceId.toStdString()
                          << " mounted=" << (blockDevice.mounted ? "true" : "false")
                          << " primaryMountPoint=" << blockDevice.primaryMountPoint.toStdString()
                          << " enabled=" << (preferences_.isDeviceEnabled(blockDevice.deviceId) ? "true" : "false")
                          << "\n";
            }

            scanEnabledKnownDevices(blockDevices);
            requestRefreshAllWindows();
        });

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

void AppController::requestWindowStatusMessage(const QString& message, const int timeoutMs) {
    // Remove dead entries first so iteration is safe and clean.
    cleanupWindows();

    // Send message to every live window. QPointer becomes null automatically if a window
    // has already been destroyed.
    for (auto& window : windows_) {
        if (window) {
            window->showTemporaryStatus(message, timeoutMs);
        }
    }
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

IndexController* AppController::indexController() const noexcept {
    return indexController_;
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

void AppController::requestKnownDevices() {
    if (!daemonClient_ || !daemonClient_->isReady()) {
        return;
    }

    quint32 requestId = 0;
    if (!daemonClient_->sendRequest(Protocol::MessageType::ListKnownDevices, QByteArray{}, &requestId)) {
        std::cerr << "GUI: failed to send ListKnownDevices request\n";
        return;
    }

    std::cout << "GUI: ListKnownDevices request sent requestId=" << requestId << "\n";
}

void AppController::scanEnabledKnownDevices(const std::vector<BlockDevice>& blockDevices) {
    if (!daemonClient_ || !daemonClient_->isReady()) {
        return;
    }

    for (const BlockDevice& blockDevice : blockDevices) {
        if (!preferences_.isDeviceEnabled(blockDevice.deviceId)) {
            continue;
        }

        if (activeScanDeviceIds_.contains(blockDevice.deviceId)) {
            std::cout << "GUI: skipping scan because device is already queued/scanning deviceId="
                      << blockDevice.deviceId.toStdString()
                      << "\n";
            continue;
        }

        const auto preference = preferences_.indexedDevicePreference(blockDevice.deviceId);
        if (!blockDevice.mounted && (!preference || !preference->scanWhenUnmounted)) {
            std::cout << "GUI: skipping enabled device because it is unmounted and scanWhenUnmounted=false deviceId="
                      << blockDevice.deviceId.toStdString()
                      << "\n";
            continue;
        }

        const QByteArray payload = Protocol::makeScanDevicePayload(blockDevice.deviceId);

        quint32 requestId = 0;
        if (!daemonClient_->sendRequest(Protocol::MessageType::ScanDevice, payload, &requestId)) {
            std::cerr << "GUI: failed to send ScanDevice request for deviceId="
                      << blockDevice.deviceId.toStdString()
                      << "\n";
            continue;
        }

        scanRequestDeviceIds_.insert(requestId, blockDevice.deviceId);
        activeScanDeviceIds_.insert(blockDevice.deviceId);

        std::cout << "GUI: ScanDevice request sent requestId=" << requestId
                  << " deviceId=" << blockDevice.deviceId.toStdString()
                  << " fsType=" << blockDevice.fsType.toStdString()
                  << "\n";
    }
}

bool AppController::validateScanDeviceId(quint32 requestId, const QString& actualDeviceId, const char* eventName) const {
    const QString expectedDeviceId = scanRequestDeviceIds_.value(requestId);

    if (expectedDeviceId.isEmpty()) {
        std::cerr << "GUI: " << eventName
                  << " for unknown scan requestId=" << requestId
                  << " actualDeviceId=" << actualDeviceId.toStdString()
                  << "\n";
        return false;
    }

    if (expectedDeviceId != actualDeviceId) {
        std::cerr << "GUI: " << eventName
                  << " deviceId mismatch for requestId=" << requestId
                  << " expectedDeviceId=" << expectedDeviceId.toStdString()
                  << " actualDeviceId=" << actualDeviceId.toStdString()
                  << "\n";
        return false;
    }

    return true;
}

QString AppController::takeTrackedScanDeviceId(quint32 requestId, const QString& fallbackDeviceId) {
    QString deviceId = scanRequestDeviceIds_.take(requestId);

    if (deviceId.isEmpty()) {
        deviceId = fallbackDeviceId;
    }

    if (!deviceId.isEmpty()) {
        activeScanDeviceIds_.remove(deviceId);
    }

    return deviceId;
}