// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_MAINWINDOW_H
#define KERYTHING_MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QLineEdit>
#include <QTableView>
#include <QString>
#include <vector>
#include <string>

#include "FileModel.h"

class AppController;

/**
 * @brief The main application window for searching and viewing files.
 */
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /**
     * @brief Constructs the MainWindow.
     */
    explicit MainWindow(AppController* controller, QWidget* parent = nullptr);

    /**
     * @brief Updates the current database and UI.
     */
    // void setDatabase(IndexController::DeviceIndex&& database, QString mountPath, QString devicePath, const QString& fsType);

    [[nodiscard]] int hoveredRow() const { return hoveredRow_; }

    void refresh();
    void refreshLiveMetadata();
    void markLiveStructuralRefreshDirty();
    void markLiveMetadataRefreshDirty();
    void showTemporaryStatus(const QString& text, int timeoutMs);

protected:
    /**
     * @brief Handles right-click events to show the file context menu.
     */
    void contextMenuEvent(QContextMenuEvent *event) override;

    void changeEvent(QEvent* event) override;

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
    // bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    /**
     * @brief Tracks which row is currently hovered so we can paint a full-row hover highlight.
     */
    // void onTableHovered(const QModelIndex& index);

    /**
     * @brief Tracks when the empty area of the table is hovered (below the last item in the list)
     */
    // void onTableViewportHovered();

    /**
     * @brief Triggered when the search text changes. Performs a trigram search and updates the view.
     */
    void updateSearch(const QString &text);

    /**
     * @brief Shows a placeholder for the change partition logic.
     */
    // void changePartition();

    /**
     * @brief Shows a placeholder for the rescan partition logic.
     */
    // void rescanPartition();

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
     * @brief Copies the parent paths of selected items to the clipboard.
     */
    void copyParentPaths();

    /**
     * @brief Copies the selected files themselves to the clipboard (for pasting in Dolphin).
     */
    void copyFiles();

    /**
     * @brief Opens a terminal in the folder of the selected item.
     */
    void openTerminal();

private:
    void showUnavailableSelectionStatus(qsizetype selectedCount, qsizetype mountedCount, const QString& actionText);
    void showSkippedUnmountedStatus(qsizetype attemptedCount, qsizetype completedCount, const QString& actionText);
    QString actionTextForOpenableCount(
        const QString& singularText,
        const QString& singularCountedText,
        const QString& pluralCountedText,
        qsizetype selectedCount,
        qsizetype openableCount
    );
    [[nodiscard]] std::vector<IndexController::RecordHandle> captureSelectedRecordHandles() const;
    [[nodiscard]] std::optional<IndexController::RecordHandle> captureCurrentRecordHandle() const;
    void restoreSelectedRecordHandles(
        const std::vector<IndexController::RecordHandle>& selectedHandles,
        const std::optional<IndexController::RecordHandle>& currentHandle
    );
    void rebuildFilterMenu();
    void applySearchFilter(const QString& filterId, const QString& filterName, const QString& queryFragment);
    void updateSearchLineFilterHint();

    AppController* controller_ = nullptr;
    QLineEdit *searchLine_ = nullptr;
    QTableView *tableView_ = nullptr;
    FileModel *model_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    quint64 statusMessageId_ = 0;
    QMenu* filterMenu_ = nullptr;
    QAction* autoRefreshLiveUpdatesAct_ = nullptr;

    QString activeSearchFilterId_;
    QString activeSearchFilterName_;
    QString activeSearchFilter_;
    bool liveStructuralRefreshDirty_ = false;
    bool liveMetadataRefreshDirty_ = false;

    int hoveredRow_ = -1;
};

#endif // KERYTHING_MAINWINDOW_H