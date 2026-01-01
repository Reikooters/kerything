// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QVBoxLayout>
#include <QHeaderView>
#include <QStatusBar>
#include <QDesktopServices>
#include <QDir>
#include <QMessageBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMimeData>
#include <QClipboard>
#include <QShortcut>
#include <QProcess>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QMimeType>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KFileItem>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/CommandLauncherJob>
#include <KTerminalLauncherJob>
#include <KService>
#include <KApplicationTrader>
#include <chrono>
#include <algorithm>
#include <numeric>
#include "MainWindow.h"
#include "FileModel.h"

MainWindow::MainWindow(ScannerEngine::SearchDatabase&& database, QString mountPath)
    : db(std::move(database)), m_mountPath(std::move(mountPath)) {
    auto *centralWidget = new QWidget(this);
    auto *layout = new QVBoxLayout(centralWidget);

    searchLine = new QLineEdit(centralWidget);
    searchLine->setPlaceholderText("Search files...");
    searchLine->setClearButtonEnabled(true);

    // Add magnifying glass icon to the search bar
    searchLine->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);

    layout->addWidget(searchLine);

    tableView = new QTableView(centralWidget);
    model = new FileModel(this);
    tableView->setModel(model);

    // Enable Sorting
    tableView->setSortingEnabled(true);
    tableView->horizontalHeader()->setSortIndicatorShown(true);

    // Table Styling
    tableView->setAlternatingRowColors(true);
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView->verticalHeader()->setVisible(false);

    // Allow resizing and horizontal scrolling
    tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    tableView->horizontalHeader()->setStretchLastSection(true);
    tableView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Set reasonable default column widths
    tableView->setColumnWidth(0, 375); // Name
    tableView->setColumnWidth(1, 525); // Path
    tableView->setColumnWidth(2, 100); // Size
    // Column 3 (Date) will take the remaining space due to stretchLastSection

    layout->addWidget(tableView);

    // --- Action State Management ---
    auto updateActionStates = [this]() {
        QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();
        int count = selectedRows.count();
        bool isMounted = !m_mountPath.isEmpty();

        // Open: Enabled if mounted and something is selected
        QAction* openAction = findChild<QAction*>("openAction");
        if (openAction) {
            openAction->setEnabled(isMounted && count > 0);
            openAction->setText(count == 1 ? "Open" : "Open " + QString::number(count) + " Files");
        }

        // Open Location & Terminal: Only for single selection
        QAction* openLocAction = findChild<QAction*>("openLocationAction");
        if (openLocAction) {
            openLocAction->setEnabled(isMounted && count == 1);
        }

        QAction* openTerminalAction = findChild<QAction*>("openTerminalAction");
        if (openTerminalAction) {
            openTerminalAction->setEnabled(isMounted && count == 1);
        }

        // Copy Actions: Enabled if something is selected
        QAction* copyFilesAction = findChild<QAction*>("copyFilesAction");
        if (copyFilesAction) {
            copyFilesAction->setEnabled(isMounted && count > 0);
            copyFilesAction->setText(count == 1 ? "Copy File" : "Copy " + QString::number(count) + " Files");
        }

        QAction* copyPathsAction = findChild<QAction*>("copyPathsAction");
        if (copyPathsAction) {
            copyPathsAction->setEnabled(count > 0);
            copyPathsAction->setText(count == 1 ? "Copy Full Path" : "Copy Full Paths");
        }
    };

    // Trigger update whenever selection changes
    connect(tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, updateActionStates);

    // Also trigger it when the search results change (model reset)
    connect(tableView->model(), &QAbstractItemModel::modelReset, this, updateActionStates);

    // Status Bar
    statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel);

    setCentralWidget(centralWidget);
    resize(1200, 800);

    // Connect search bar to our search logic
    connect(searchLine, &QLineEdit::textChanged, this, &MainWindow::updateSearch);

    // --- Keyboard Navigation (Search Bar focus logic) ---
    // Arrow Up/Down in search line moves focus to table
    // We set the context to Qt::WidgetShortcut so it only triggers when the searchLine has focus
    auto *downToTable = new QShortcut(QKeySequence(Qt::Key_Down), searchLine);
    auto *upToTable = new QShortcut(QKeySequence(Qt::Key_Up), searchLine);
    downToTable->setContext(Qt::WidgetShortcut);
    upToTable->setContext(Qt::WidgetShortcut);

    auto focusTable = [this]() {
        tableView->setFocus();
        if (tableView->currentIndex().row() < 0 && model->rowCount() > 0) {
            tableView->setCurrentIndex(model->index(0, 0));
        }
    };
    connect(downToTable, &QShortcut::activated, focusTable);
    connect(upToTable, &QShortcut::activated, focusTable);
    // ---------------------

    // --- Global Window Actions (Shortcuts + Menu items) ---
    // Ctrl+L: Focus Search
    auto *focusSearchAct = new QAction(this);
    focusSearchAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    connect(focusSearchAct, &QAction::triggered, searchLine, [this]() {
        searchLine->setFocus();
        searchLine->selectAll();
    });
    addAction(focusSearchAct);

    // Enter: Open
    auto *openAct = new QAction(QIcon::fromTheme("system-run"), "Open", this);
    openAct->setShortcut(QKeySequence(Qt::Key_Return));
    openAct->setObjectName("openAction");
    connect(openAct, &QAction::triggered, this, &MainWindow::openSelectedFiles);
    addAction(openAct);

    // Ctrl+Enter: Open File Location
    auto *openLocAct = new QAction(QIcon::fromTheme("folder-open"), "Open File Location", this);
    openLocAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    openLocAct->setObjectName("openLocationAction");
    connect(openLocAct, &QAction::triggered, this, &MainWindow::openSelectedLocation);
    addAction(openLocAct);

    // Ctrl+C: Copy Files
    auto *copyFilesAct = new QAction(QIcon::fromTheme("edit-copy"), "Copy", this);
    copyFilesAct->setShortcut(QKeySequence::Copy);
    copyFilesAct->setObjectName("copyFilesAction");
    connect(copyFilesAct, &QAction::triggered, this, &MainWindow::copyFiles);
    addAction(copyFilesAct);

    // Ctrl+Alt+C: Copy Full Path
    auto *copyPathsAct = new QAction(QIcon::fromTheme("edit-copy-path"), "Copy Full Path", this);
    copyPathsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    copyPathsAct->setObjectName("copyPathsAction");
    connect(copyPathsAct, &QAction::triggered, this, &MainWindow::copyPaths);
    addAction(copyPathsAct);

    // Alt+Shift+F4: Open Terminal
    auto *terminalAct = new QAction(QIcon::fromTheme("utilities-terminal"), "Open Terminal Here", this);
    terminalAct->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F4));
    terminalAct->setObjectName("openTerminalAction");
    connect(terminalAct, &QAction::triggered, this, &MainWindow::openTerminal);
    addAction(terminalAct);
    // ---------------------

    // Handle double-click on item in table view to open file
    connect(tableView, &QTableView::doubleClicked, this, &MainWindow::openFile);

    // Start with a full list, sorted by name ascending
    tableView->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    updateSearch("");
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event) {
    // Map the position correctly to the viewport
    // This ensures the row index is perfectly aligned with the mouse
    QPoint viewportPos = tableView->viewport()->mapFrom(this, event->pos());
    QModelIndex clickIndex = tableView->indexAt(viewportPos);

    // If user clicks empty space, don't show the full file menu
    if (!clickIndex.isValid()) {
        return;
    }

    // If the user right-clicks an item that ISN'T selected,
    // select it and clear the old selection (standard file manager behavior).
    if (!tableView->selectionModel()->isSelected(clickIndex)) {
        tableView->setCurrentIndex(clickIndex);
    }

    QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();
    bool isMounted = !m_mountPath.isEmpty();

    QMenu menu(this);
    KFileItemActions menuActions;

    if (isMounted) {
        KFileItemList items;

        // Process all selected items
        for (const QModelIndex &index : selectedRows) {
            uint32_t recordIdx = model->getRecordIndex(index.row());
            const auto& rec = db.records[recordIdx];

            QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
            QString internalPath;
            auto it = db.directoryPaths.find(rec.parentRecordIdx);

            if (it != db.directoryPaths.end()) {
                internalPath = QString::fromStdString(it->second);
            }

            QString fullPath = QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName);

            QUrl url = QUrl::fromLocalFile(fullPath);

            // Explicitly create a KFileItem and determine MIME type
            // This is so the context menu will have the correct options
            KFileItem fileItem(url);
            fileItem.determineMimeType();
            items.append(fileItem);
        }

        menuActions.setItemListProperties(KFileItemListProperties(items));
        menuActions.insertOpenWithActionsTo(nullptr, &menu, QStringList());
    }

    // Add the actions we defined in the constructor
    // This automatically handles the text, icons, and showing the shortcuts

    // Add "Open" with grouping logic
    menu.addAction(findChild<QAction*>("openAction"));
    menu.addAction(findChild<QAction*>("openLocationAction"));
    menu.addAction(findChild<QAction*>("openTerminalAction"));

    menu.addSeparator();
    menu.addAction(findChild<QAction*>("copyFilesAction"));
    menu.addAction(findChild<QAction*>("copyPathsAction"));

    menu.addSeparator();

    // Add the KDE "Open With" and system actions
    if (isMounted) {
        menuActions.addActionsTo(&menu);
    } else {
        menu.addAction("Metadata/Actions unavailable (Unmounted)")->setEnabled(false);
    }

    menu.exec(event->globalPos());
}

