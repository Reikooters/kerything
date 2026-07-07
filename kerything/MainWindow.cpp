// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "MainWindow.h"
#include "SearchResultTableView.h"

#include <iostream>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#ifdef KERYTHING_WITH_KIO
#include <QMimeDatabase>
#include <QMimeType>

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KApplicationTrader>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenFileManagerWindowJob>
#include <KService>
#include <KTerminalLauncherJob>
#endif

#include "AppController.h"

namespace {
    constexpr int OpenManyFilesConfirmationThreshold = 10;
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
        int count = selectedRows.count();

        // Open: Enabled if mounted and something is selected
        QAction* openAction = findChild<QAction*>("openAction");
        if (openAction) {
            openAction->setEnabled(count > 0);
            openAction->setText(count <= 1 ? "Open File" : "Open " + QString::number(count) + " Files");
        }

        // Open Location & Terminal: Only for single selection
        QAction* openLocAction = findChild<QAction*>("openLocationAction");
        if (openLocAction) {
            openLocAction->setEnabled(count == 1);
        }

        QAction* openTerminalAction = findChild<QAction*>("openTerminalAction");
        if (openTerminalAction) {
            openTerminalAction->setEnabled(count == 1);
        }

        // Copy Actions: Enabled if something is selected
        QAction* copyFilesAction = findChild<QAction*>("copyFilesAction");
        if (copyFilesAction) {
            copyFilesAction->setEnabled(count > 0);
            copyFilesAction->setText(count <= 1 ? "Copy File" : "Copy " + QString::number(count) + " Files");
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
    auto *openAct = new QAction(QIcon::fromTheme("system-run"), "Open File", this);
    openAct->setShortcuts({
        QKeySequence(Qt::Key_Return),
        QKeySequence(Qt::Key_Enter)
    });
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
    fileMenu->addAction(openLocAct);
    fileMenu->addAction(terminalAct);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAct);

    // Edit Menu
    auto* editMenu = menuBar()->addMenu("Edit");
    editMenu->addAction(copyFilesAct);
    editMenu->addAction(copyFileNamesAct);
    editMenu->addAction(copyPathsAct);
    editMenu->addAction(copyParentPathsAct);

    // Help Menu
    auto* helpMenu = menuBar()->addMenu("Help");
    helpMenu->addAction(aboutAct);
    // ---------------------

    updateActionStates();

    // Handle double-click on item in table view to open file
    connect(tableView_, &SearchResultTableView::doubleClicked, this, &MainWindow::openFile);

    // Start with a full list, sorted by name ascending
    tableView_->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
    updateSearch("");
}

void MainWindow::updateSearch(const QString &text) {
    auto start1 = std::chrono::steady_clock::now();
    auto results = controller_->indexController()->performTrigramSearch(text.toStdString());
    model_->setSearchResults(std::move(results));
    auto end1 = std::chrono::steady_clock::now();

    auto start2 = std::chrono::steady_clock::now();
    int sortCol = tableView_->horizontalHeader()->sortIndicatorSection();
    model_->sort(sortCol, tableView_->horizontalHeader()->sortIndicatorOrder());
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
    updateSearch(searchLine_->text());

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

void MainWindow::showAbout()
{
#ifdef KERYTHING_WITH_KIO
    KAboutData aboutData(
        QStringLiteral("kerything"),
        QStringLiteral("Kerything"),
        QApplication::applicationVersion(),
        QStringLiteral("Fast file search for Linux block devices, inspired by the Windows utility \"Everything\" by Voidtools."),
        KAboutLicense::GPL_V3,
        QStringLiteral("Copyright © 2026 Reikooters")
    );

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
            "<p>Copyright &copy; 2026 Reikooters</p>"
            "<p><a href=\"https://github.com/Reikooters/kerything\">"
            "github.com/Reikooters/kerything"
            "</a></p>"
            "<p>Licensed under the GNU General Public License v3.0 or later.</p>"
        ).arg(QApplication::applicationVersion())
    );
