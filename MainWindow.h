// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_MAINWINDOW_H
#define KERYTHING_MAINWINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QTableView>
#include <QLabel>
#include <QString>
#include <QTimer>
#include <QComboBox>
#include <QRect>
#include <QtDBus/QDBusServiceWatcher>
#include <vector>
#include <string>
#include <memory>
#include "ScannerEngine.h"
#include "DbusIndexerClient.h"

class FileModel;
class RemoteFileModel;

/**
 * @brief The main application window for searching and viewing files.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /**
     * @brief Constructs the MainWindow.
     */
    explicit MainWindow(QWidget *parent = nullptr);

    /**
     * @brief Updates the current database and UI.
     */
    void setDatabase(ScannerEngine::SearchDatabase&& database, QString mountPath, QString devicePath, const QString& fsType);

    /**
     * @brief Retrieves the index of the currently hovered row in the table view.
     *
     * This method returns the row index that is currently being hovered by the
     * mouse pointer. If no row is hovered, it returns -1.
     *
     * @return The index of the hovered row, or -1 if no row is hovered.
     */
    int hoveredRow() const { return m_hoveredRow; }

protected:
    /**
     * @brief Handles right-click events to show the file context menu.
     */
    void contextMenuEvent(QContextMenuEvent *event) override;

    /**
     * @brief Filters events for specific objects in the application.
     *
     * This method intercepts and handles events for the given watched object
     * and applies custom behavior. If the event is not explicitly handled,
     * it delegates the processing to the base class implementation.
     *
     * @param watched The QObject that this method is filtering events for.
     * @param event The QEvent being intercepted for this object.
     * @return True if the event is handled and should not propagate further;
     *         otherwise returns false to pass the event to the base class or default handlers.
     */
    bool eventFilter(QObject* watched, QEvent* event) override;

    void closeEvent(QCloseEvent* event) override;

    void changeEvent(QEvent* event) override;

    void showEvent(QShowEvent* event) override;

private slots:
    /**
     * @brief Handles the event of a daemon service being registered.
     *
     * This method is triggered when a daemon service becomes available or restarts.
     * It updates the daemon status and refreshes the current query page if necessary.
     *
     * @param serviceName The name of the registered daemon service.
     */
    void onDaemonServiceRegistered(const QString& serviceName);

    /**
     * @brief Handles the event when a daemon service is unregistered.
     *
     * This method updates the UI and internal states to reflect the disconnection
     * of the specified daemon service. It ensures users are informed about the
     * unavailability of live updates and transitions the search model to an offline
     * state if necessary.
     *
     * @param serviceName The name of the daemon service that was unregistered.
     */
    void onDaemonServiceUnregistered(const QString& serviceName);

    /**
     * @brief Updates the device index and refreshes the search results in the UI.
     *
     * This slot is triggered when the device index is updated. It ensures that
     * the search results are refreshed based on the new index data, particularly
     * when using the daemon search functionality.
     *
     * @param deviceId The unique identifier of the device whose index was updated.
     * @param generation The generation number of the updated index.
     * @param entryCount The number of entries in the updated index.
     */
    void onDeviceIndexUpdated(const QString& deviceId, quint64 generation, quint64 entryCount);

    /**
     * @brief Tracks which row is currently hovered so we can paint a full-row hover highlight.
     */
    void onTableHovered(const QModelIndex& index);

    /**
     * @brief Tracks when the empty area of the table is hovered (below the last item in the list)
     */
    void onTableViewportHovered();

    /**
     * @brief Triggered when the search text changes. Performs a trigram search and updates the view.
     */
    void updateSearch(const QString &text);

    /**
     * @brief Opens the settings dialog for modifying application preferences.
     *
     * This method displays the settings dialog, allowing the user to configure
     * application preferences. If no D-Bus client is available, a warning is
     * displayed. After the settings are modified, the daemon status label is
     * refreshed, and remote search results are invalidated if applicable.
     */
    void openSettings();

    /**
     * @brief Shows a placeholder for the change partition logic.
     */
    void changePartition();

    /**
     * @brief Shows a placeholder for the rescan partition logic.
     */
    void rescanPartition();

    /**
     * @brief Shows the About dialog using KAboutData.
     */
    void showAbout();

    /**
     * @brief Opens the selected file or folder using system defaults.
     */
    void openFile(const QModelIndex &index);

    /**
     * @brief Opens all currently selected files in the table.
     */
    void openSelectedFiles();

    /**
     * @brief Opens the folder containing the currently selected file.
     */
    void openSelectedLocation();

    /**
     * @brief Copies the names of the selected items to the clipboard.
     */
    void copyFileNames();

    /**
     * @brief Copies the full paths of selected items to the clipboard.
     */
    void copyPaths();

    /**
     * @brief Copies the selected files themselves to the clipboard (for pasting in Dolphin).
     */
    void copyFiles();

    /**
     * @brief Opens a terminal in the folder of the selected item.
     */
    void openTerminal();