void MainWindow::openFile(const QModelIndex &index) {
    if (!index.isValid()) {
        return;
    }

    // Handle unmounted drives
    if (m_mountPath.isEmpty()) {
        QMessageBox::information(this, "Drive Not Mounted",
            "This partition is not currently mounted in Linux."
            "Please mount it first then run this application again to open files.");
        return;
    }

    // Update the selection to just this item and trigger the selected files opener
    tableView->setCurrentIndex(index);
    openSelectedFiles();
}

void MainWindow::openSelectedFiles() {
    // Handle unmounted drives
    if (m_mountPath.isEmpty()) {
        QMessageBox::information(this, "Drive Not Mounted",
            "This partition is not currently mounted in Linux."
            "Please mount it first then run this application again to open files.");
        return;
    }

    QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    // If nothing is selected, don't open anything
    if (selectedRows.isEmpty()) {
        return;
    }

    // Group URLs by their MIME type
    QMimeDatabase mimeDb;
    QMap<QString, QList<QUrl>> mimeGroups;

    for (const QModelIndex &index : selectedRows) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];

        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        QString internalPath;
        auto it = db.directoryPaths.find(rec.parentRecordIdx);
        if (it != db.directoryPaths.end()) {
            internalPath = QString::fromStdString(it->second);
        }

        QString fullPath = QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName);
        QUrl url = QUrl::fromLocalFile(fullPath);

        QString mimeType = mimeDb.mimeTypeForUrl(url).name();
        mimeGroups[mimeType].append(url);
    }

    // Launch one job per MIME group using its preferred service
    for (auto it = mimeGroups.begin(); it != mimeGroups.end(); ++it) {
        const QString &mimeType = it.key();
        const QList<QUrl> &urls = it.value();

        KService::Ptr service = KApplicationTrader::preferredService(mimeType);

        auto *job = service ? new KIO::ApplicationLauncherJob(service)
                            : new KIO::ApplicationLauncherJob();

        job->setUrls(urls);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
        job->start();
    }
}

