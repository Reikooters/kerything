// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_APPCONTROLLER_H
#define KERYTHING_APPCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QList>
#include <QHash>
#include <QSet>
#include <QTimer>

#include "DaemonClient.h"
#include "DevicePreferenceChange.h"
#include "IndexController.h"
#include "Preferences.h"
#include "PreferencesDialogPage.h"

class QApplication;
class MainWindow;
class PreferencesDialog;
class SingleInstanceServer;

class AppController final : public QObject {
    Q_OBJECT

public:
    explicit AppController(QApplication& app, QObject* parent = nullptr);

    bool start();
    void openNewWindow();
    void openWindowForLaunchRequest();
    void presentExistingWindow();
    void showPreferencesDialog(PreferencesDialogPage initialPage = PreferencesDialogPage::Devices);
    void refreshIndexes();
    void requestRefreshAllWindows();
    void requestWindowStatusMessage(const QString& message, int timeoutMs);
    [[nodiscard]] bool isDaemonConnected() const noexcept;
    [[nodiscard]] bool isDaemonReady() const noexcept;
    [[nodiscard]] std::vector<SearchFilterPreference> searchFilters() const;
    [[nodiscard]] bool autoRefreshResultsForLiveUpdates() const;
    void setAutoRefreshResultsForLiveUpdates(bool enabled);
    [[nodiscard]] bool sortDateDescendingFirst() const;
    [[nodiscard]] bool sortSizeDescendingFirst() const;
    IndexController* indexController() const noexcept;

Q_SIGNALS:
    void searchFiltersChanged();
    void autoRefreshResultsForLiveUpdatesChanged(bool enabled);

private Q_SLOTS:
    void onPrimaryRequestedOpenWindow();
    void onPrimaryRequestedCommand(const QString& command);

private:
    void cleanupWindows();
    void trimSortScratchAllWindows();
    void updateOpenPreferencesDialog();
    bool requestKnownDevices(quint32* requestIdOut = nullptr);
    void handleKnownDevicesUpdated(quint32 requestId, const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] std::vector<BlockDevice> devicesNeedingScanAfterKnownDeviceUpdate(
        const std::vector<BlockDevice>& oldDevices,
        const std::vector<BlockDevice>& newDevices
    ) const;
    qsizetype scanEnabledKnownDevices(const std::vector<BlockDevice>& blockDevices);
    void applyDevicePreferenceChanges(const QList<DevicePreferenceChange>& changes);
    void cancelActiveScansForDevice(const QString& deviceId, bool removeDeviceIndex);
    void updateIndexedDeviceRuntimeState(const BlockDevice& blockDevice);
    void updateIndexedDeviceRuntimeStates(const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] std::optional<BlockDevice> knownDeviceById(const QString& deviceId) const;
    [[nodiscard]] bool isKnownDeviceMounted(const QString& deviceId) const;
    [[nodiscard]] bool deviceSupportsLiveUpdates(const QString& deviceId) const;
    void maybeShowFirstRunDevicePicker(const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] bool validateScanDeviceId(quint32 requestId, const QString& actualDeviceId, const char* eventName) const;
    QString takeTrackedScanDeviceId(quint32 requestId, const QString& fallbackDeviceId = {});
    [[nodiscard]] int liveRefreshIntervalMs() const;
    [[nodiscard]] bool hasLiveRefreshEligibleWindow() const;
    void scheduleLiveUpdateRefresh();
    void scheduleLiveMetadataRefresh();
    void requestLiveMetadataRefreshAllWindows();
    void addLiveUpdateIndexedDevice(const QString& deviceId);
    void removeLiveUpdateIndexedDevice(const QString& deviceId);
    [[nodiscard]] bool liveUpdatesEnabledForDevice(const QString& deviceId) const;
    bool requestScanForDeviceId(const QString& deviceId);
    void syncLiveUpdateDevices();

    QApplication& app_;
    SingleInstanceServer* instanceServer_ = nullptr;
    DaemonClient* daemonClient_ = nullptr;
    IndexController* indexController_ = nullptr;
    Preferences preferences_;

    // requestId -> stable deviceId
    QHash<quint32, QString> scanRequestDeviceIds_;

    // stable deviceIds currently queued/scanning
    QSet<QString> activeScanDeviceIds_;

    QSet<quint32> manualRefreshKnownDeviceRequestIds_;
    QSet<QString> liveUpdateIndexedDeviceIds_;

    std::vector<BlockDevice> knownDevices_;
    bool hasReceivedKnownDevices_ = false;

    QPointer<PreferencesDialog> preferencesDialog_;
    QList<QPointer<MainWindow>> windows_;
    QTimer liveUpdateRefreshTimer_;
    QTimer liveMetadataRefreshTimer_;
    bool liveUpdateRefreshPausedDirty_ = false;
};

#endif // KERYTHING_APPCONTROLLER_H