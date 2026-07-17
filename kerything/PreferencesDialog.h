// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PREFERENCESDIALOG_H
#define KERYTHING_PREFERENCESDIALOG_H

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <vector>

#include "BlockDevice.h"
#include "DevicePreferenceChange.h"
#include "Preferences.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;

class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(
        Preferences& preferences,
        const std::vector<BlockDevice>& knownDevices,
        QWidget* parent = nullptr
    );

    void setKnownDevices(const std::vector<BlockDevice>& knownDevices);

Q_SIGNALS:
    void preferencesApplied(QList<DevicePreferenceChange> changes);

private:
    enum DeviceColumn {
        DeviceEnabledColumn = 0,
        DeviceNameColumn,
        DeviceStatusColumn,
        DeviceFsTypeColumn,
        DeviceMountPointColumn,
        DeviceNodeColumn,
        DeviceModelColumn,
        DeviceColumnCount
    };

    QWidget* createDevicesPage();
    QWidget* createIndexingPage();
    QWidget* createAdvancedPage();

    void populateNavigation();
    void populateDeviceTable();
    void updateApplyButtonEnabled();
    void applyChanges();
    bool hasChanges() const;

    QStringList enabledDeviceIdsFromTable() const;
    bool scanWhenUnmountedForDevice(const QString& deviceId) const;
    bool showOfflineResultsForDevice(const QString& deviceId) const;

    Preferences& preferences_;
    std::vector<BlockDevice> knownDevices_;

    QHash<QString, BlockDevice> knownDeviceById_;
    QHash<QString, IndexedDevicePreference> originalPreferencesByDeviceId_;

    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    QPushButton* applyButton_ = nullptr;

    QTableWidget* deviceTable_ = nullptr;
    QCheckBox* scanWhenUnmountedCheckBox_ = nullptr;
    QCheckBox* showOfflineResultsCheckBox_ = nullptr;
    QLabel* selectedDeviceLabel_ = nullptr;
};

#endif // KERYTHING_PREFERENCESDIALOG_H