// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_MAINWINDOW_H
#define KERYTHING_MAINWINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QTableView>
#include <QLabel>
#include <QString>
#include <vector>
#include <string>
#include "ScannerEngine.h"

class FileModel;

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

private slots:
    /**
     * @brief Tracks which row is currently hovered so we can paint a full-row hover highlight.
     */
    void onTableHovered(const QModelIndex& index);

    /**
     * @brief Triggered when the search text changes. Performs a trigram search and updates the view.
     */
    void updateSearch(const QString &text);

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

    ScannerEngine::SearchDatabase db;
    QString m_fsType;
    QString m_mountPath;
    QString m_devicePath;
    QLineEdit *searchLine;
    QTableView *tableView;
    FileModel *model;
    QLabel *statusLabel;

    int m_hoveredRow = -1;
};

#endif //KERYTHING_MAINWINDOW_H