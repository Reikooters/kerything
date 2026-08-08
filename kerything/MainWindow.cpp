// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "MainWindow.h"
#include "SearchResultTableView.h"

#include <algorithm>
#include <iostream>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelection>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QActionGroup>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#ifdef KERYTHING_WITH_KF6
#include <QMimeDatabase>
#include <QMimeType>

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KApplicationTrader>
#include <KFileItem>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenFileManagerWindowJob>
#include <KService>
#include <KTerminalLauncherJob>
#else
#include <QProcess>
#endif

#include "AppController.h"
#include "HoverRowHighlight.h"
#include "Version.h"

namespace {
    constexpr qsizetype OpenManyFilesConfirmationThreshold = 10;
}

MainWindow::MainWindow(AppController* controller, QWidget* parent)
    : QMainWindow(parent),
      controller_(controller) {
    setWindowTitle("Kerything");

    // Delete the QWidget object when the user closes the window.
    setAttribute(Qt::WA_DeleteOnClose);

    // Central widget that holds the window's main UI.
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);

    searchLine_ = new QLineEdit(centralWidget);
    searchLine_->setPlaceholderText("Search files...");
    searchLine_->setClearButtonEnabled(true);

    // Add magnifying glass icon to the search bar
    searchLine_->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);

    layout->addWidget(searchLine_);

    tableView_ = new SearchResultTableView(centralWidget);
    model_ = new FileModel(controller_, this);
    tableView_->setModel(model_);

    // Enable Sorting
    tableView_->setSortingEnabled(true);
    tableView_->horizontalHeader()->setSortIndicatorShown(true);

    // Table Styling
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->setWordWrap(false);
    installHoverRowHighlight(tableView_);

    // Full-row hover
    // tableView->setItemDelegate(new HoverRowDelegate(this));
    tableView_->setMouseTracking(true);
    tableView_->viewport()->setMouseTracking(true);
    // connect(tableView, &QAbstractItemView::entered, this, &MainWindow::onTableHovered);
    // connect(tableView, &QAbstractItemView::viewportEntered, this, &MainWindow::onTableViewportHovered);
    tableView_->viewport()->installEventFilter(this);

    // --- Drag and Drop Configuration ---
    // setDragEnabled(true) tells the view to start a drag if the user moves the
    // mouse while pressing the left button on a selected item.
    tableView_->setDragEnabled(true);

    // DragOnly means we can drag items out, but the application doesn't accept drops.
    tableView_->setDragDropMode(QAbstractItemView::DragOnly);

    // Setting the default action to CopyAction signals to the OS
    // that we want to share/copy the data, which helps the Portal
    // decide to grant permission.
    tableView_->setDefaultDropAction(Qt::CopyAction);
    // ---------------------

    // Allow resizing and horizontal scrolling
    tableView_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    tableView_->horizontalHeader()->setStretchLastSection(true);
    tableView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Set reasonable default column widths
    tableView_->setColumnWidth(0, 375); // Name
    tableView_->setColumnWidth(1, 525); // Path
    tableView_->setColumnWidth(2, 100); // Size
    // Column 3 (Date) will take the remaining space due to stretchLastSection

    layout->addWidget(tableView_);

    // --- Action State Management ---
    auto updateActionStates = [this]() {
        const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();
        const qsizetype count = selectedRows.count();
        const qsizetype mountedCount = model_ ? model_->mountedRowCount(selectedRows) : 0;
        const bool hasMountedSelection = mountedCount > 0;

        // Open: enabled if at least one selected item is currently mounted.
        QAction* openAction = findChild<QAction*>("openAction");
        if (openAction) {
            openAction->setEnabled(hasMountedSelection);
            openAction->setText(actionTextForOpenableCount(
                QStringLiteral("Open"),
                QStringLiteral("Open 1 File"),
                QStringLiteral("Open %1 Files"),
                count,
                mountedCount
            ));
            openAction->setStatusTip(
                hasMountedSelection
                    ? QString()
                    : QStringLiteral("Device is not mounted. Mount the device to open this item.")
            );
        }

        // Open Location & Terminal: only for single mounted selection.
        QAction* openLocAction = findChild<QAction*>("openLocationAction");
        if (openLocAction) {
            openLocAction->setEnabled(count == 1 && hasMountedSelection);
            openLocAction->setStatusTip(
                count == 1 && !hasMountedSelection
                    ? QStringLiteral("Device is not mounted. Mount the device to open its containing folder.")
                    : QString()
            );
        }

        QAction* openTerminalAction = findChild<QAction*>("openTerminalAction");
        if (openTerminalAction) {
            openTerminalAction->setEnabled(count == 1 && hasMountedSelection);
            openTerminalAction->setStatusTip(
                count == 1 && !hasMountedSelection
                    ? QStringLiteral("Device is not mounted. Mount the device to open a terminal there.")
                    : QString()
            );
        }

        // Copy Actions: Enabled if something is selected
        QAction* copyFilesAction = findChild<QAction*>("copyFilesAction");
        if (copyFilesAction) {
            copyFilesAction->setEnabled(hasMountedSelection);
            copyFilesAction->setText(actionTextForOpenableCount(
                QStringLiteral("Copy File"),
                QStringLiteral("Copy 1 File"),
                QStringLiteral("Copy %1 Files"),
                count,
                mountedCount
            ));
            copyFilesAction->setStatusTip(
                hasMountedSelection
                    ? QString()
                    : QStringLiteral("Device is not mounted. Mount the device to copy files.")
            );
        }

        QAction* copyFileNamesAction = findChild<QAction*>("copyFileNamesAction");
        if (copyFileNamesAction) {
            copyFileNamesAction->setEnabled(count > 0);
            copyFileNamesAction->setText(count <= 1 ? "Copy File Name" : "Copy " + QString::number(count) + " File Names");
        }

        QAction* copyPathsAction = findChild<QAction*>("copyPathsAction");
        if (copyPathsAction) {
            copyPathsAction->setEnabled(count > 0);
            copyPathsAction->setText(count <= 1 ? "Copy Full Path" : "Copy " + QString::number(count) + " Full Paths");
        }

        QAction* copyParentPathsAction = findChild<QAction*>(QStringLiteral("copyParentPathsAction"));
        if (copyParentPathsAction) {
            copyParentPathsAction->setEnabled(count > 0);
            copyParentPathsAction->setText(
                count <= 1
                    ? QStringLiteral("Copy Parent Path")
                    : QStringLiteral("Copy %1 Parent Paths").arg(count)
            );
        }
    };

    // Trigger update whenever selection changes
    connect(tableView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, updateActionStates);

    // Also trigger it when the search results change (model reset)
    connect(tableView_->model(), &QAbstractItemModel::modelReset, this, updateActionStates);
    // ---------------------

    // Status Bar
    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);

    setCentralWidget(centralWidget);
    resize(1200, 800);

    // Connect search bar to our search logic
    connect(searchLine_, &QLineEdit::textChanged, this, &MainWindow::updateSearch);

    // --- Keyboard Navigation (Search Bar focus logic) ---
    // Arrow Up/Down in search line moves focus to table
    // We set the context to Qt::WidgetShortcut so it only triggers when the searchLine has focus
    auto *downToTable = new QShortcut(QKeySequence(Qt::Key_Down), searchLine_);
    auto *upToTable = new QShortcut(QKeySequence(Qt::Key_Up), searchLine_);
    downToTable->setContext(Qt::WidgetShortcut);
    upToTable->setContext(Qt::WidgetShortcut);

    auto focusTable = [this]() {
        tableView_->setFocus();
        if (tableView_->currentIndex().row() < 0 && model_->rowCount() > 0) {
            tableView_->setCurrentIndex(model_->index(0, 0));
        }
    };
    connect(downToTable, &QShortcut::activated, focusTable);
    connect(upToTable, &QShortcut::activated, focusTable);

    auto clearSearchOnly = [this]() {
        searchLine_->clear();
    };

    auto clearSearchAndFocus = [this]() {
        searchLine_->clear();
        searchLine_->setFocus();
    };

    // Escape in the search line clears the search.
    auto *clearSearch = new QShortcut(QKeySequence(Qt::Key_Escape), searchLine_);
    clearSearch->setContext(Qt::WidgetShortcut);
    connect(clearSearch, &QShortcut::activated, this, clearSearchOnly);

    // Escape in the results list clears the search and returns focus to the search line.
    auto *clearSearchFromTable = new QShortcut(QKeySequence(Qt::Key_Escape), tableView_);
    clearSearchFromTable->setContext(Qt::WidgetShortcut);
    connect(clearSearchFromTable, &QShortcut::activated, this, clearSearchAndFocus);
    // ---------------------

    // --- Global Window Actions (Shortcuts + Menu items) ---

    // New Window
    auto *newWindowAct = new QAction(QIcon::fromTheme("window-new"), "New Window", this);
    newWindowAct->setShortcut(QKeySequence::New);
    connect(newWindowAct, &QAction::triggered, this, [this]() {
        if (controller_) {
            controller_->openNewWindow();
        }
    });
    addAction(newWindowAct);

    // Close Window
    auto *closeWindowAct = new QAction(QIcon::fromTheme("window-close"), "Close Window", this);
    closeWindowAct->setShortcut(QKeySequence::Close);
    connect(closeWindowAct, &QAction::triggered, this, &QWidget::close);
    addAction(closeWindowAct);

    // Quit Kerything
    auto *quitAct = new QAction(QIcon::fromTheme("application-exit"), "Quit Kerything", this);
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, qApp, &QCoreApplication::quit);
    addAction(quitAct);

    // Configure Kerything
    auto* configureAct = new QAction(QIcon::fromTheme("configure"), QStringLiteral("Configure Kerything..."), this);
    configureAct->setShortcut(QKeySequence::Preferences);
    connect(configureAct, &QAction::triggered, this, [this]() {
        if (controller_) {
            controller_->showPreferencesDialog();
        }
    });
    addAction(configureAct);

    // Refresh Indexes
    auto* refreshIndexesAct = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Refresh Indexes"), this);
    refreshIndexesAct->setShortcut(QKeySequence(Qt::Key_F5));
    refreshIndexesAct->setStatusTip(QStringLiteral("Refresh indexes for all enabled devices"));
    connect(refreshIndexesAct, &QAction::triggered, this, [this]() {
        if (controller_) {
            controller_->refreshIndexes();
        }
    });
    addAction(refreshIndexesAct);

    // About Kerything
    auto *aboutAct = new QAction(QIcon::fromTheme("kerything"), "About Kerything", this);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);

    // Ctrl+F, Ctrl+L and Alt+D: Focus Search
    auto *focusSearchAct = new QAction(this);
    focusSearchAct->setShortcuts({
        QKeySequence::Find,
        QKeySequence(Qt::CTRL | Qt::Key_L),
        QKeySequence(Qt::ALT | Qt::Key_D)
    });
    connect(focusSearchAct, &QAction::triggered, searchLine_, [this]() {
        searchLine_->setFocus();
        searchLine_->selectAll();
    });
    addAction(focusSearchAct);

    // Enter: Open
    auto *openAct = new QAction(QIcon::fromTheme("system-run"), "Open", this);
    openAct->setShortcuts({
        QKeySequence(Qt::Key_Return),
        QKeySequence(Qt::Key_Enter)
    });
    openAct->setObjectName("openAction");
    connect(openAct, &QAction::triggered, this, &MainWindow::openSelectedFiles);
    addAction(openAct);

    // Ctrl+Enter: Show in File Manager
    auto *showInFileManagerAct = new QAction(QIcon::fromTheme("folder-open"), "Show in File Manager", this);
    showInFileManagerAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    showInFileManagerAct->setObjectName("openLocationAction");
    connect(showInFileManagerAct, &QAction::triggered, this, &MainWindow::openSelectedLocation);
    addAction(showInFileManagerAct);

    // Ctrl+C: Copy Files
    auto *copyFilesAct = new QAction(QIcon::fromTheme("edit-copy"), "Copy File", this);
    copyFilesAct->setShortcut(QKeySequence::Copy);
    copyFilesAct->setObjectName("copyFilesAction");
    connect(copyFilesAct, &QAction::triggered, this, &MainWindow::copyFiles);
    addAction(copyFilesAct);

    // Ctrl+Shift+C: Copy File Names
    auto *copyFileNamesAct = new QAction(QIcon::fromTheme("edit-copy"), "Copy File Name", this);
    copyFileNamesAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyFileNamesAct->setObjectName("copyFileNamesAction");
    connect(copyFileNamesAct, &QAction::triggered, this, &MainWindow::copyFileNames);
    addAction(copyFileNamesAct);

    // Ctrl+Alt+C: Copy Full Paths
    auto *copyPathsAct = new QAction(QIcon::fromTheme("edit-copy-path"), "Copy Full Path", this);
    copyPathsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    copyPathsAct->setObjectName("copyPathsAction");
    connect(copyPathsAct, &QAction::triggered, this, &MainWindow::copyPaths);
    addAction(copyPathsAct);

    // Copy Parent Paths
    auto *copyParentPathsAct = new QAction(QIcon::fromTheme("edit-copy-path"), "Copy Parent Path", this);
    copyParentPathsAct->setObjectName("copyParentPathsAction");
    connect(copyParentPathsAct, &QAction::triggered, this, &MainWindow::copyParentPaths);
    addAction(copyParentPathsAct);

    // Alt+Shift+F4: Open Terminal
    auto *terminalAct = new QAction(QIcon::fromTheme("utilities-terminal"), "Open Terminal Here", this);
    terminalAct->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F4));
    terminalAct->setObjectName("openTerminalAction");
    connect(terminalAct, &QAction::triggered, this, &MainWindow::openTerminal);
    addAction(terminalAct);

    // File Menu
    auto* fileMenu = menuBar()->addMenu("File");
    fileMenu->addAction(newWindowAct);
    fileMenu->addAction(closeWindowAct);
    fileMenu->addSeparator();
    fileMenu->addAction(openAct);
    fileMenu->addAction(showInFileManagerAct);
    fileMenu->addAction(terminalAct);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAct);

    // Edit Menu
    auto* editMenu = menuBar()->addMenu("Edit");
    editMenu->addAction(copyFilesAct);
    editMenu->addAction(copyFileNamesAct);
    editMenu->addAction(copyPathsAct);
    editMenu->addAction(copyParentPathsAct);

    // Filter Menu
    filterMenu_ = menuBar()->addMenu(QStringLiteral("Filter"));
    rebuildFilterMenu();

    // Index Menu
    auto* indexMenu = menuBar()->addMenu(QStringLiteral("Index"));
    indexMenu->addAction(refreshIndexesAct);

    // Settings Menu
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("Settings"));
    settingsMenu->addAction(configureAct);

    // Help Menu
    auto* helpMenu = menuBar()->addMenu("Help");
    helpMenu->addAction(aboutAct);
    // ---------------------

    if (controller_) {
        connect(controller_, &AppController::searchFiltersChanged, this, [this]() {
            rebuildFilterMenu();
            updateSearch(searchLine_->text());
        });
    }

    updateActionStates();

    // Handle double-click on item in table view to open file
    connect(tableView_, &SearchResultTableView::doubleClicked, this, &MainWindow::openFile);

    // Start with a full list, sorted by name ascending
    tableView_->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    updateSearch("");
}