void MainWindow::openSelectedLocation() {
    QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    uint32_t recordIdx = model->getRecordIndex(selectedRows.first().row());
    const auto& rec = db.records[recordIdx];
    auto it = db.directoryPaths.find(rec.parentRecordIdx);
    QString internalPath = (it != db.directoryPaths.end()) ? QString::fromStdString(it->second) : "";

    QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::cleanPath(m_mountPath + "/" + internalPath)));
}

void MainWindow::copyPaths() {
    QStringList paths;

    for (const QModelIndex &index : tableView->selectionModel()->selectedRows()) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];
        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        auto it = db.directoryPaths.find(rec.parentRecordIdx);
        QString internalPath = (it != db.directoryPaths.end()) ? QString::fromStdString(it->second) : "";
        paths.append(QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName));
    }

    QGuiApplication::clipboard()->setText(paths.join('\n'));
}

void MainWindow::copyFiles() {
    QList<QUrl> urls;

    for (const QModelIndex &index : tableView->selectionModel()->selectedRows()) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];
        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        auto it = db.directoryPaths.find(rec.parentRecordIdx);
        QString internalPath = (it != db.directoryPaths.end()) ? QString::fromStdString(it->second) : "";
        urls.append(QUrl::fromLocalFile(QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName)));
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);
    QGuiApplication::clipboard()->setMimeData(mimeData);
}

// void MainWindow::openTerminal() {
//     QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();
//     if (selectedRows.isEmpty()) return;
//
//     uint32_t recordIdx = model->getRecordIndex(selectedRows.first().row());
//     const auto& rec = db.records[recordIdx];
//     auto it = db.directoryPaths.find(rec.parentRecordIdx);
//     QString internalPath = (it != db.directoryPaths.end()) ? QString::fromStdString(it->second) : "";
//
//     QProcess::startDetached("konsole", {"--workdir", QDir::cleanPath(m_mountPath + "/" + internalPath)});
// }

void MainWindow::openTerminal() {
    QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    uint32_t recordIdx = model->getRecordIndex(selectedRows.first().row());
    const auto& rec = db.records[recordIdx];
    auto it = db.directoryPaths.find(rec.parentRecordIdx);
    QString internalPath = (it != db.directoryPaths.end()) ? QString::fromStdString(it->second) : "";
    QString fullDirPath = QDir::cleanPath(m_mountPath + "/" + internalPath);

    // KTerminalLauncherJob automatically finds the preferred terminal
    // and handles the command-line arguments to set the working directory.
    auto *job = new KTerminalLauncherJob(QString()); // Empty string means "default terminal"
    job->setWorkingDirectory(fullDirPath);

    // Provide a UI delegate for error reporting (e.g., if no terminal is found)
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));

    job->start();
}

