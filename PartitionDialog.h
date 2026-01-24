// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PARTITIONDIALOG_H
#define KERYTHING_PARTITIONDIALOG_H

#include <QDialog>
#include <QString>
#include <QTreeWidget>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <vector>
#include <optional>
#include "ScannerEngine.h"
#include "ScannerManager.h"

/**
 * @brief Represents basic information about a detected disk partition.
 */
struct PartitionInfo {
    QString fsType;
    QString name;
    QString devicePath;
    QString mountPoint;
};

/**
 * @brief A dialog that allows users to select a partition and triggers the scanning process.
 */
class PartitionDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief Constructs the dialog and populates the list with available partitions.
     */
    explicit PartitionDialog(QWidget *parent = nullptr);

    /**
     * @brief Clears the list and re-scans for partitions.
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

    // /**
    //  * @brief Retrieves a list of partitions currently selected in the dialog.
    //  *
    //  * This method scans the partition list UI for selected items and compiles their
    //  * details into a list of PartitionInfo objects.
    //  *
    //  * @return A list of PartitionInfo objects representing the selected partitions.
    //  */
    // QList<PartitionInfo> getSelectedPartitions();

private:
    std::optional<ScannerEngine::SearchDatabase> m_scannedDb; ///< Storage for the database returned by the helper
    std::unique_ptr<ScannerManager> m_manager; ///< Helper process manager
    bool m_isHandlingClick = false; ///< Prevents re-entrant clicks
    QTreeWidget *treeWidget; ///< Table of detected partitions
    QLabel *statusLabel; ///< Label showing instructions or scan status
    QPushButton *startBtn; ///< Action button (Start Indexing / Cancel)
    QPushButton *refreshBtn; ///< Action button (Refresh)
    std::vector<PartitionInfo> partitions; ///< Internal metadata for the list items
    QProgressBar *progressBar; ///< Progress bar showing scanner progress
};

#endif //KERYTHING_PARTITIONDIALOG_H