void MainWindow::updateSearch(const QString &text) {
    auto start1 = std::chrono::steady_clock::now();

    const std::vector<IndexController::RecordHandle> selectedHandles =
        captureSelectedRecordHandles();
    const std::optional<IndexController::RecordHandle> currentHandle =
        captureCurrentRecordHandle();

    QString effectiveQuery = text;

    if (!activeSearchFilter_.isEmpty()) {
        if (!effectiveQuery.trimmed().isEmpty()) {
            effectiveQuery += QLatin1Char(' ');
        }

        effectiveQuery += activeSearchFilter_;
    }

    auto results = controller_->indexController()->performTrigramSearch(effectiveQuery.toStdString());
    model_->setSearchResults(std::move(results));
    auto end1 = std::chrono::steady_clock::now();

    auto start2 = std::chrono::steady_clock::now();
    int sortCol = tableView_->horizontalHeader()->sortIndicatorSection();
    model_->sort(sortCol, tableView_->horizontalHeader()->sortIndicatorOrder());
    restoreSelectedRecordHandles(selectedHandles, currentHandle);
    auto end2 = std::chrono::steady_clock::now();

    std::chrono::duration<double> elapsed1 = end1 - start1;
    std::chrono::duration<double> elapsed2 = end2 - start2;
    std::chrono::duration<double> elapsed = end2 - start1;

    // Update status bar
    statusBar()->showMessage(QString("%L1 objects found in %2s (search: %3s, sort: %4s)")
        .arg(model_->rowCount())
        .arg(elapsed.count(), 0, 'f', 4)
        .arg(elapsed1.count(), 0, 'f', 4)
        .arg(elapsed2.count(), 0, 'f', 4));
}

