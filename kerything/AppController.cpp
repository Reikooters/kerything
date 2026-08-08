// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "AppController.h"

#include <algorithm>
#include <iostream>
#include <QApplication>
#include <QLocale>

#include "DaemonClient.h"
#include "DevicePickerDialog.h"
#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "SingleInstanceServer.h"
#include "FileRecord.h"
#include "LiveUpdateEvent.h"

namespace {
    struct LiveUpdateBatchSummary {
        qsizetype created = 0;
        qsizetype deleted = 0;
        qsizetype movedFrom = 0;
        qsizetype movedTo = 0;
        qsizetype metadataChanged = 0;
        qsizetype selfDeletedOrMoved = 0;
        qsizetype unknown = 0;
    };

    LiveUpdateBatchSummary summarizeLiveUpdateBatch(const std::vector<LiveUpdateEvent>& events)
    {
        LiveUpdateBatchSummary summary;

        for (const LiveUpdateEvent& event : events) {
            const NormalizedLiveUpdateEvent normalized = normalizeLiveUpdateEvent(event);

            switch (normalized.kind) {
                case NormalizedLiveUpdateKind::Created:
                    ++summary.created;
                    break;
                case NormalizedLiveUpdateKind::Deleted:
                    ++summary.deleted;
                    break;
                case NormalizedLiveUpdateKind::MovedFrom:
                    ++summary.movedFrom;
                    break;
                case NormalizedLiveUpdateKind::MovedTo:
                    ++summary.movedTo;
                    break;
                case NormalizedLiveUpdateKind::MetadataChanged:
                    ++summary.metadataChanged;
                    break;
                case NormalizedLiveUpdateKind::SelfDeletedOrMoved:
                    ++summary.selfDeletedOrMoved;
                    break;
                case NormalizedLiveUpdateKind::Unknown:
                default:
                    ++summary.unknown;
                    break;
            }
        }

        return summary;
    }
}

