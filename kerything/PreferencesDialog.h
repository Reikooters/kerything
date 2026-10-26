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
#include "IndexSummary.h"
#include "Preferences.h"
#include "SearchFilterUtils.h"

enum class PreferencesDialogPage;

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTextBrowser;
class QToolButton;

class PreferencesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(
        Preferences& preferences,
        const std::vector<BlockDevice>& knownDevices,
        const std::vector<IndexSummary>& indexSummaries,
        QWidget* parent = nullptr
    );

    void setKnownDevices(const std::vector<BlockDevice>& knownDevices);
    void setIndexSummaries(const std::vector<IndexSummary>& indexSummaries);
    void setCurrentPage(PreferencesDialogPage page);
    void setAutoRefreshResultsForLiveUpdates(bool enabled);
    void setShowFiltersDropdown(bool enabled);
    void setShowPreviewPane(bool enabled);
    void appendSearchFilter(const SearchFilterPreference& filter);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    void preferencesApplied(QList<DevicePreferenceChange> changes);
    void refreshIndexRequested(const QString& deviceId);
    void refreshStaleIndexesRequested();
    void forgetIndexRequested(const QString& deviceId);
    void deleteSavedIndexRequested(const QString& deviceId);
    void searchFiltersApplied();
    void autoRefreshResultsForLiveUpdatesApplied(bool enabled);
    void searchResultHighlightingApplied(bool enabled);
    void showFiltersDropdownApplied(bool enabled);
    void showPreviewPaneApplied(bool enabled);
    void trayPreferencesApplied(bool showTrayIcon, bool closeToTray);

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

    enum IndexColumn {
        IndexNameColumn = 0,
        IndexStatusColumn,
        IndexRecordsColumn,
        IndexFilesystemColumn,
        IndexLastIndexedColumn,
        IndexLastUpdatedColumn,
        IndexColumnCount
    };

    enum FilterColumn {
        FilterNameColumn = 0,
        FilterMacroColumn,
        FilterQueryColumn,
        FilterColumnCount
    };

    QWidget* createDevicesPage();
    QWidget* createIndexesPage();
    QWidget* createFiltersPage();
    QWidget* createWindowsPage();
    QWidget* createUiPage();
    QWidget* createAdvancedPage();

    void populateNavigation();
    void populateDeviceTable();
    void populateIndexTable();
    void populateFilterTable();
    void populateFilterTable(const std::vector<SearchFilterPreference>& filters);
    void updateSelectedDeviceDetailsHeight();
    void scheduleSelectedDeviceDetailsHeightUpdate();
    void updateSelectedIndexDetails();
    void scheduleSelectedIndexDetailsUpdate();
    void noteSelectedIndexDetailsUserScroll();
    void updateApplyButtonEnabled();
    bool applyChanges();
    bool hasChanges() const;
    bool hasDeviceChanges() const;
    bool hasFilterChanges() const;
    bool hasWindowChanges() const;
    bool hasUIChanges() const;
    bool hasGeneralChanges() const;
    bool validateFilters(QString* errorText = nullptr, bool focusFirstInvalid = true) const;

    QStringList enabledDeviceIdsFromTable() const;
    bool scanWhenUnmountedForDevice(const QString& deviceId) const;
    bool showOfflineResultsForDevice(const QString& deviceId) const;
    bool saveIndexToDiskForDevice(const QString& deviceId) const;
    bool refreshOnStartupForDevice(const QString& deviceId) const;
    bool isBtrfsDevice(const QString& deviceId) const;
    bool unmountedScanningSupportedForDevice(const QString& deviceId) const;
    bool liveUpdatesEnabledForDevice(const QString& deviceId) const;
    bool liveUpdatesSupportedForDevice(const QString& deviceId) const;
    bool refreshIndexBeforeEnablingLiveUpdatesForDevice(const QString& deviceId) const;
    bool shouldShowRefreshIndexBeforeEnablingLiveUpdatesForDevice(const QString& deviceId) const;
    void toggleDeviceRowChecked(int row);

    static QString displayNameForIndexSummary(
        const IndexSummary& summary,
        const QHash<QString, BlockDevice>& knownDeviceById
    );

    std::vector<SearchFilterPreference> filtersFromTable() const;
    QList<int> selectedFilterRows() const;
    void updateFilterButtonStates();
    void moveSelectedFilters(int direction);
    void clearFilterValidationState() const;
    void markFilterCellInvalid(int row, int column, const QString& message) const;
    void focusFilterCell(int row, int column) const;
    bool hasFilterValidationState() const;
    void exportFilters();
    void importFilters();
    static QString filterImportIssueSummary(
        const std::vector<SearchFilterUtils::SearchFilterValidationIssue>& issues,
        const std::vector<SearchFilterPreference>& filters
    );
    static bool validateImportedFilters(
        std::vector<SearchFilterPreference>& filters,
        QString* errorText
    );
    static QString importedFilterConflictSummary(
        const std::vector<SearchFilterPreference>& existingFilters,
        const std::vector<SearchFilterPreference>& importedFilters
    );

    Preferences& preferences_;
    std::vector<BlockDevice> knownDevices_;
    std::vector<IndexSummary> indexSummaries_;

    QHash<QString, BlockDevice> knownDeviceById_;
    QHash<QString, IndexedDevicePreference> originalPreferencesByDeviceId_;
    std::vector<SearchFilterPreference> originalSearchFilters_;

    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    QPushButton* applyButton_ = nullptr;

    QTableWidget* deviceTable_ = nullptr;
    QPushButton* refreshSelectedDeviceIndexButton_ = nullptr;
    QCheckBox* scanWhenUnmountedCheckBox_ = nullptr;
    QCheckBox* showOfflineResultsCheckBox_ = nullptr;
    QCheckBox* saveIndexToDiskCheckBox_ = nullptr;
    QCheckBox* refreshOnStartupCheckBox_ = nullptr;
    QLabel* liveUpdatesWarningIconLabel_ = nullptr;
    QCheckBox* liveUpdatesEnabledCheckBox_ = nullptr;
    QCheckBox* refreshIndexBeforeEnablingLiveUpdatesCheckBox_ = nullptr;
    QTextBrowser* selectedDeviceDetailsText_ = nullptr;
    quint64 deviceTableScrollRestoreToken_ = 0;

    QTableWidget* indexTable_ = nullptr;
    QTextBrowser* selectedIndexDetailsText_ = nullptr;
    QPushButton* refreshIndexButton_ = nullptr;
    QPushButton* refreshStaleIndexesButton_ = nullptr;
    QPushButton* forgetIndexButton_ = nullptr;
    QPushButton* deleteSavedIndexButton_ = nullptr;
    QString selectedIndexDetailsDeviceId_;
    quint64 selectedIndexDetailsIndexId_ = 0;
    quint64 indexTableScrollRestoreToken_ = 0;
    quint64 selectedIndexDetailsScrollRestoreToken_ = 0;
    bool selectedIndexDetailsUpdatePending_ = false;
    qint64 selectedIndexDetailsLastUserScrollMs_ = 0;

    QTableWidget* filterTable_ = nullptr;
    QPushButton* addFilterButton_ = nullptr;
    QPushButton* duplicateFilterButton_ = nullptr;
    QPushButton* removeFilterButton_ = nullptr;
    QPushButton* moveFilterUpButton_ = nullptr;
    QPushButton* moveFilterDownButton_ = nullptr;
    QPushButton* restoreDefaultFiltersButton_ = nullptr;
    QToolButton* filterMoreButton_ = nullptr;

    QCheckBox* createNewWindowOnLaunchCheckBox_ = nullptr;
    QCheckBox* carryFilterToNewWindowsCheckBox_ = nullptr;
    QCheckBox* carrySearchOptionsToNewWindowsCheckBox_ = nullptr;
    QCheckBox* carrySearchTextToNewWindowsCheckBox_ = nullptr;
    QCheckBox* carryResultSortingToNewWindowsCheckBox_ = nullptr;
    QCheckBox* carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_ = nullptr;
    QCheckBox* showTrayIconCheckBox_ = nullptr;
    QCheckBox* closeToTrayCheckBox_ = nullptr;
    QCheckBox* showCloseToTrayNotificationCheckBox_ = nullptr;
    QComboBox* defaultSortColumnComboBox_ = nullptr;
    QComboBox* defaultSortOrderComboBox_ = nullptr;
    QCheckBox* sortDateDescendingFirstCheckBox_ = nullptr;
    QCheckBox* sortSizeDescendingFirstCheckBox_ = nullptr;
    QCheckBox* showInFileManagerOnPathDoubleClickCheckBox_ = nullptr;
    QCheckBox* showHighlightedSearchTermsCheckBox_ = nullptr;
    QCheckBox* showFiltersDropdownCheckBox_ = nullptr;
    QCheckBox* showPreviewPaneCheckBox_ = nullptr;

    QCheckBox* autoRefreshLiveUpdatesCheckBox_ = nullptr;
};

#endif // KERYTHING_PREFERENCESDIALOG_H