bool MainWindow::contains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }

    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
        }
    );

    return it != haystack.end();
}

std::vector<uint32_t> MainWindow::performTrigramSearch(const std::string& query) {
    // 1. Tokenize query: "valley dragonforce" -> ["valley", "dragonforce"]
    // 2. For each word >= 3 chars, get candidate IDs from trigramIndex
    // 3. Intersect the ID lists (Candidate Filtering)
    // 4. For remaining candidates, do a case-insensitive sub-string check (Refinement)

    std::vector<uint32_t> results;

    // 1. Tokenize query by spaces
    std::vector<std::string> keywords;
    size_t start = 0, end = 0;

    while ((end = query.find(' ', start)) != std::string::npos) {
        if (end != start) {
            keywords.push_back(query.substr(start, end - start));
        }
        start = end + 1;
    }

    if (start < query.length()) {
        keywords.push_back(query.substr(start));
    }

    // IF EMPTY: Return everything
    if (keywords.empty()) {
        results.resize(db.records.size());
        std::iota(results.begin(), results.end(), 0);
        return results;
    }

    // 2. Candidate Filtering via Trigrams
    std::vector<uint32_t> candidates;
    bool firstKeyword = true;
    bool trigramsUsed = false; // Track if we actually used the index

    for (const auto& kw : keywords) {
        if (kw.length() < 3) {
            // Skip short words for now, as they won't be in our trigram index
            continue;
        }

        // Generate all trigrams for this keyword and intersect them
        for (size_t i = 0; i <= kw.length() - 3; ++i) {
            trigramsUsed = true;
            uint32_t tri = (static_cast<uint32_t>(std::tolower(kw[i])) << 16) |
                           (static_cast<uint32_t>(std::tolower(kw[i+1])) << 8) |
                           (static_cast<uint32_t>(std::tolower(kw[i+2])));

            // Binary search for the trigram in the flat index
            auto range = std::equal_range(db.flatIndex.begin(), db.flatIndex.end(),
                                                  ScannerEngine::TrigramEntry{tri, 0},
                                                  [](const auto& a, const auto& b) { return a.trigram < b.trigram; });

            if (range.first == range.second) {
                // No matches for this trigram
                return results;
            }

            // 3. Intersect candidates (Candidate Filtering)
            if (firstKeyword) {
                // First trigram: populate candidates directly from the range
                candidates.reserve(std::distance(range.first, range.second));

                for (auto it = range.first; it != range.second; ++it) {
                    candidates.push_back(it->recordIdx);
                }

                firstKeyword = false;
            } else {
                // Subsequent trigrams: intersect existing candidates with the range
                std::vector<uint32_t> nextCandidates;
                nextCandidates.reserve(std::min(candidates.size(), static_cast<size_t>(std::distance(range.first, range.second))));

                // Custom intersection that works between a vector<uint32_t> and a range of TrigramEntry
                auto candIt = candidates.begin();
                auto rangeIt = range.first;

                while (candIt != candidates.end() && rangeIt != range.second) {
                    if (*candIt < rangeIt->recordIdx) {
                        ++candIt;
                    } else if (rangeIt->recordIdx < *candIt) {
                        ++rangeIt;
                    } else {
                        nextCandidates.push_back(*candIt);
                        ++candIt;
                        ++rangeIt;
                    }
                }

                candidates = std::move(nextCandidates);
            }

            if (candidates.empty()) {
                return results;
            }
        }
    }

    // 4. Refinement Phase
    // If no trigrams were used (all keywords < 3 chars), we scan everything.
    // Otherwise, we only scan the filtered candidates.
    auto resultCallback = [&](uint32_t recordIdx) {
        const auto& rec = db.records[recordIdx];
        std::string_view name(&db.stringPool[rec.nameOffset], rec.nameLen);

        for (const auto& kw : keywords) {
            if (!contains(name, kw)) {
                return;
            }
        }

        results.push_back(recordIdx);
    };

    if (!trigramsUsed) {
        // Fallback: Linear scan of all records (All keywords were too short)
        for (uint32_t i = 0; i < static_cast<uint32_t>(db.records.size()); ++i) {
            resultCallback(i);
        }
    } else {
        // High-speed scan of candidates
        for (uint32_t idx : candidates) {
            resultCallback(idx);
        }
    }

    return results;
}

void MainWindow::updateSearch(const QString &text) {
    auto start = std::chrono::steady_clock::now();

    auto results = performTrigramSearch(text.toStdString());
    model->setResults(std::move(results), &db);

    int sortCol = tableView->horizontalHeader()->sortIndicatorSection();
    model->sort(sortCol, tableView->horizontalHeader()->sortIndicatorOrder());

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    // Update status bar
    statusLabel->setText(QString("%L1 objects found in %2s")
        .arg(model->rowCount())
        .arg(elapsed.count(), 0, 'f', 4));
}