void MainWindow::refresh() {
    liveStructuralRefreshDirty_ = false;
    liveMetadataRefreshDirty_ = false;

    updateSearch(searchLine_->text());
}

void MainWindow::refreshLiveMetadata()
{
    liveMetadataRefreshDirty_ = false;

    if (!model_ || !tableView_ || model_->rowCount() <= 0) {
        return;
    }

    const int sortColumn = tableView_->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = tableView_->horizontalHeader()->sortIndicatorOrder();

    if (sortColumn == 2 || sortColumn == 3) {
        const std::vector<IndexController::RecordHandle> selectedHandles =
            captureSelectedRecordHandles();
        const std::optional<IndexController::RecordHandle> currentHandle =
            captureCurrentRecordHandle();

        // Metadata-only updates can affect Size and Modified Date ordering, but
        // they do not affect search membership. Re-sort the existing result set
        // instead of doing a full refresh/search.
        model_->sort(sortColumn, sortOrder);
        restoreSelectedRecordHandles(selectedHandles, currentHandle);
        return;
    }

    const QRect visibleRect = tableView_->viewport()->rect();
    int firstVisibleRow = tableView_->rowAt(visibleRect.top());
    int lastVisibleRow = tableView_->rowAt(visibleRect.bottom());

    if (firstVisibleRow < 0) {
        firstVisibleRow = tableView_->indexAt(QPoint(0, 0)).row();
    }

    if (lastVisibleRow < 0) {
        lastVisibleRow = tableView_->indexAt(QPoint(0, visibleRect.bottom())).row();
    }

    if (firstVisibleRow < 0) {
        firstVisibleRow = 0;
    }

    if (lastVisibleRow < 0) {
        lastVisibleRow = std::min(model_->rowCount() - 1, firstVisibleRow + 100);
    }

    model_->notifyRowsDataChanged(firstVisibleRow, lastVisibleRow);
}