#endif
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event)
{
    if (!tableView_ || !tableView_->selectionModel()) {
        return;
    }

    const QPoint viewportPos = tableView_->viewport()->mapFrom(this, event->pos());
    const QModelIndex clickIndex = tableView_->indexAt(viewportPos);

    if (!clickIndex.isValid()) {
        return;
    }

    if (!tableView_->selectionModel()->isSelected(clickIndex)) {
        tableView_->setCurrentIndex(clickIndex);
        tableView_->selectionModel()->select(
            clickIndex,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
        );
    }

    QMenu menu(this);

    menu.addAction(findChild<QAction*>(QStringLiteral("openAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("openLocationAction")));

    QAction* terminalAction = findChild<QAction*>(QStringLiteral("openTerminalAction"));
    if (terminalAction) {
        menu.addAction(terminalAction);
    }

    menu.addSeparator();

    menu.addAction(findChild<QAction*>(QStringLiteral("copyFilesAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("copyFileNamesAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("copyPathsAction")));
    menu.addAction(findChild<QAction*>(QStringLiteral("copyParentPathsAction")));

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

    if (selectedRows.size() > OpenManyFilesConfirmationThreshold) {
        const QMessageBox::StandardButton result = QMessageBox::question(
            this,
            QStringLiteral("Open %1 Files?").arg(selectedRows.size()),
            QStringLiteral(
                "You are about to open %1 files.\n\n"
                "This may open many application windows or tabs."
            ).arg(selectedRows.size()),
            QMessageBox::Cancel | QMessageBox::Open,
            QMessageBox::Cancel
        );

        if (result != QMessageBox::Open) {
            return;
        }
    }

    QList<QUrl> urls;
    bool skippedUnmounted = false;

    for (const QModelIndex& index : selectedRows) {
        if (!index.isValid()) {
            continue;
        }

        const std::optional<QUrl> url = model_->localUrlForRow(index.row());

        if (!url) {
            skippedUnmounted = true;
            continue;
        }

        urls.append(*url);
    }

    if (urls.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("Drive Not Mounted"),
            QStringLiteral(
                "The selected result is from a device that is not currently mounted.\n\n"
                "Mount the device to open files from it."
            )
        );
        return;
    }

    if (skippedUnmounted) {
        statusBar()->showMessage(
            QStringLiteral("Some selected items were skipped because their devices are not mounted."),
            5000
        );
    }

#ifdef KERYTHING_WITH_KIO
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
        QMessageBox::information(
            this,
            QStringLiteral("Drive Not Mounted"),
            QStringLiteral(
                "The selected result is from a device that is not currently mounted.\n\n"
                "Mount the device to open its containing folder."
            )
        );
        return;
    }

#ifdef KERYTHING_WITH_KIO
    if (KIO::highlightInFileManager({*url})) {
        return;
    }
#endif

    const QFileInfo fileInfo(url->toLocalFile());
    const QString dirPath = fileInfo.isDir()
        ? fileInfo.absoluteFilePath()
        : fileInfo.absolutePath();

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

        if (!displayPath.isEmpty() && !fileName.isEmpty()) {
            paths.append(QDir::cleanPath(displayPath + QStringLiteral("/") + fileName));
        }
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
            parentPaths.append(QDir::cleanPath(displayPath));
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
        QMessageBox::information(
            this,
            QStringLiteral("Drive Not Mounted"),
            QStringLiteral(
                "None of the selected results are on currently mounted devices.\n\n"
                "Mount the device to copy files."
            )
        );
        return;
    }

    auto* mimeData = new QMimeData();
    mimeData->setUrls(urls);
    QApplication::clipboard()->setMimeData(mimeData);

    if (urls.size() != selectedRows.size()) {
        statusBar()->showMessage(
            QStringLiteral("Copied mounted files only; unmounted results were skipped."),
            5000
        );
    }
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
        QMessageBox::information(
            this,
            QStringLiteral("Drive Not Mounted"),
            QStringLiteral(
                "The selected result is from a device that is not currently mounted.\n\n"
                "Mount the device to open a terminal there."
            )
        );
        return;
    }

    const QFileInfo fileInfo(url->toLocalFile());
    const QString dirPath = fileInfo.isDir()
        ? fileInfo.absoluteFilePath()
        : fileInfo.absolutePath();

#ifdef KERYTHING_WITH_KIO
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
