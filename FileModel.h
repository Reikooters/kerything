// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_FILEMODEL_H
#define KERYTHING_FILEMODEL_H

#include <QAbstractTableModel>
#include <vector>
#include "ScannerEngine.h"

/**
 * @brief The FileModel class provides a custom table model for displaying NTFS search results.
 *
 * It interacts with the ScannerEngine's SearchDatabase to present file information
 * (Name, Path, Size, Date) in a QTableView. It supports parallel sorting via TBB.
 */
class FileModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit FileModel(QObject *parent = nullptr);

    /**
     * @brief Updates the model with a new set of search results.
     * @param newResults A vector of record indices pointing into the database.
     * @param db A pointer to the search database containing file metadata.
     * @param mountPath The base path where the scanned partition is mounted.
     */
    void setResults(std::vector<uint32_t> newResults, const ScannerEngine::SearchDatabase* db, QString mountPath);

    /**
     * @brief Sorts the search results based on the specified column and order.
     * Uses C++17 parallel algorithms (TBB) for high performance on large datasets.
     */
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    /**
     * @brief Returns the labels for the table headers.
     */
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    /**
     * @brief Provides data for the view, including text display and file/folder icons.
     */
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    /**
     * @brief Returns the raw database index for a given model row.
     * Useful for looking up full file details when an item is clicked/opened.
     */
    uint32_t getRecordIndex(int row) const;

    /**
     * @brief Returns the item flags for a given index.
     * In addition to default flags, we enable Drag support for valid items.
     */
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    /**
     * @brief Returns the MIME types supported by this model for drag-and-drop.
     * We use text/uri-list which is the standard format for file transfers on Linux.
     */
    QStringList mimeTypes() const override;

    /**
     * @brief Packages the data for the selected rows into a QMimeData object.
     * This is called by the view when a drag operation begins.
     */
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    std::vector<uint32_t> m_results; // The record indices from the trigram search
    const ScannerEngine::SearchDatabase* m_db = nullptr;
    QString m_mountPath;
};

#endif //KERYTHING_FILEMODEL_H