void MainWindow::markLiveStructuralRefreshDirty()
{
    liveStructuralRefreshDirty_ = true;
}

void MainWindow::markLiveMetadataRefreshDirty()
{
    liveMetadataRefreshDirty_ = true;
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);

    if (event->type() != QEvent::WindowStateChange) {
        return;
    }

    if (!isVisible() || isMinimized()) {
        return;
    }

    if (liveStructuralRefreshDirty_) {
        refresh();
        return;
    }

    if (liveMetadataRefreshDirty_) {
        refreshLiveMetadata();
    }
}

void MainWindow::rebuildFilterMenu()
{
    if (!filterMenu_) {
        return;
    }

    filterMenu_->clear();

    auto* filterActionGroup = new QActionGroup(filterMenu_);
    filterActionGroup->setExclusive(true);

    auto* allFilesAction = new QAction(QStringLiteral("All Files"), filterMenu_);
    allFilesAction->setCheckable(true);
    allFilesAction->setChecked(activeSearchFilterId_.isEmpty());
    filterActionGroup->addAction(allFilesAction);
    filterMenu_->addAction(allFilesAction);

    connect(allFilesAction, &QAction::triggered, this, [this]() {
        applySearchFilter(QString(), QString(), QString());
    });

    auto* foldersAction = new QAction(QStringLiteral("Folders"), filterMenu_);
    foldersAction->setCheckable(true);
    foldersAction->setStatusTip(QStringLiteral("folder:"));
    foldersAction->setToolTip(QStringLiteral("folder:"));
    foldersAction->setChecked(activeSearchFilterId_ == QStringLiteral("builtin-folders"));
    filterActionGroup->addAction(foldersAction);
    filterMenu_->addAction(foldersAction);

    connect(foldersAction, &QAction::triggered, this, [this]() {
        applySearchFilter(
            QStringLiteral("builtin-folders"),
            QStringLiteral("Folders"),
            QStringLiteral("folder:")
        );
    });

    filterMenu_->addSeparator();

    bool activeFilterStillExists =
        activeSearchFilterId_.isEmpty() ||
        activeSearchFilterId_ == QStringLiteral("builtin-folders");

    const std::vector<SearchFilterPreference> filters =
        controller_ ? controller_->searchFilters() : std::vector<SearchFilterPreference>{};

    for (const SearchFilterPreference& filter : filters) {
        auto* filterAction = new QAction(filter.name, filterMenu_);
        filterAction->setCheckable(true);
        filterAction->setStatusTip(filter.query);
        filterAction->setToolTip(filter.query);

        if (filter.id == activeSearchFilterId_) {
            filterAction->setChecked(true);
            activeSearchFilterName_ = filter.name;
            activeSearchFilter_ = filter.query;
            activeFilterStillExists = true;
        }

        filterActionGroup->addAction(filterAction);
        filterMenu_->addAction(filterAction);

        connect(filterAction, &QAction::triggered, this, [this, filter]() {
            applySearchFilter(filter.id, filter.name, filter.query);
        });
    }

    if (!activeFilterStillExists) {
        activeSearchFilterId_.clear();
        activeSearchFilterName_.clear();
        activeSearchFilter_.clear();
        allFilesAction->setChecked(true);
    }

    filterMenu_->addSeparator();

    auto* manageFiltersAction = new QAction(QStringLiteral("Manage Filters..."), filterMenu_);
    connect(manageFiltersAction, &QAction::triggered, this, [this]() {
        if (controller_) {
            controller_->showPreferencesDialog(PreferencesDialogPage::Filters);
        }
    });
    filterMenu_->addAction(manageFiltersAction);

    updateSearchLineFilterHint();
}