AppController::AppController(QApplication& app, QObject* parent)
    : QObject(parent),
      app_(app) {
    // Create the IPC/single-instance manager and make this controller its parent
    // so Qt cleans it up automatically with the controller.
    instanceServer_ = new SingleInstanceServer(QStringLiteral("kerything.single_instance_server"), this);

    // Debounce/rate-limit live update refreshes to avoid excessive full
    // search/sort refreshes when a busy filesystem such as "/" is watched.
    //
    // This is intentionally a leading-edge throttle rather than a pure
    // trailing-edge debounce: on a continuously busy OS drive, a pure debounce
    // could postpone visible live updates forever.
    liveUpdateRefreshTimer_.setSingleShot(true);
    liveUpdateRefreshTimer_.setInterval(1000);
    connect(&liveUpdateRefreshTimer_, &QTimer::timeout,
            this, &AppController::requestRefreshAllWindows);

    // Metadata-only live updates do not affect filename search membership.
    // They only need existing visible rows to refresh their displayed metadata
    // unless a window is currently sorted by size/date.
    liveMetadataRefreshTimer_.setSingleShot(true);
    liveMetadataRefreshTimer_.setInterval(1000);
    connect(&liveMetadataRefreshTimer_, &QTimer::timeout,
            this, &AppController::requestLiveMetadataRefreshAllWindows);

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
                syncLiveUpdateDevices();

                // Update UI: daemon is ready
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::daemonUnavailable,
            this, [this]() {
                // Any in-flight daemon requests are no longer valid after disconnect.
                for (const quint32 requestId : scanRequestDeviceIds_.keys()) {
                    indexController_->removeDeviceByRequestId(requestId);
                }

                scanRequestDeviceIds_.clear();
                activeScanDeviceIds_.clear();
                manualRefreshKnownDeviceRequestIds_.clear();

                // Update UI: backend not available
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::connectedChanged,
            this, [this](bool connected) {
                // Use this for a status bar / indicator
                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanStarted,
            this, [this](
                quint32 requestId,
                const QString& deviceId,
                const QString& devNode,
                const QString& fsType,
                const QString& label,
                const QStringList& mountPoints,
                const QString& primaryMountPoint
            ) {
                const bool deviceIdMatches = validateScanDeviceId(requestId, deviceId, "scanStarted");

#ifdef KERYTHING_ENABLE_LOGGING
                if (!deviceIdMatches) {
                    std::cerr << "GUI: continuing scanStarted handling despite deviceId mismatch requestId="
                              << requestId
                              << "\n";
                }

                std::cout << "GUI: scan started requestId=" << requestId
                          << " deviceId=" << deviceId.toStdString()
                          << " devNode=" << devNode.toStdString()
                          << " fsType=" << fsType.toStdString()
                          << " label=" << label.toStdString()
                          << " primaryMountPoint=" << primaryMountPoint.toStdString()
                          << "\n";
#endif

                indexController_->addDevice(
                    deviceId,
                    devNode,
                    fsType,
                    label,
                    mountPoints,
                    primaryMountPoint,
                    requestId
                );

                const auto preference = preferences_.indexedDevicePreference(deviceId);
                indexController_->updateDeviceRuntimeStateByDeviceId(
                    deviceId,
                    !mountPoints.isEmpty(),
                    !preference || preference->showOfflineResults,
                    mountPoints,
                    primaryMountPoint
                );

                requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanProgress,
            this, [this](quint32 requestId, quint64 processed, quint64 total) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: scan progress requestId=" << requestId
                          << " processed=" << processed
                          << " total=" << total << "\n";
#endif

                const QString deviceId = scanRequestDeviceIds_.value(requestId);
                QString deviceText = QStringLiteral("device");

                if (const std::optional<BlockDevice> blockDevice = knownDeviceById(deviceId)) {
                    const QString primaryMountPoint = blockDevice->primaryMountPoint.trimmed();
                    const QString label = blockDevice->label.trimmed();
                    const QString devNode = blockDevice->devNode.trimmed();

                    if (blockDevice->mounted && !primaryMountPoint.isEmpty()) {
                        deviceText = primaryMountPoint;
                    }
                    else if (!label.isEmpty()) {
                        deviceText = label;
                    }
                    else if (!devNode.isEmpty()) {
                        deviceText = devNode;
                    }
                    else if (!deviceId.isEmpty()) {
                        deviceText = deviceId;
                    }
                }
                else if (!deviceId.isEmpty()) {
                    deviceText = deviceId;
                }

                const QLocale locale;

                QString message;
                int timeoutMs = 0;

                if (total > 0) {
                    const quint64 clampedProcessed = std::min(processed, total);

                    // Calculate percentage progress with rounding.
                    const quint64 pct64 = (clampedProcessed * 100 + total / 2) / total;
                    const quint8 pct = static_cast<quint8>(std::min<quint64>(pct64, 100));

                    message = QStringLiteral("Indexing %1: %2 of %3 files scanned (%4%)")
                        .arg(deviceText)
                        .arg(locale.toString(static_cast<qulonglong>(clampedProcessed)))
                        .arg(locale.toString(static_cast<qulonglong>(total)))
                        .arg(pct);

                    timeoutMs = clampedProcessed < total ? 0 : 3000;
                }
                else {
                    message = QStringLiteral("Indexing %1: %2 files scanned")
                        .arg(deviceText)
                        .arg(locale.toString(static_cast<qulonglong>(processed)));
                }

                requestWindowStatusMessage(message, timeoutMs);
            });

    connect(daemonClient_, &DaemonClient::scanFileRecordChunkReceived,
            this, [this](quint32 requestId, const std::vector<FileRecord>& chunk) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: received scan file record chunk requestId=" << requestId
                          << " size=" << chunk.size() << "\n";
#endif

                indexController_->appendDeviceFileRecordsByRequestId(requestId, chunk);

                // requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanStringPoolChunkReceived,
            this, [this](quint32 requestId, QByteArrayView chunk) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: received scan file record chunk requestId=" << requestId
                          << " size=" << chunk.size() << "\n";
#endif

                indexController_->appendDeviceStringPoolByRequestId(requestId, chunk);

                // requestRefreshAllWindows();
            });

    connect(daemonClient_, &DaemonClient::scanCompleted,
            this, [this](quint32 requestId, const QString& deviceId, const QString& devNode, const QString& fsType) {
                const bool deviceIdMatches = validateScanDeviceId(requestId, deviceId, "scanCompleted");

#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: scan completed successfully requestId=" << requestId
                          << " deviceId=" << deviceId.toStdString()
                          << " devNode=" << devNode.toStdString()
                          << " fsType=" << fsType.toStdString()
                          << "\n";
#endif

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
                    addLiveUpdateIndexedDevice(deviceId);
                }

                // Update indexed device runtime state again after scan completion,
                // to help keep state consistent if preferences changed during the scan
                if (const std::optional<BlockDevice> blockDevice = knownDeviceById(deviceId)) {
                    updateIndexedDeviceRuntimeState(*blockDevice);
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
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: received known devices requestId=" << requestId
                              << " size=" << blockDevices.size() << "\n";
#endif

                handleKnownDevicesUpdated(requestId, blockDevices);
            });

    connect(daemonClient_, &DaemonClient::liveUpdateBatchReceived,
            this, [this](
                const QString& deviceId,
                const QString& mountPoint,
                const std::vector<LiveUpdateEvent>& events
            ) {
#ifdef KERYTHING_ENABLE_LOGGING
                const LiveUpdateBatchSummary summary = summarizeLiveUpdateBatch(events);

                std::cout << "GUI: live update batch received"
                          << " deviceId=" << deviceId.toStdString()
                          << " mountPoint=" << mountPoint.toStdString()
                          << " count=" << events.size()
                          << " created=" << summary.created
                          << " deleted=" << summary.deleted
                          << " movedFrom=" << summary.movedFrom
                          << " movedTo=" << summary.movedTo
                          << " metadataChanged=" << summary.metadataChanged
                          << " selfDeletedOrMoved=" << summary.selfDeletedOrMoved
                          << " unknown=" << summary.unknown
                          << "\n";

                qsizetype logged = 0;
                for (const LiveUpdateEvent& event : events) {
                    if (logged >= 5) {
                        break;
                    }

                    const NormalizedLiveUpdateEvent normalized = normalizeLiveUpdateEvent(event);

                    std::cout << "  normalized kind="
                              << normalizedLiveUpdateKindToString(normalized.kind).toStdString()
                              << " name=" << normalized.name.toStdString()
                              << " parentHandle=" << normalized.parentHandleHex.toStdString()
                              << " objectHandle=" << normalized.objectHandleHex.toStdString()
                              << "\n";

                    ++logged;
                }
#endif
                Q_UNUSED(deviceId);
                Q_UNUSED(mountPoint);
                Q_UNUSED(events);
            });

    connect(daemonClient_, &DaemonClient::liveUpdateStatusChanged,
            this, [this](
                const QString& deviceId,
                LiveUpdateStatus status,
                const QString& reason
            ) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: live update status changed"
                          << " deviceId=" << deviceId.toStdString()
                          << " status=" << liveUpdateStatusToString(status).toStdString()
                          << " reason=" << reason.toStdString()
                          << "\n";
#endif

                QString deviceText = deviceId;
                if (const std::optional<BlockDevice> blockDevice = knownDeviceById(deviceId)) {
                    if (blockDevice->mounted && !blockDevice->primaryMountPoint.isEmpty()) {
                        deviceText = blockDevice->primaryMountPoint;
                    }
                    else if (!blockDevice->label.isEmpty()) {
                        deviceText = blockDevice->label;
                    }
                    else if (!blockDevice->devNode.isEmpty()) {
                        deviceText = blockDevice->devNode;
                    }
                }

                switch (status) {
                    case LiveUpdateStatus::Watching:
                        requestWindowStatusMessage(
                            QStringLiteral("Live updates active for %1").arg(deviceText),
                            3000
                        );
                        break;

                    case LiveUpdateStatus::NotWatching:
                        requestWindowStatusMessage(
                            QStringLiteral("Live updates stopped for %1").arg(deviceText),
                            5000
                        );
                        break;

                    case LiveUpdateStatus::StaleNeedsRescan:
                        requestWindowStatusMessage(
                            QStringLiteral("Index may be stale for %1: %2")
                                .arg(deviceText, reason),
                            10000
                        );
                        break;
                }
            });

    connect(daemonClient_, &DaemonClient::liveUpdateOperationBatchReceived,
            this, [this](
                const QString& deviceId,
                const QString& mountPoint,
                const std::vector<LiveUpdateOperation>& operations
            ) {
#ifdef KERYTHING_ENABLE_LOGGING
                std::cout << "GUI: live update operation batch received"
                          << " deviceId=" << deviceId.toStdString()
                          << " mountPoint=" << mountPoint.toStdString()
                          << " count=" << operations.size()
                          << "\n";

                for (const LiveUpdateOperation& operation : operations) {
                    std::cout << "  operation kind="
                              << liveUpdateOperationKindToString(operation.kind).toStdString();

                    if (operation.inode != 0) {
                        std::cout << " inode=" << operation.inode;
                    }

                    if (operation.parentInode != 0) {
                        std::cout << " parentInode=" << operation.parentInode;
                    }

                    if (!operation.name.isEmpty()) {
                        std::cout << " name=" << operation.name.toStdString();
                    }

                    if (operation.kind == LiveUpdateOperationKind::MetadataChanged ||
                        operation.kind == LiveUpdateOperationKind::Upsert) {
                        std::cout << " size=" << operation.size
                                  << " mtime=" << operation.modificationTime
                                  << " isDirectory=" << (operation.isDirectory ? "true" : "false")
                                  << " isSymlink=" << (operation.isSymlink ? "true" : "false");
                    }

                    if (!operation.reason.isEmpty()) {
                        std::cout << " reason=" << operation.reason.toStdString();
                    }

                    std::cout << "\n";
                }
#else
                Q_UNUSED(mountPoint);
#endif

                if (!indexController_) {
                    return;
                }

                const IndexController::LiveUpdateApplyResult result =
                    indexController_->applyLiveUpdateOperations(deviceId, operations);

                if (result.upserted > 0 || result.deleted > 0) {
                    scheduleLiveUpdateRefresh();
                }
                else if (result.metadataChanged > 0) {
                    scheduleLiveMetadataRefresh();
                }

                if (result.needsRescan > 0 || result.missingParent > 0 || result.unsupported > 0) {
                    QString deviceText = deviceId;
                    if (const std::optional<BlockDevice> blockDevice = knownDeviceById(deviceId)) {
                        if (blockDevice->mounted && !blockDevice->primaryMountPoint.isEmpty()) {
                            deviceText = blockDevice->primaryMountPoint;
                        }
                        else if (!blockDevice->label.isEmpty()) {
                            deviceText = blockDevice->label;
                        }
                        else if (!blockDevice->devNode.isEmpty()) {
                            deviceText = blockDevice->devNode;
                        }
                    }

                    requestWindowStatusMessage(
                        QStringLiteral("Index may be stale for %1: live update could not be applied safely")
                            .arg(deviceText),
                        10000
                    );
                }

#ifdef KERYTHING_ENABLE_LOGGING
                if (result.metadataChanged > 0 ||
                    result.upserted > 0 ||
                    result.deleted > 0 ||
                    result.needsRescan > 0 ||
                    result.unsupported > 0 ||
                    result.missingDevice > 0 ||
                    result.missingInode > 0 ||
                    result.missingParent > 0 ||
                    result.missingEntry > 0) {
                    std::cout << "GUI: applied live update operation batch"
                              << " deviceId=" << deviceId.toStdString()
                              << " metadataChanged=" << result.metadataChanged
                              << " upserted=" << result.upserted
                              << " deleted=" << result.deleted
                              << " needsRescan=" << result.needsRescan
                              << " unsupported=" << result.unsupported
                              << " missingDevice=" << result.missingDevice
                              << " missingInode=" << result.missingInode
                              << " missingParent=" << result.missingParent
                              << " missingEntry=" << result.missingEntry
                              << "\n";
                }
#endif
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

void AppController::showPreferencesDialog(PreferencesDialogPage initialPage)
{
    if (preferencesDialog_) {
        preferencesDialog_->setKnownDevices(knownDevices_);
        preferencesDialog_->setCurrentPage(initialPage);
        preferencesDialog_->show();
        preferencesDialog_->raise();
        preferencesDialog_->activateWindow();
        return;
    }

    auto* dialog = new PreferencesDialog(preferences_, knownDevices_, nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setCurrentPage(initialPage);

    preferencesDialog_ = dialog;

    connect(dialog, &QObject::destroyed, this, [this]() {
        preferencesDialog_ = nullptr;
    });

    connect(dialog, &PreferencesDialog::preferencesApplied,
            this, &AppController::applyDevicePreferenceChanges);

    connect(dialog, &PreferencesDialog::searchFiltersApplied,
        this, [this]() {
            Q_EMIT searchFiltersChanged();
            requestRefreshAllWindows();
        });

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void AppController::refreshIndexes()
{
    if (!daemonClient_ || !daemonClient_->isReady()) {
        requestWindowStatusMessage(
            QStringLiteral("Cannot refresh indexes: daemon is not ready."),
            5000
        );
        return;
    }

    quint32 requestId = 0;
    if (!requestKnownDevices(&requestId)) {
        requestWindowStatusMessage(
            QStringLiteral("Cannot refresh indexes: failed to request device list."),
            5000
        );
        return;
    }

    manualRefreshKnownDeviceRequestIds_.insert(requestId);

    requestWindowStatusMessage(
        QStringLiteral("Refreshing device list before indexing…"),
        3000
    );
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

    // Refresh every live, visible, non-minimized window. Minimized windows are
    // marked dirty and refreshed once when restored.
    for (auto& window : windows_) {
        if (!window) {
            continue;
        }

        if (window->isVisible() && !window->isMinimized()) {
            window->refresh();
        } else {
            window->markLiveStructuralRefreshDirty();
        }
    }
}

void AppController::requestLiveMetadataRefreshAllWindows()
{
    cleanupWindows();

    for (auto& window : windows_) {
        if (!window) {
            continue;
        }

        if (window->isVisible() && !window->isMinimized()) {
            window->refreshLiveMetadata();
        } else {
            window->markLiveMetadataRefreshDirty();
        }
    }
}

void AppController::scheduleLiveUpdateRefresh()
{
    if (!liveUpdateRefreshTimer_.isActive()) {
        liveUpdateRefreshTimer_.start();
    }
}

void AppController::scheduleLiveMetadataRefresh()
{
    if (!liveMetadataRefreshTimer_.isActive()) {
        liveMetadataRefreshTimer_.start();
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

std::vector<SearchFilterPreference> AppController::searchFilters() const
{
    return preferences_.searchFilters();
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

void AppController::updateOpenPreferencesDialog()
{
    if (preferencesDialog_) {
        preferencesDialog_->setKnownDevices(knownDevices_);
    }
}

bool AppController::requestKnownDevices(quint32* requestIdOut) {
    if (!daemonClient_ || !daemonClient_->isReady()) {
        return false;
    }

    quint32 requestId = 0;
    if (!daemonClient_->sendRequest(Protocol::MessageType::ListKnownDevices, QByteArray{}, &requestId)) {
        std::cerr << "GUI: failed to send ListKnownDevices request\n";
        return false;
    }

    if (requestIdOut) {
        *requestIdOut = requestId;
    }

#ifdef KERYTHING_ENABLE_LOGGING
    std::cout << "GUI: ListKnownDevices request sent requestId=" << requestId << "\n";
#endif
    return true;
}

void AppController::handleKnownDevicesUpdated(quint32 requestId, const std::vector<BlockDevice>& blockDevices)
{
    const bool manualRefresh = manualRefreshKnownDeviceRequestIds_.remove(requestId) > 0;
    const bool firstSnapshot = !hasReceivedKnownDevices_;
    const std::vector<BlockDevice> previousKnownDevices = knownDevices_;

    knownDevices_ = blockDevices;
    hasReceivedKnownDevices_ = true;

    // The first run device picket dialog should be shown before preferences are updated with known devices,
    // otherwise the dialog can't tell the difference between devices with existing preferences and new devices.
    maybeShowFirstRunDevicePicker(blockDevices);
    preferences_.updateKnownDevices(blockDevices);
    updateIndexedDeviceRuntimeStates(blockDevices);
    updateOpenPreferencesDialog();

#ifdef KERYTHING_ENABLE_LOGGING
    for (const auto& blockDevice : blockDevices) {
        std::cout << "GUI: received known device devNode=" << blockDevice.devNode.toStdString()
                  << " fsType=" << blockDevice.fsType.toStdString()
                  << " label=" << blockDevice.label.toStdString()
                  << " diskModel=" << blockDevice.diskModel.toStdString()
                  << " uuid=" << blockDevice.uuid.toStdString()
                  << " partuuid=" << blockDevice.partuuid.toStdString()
                  << " deviceId=" << blockDevice.deviceId.toStdString()
                  << " mounted=" << (blockDevice.mounted ? "true" : "false")
                  << " primaryMountPoint=" << blockDevice.primaryMountPoint.toStdString()
                  << " enabled=" << (preferences_.isDeviceEnabled(blockDevice.deviceId) ? "true" : "false")
                  << "\n";
    }
#endif

    if (manualRefresh) {
        const qsizetype scansStarted = scanEnabledKnownDevices(blockDevices);

        if (scansStarted == 0) {
            requestWindowStatusMessage(
                QStringLiteral("No indexes needed refreshing."),
                5000
            );
        } else {
            requestWindowStatusMessage(
                scansStarted == 1
                    ? QStringLiteral("Refreshing index for 1 device…")
                    : QStringLiteral("Refreshing indexes for %1 devices…").arg(scansStarted),
                3000
            );
        }
    } else if (firstSnapshot) {
        /*
         * Initial startup behavior: scan enabled devices that are currently eligible.
         */
        scanEnabledKnownDevices(blockDevices);
    } else {
        /*
         * Later daemon-pushed updates: scan only important transitions.
         */
        scanEnabledKnownDevices(
            devicesNeedingScanAfterKnownDeviceUpdate(previousKnownDevices, blockDevices)
        );
    }

    requestRefreshAllWindows();
}

void AppController::maybeShowFirstRunDevicePicker(const std::vector<BlockDevice>& blockDevices) {
    if (preferences_.initialDeviceSelectionCompleted()) {
        return;
    }

    if (blockDevices.empty()) {
#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "GUI: no supported devices found during first-run selection\n";
#endif
        // preferences_.setInitialDeviceSelectionCompleted(true);
        return;
    }

    DevicePickerDialog dialog(
        blockDevices,
        preferences_,
        windows_.isEmpty() ? nullptr : windows_.first().data()
    );
    const int result = dialog.exec();

    if (result != QDialog::Accepted) {
#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "GUI: first-run device selection skipped by user\n";
#endif
        preferences_.setInitialDeviceSelectionCompleted(true);
        return;
    }

    const QStringList selectedDeviceIds = dialog.selectedDeviceIds();

    for (const BlockDevice& blockDevice : blockDevices) {
        const bool enabled = selectedDeviceIds.contains(blockDevice.deviceId);
        preferences_.setDeviceEnabled(blockDevice, enabled);

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "GUI: first-run device selection deviceId="
                  << blockDevice.deviceId.toStdString()
                  << " enabled=" << (enabled ? "true" : "false")
                  << "\n";
#endif
    }

    preferences_.setInitialDeviceSelectionCompleted(true);
}

qsizetype AppController::scanEnabledKnownDevices(const std::vector<BlockDevice>& blockDevices) {
    if (!daemonClient_ || !daemonClient_->isReady()) {
        return 0;
    }

    qsizetype scansStarted = 0;

    for (const BlockDevice& blockDevice : blockDevices) {
        if (!preferences_.isDeviceEnabled(blockDevice.deviceId)) {
            continue;
        }

        if (activeScanDeviceIds_.contains(blockDevice.deviceId)) {
#ifdef KERYTHING_ENABLE_LOGGING
            std::cout << "GUI: skipping scan because device is already queued/scanning deviceId="
                      << blockDevice.deviceId.toStdString()
                      << "\n";
#endif
            continue;
        }

        const auto preference = preferences_.indexedDevicePreference(blockDevice.deviceId);
        if (!blockDevice.mounted && (!preference || !preference->scanWhenUnmounted)) {
#ifdef KERYTHING_ENABLE_LOGGING
            std::cout << "GUI: skipping enabled device because it is unmounted and scanWhenUnmounted=false deviceId="
                      << blockDevice.deviceId.toStdString()
                      << "\n";
#endif
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
        ++scansStarted;

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "GUI: ScanDevice request sent requestId=" << requestId
                  << " deviceId=" << blockDevice.deviceId.toStdString()
                  << " fsType=" << blockDevice.fsType.toStdString()
                  << "\n";
#endif
    }

    return scansStarted;
}

void AppController::applyDevicePreferenceChanges(const QList<DevicePreferenceChange>& changes) {
    QStringList disabledDeviceIds;
    QStringList deviceIdsToScan;
    QStringList unmountedScanDisabledDeviceIds;

    for (const DevicePreferenceChange& change : changes) {
        if (change.becameDisabled()) {
            disabledDeviceIds << change.deviceId;
            continue;
        }

        if (change.becameEnabled()) {
            deviceIdsToScan << change.deviceId;
        }

        if (change.enabled && change.scanWhenUnmountedChanged()) {
            if (change.scanWhenUnmounted) {
                deviceIdsToScan << change.deviceId;
            } else if (!isKnownDeviceMounted(change.deviceId)) {
                unmountedScanDisabledDeviceIds << change.deviceId;
            }
        }

        if (change.enabled && change.showOfflineResultsChanged()) {
            if (const std::optional<BlockDevice> blockDevice = knownDeviceById(change.deviceId)) {
                updateIndexedDeviceRuntimeState(*blockDevice);
            } else {
                indexController_->updateDeviceRuntimeStateByDeviceId(
                    change.deviceId,
                    false,
                    change.showOfflineResults
                );
            }
        }

        if (change.enabled && change.liveUpdatesEnabledChanged()) {
            syncLiveUpdateDevices();
        }
    }

    disabledDeviceIds.removeDuplicates();
    deviceIdsToScan.removeDuplicates();
    unmountedScanDisabledDeviceIds.removeDuplicates();

    for (const QString& disabledDeviceId : disabledDeviceIds) {
        cancelActiveScansForDevice(disabledDeviceId, true);
        removeLiveUpdateIndexedDevice(disabledDeviceId);

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "GUI: disabled indexed device deviceId="
                  << disabledDeviceId.toStdString()
                  << "\n";
#endif
    }

    for (const QString& deviceId : unmountedScanDisabledDeviceIds) {
        cancelActiveScansForDevice(deviceId, false);

        if (const std::optional<BlockDevice> blockDevice = knownDeviceById(deviceId)) {
            updateIndexedDeviceRuntimeState(*blockDevice);
        }

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "GUI: cancelled unmounted scan because scanWhenUnmounted was disabled deviceId="
                  << deviceId.toStdString()
                  << "\n";
#endif
    }

    std::vector<BlockDevice> devicesToScan;

    for (const BlockDevice& blockDevice : knownDevices_) {
        if (deviceIdsToScan.contains(blockDevice.deviceId)) {
            updateIndexedDeviceRuntimeState(blockDevice);
            devicesToScan.push_back(blockDevice);
        }
    }

    scanEnabledKnownDevices(devicesToScan);
    requestRefreshAllWindows();
}

void AppController::cancelActiveScansForDevice(const QString& deviceId, bool removeDeviceIndex)
{
    if (deviceId.isEmpty()) {
        return;
    }

    QList<quint32> requestIdsToCancel;

    for (auto it = scanRequestDeviceIds_.cbegin(); it != scanRequestDeviceIds_.cend(); ++it) {
        if (it.value() == deviceId) {
            requestIdsToCancel << it.key();
        }
    }

    for (const quint32 requestId : requestIdsToCancel) {
        if (daemonClient_) {
            daemonClient_->cancelRequest(requestId);
        }

        takeTrackedScanDeviceId(requestId, deviceId);
        indexController_->removeDeviceByRequestId(requestId);
    }

    activeScanDeviceIds_.remove(deviceId);

    if (removeDeviceIndex) {
        indexController_->removeDeviceByDeviceId(deviceId);
        removeLiveUpdateIndexedDevice(deviceId);
    }
}

std::optional<BlockDevice> AppController::knownDeviceById(const QString& deviceId) const
{
    if (deviceId.isEmpty()) {
        return std::nullopt;
    }

    for (const BlockDevice& blockDevice : knownDevices_) {
        if (blockDevice.deviceId == deviceId) {
            return blockDevice;
        }
    }

    return std::nullopt;
}

void AppController::updateIndexedDeviceRuntimeState(const BlockDevice& blockDevice)
{
    const auto preference = preferences_.indexedDevicePreference(blockDevice.deviceId);

    indexController_->updateDeviceRuntimeStateByDeviceId(
        blockDevice.deviceId,
        blockDevice.mounted,
        !preference || preference->showOfflineResults,
        blockDevice.mountPoints,
        blockDevice.primaryMountPoint
    );
}

void AppController::updateIndexedDeviceRuntimeStates(const std::vector<BlockDevice>& blockDevices)
{
    for (const BlockDevice& blockDevice : blockDevices) {
        updateIndexedDeviceRuntimeState(blockDevice);
    }
}

std::vector<BlockDevice> AppController::devicesNeedingScanAfterKnownDeviceUpdate(
    const std::vector<BlockDevice>& oldDevices,
    const std::vector<BlockDevice>& newDevices) const
{
    QHash<QString, BlockDevice> oldByDeviceId;

    for (const BlockDevice& oldDevice : oldDevices) {
        if (!oldDevice.deviceId.isEmpty()) {
            oldByDeviceId.insert(oldDevice.deviceId, oldDevice);
        }
    }

    std::vector<BlockDevice> out;

    for (const BlockDevice& newDevice : newDevices) {
        if (newDevice.deviceId.isEmpty()) {
            continue;
        }

        if (!preferences_.isDeviceEnabled(newDevice.deviceId)) {
            continue;
        }

        const bool wasKnown = oldByDeviceId.contains(newDevice.deviceId);
        const bool wasMounted = wasKnown && oldByDeviceId.value(newDevice.deviceId).mounted;
        const bool becameMounted = newDevice.mounted && !wasMounted;
        const bool appearedMounted = !wasKnown && newDevice.mounted;

        if (becameMounted || appearedMounted) {
            out.push_back(newDevice);

#ifdef KERYTHING_ENABLE_LOGGING
            std::cout << "GUI: scheduling scan for device transition deviceId="
                      << newDevice.deviceId.toStdString()
                      << " becameMounted=" << (becameMounted ? "true" : "false")
                      << " appearedMounted=" << (appearedMounted ? "true" : "false")
                      << "\n";
#endif
        }
    }

    return out;
}

bool AppController::isKnownDeviceMounted(const QString& deviceId) const
{
    const std::optional<BlockDevice> blockDevice = knownDeviceById(deviceId);
    return blockDevice && blockDevice->mounted;
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

void AppController::addLiveUpdateIndexedDevice(const QString& deviceId)
{
    if (deviceId.isEmpty()) {
        return;
    }

    if (liveUpdateIndexedDeviceIds_.contains(deviceId)) {
        return;
    }

    liveUpdateIndexedDeviceIds_.insert(deviceId);
    syncLiveUpdateDevices();
}

void AppController::removeLiveUpdateIndexedDevice(const QString& deviceId)
{
    if (deviceId.isEmpty()) {
        return;
    }

    if (liveUpdateIndexedDeviceIds_.remove(deviceId) == 0) {
        return;
    }

    syncLiveUpdateDevices();
}

bool AppController::liveUpdatesEnabledForDevice(const QString& deviceId) const
{
    if (deviceId.isEmpty()) {
        return false;
    }

    const std::optional<IndexedDevicePreference> preference =
        preferences_.indexedDevicePreference(deviceId);

    return !preference || preference->liveUpdatesEnabled;
}

void AppController::syncLiveUpdateDevices()
{
    if (!daemonClient_ || !daemonClient_->isReady()) {
        return;
    }

    QStringList deviceIds;

    for (const QString& deviceId : liveUpdateIndexedDeviceIds_) {
        if (liveUpdatesEnabledForDevice(deviceId)) {
            deviceIds << deviceId;
        }
    }

    deviceIds.removeDuplicates();
    deviceIds.removeAll(QString{});

    if (!daemonClient_->setLiveUpdateDevices(deviceIds)) {
        std::cerr << "GUI: failed to send live update device subscription\n";
    }
}