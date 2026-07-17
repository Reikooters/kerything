// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_APPCONTROLLER_H
#define KERYTHING_APPCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QList>
#include <QHash>
#include <QSet>

#include "DaemonClient.h"
#include "DevicePreferenceChange.h"
#include "IndexController.h"
#include "Preferences.h"

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
    void showPreferencesDialog();
    void requestRefreshAllWindows();
    void requestWindowStatusMessage(const QString& message, int timeoutMs);
    [[nodiscard]] bool isDaemonConnected() const noexcept;
    [[nodiscard]] bool isDaemonReady() const noexcept;
    IndexController* indexController() const noexcept;

private Q_SLOTS:
    void onPrimaryRequestedOpenWindow();
    void onPrimaryRequestedCommand(const QString& command);

private:
    void cleanupWindows();
    void updateOpenPreferencesDialog();
    void requestKnownDevices();
    void handleKnownDevicesUpdated(quint32 requestId, const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] std::vector<BlockDevice> devicesNeedingScanAfterKnownDeviceUpdate(
        const std::vector<BlockDevice>& oldDevices,
        const std::vector<BlockDevice>& newDevices
    ) const;
    void scanEnabledKnownDevices(const std::vector<BlockDevice>& blockDevices);
    void applyDevicePreferenceChanges(const QList<DevicePreferenceChange>& changes);
    void cancelActiveScansForDevice(const QString& deviceId, bool removeDeviceIndex);
    void updateIndexedDeviceRuntimeState(const BlockDevice& blockDevice);
    void updateIndexedDeviceRuntimeStates(const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] std::optional<BlockDevice> knownDeviceById(const QString& deviceId) const;
    [[nodiscard]] bool isKnownDeviceMounted(const QString& deviceId) const;
    void maybeShowFirstRunDevicePicker(const std::vector<BlockDevice>& blockDevices);
    [[nodiscard]] bool validateScanDeviceId(quint32 requestId, const QString& actualDeviceId, const char* eventName) const;
    QString takeTrackedScanDeviceId(quint32 requestId, const QString& fallbackDeviceId = {});

    QApplication& app_;
    SingleInstanceServer* instanceServer_ = nullptr;
    DaemonClient* daemonClient_ = nullptr;
    IndexController* indexController_ = nullptr;
    Preferences preferences_;

    // requestId -> stable deviceId
    QHash<quint32, QString> scanRequestDeviceIds_;

    // stable deviceIds currently queued/scanning
    QSet<QString> activeScanDeviceIds_;

    std::vector<BlockDevice> knownDevices_;
    bool hasReceivedKnownDevices_ = false;

    QPointer<PreferencesDialog> preferencesDialog_;
    QList<QPointer<MainWindow>> windows_;
};

#endif // KERYTHING_APPCONTROLLER_H