void MainWindow::applySearchFilter(const QString& filterId, const QString& filterName, const QString& queryFragment)
{
    activeSearchFilterId_ = filterId;
    activeSearchFilterName_ = filterName;
    activeSearchFilter_ = queryFragment;
    rebuildFilterMenu();
    updateSearch(searchLine_->text());

    if (queryFragment.isEmpty()) {
        showTemporaryStatus(QStringLiteral("Filter cleared"), 2500);
    } else {
        showTemporaryStatus(QStringLiteral("Filter applied: %1").arg(filterName), 3500);
    }
}

void MainWindow::updateSearchLineFilterHint()
{
    if (!searchLine_) {
        return;
    }

    if (activeSearchFilter_.isEmpty()) {
        searchLine_->setPlaceholderText(QStringLiteral("Search files..."));
        searchLine_->setToolTip(
            QStringLiteral("Search indexed file names. You can use filters such as ext:mp4, ext:wav;mp3, or folder:.")
        );
        return;
    }

    if (activeSearchFilterId_ == QStringLiteral("builtin-folders")) {
        searchLine_->setPlaceholderText(QStringLiteral("Search folders..."));
    } else {
        searchLine_->setPlaceholderText(
            QStringLiteral("Search files in %1...").arg(activeSearchFilterName_)
        );
    }

    searchLine_->setToolTip(
        QStringLiteral(
            "Active filter: %1\n"
            "Query fragment: %2"
        ).arg(
            activeSearchFilterName_,
            activeSearchFilter_
        )
    );
}

void MainWindow::showTemporaryStatus(const QString& text, int timeoutMs)
{
    const quint64 id = ++statusMessageId_;
    statusLabel_->setText(text);

    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, this, [this, id]() {
            if (id == statusMessageId_) {
                statusLabel_->clear();
            }
        });
    }
}

void MainWindow::showUnavailableSelectionStatus(const qsizetype selectedCount, const qsizetype mountedCount, const QString& actionText)
{
    Q_UNUSED(mountedCount);

    const QString itemText = selectedCount == 1
        ? QStringLiteral("item is")
        : QStringLiteral("items are");

    showTemporaryStatus(
        QStringLiteral("Cannot %1: selected %2 %3 on unmounted devices.")
            .arg(actionText)
            .arg(selectedCount)
            .arg(itemText),
        5000
    );
}

void MainWindow::showSkippedUnmountedStatus(const qsizetype attemptedCount, const qsizetype completedCount, const QString& actionText)
{
    const qsizetype skippedCount = attemptedCount - completedCount;

    if (skippedCount <= 0) {
        return;
    }

    const QString skippedText = skippedCount == 1
        ? QStringLiteral("1 item")
        : QStringLiteral("%1 items").arg(skippedCount);

    showTemporaryStatus(
        QStringLiteral("%1 %2. Skipped %3 from unmounted devices.")
            .arg(actionText)
            .arg(completedCount)
            .arg(skippedText),
        5000
    );
}