private:
    /**
     * @brief Performs the high-speed trigram-based keyword search.
     */
    std::vector<uint32_t> performTrigramSearch(const std::string& query);

    /**
     * @brief Case-insensitive substring helper.
     */
    static bool contains(std::string_view haystack, std::string_view needle);

    /**
     * @brief Updates the status label to reflect the current state of the daemon connection.
     *
     * This function checks the connection to the D-Bus and updates the status label accordingly.
     * It handles the following scenarios:
     * - If the D-Bus is not initialized, it indicates that the daemon is disconnected and live
     *   updates are paused.
     * - If the daemon ping fails, the disconnection is reported along with the error message.
     * - If fetching the list of indexed devices fails, the connection status is shown with an
     *   error message indicating the issue with retrieving indexes.
     * - If connected and indexes are available, the label displays the number of indexed
     *   partitions and total objects indexed by the daemon.
     */
    void refreshDaemonStatusLabel();

    void refreshDeviceScopeCombo();

    void loadUiSettings();
    void saveUiSettings() const;
    void applyPersistedSortToView();

    void loadWindowPlacement();
    void saveWindowPlacement() const;

    // --- Table header (column width/state) persistence ---
    void loadTableHeaderState();
    void scheduleSaveTableHeaderState();
    void saveTableHeaderState() const;
    // --- end table header persistence ---

    void updateLegacyPartitionActions();

    void showBaselineStatus(const QString& msg);
    void showTransientStatus(const QString& msg, int timeoutMs);

    QAction* m_settingsAction = nullptr;
    QAction* m_changePartitionAction = nullptr;
    QAction* m_rescanPartitionAction = nullptr;

    bool m_useDaemonSearch = false;

    ScannerEngine::SearchDatabase db;
    QString m_fsType;
    QString m_mountPath;
    QString m_devicePath;

    QLineEdit *searchLine = nullptr;
    QComboBox* m_deviceScopeCombo = nullptr;
    QString m_selectedDeviceScopeId; // empty = all devices

    QTableView *tableView = nullptr;

    FileModel *model = nullptr; // local model
    RemoteFileModel *remoteModel = nullptr; // daemon model

    QLabel *daemonStatusLabel = nullptr;
    std::unique_ptr<DbusIndexerClient> m_dbus;

    QDBusServiceWatcher* m_daemonWatcher = nullptr;

    QTimer* m_indexUpdateDebounceTimer = nullptr;
    bool m_indexUpdatePending = false;

    int m_hoveredRow = -1;

    QString m_statusBaseline;
    quint64 m_statusMessageToken = 0;

    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // Used for *startup restore* only; the checkbox lives in SettingsDialog (QSettings).
    bool m_persistLastQuery = false;
    QString m_initialQueryText;

    QRect m_lastNormalGeometry;                 // last geometry when NOT maximized/fullscreen
    Qt::WindowStates m_lastWindowState = {};    // to detect transitions
    bool m_initialPlacementApplied = false;     // enforce geometry once after first show

    // Header save debounce + guard to avoid saving during restore
    QTimer* m_tableHeaderSaveDebounceTimer = nullptr;
    bool m_restoringTableHeaderState = false;
};

#endif //KERYTHING_MAINWINDOW_H