// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "MainWindow.h"

#include <iostream>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "AppController.h"

MainWindow::MainWindow(AppController* controller, QWidget* parent)
    : QMainWindow(parent),
      controller_(controller) {
    setWindowTitle("Kerything");

    // Delete the QWidget object when the user closes the window.
    setAttribute(Qt::WA_DeleteOnClose);

    // Central widget that holds the window's main UI.
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);

    searchLine = new QLineEdit(centralWidget);
    searchLine->setPlaceholderText("Search files...");
    searchLine->setClearButtonEnabled(true);

    // Add magnifying glass icon to the search bar
    searchLine->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);

    // --- Burger Menu Setup ---
    auto *menu = new QMenu(this);

    auto *changePartitionAct = new QAction(QIcon::fromTheme("drive-harddisk"), "Change Partition", this);
    //connect(changePartitionAct, &QAction::triggered, this, &MainWindow::changePartition);
    changePartitionAct->setShortcut(QKeySequence::Open);
    menu->addAction(changePartitionAct);
    addAction(changePartitionAct); // Register with window for shortcuts

    auto *rescanPartitionAct = new QAction(QIcon::fromTheme("view-refresh"), "Rescan Partition", this);
    //connect(rescanPartitionAct, &QAction::triggered, this, &MainWindow::rescanPartition);
    rescanPartitionAct->setShortcut(QKeySequence::Refresh);
    menu->addAction(rescanPartitionAct);
    addAction(rescanPartitionAct); // Register with window for shortcuts

    menu->addSeparator();

    auto *aboutAct = new QAction(QIcon::fromTheme("kerything"), "About Kerything", this);
    //connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
    menu->addAction(aboutAct);

    auto *quitAct = new QAction(QIcon::fromTheme("application-exit"), "Quit", this);
    quitAct->setShortcut(QKeySequence::Quit);
    //connect(quitAct, &QAction::triggered, qApp, &QCoreApplication::quit);
    menu->addAction(quitAct);
    addAction(quitAct); // Register with window for shortcuts

    // Add the menu to a button inside the search line
    auto *menuAction = searchLine->addAction(QIcon::fromTheme("application-menu"), QLineEdit::TrailingPosition);
    connect(menuAction, &QAction::triggered, [this, menu, menuAction]() {
        // Show the menu just below the icon
        menu->exec(QCursor::pos());
    });
    // ---------------------

    layout->addWidget(searchLine);

    tableView = new QTableView(centralWidget);
    model = new FileModel(controller_, this);
    tableView->setModel(model);

    // Enable Sorting
    tableView->setSortingEnabled(true);
    tableView->horizontalHeader()->setSortIndicatorShown(true);

    // Table Styling
    tableView->setAlternatingRowColors(true);
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView->verticalHeader()->setVisible(false);
    tableView->setWordWrap(false);

    // Full-row hover
    // tableView->setItemDelegate(new HoverRowDelegate(this));
    tableView->setMouseTracking(true);
    tableView->viewport()->setMouseTracking(true);
    // connect(tableView, &QAbstractItemView::entered, this, &MainWindow::onTableHovered);
    // connect(tableView, &QAbstractItemView::viewportEntered, this, &MainWindow::onTableViewportHovered);
    tableView->viewport()->installEventFilter(this);

    // --- Drag and Drop Configuration ---
    // setDragEnabled(true) tells the view to start a drag if the user moves the
    // mouse while pressing the left button on a selected item.
    tableView->setDragEnabled(true);

    // DragOnly means we can drag items out, but the application doesn't accept drops.
    tableView->setDragDropMode(QAbstractItemView::DragOnly);

    // Setting the default action to CopyAction signals to the OS
    // that we want to share/copy the data, which helps the Portal
    // decide to grant permission.
    tableView->setDefaultDropAction(Qt::CopyAction);
    // ---------------------

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
        const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();
        int count = selectedRows.count();
        //bool isMounted = !m_mountPath.isEmpty();
        bool isMounted = false;

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

        QAction* copyFileNamesAction = findChild<QAction*>("copyFileNamesAction");
        if (copyFileNamesAction) {
            copyFileNamesAction->setEnabled(count > 0);
            copyFileNamesAction->setText(count == 1 ? "Copy File Name" : "Copy File Names");
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
    // ---------------------

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

    // Escape key in search line clears the search
    auto *clearSearch = new QShortcut(QKeySequence(Qt::Key_Escape), searchLine);
    clearSearch->setContext(Qt::WidgetShortcut);
    connect(clearSearch, &QShortcut::activated, searchLine, &QLineEdit::clear);
    // ---------------------

    // --- Global Window Actions (Shortcuts + Menu items) ---
    // Ctrl+L and Alt+D: Focus Search
    auto *focusSearchAct = new QAction(this);
    focusSearchAct->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D)});
    connect(focusSearchAct, &QAction::triggered, searchLine, [this]() {
        searchLine->setFocus();
        searchLine->selectAll();
    });
    addAction(focusSearchAct);

    // Enter: Open
    auto *openAct = new QAction(QIcon::fromTheme("system-run"), "Open", this);
    openAct->setShortcut(QKeySequence(Qt::Key_Return));
    openAct->setObjectName("openAction");
    // connect(openAct, &QAction::triggered, this, &MainWindow::openSelectedFiles);
    addAction(openAct);

    // Ctrl+Enter: Open File Location
    auto *openLocAct = new QAction(QIcon::fromTheme("folder-open"), "Open File Location", this);
    openLocAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    openLocAct->setObjectName("openLocationAction");
    // connect(openLocAct, &QAction::triggered, this, &MainWindow::openSelectedLocation);
    addAction(openLocAct);

    // Ctrl+C: Copy Files
    auto *copyFilesAct = new QAction(QIcon::fromTheme("edit-copy"), "Copy", this);
    copyFilesAct->setShortcut(QKeySequence::Copy);
    copyFilesAct->setObjectName("copyFilesAction");
    // connect(copyFilesAct, &QAction::triggered, this, &MainWindow::copyFiles);
    addAction(copyFilesAct);

    // Ctrl+Shift+C: Copy File Names
    auto *copyFileNamesAct = new QAction(QIcon::fromTheme("edit-copy"), "Copy File Name", this);
    copyFileNamesAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyFileNamesAct->setObjectName("copyFileNamesAction");
    // connect(copyFileNamesAct, &QAction::triggered, this, &MainWindow::copyFileNames);
    addAction(copyFileNamesAct);

    // Ctrl+Alt+C: Copy Full Paths
    auto *copyPathsAct = new QAction(QIcon::fromTheme("edit-copy-path"), "Copy Full Path", this);
    copyPathsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C));
    copyPathsAct->setObjectName("copyPathsAction");
    // connect(copyPathsAct, &QAction::triggered, this, &MainWindow::copyPaths);
    addAction(copyPathsAct);

    // Alt+Shift+F4: Open Terminal
    auto *terminalAct = new QAction(QIcon::fromTheme("utilities-terminal"), "Open Terminal Here", this);
    terminalAct->setShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_F4));
    terminalAct->setObjectName("openTerminalAction");
    // connect(terminalAct, &QAction::triggered, this, &MainWindow::openTerminal);
    addAction(terminalAct);
    // ---------------------

    // Handle double-click on item in table view to open file
    // connect(tableView, &QTableView::doubleClicked, this, &MainWindow::openFile);

    // Start with a full list, sorted by name ascending
    tableView->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    updateSearch("");
}

void MainWindow::updateSearch(const QString &text) {
    auto start = std::chrono::steady_clock::now();

    auto results = controller_->indexController()->performTrigramSearch(text.toStdString());
    model->setSearchResults(std::move(results));

    int sortCol = tableView->horizontalHeader()->sortIndicatorSection();
    model->sort(sortCol, tableView->horizontalHeader()->sortIndicatorOrder());

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    // Update status bar
    statusLabel->setText(QString("%L1 objects found in %2s")
        .arg(model->rowCount())
        .arg(elapsed.count(), 0, 'f', 4));
}

void MainWindow::refresh() {
    updateSearch(searchLine->text());

    // infoLabel_->setText(
    //     "This window belongs to the primary process.\n"
    //     "Launching the app again will ask this process to open another window."
    // );
    //
    // counterLabel_->setText(
    //     QString("Shared counter value: %1\nDaemon connection status: %2\nDaemon ready status: %3")
    //         .arg(controller_->sharedCounter())
    //         .arg(controller_->isDaemonConnected() ? "Connected" : "Disconnected")
    //         .arg(controller_->isDaemonReady() ? "Ready" : "Not Ready")
    //     );
}
