// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PARTITIONDIALOG_H
#define KERYTHING_PARTITIONDIALOG_H

#include <QDialog>
#include <QString>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <vector>
#include <optional>
#include "ScannerEngine.h"
#include "ScannerManager.h"

/**
 * @brief Represents basic information about a detected disk partition.
 */
struct PartitionInfo {
    QString name;
    QString devicePath;
    QString mountPoint;
};

/**
 * @brief A dialog that allows users to select an NTFS partition and triggers the MFT scanning process.
 */
class PartitionDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief Constructs the dialog and populates the list with available NTFS partitions.
     */
    explicit PartitionDialog(QWidget *parent = nullptr);

    /**
     * @brief Clears the list and re-scans for NTFS partitions.
     */
    void refreshPartitions();

    /**
     * @brief Handles the 'Start' button click. Triggers the scanning helper or requests cancellation if already running.
     */
    void onStartClicked();

    /**
     * @brief Transfers ownership of the scanned database out of the dialog.
     * @return An optional containing the database if scanning was successful.
     */
    std::optional<ScannerEngine::SearchDatabase> takeDatabase();

    /**
     * @brief Updates the UI state (buttons, labels, list) based on whether a scan is currently active.
     */
    void setScanning(bool scanning);

    /**
     * @brief Manually enables or disables the action button.
     */
    void setButtonEnabled(bool enabled) const;

    /**
     * @brief Returns the PartitionInfo for the currently selected item in the list.
     */
    PartitionInfo getSelected();

private:
    std::optional<ScannerEngine::SearchDatabase> m_scannedDb; ///< Storage for the database returned by the helper
    std::unique_ptr<ScannerManager> m_manager; ///< Helper process manager
    bool m_isHandlingClick = false; ///< Prevents re-entrant clicks
    QListWidget *listWidget; ///< List of detected NTFS partitions
    QLabel *statusLabel; ///< Label showing instructions or scan status
    QPushButton *startBtn; ///< Action button (Start Indexing / Cancel)
    QPushButton *refreshBtn; ///< Action button (Refresh)
    std::vector<PartitionInfo> partitions; ///< Internal metadata for the list items

    /**
     * @brief Executes a helper process to scan the specified device for partition information.
     *
     * This function invokes an external helper process using `pkexec` to perform a scan on the
     * provided device path. It handles data returned by the helper process, parses the results,
     * and builds the necessary data structures for the partition search database.
     *
     * @param devicePath The path to the device to be scanned (e.g., "/dev/sda").
     * @return An optional `ScannerEngine::SearchDatabase` containing the scan results. Returns
     *         `std::nullopt` if the scan fails or an error occurs during processing.
     */
    std::optional<ScannerEngine::SearchDatabase> scanViaHelper(const QString& devicePath);
};

#endif //KERYTHING_PARTITIONDIALOG_H