QString MainWindow::actionTextForOpenableCount(
    const QString& singularText,
    const QString& singularCountedText,
    const QString& pluralCountedText,
    qsizetype selectedCount,
    qsizetype openableCount
) {
    if (openableCount <= 0) {
        return singularText;
    }

    if (selectedCount == 1 && openableCount == 1) {
        return singularText;
    }

    if (openableCount == 1) {
        return singularCountedText;
    }

    return pluralCountedText.arg(openableCount);
}

std::vector<IndexController::RecordHandle> MainWindow::captureSelectedRecordHandles() const
{
    std::vector<IndexController::RecordHandle> handles;

    if (!tableView_ || !tableView_->selectionModel() || !model_) {
        return handles;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();
    handles.reserve(static_cast<std::size_t>(selectedRows.size()));

    for (const QModelIndex& index : selectedRows) {
        if (!index.isValid()) {
            continue;
        }

        if (std::optional<IndexController::RecordHandle> handle = model_->recordHandleForRow(index.row())) {
            handles.push_back(*handle);
        }
    }

    return handles;
}

std::optional<IndexController::RecordHandle> MainWindow::captureCurrentRecordHandle() const
{
    if (!tableView_ || !model_) {
        return std::nullopt;
    }

    const QModelIndex currentIndex = tableView_->currentIndex();

    if (!currentIndex.isValid()) {
        return std::nullopt;
    }

    return model_->recordHandleForRow(currentIndex.row());
}

void MainWindow::restoreSelectedRecordHandles(
    const std::vector<IndexController::RecordHandle>& selectedHandles,
    const std::optional<IndexController::RecordHandle>& currentHandle
) {
    if (!tableView_ || !tableView_->selectionModel() || !model_) {
        return;
    }

    auto* selectionModel = tableView_->selectionModel();

    const int verticalScrollValue = tableView_->verticalScrollBar()
        ? tableView_->verticalScrollBar()->value()
        : 0;
    const int horizontalScrollValue = tableView_->horizontalScrollBar()
        ? tableView_->horizontalScrollBar()->value()
        : 0;

    selectionModel->clearSelection();

    if (selectedHandles.empty()) {
        tableView_->setCurrentIndex(QModelIndex());
        return;
    }

    QItemSelection selection;

    for (const IndexController::RecordHandle& handle : selectedHandles) {
        const int row = model_->rowForRecordHandle(handle);

        if (row < 0) {
            continue;
        }

        const QModelIndex left = model_->index(row, 0);
        const QModelIndex right = model_->index(row, model_->columnCount() - 1);

        if (!left.isValid() || !right.isValid()) {
            continue;
        }

        selection.select(left, right);
    }

    if (selection.isEmpty()) {
        tableView_->setCurrentIndex(QModelIndex());
        return;
    }

    selectionModel->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    if (currentHandle) {
        const int currentRow = model_->rowForRecordHandle(*currentHandle);

        if (currentRow >= 0) {
            const QModelIndex restoredCurrentIndex = model_->index(currentRow, 0);

            if (restoredCurrentIndex.isValid()) {
                selectionModel->setCurrentIndex(
                    restoredCurrentIndex,
                    QItemSelectionModel::NoUpdate
                );
            }
        }
    }

    if (tableView_->verticalScrollBar()) {
        tableView_->verticalScrollBar()->setValue(verticalScrollValue);
    }

    if (tableView_->horizontalScrollBar()) {
        tableView_->horizontalScrollBar()->setValue(horizontalScrollValue);
    }
}

void MainWindow::showAbout()
{
#ifdef KERYTHING_WITH_KF6
    KAboutData aboutData(
        QStringLiteral("kerything"),
        QStringLiteral("Kerything"),
        QApplication::applicationVersion(),
        QStringLiteral("Fast file search for Linux block devices, inspired by the Windows utility \"Everything\" by Voidtools."),
        KAboutLicense::GPL_V3,
        QStringLiteral("Copyright © 2026 Reikooters")
    );

    aboutData.setOtherText(QStringLiteral("Release date: %1").arg(KerythingVersion::ReleaseDate));
    aboutData.setHomepage(QStringLiteral("https://github.com/Reikooters/kerything"));
    aboutData.setBugAddress("https://github.com/Reikooters/kerything/issues");
    aboutData.addAuthor(
        QStringLiteral("Reikooters"),
        QStringLiteral("Developer"),
        QString(),
        QStringLiteral("https://github.com/Reikooters")
    );

    auto* dialog = new KAboutApplicationDialog(aboutData, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
#else
    QMessageBox::about(
        this,
        QStringLiteral("About Kerything"),
        QStringLiteral(
            "<h3>Kerything</h3>"
            "<p>Fast file search for Linux block devices, inspired by the Windows utility \"Everything\" by Voidtools.</p>"
            "<p>Version %1</p>"
            "<p>Release date: %2</p>"
            "<p>Copyright &copy; 2026 Reikooters</p>"
            "<p><a href=\"https://github.com/Reikooters/kerything\">"
            "https://github.com/Reikooters/kerything"
            "</a></p>"
            "<p>Licensed under the GNU General Public License v3.0 or later.</p>"
        ).arg(QApplication::applicationVersion(), KerythingVersion::ReleaseDate)
    );
#endif
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event)
{
    if (!tableView_ || !tableView_->selectionModel()) {
        return;
    }

    // Map the position correctly to the viewport
    // This ensures the row index is perfectly aligned with the mouse
    const QPoint viewportPos = tableView_->viewport()->mapFrom(this, event->pos());
    const QModelIndex clickIndex = tableView_->indexAt(viewportPos);

    // If user clicks empty space, don't show the full file menu
    if (!clickIndex.isValid()) {
        return;
    }

    // If the user right-clicks an item that ISN'T selected,
    // select it and clear the old selection (standard file manager behavior).
    if (!tableView_->selectionModel()->isSelected(clickIndex)) {
        tableView_->setCurrentIndex(clickIndex);
        tableView_->selectionModel()->select(
            clickIndex,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();
    const qsizetype mountedCount = model_ ? model_->mountedRowCount(selectedRows) : 0;

#ifdef KERYTHING_WITH_KF6
    KFileItemList kdeItems;

    if (model_) {
        for (const QModelIndex& index : selectedRows) {
            if (!index.isValid()) {
                continue;
            }

            const std::optional<QUrl> url = model_->localUrlForRow(index.row());
            if (!url) {
                continue;
            }

            KFileItem item(*url);
            item.determineMimeType();
            kdeItems.append(item);
        }
    }

    KFileItemActions kdeFileActions;
    const bool hasKdeItems = !kdeItems.isEmpty();

    if (hasKdeItems) {
        kdeFileActions.setItemListProperties(KFileItemListProperties(kdeItems));
    }
#endif

    QMenu menu(this);

    menu.addAction(findChild<QAction*>(QStringLiteral("openAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("openLocationAction")));

    QAction* terminalAction = findChild<QAction*>(QStringLiteral("openTerminalAction"));
    if (terminalAction) {
        menu.addAction(terminalAction);
    }

#ifdef KERYTHING_WITH_KF6
    if (hasKdeItems) {
        kdeFileActions.insertOpenWithActionsTo(nullptr, &menu, QStringList());
    }
#endif

    if (!selectedRows.isEmpty() && mountedCount == 0) {
        menu.addSeparator();

        auto* unavailableAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("dialog-warning")),
            QStringLiteral("Device not mounted")
        );
        unavailableAction->setEnabled(false);
    }

    menu.addSeparator();

    menu.addAction(findChild<QAction*>(QStringLiteral("copyFilesAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("copyFileNamesAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("copyPathsAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("copyParentPathsAction")));

#ifdef KERYTHING_WITH_KF6
    if (hasKdeItems) {
        menu.addSeparator();
        kdeFileActions.addActionsTo(&menu);
    } else if (!selectedRows.isEmpty() && mountedCount == 0) {
        menu.addSeparator();

        auto* kdeUnavailableAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("dialog-warning")),
            QStringLiteral("KDE file actions unavailable for unmounted results")
        );
        kdeUnavailableAction->setEnabled(false);
    }
#endif

    menu.exec(event->globalPos());
}

void MainWindow::openFile(const QModelIndex &index) {
    if (!index.isValid()) {
        return;
    }

    tableView_->setCurrentIndex(index);

    if (tableView_->selectionModel()) {
        tableView_->selectionModel()->select(
            index,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
    }

    openSelectedFiles();
}

void MainWindow::openSelectedFiles() {
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    const qsizetype mountedCount = model_->mountedRowCount(selectedRows);

    if (mountedCount == 0) {
        showUnavailableSelectionStatus(
            selectedRows.size(),
            mountedCount,
            QStringLiteral("open")
        );
        return;
    }

    if (mountedCount > OpenManyFilesConfirmationThreshold) {
        const QMessageBox::StandardButton result = QMessageBox::question(
            this,
            QStringLiteral("Open %1 Files?").arg(mountedCount),
            QStringLiteral(
                "You are about to open %1 files.\n\n"
                "This may open many application windows or tabs."
            ).arg(mountedCount),
            QMessageBox::Cancel | QMessageBox::Open,
            QMessageBox::Cancel
        );

        if (result != QMessageBox::Open) {
            return;
        }
    }

    QList<QUrl> urls;

    for (const QModelIndex& index : selectedRows) {
        if (!index.isValid()) {
            continue;
        }

        const std::optional<QUrl> url = model_->localUrlForRow(index.row());

        if (!url) {
            continue;
        }

        urls.append(*url);
    }

#ifdef KERYTHING_WITH_KF6
    QMimeDatabase mimeDatabase;
    QMap<QString, QList<QUrl>> urlsByMimeType;

    for (const QUrl& url : urls) {
        const QMimeType mimeType = mimeDatabase.mimeTypeForUrl(url);
        urlsByMimeType[mimeType.name()].append(url);
    }

    for (auto it = urlsByMimeType.cbegin(); it != urlsByMimeType.cend(); ++it) {
        const QString& mimeType = it.key();
        const QList<QUrl>& mimeUrls = it.value();

        KService::Ptr service = KApplicationTrader::preferredService(mimeType);

        auto* job = service
            ? new KIO::ApplicationLauncherJob(service)
            : new KIO::ApplicationLauncherJob();

        job->setUrls(mimeUrls);
        job->setAutoDelete(true);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
        job->start();
    }
#else
    for (const QUrl& url : urls) {
        if (!QDesktopServices::openUrl(url)) {
            statusBar()->showMessage(
                QStringLiteral("Could not open: %1").arg(url.toLocalFile()),
                5000
            );
        }
    }
#endif

    showSkippedUnmountedStatus(
        selectedRows.size(),
        urls.size(),
        QStringLiteral("Opened")
    );
}

void MainWindow::openSelectedLocation()
{
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    const std::optional<QUrl> url = model_->localUrlForRow(selectedRows.first().row());

    if (!url) {
        showUnavailableSelectionStatus(
            selectedRows.size(),
            0,
            QStringLiteral("show location")
        );
        return;
    }

#ifdef KERYTHING_WITH_KF6
    if (KIO::highlightInFileManager({*url})) {
        return;
    }
#endif

    const QFileInfo fileInfo(url->toLocalFile());
    const QString dirPath = fileInfo.absolutePath();

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath))) {
        statusBar()->showMessage(
            QStringLiteral("Could not open location: %1").arg(dirPath),
            5000
        );
    }
}

void MainWindow::copyFileNames()
{
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    QStringList fileNames;
    fileNames.reserve(selectedRows.size());

    for (const QModelIndex& index : selectedRows) {
        const QString fileName = model_->data(model_->index(index.row(), 0), Qt::DisplayRole).toString();

        if (!fileName.isEmpty()) {
            fileNames.append(fileName);
        }
    }

    if (!fileNames.isEmpty()) {
        QApplication::clipboard()->setText(fileNames.join(QLatin1Char('\n')));
    }
}

void MainWindow::copyPaths()
{
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    QStringList paths;
    paths.reserve(selectedRows.size());

    for (const QModelIndex& index : selectedRows) {
        const std::optional<QUrl> url = model_->localUrlForRow(index.row());

        if (url) {
            paths.append(QDir::cleanPath(url->toLocalFile()));
            continue;
        }

        const QString displayPath = model_->data(model_->index(index.row(), 1), Qt::DisplayRole).toString();
        const QString fileName = model_->data(model_->index(index.row(), 0), Qt::DisplayRole).toString();

        if (displayPath.isEmpty()) {
            continue;
        }

        if (fileName.isEmpty()) {
            paths.append(displayPath);
            continue;
        }

        paths.append(
            displayPath.endsWith(QLatin1Char('/'))
                ? displayPath + fileName
                : displayPath + QStringLiteral("/") + fileName
        );
    }

    if (!paths.isEmpty()) {
        QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    }
}

void MainWindow::copyParentPaths()
{
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    QStringList parentPaths;
    parentPaths.reserve(selectedRows.size());

    for (const QModelIndex& index : selectedRows) {
        const std::optional<QUrl> url = model_->localUrlForRow(index.row());

        if (url) {
            const QFileInfo fileInfo(url->toLocalFile());
            parentPaths.append(QDir::cleanPath(
                fileInfo.isDir()
                    ? fileInfo.absoluteFilePath()
                    : fileInfo.absolutePath()
            ));
            continue;
        }

        const QString displayPath = model_->data(model_->index(index.row(), 1), Qt::DisplayRole).toString();

        if (!displayPath.isEmpty()) {
            parentPaths.append(displayPath);
        }
    }

    if (!parentPaths.isEmpty()) {
        QApplication::clipboard()->setText(parentPaths.join(QLatin1Char('\n')));
    }
}

void MainWindow::copyFiles()
{
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    QList<QUrl> urls;
    urls.reserve(selectedRows.size());

    for (const QModelIndex& index : selectedRows) {
        const std::optional<QUrl> url = model_->localUrlForRow(index.row());

        if (url) {
            urls.append(*url);
        }
    }

    if (urls.isEmpty()) {
        showUnavailableSelectionStatus(
            selectedRows.size(),
            0,
            QStringLiteral("copy files")
        );
        return;
    }

    auto* mimeData = new QMimeData();
    mimeData->setUrls(urls);
    QApplication::clipboard()->setMimeData(mimeData);

    showSkippedUnmountedStatus(
        selectedRows.size(),
        urls.size(),
        QStringLiteral("Copied")
    );
}

void MainWindow::openTerminal()
{
    if (!tableView_->selectionModel()) {
        return;
    }

    const QModelIndexList selectedRows = tableView_->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    const std::optional<QUrl> url = model_->localUrlForRow(selectedRows.first().row());

    if (!url) {
        showUnavailableSelectionStatus(
            selectedRows.size(),
            0,
            QStringLiteral("open terminal")
        );
        return;
    }

    const QFileInfo fileInfo(url->toLocalFile());
    const QString dirPath = fileInfo.isDir()
        ? fileInfo.absoluteFilePath()
        : fileInfo.absolutePath();

#ifdef KERYTHING_WITH_KF6
    auto* job = new KTerminalLauncherJob(QString());
    job->setWorkingDirectory(dirPath);
    job->setAutoDelete(true);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
    job->start();
#else
    if (!QProcess::startDetached(QStringLiteral("xdg-terminal-exec"), QStringList{}, dirPath)) {
        statusBar()->showMessage(
            QStringLiteral("Could not open terminal in: %1").arg(dirPath),
            5000
        );
    }
#endif
}
