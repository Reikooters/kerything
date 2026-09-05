// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_FILEMODEL_H
#define KERYTHING_FILEMODEL_H

#include <QAbstractTableModel>
#include <vector>

#include "AppController.h"

/**
 * @brief The FileModel class provides a custom table model for displaying NTFS search results.
 *
 * It interacts with the ScannerEngine's SearchDatabase to present file information
 * (Name, Path, Size, Date) in a QTableView. It supports parallel sorting via TBB.
 */
class FileModel : public QAbstractTableModel {
    Q_OBJECT

public:
    static constexpr int HighlightTermsRole = Qt::UserRole + 100;
    static constexpr int HighlightMatchCaseRole = Qt::UserRole + 101;
    static constexpr int HighlightMatchWholeWordRole = Qt::UserRole + 102;
    static constexpr int HighlightUseRegexRole = Qt::UserRole + 103;

    explicit FileModel(AppController* controller, QObject *parent = nullptr);

    /**
     * @brief Configures the search highlight terms and their display options for the model.
     *
     * This method updates the internal state of the model with the provided search highlight terms,
     * whether highlighting is enabled, and whether the search is case-sensitive. If the new state
     * differs from the current state, it triggers the `dataChanged` signal to notify the view to refresh
     * the display of highlighted terms in the "Name" column.
     *
     * @param terms A list of search terms to highlight. Empty or duplicate terms are removed, and
     *              each term is stripped of surrounding whitespace.
     * @param enabled A boolean indicating whether highlighting of search terms is enabled or disabled.
     * @param matchCase A boolean indicating whether the search highlighting should be case-sensitive.
     * @param matchWholeWord A boolean indicating whether the search highlighting should match whole words only.
     */
    void setSearchHighlightTerms(
        QStringList terms,
        bool enabled,
        bool matchCase = false,
        bool matchWholeWord = false,
        bool useRegex = false
    );

    /**
     * @brief Updates the model with a new set of search results.
     * @param newResults A vector of record handles pointing into the database.
     */
    void setSearchResults(std::vector<IndexController::RecordHandle> newResults);

    /**
     * @brief Sorts the given results using the model's reusable scratch buffers,
     * then replaces the current model contents with one model reset.
     */
    void setSortedSearchResults(
        std::vector<IndexController::RecordHandle> newResults,
        int column,
        Qt::SortOrder order = Qt::AscendingOrder
    );

    /**
     * @brief Notifies the view that the data in the specified rows has changed.
     *
     * This method emits the `dataChanged` signal for the given range of rows,
     * ensuring the view updates the display for those rows. The method also
     * ensures that the range indices are clamped to valid bounds and that
     * notifications are only sent for meaningful ranges.
     *
     * @param firstRow The index of the first row to mark as changed.
     * @param lastRow The index of the last row to mark as changed.
     */
    void notifyRowsDataChanged(int firstRow, int lastRow);

    /**
     * @brief Releases reusable sort scratch buffers retained for large result sets.
     *
     * Useful after rescans or device removals, where the maximum possible result
     * count may have permanently decreased.
     */
    void trimSortScratch();

    /**
     * @brief Returns debug memory statistics for currently retained model buffers.
     */
    [[nodiscard]] QString memoryStatsText() const;

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
     * @brief Resolves a model row to a local file URL if the result is currently mounted.
     */
    [[nodiscard]] std::optional<QUrl> localUrlForRow(int row) const;

    /**
     * @brief Returns true if the row currently resolves to a mounted local path.
     */
    [[nodiscard]] bool isMountedRow(int row) const;

    /**
     * @brief Counts how many of the given rows currently resolve to mounted local paths.
     */
    [[nodiscard]] qsizetype mountedRowCount(const QModelIndexList& rows) const;

    /**
     * @brief Returns the stable search-result handle for a model row.
     */
    [[nodiscard]] std::optional<IndexController::RecordHandle> recordHandleForRow(int row) const;

    /**
     * @brief Finds the current row for a previously captured search-result handle.
     */
    [[nodiscard]] int rowForRecordHandle(const IndexController::RecordHandle& handle) const;

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
    void trimSearchResultsOverCapacity();

    AppController* controller_ = nullptr;
    std::vector<IndexController::RecordHandle> searchResults_;
    IndexController::SortScratch sortScratch_;
    QStringList searchHighlightTerms_;
    bool searchHighlightTermsEnabled_ = true;
    bool searchHighlightMatchCase_ = false;
    bool searchHighlightMatchWholeWord_ = false;
    bool searchHighlightUseRegex_ = false;
};

#endif //KERYTHING_FILEMODEL_H