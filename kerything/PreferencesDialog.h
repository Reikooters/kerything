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

enum class PreferencesDialogPage;

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
    void setCurrentPage(PreferencesDialogPage page);
    void setAutoRefreshResultsForLiveUpdates(bool enabled);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    void preferencesApplied(QList<DevicePreferenceChange> changes);
    void searchFiltersApplied();
    void autoRefreshResultsForLiveUpdatesApplied(bool enabled);

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

    enum FilterColumn {
        FilterNameColumn = 0,
        FilterQueryColumn,
        FilterColumnCount
    };

    QWidget* createDevicesPage();
    QWidget* createFiltersPage();
    QWidget* createUiPage();
    QWidget* createIndexingPage();
    QWidget* createAdvancedPage();

    void populateNavigation();
    void populateDeviceTable();
    void populateFilterTable();
    void populateFilterTable(const std::vector<SearchFilterPreference>& filters);
    void updateApplyButtonEnabled();
    void applyChanges();
    bool hasChanges() const;
    bool hasDeviceChanges() const;
    bool hasFilterChanges() const;
    bool hasUIChanges() const;
    bool hasGeneralChanges() const;
    bool validateFilters(QString* errorText = nullptr) const;

    QStringList enabledDeviceIdsFromTable() const;
    bool scanWhenUnmountedForDevice(const QString& deviceId) const;
    bool showOfflineResultsForDevice(const QString& deviceId) const;
    bool unmountedScanningSupportedForDevice(const QString& deviceId) const;
    bool liveUpdatesEnabledForDevice(const QString& deviceId) const;
    bool liveUpdatesSupportedForDevice(const QString& deviceId) const;
    void toggleDeviceRowChecked(int row);

    std::vector<SearchFilterPreference> filtersFromTable() const;
    QString uniqueFilterName(const QString& baseName) const;
    QString newCustomFilterId() const;
    QList<int> selectedFilterRows() const;
    void updateFilterButtonStates();
    void moveSelectedFilters(int direction);

    Preferences& preferences_;
    std::vector<BlockDevice> knownDevices_;

    QHash<QString, BlockDevice> knownDeviceById_;
    QHash<QString, IndexedDevicePreference> originalPreferencesByDeviceId_;
    std::vector<SearchFilterPreference> originalSearchFilters_;

    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    QPushButton* applyButton_ = nullptr;

    QTableWidget* deviceTable_ = nullptr;
    QCheckBox* scanWhenUnmountedCheckBox_ = nullptr;
    QCheckBox* showOfflineResultsCheckBox_ = nullptr;
    QLabel* liveUpdatesWarningIconLabel_ = nullptr;
    QCheckBox* liveUpdatesEnabledCheckBox_ = nullptr;
    QLabel* selectedDeviceLabel_ = nullptr;

    QTableWidget* filterTable_ = nullptr;
    QPushButton* addFilterButton_ = nullptr;
    QPushButton* duplicateFilterButton_ = nullptr;
    QPushButton* removeFilterButton_ = nullptr;
    QPushButton* moveFilterUpButton_ = nullptr;
    QPushButton* moveFilterDownButton_ = nullptr;
    QPushButton* restoreDefaultFiltersButton_ = nullptr;

    QCheckBox* createNewWindowOnLaunchCheckBox_ = nullptr;
    QCheckBox* sortDateDescendingFirstCheckBox_ = nullptr;
    QCheckBox* sortSizeDescendingFirstCheckBox_ = nullptr;
    QCheckBox* showInFileManagerOnPathDoubleClickCheckBox_ = nullptr;

    QCheckBox* autoRefreshLiveUpdatesCheckBox_ = nullptr;
};

#endif // KERYTHING_PREFERENCESDIALOG_H