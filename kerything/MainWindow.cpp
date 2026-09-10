// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "MainWindow.h"
#include "SearchResultTableView.h"

#include <algorithm>
#include <iostream>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QItemSelection>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

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

#ifdef KERYTHING_ENABLE_MEMORY_STATS
#include <QDialog>
#include <QFontDatabase>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextStream>
#endif

#include "AppController.h"
#include "HoverRowHighlight.h"
#include "Version.h"
#include "SearchResultColumns.h"

namespace {
    constexpr qsizetype OpenManyFilesConfirmationThreshold = 10;
    constexpr int FilterDropdownIdRole = Qt::UserRole + 1;
    constexpr int FilterDropdownNameRole = Qt::UserRole + 2;
    constexpr int FilterDropdownQueryRole = Qt::UserRole + 3;
    constexpr int FilterDropdownKindRole = Qt::UserRole + 4;

    enum FilterDropdownKind {
        FilterDropdownKindFilter = 0,
        FilterDropdownKindManageFilters = 1,
    };

    QString menuTextFromUserText(QString text)
    {
        text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        return text;
    }

    bool isBuiltInSearchFilterToken(const QString& token)
    {
        const QString foldedToken = token.trimmed().toCaseFolded();

        return foldedToken.startsWith(QStringLiteral("ext:")) ||
               foldedToken.startsWith(QStringLiteral("extension:")) ||
               foldedToken == QStringLiteral("file:") ||
               foldedToken == QStringLiteral("files:") ||
               foldedToken == QStringLiteral("folder:") ||
               foldedToken == QStringLiteral("folders:") ||
               foldedToken == QStringLiteral("type:file") ||
               foldedToken == QStringLiteral("type:files") ||
               foldedToken == QStringLiteral("type:folder") ||
               foldedToken == QStringLiteral("type:folders");
    }

    QStringList highlightTermsForSearchText(const QString& text)
    {
        QStringList terms;

        const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

        for (const QString& part : parts) {
            const QString term = part.trimmed();

            if (term.isEmpty()) {
                continue;
            }

            if (term.contains(QLatin1Char(':'))) {
                continue;
            }

            terms << term;
        }

        terms.removeDuplicates();
        return terms;
    }

    QString effectiveSearchQueryWithFilterMacros(
            const QString& text,
            const std::vector<SearchFilterPreference>& filters
        ) {
        if (filters.empty()) {
            return text;
        }

        QHash<QString, QString> queryByMacro;

        for (const SearchFilterPreference& filter : filters) {
            QString macro = filter.macro.trimmed();

            while (macro.endsWith(QLatin1Char(':'))) {
                macro.chop(1);
                macro = macro.trimmed();
            }

            if (macro.isEmpty() || filter.query.trimmed().isEmpty()) {
                continue;
            }

            queryByMacro.insert(macro.toCaseFolded(), filter.query.trimmed());
        }

        if (queryByMacro.isEmpty()) {
            return text;
        }

        static constexpr int MaxFilterMacroExpansionDepth = 32;

        auto expandText = [&](const QString& input, QSet<QString>& expandingMacros, int depth, auto&& expandTextRef) -> QString {
            if (depth >= MaxFilterMacroExpansionDepth) {
                return input;
            }

            QStringList expandedTokens;
            const QStringList parts = input.split(
                QRegularExpression(QStringLiteral("\\s+")),
                Qt::SkipEmptyParts
            );

            expandedTokens.reserve(parts.size());

            for (const QString& part : parts) {
                const QString token = part.trimmed();

                if (token.size() > 1 && token.endsWith(QLatin1Char(':'))) {
                    const QString macro = token.left(token.size() - 1).trimmed().toCaseFolded();

                    if (const auto it = queryByMacro.constFind(macro); it != queryByMacro.constEnd()) {
                        if (expandingMacros.contains(macro)) {
                            // Preferences validation should prevent circular macro references.
                            // If a cycle still appears at runtime, keep the original token rather
                            // than dropping it: dropping would silently broaden the search.
                            expandedTokens << token;
                            continue;
                        }

                        expandingMacros.insert(macro);
                        expandedTokens << expandTextRef(
                            it.value(),
                            expandingMacros,
                            depth + 1,
                            expandTextRef
                        );
                        expandingMacros.remove(macro);
                        continue;
                    }
                }

                expandedTokens << token;
            }

            return expandedTokens.join(QLatin1Char(' '));
        };

        QSet<QString> expandingMacros;
        return expandText(text, expandingMacros, 0, expandText);
    }

    QString regexHighlightPatternForSearchText(
        const QString& text,
        const std::vector<SearchFilterPreference>& filters
    ) {
        const QString expandedText = effectiveSearchQueryWithFilterMacros(text, filters);

        QStringList regexTokens;
        const QStringList parts = expandedText.split(
            QRegularExpression(QStringLiteral("\\s+")),
            Qt::SkipEmptyParts
        );

        regexTokens.reserve(parts.size());

        for (const QString& part : parts) {
            const QString token = part.trimmed();

            if (token.isEmpty() || isBuiltInSearchFilterToken(token)) {
                continue;
            }

            regexTokens << token;
        }

        return regexTokens.join(QLatin1Char(' ')).trimmed();
    }
}

MainWindow::MainWindow(
    AppController* controller,
    std::optional<NewWindowState> initialState,
    QWidget* parent
)
    : QMainWindow(parent),
      controller_(controller) {
    setWindowTitle("Kerything");

    // Delete the QWidget object when the user closes the window.
    setAttribute(Qt::WA_DeleteOnClose);

    // Central widget that holds the window's main UI.
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);

    searchLine_ = new QLineEdit(centralWidget);
    searchLine_->setPlaceholderText("Search...");
    searchLine_->setClearButtonEnabled(true);

    // Add magnifying glass icon to the search bar
    searchLine_->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);

    filterDropdown_ = new QComboBox(centralWidget);
    filterDropdown_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    filterDropdown_->setMinimumContentsLength(12);
    filterDropdown_->setVisible(controller_ && controller_->showFiltersDropdown());
    filterDropdown_->setStatusTip(QStringLiteral("Select the active search filter"));
    filterDropdown_->setToolTip(QStringLiteral("Select the active search filter"));

    auto* searchRowLayout = new QHBoxLayout();
    searchRowLayout->setContentsMargins(0, 0, 0, 0);
    searchRowLayout->setSpacing(6);
    searchRowLayout->addWidget(searchLine_, 1);
    searchRowLayout->addWidget(filterDropdown_);

    layout->addLayout(searchRowLayout);

    tableView_ = new SearchResultTableView(centralWidget);
    model_ = new FileModel(controller_, this);
    tableView_->setModel(model_);

    // Enable Sorting
    tableView_->setSortingEnabled(true);
    tableView_->horizontalHeader()->setSortIndicatorShown(true);

    connect(tableView_->horizontalHeader(), &QHeaderView::sectionClicked,
            this, &MainWindow::handleSortSectionClicked);

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
    tableView_->setColumnWidth(SearchResultColumn::Name, 375);
    tableView_->setColumnWidth(SearchResultColumn::Path, 525);
    tableView_->setColumnWidth(SearchResultColumn::Size, 100);
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
    statusLabel_->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  margin-right: 6px;"
        "}"
    ));
    statusBar()->addPermanentWidget(statusLabel_);

    statusBar()->setSizeGripEnabled(false);

    filterChip_ = new QToolButton(this);
    filterChip_->setAutoRaise(true);
    filterChip_->setCursor(Qt::PointingHandCursor);
    filterChip_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    filterChip_->setVisible(false);
    filterChip_->setStatusTip(QStringLiteral("Click to clear the active filter"));

    auto makeSearchOptionChip = [this](const QString& statusTip) {
        auto* chip = new QToolButton(this);
        chip->setAutoRaise(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chip->setVisible(false);
        chip->setStatusTip(statusTip);
        return chip;
    };

    matchCaseChip_ = makeSearchOptionChip(QStringLiteral("Click to turn off Match Case"));
    matchWholeWordChip_ = makeSearchOptionChip(QStringLiteral("Click to turn off Match Whole Word"));
    regexChip_ = makeSearchOptionChip(QStringLiteral("Click to turn off Regex"));

    connect(filterChip_, &QToolButton::clicked, this, [this]() {
        applySearchFilter(QString(), QString(), QString());
    });

    connect(matchCaseChip_, &QToolButton::clicked, this, [this]() {
        setMatchCaseEnabled(false);
    });

    connect(matchWholeWordChip_, &QToolButton::clicked, this, [this]() {
        setMatchWholeWordEnabled(false);
    });

    connect(regexChip_, &QToolButton::clicked, this, [this]() {
        setRegexEnabled(false);
    });

    chipContainer_ = new QWidget(this);
    auto* chipLayout = new QHBoxLayout(chipContainer_);
    chipLayout->setContentsMargins(6, 0, 0, 0);
    chipLayout->setSpacing(1);
    chipLayout->addWidget(matchCaseChip_);
    chipLayout->addWidget(matchWholeWordChip_);
    chipLayout->addWidget(regexChip_);
    chipLayout->addWidget(filterChip_);
    chipContainer_->setVisible(false);

    statusBar()->addPermanentWidget(chipContainer_, 0);

    setCentralWidget(centralWidget);
    resize(1200, 800);

    // Connect search bar to our search logic
    connect(searchLine_, &QLineEdit::textChanged, this, &MainWindow::updateSearch);

    // Connect filter dropdown to filter logic
    connect(filterDropdown_, &QComboBox::activated, this, [this](int index) {
        if (!filterDropdown_ || index < 0) {
            return;
        }

        const int kind = filterDropdown_->itemData(index, FilterDropdownKindRole).toInt();

        if (kind == FilterDropdownKindManageFilters) {
            syncFilterDropdownSelection();

            if (controller_) {
                controller_->showPreferencesDialog(PreferencesDialogPage::Filters);
            }

            return;
        }

        const QString filterId = filterDropdown_->itemData(index, FilterDropdownIdRole).toString();
        const QString filterName = filterDropdown_->itemData(index, FilterDropdownNameRole).toString();
        const QString queryFragment = filterDropdown_->itemData(index, FilterDropdownQueryRole).toString();

        if (filterId == activeSearchFilterId_) {
            return;
        }

        applySearchFilter(filterId, filterName, queryFragment);
    });

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
    connect(newWindowAct, &QAction::triggered, this, &MainWindow::openNewWindowFromThisWindow);
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

    // Automatically Refresh Results for Live Updates
    autoRefreshLiveUpdatesAct_ = new QAction(
        QIcon::fromTheme(
            QStringLiteral("folder-sync"),
            QIcon::fromTheme(
                QStringLiteral("emblem-synchronizing"),
                QIcon::fromTheme(QStringLiteral("view-refresh"))
            )
        ),
        QStringLiteral("Automatically Refresh Results for Live Updates"),
        this
    );
    autoRefreshLiveUpdatesAct_->setCheckable(true);
    autoRefreshLiveUpdatesAct_->setChecked(
        controller_ && controller_->autoRefreshResultsForLiveUpdates()
    );
    autoRefreshLiveUpdatesAct_->setStatusTip(
        QStringLiteral("Refresh and re-sort visible search results as live update events arrive")
    );
    connect(autoRefreshLiveUpdatesAct_, &QAction::toggled, this, [this](bool checked) {
        if (controller_) {
            controller_->setAutoRefreshResultsForLiveUpdates(checked);
        }
    });
    addAction(autoRefreshLiveUpdatesAct_);

    // Filters Dropdown
    showFiltersDropdownAct_ = new QAction(
        QIcon::fromTheme(
            QStringLiteral("view-filter"),
            QIcon::fromTheme(QStringLiteral("filter"))
        ),
        QStringLiteral("Filters Dropdown"),
        this
    );
    showFiltersDropdownAct_->setCheckable(true);
    showFiltersDropdownAct_->setChecked(controller_ && controller_->showFiltersDropdown());
    showFiltersDropdownAct_->setStatusTip(QStringLiteral("Show the filters dropdown next to the search box"));
    connect(showFiltersDropdownAct_, &QAction::toggled, this, [this](bool checked) {
        if (controller_) {
            controller_->setShowFiltersDropdown(checked);
        }
    });
    addAction(showFiltersDropdownAct_);

    // About Kerything
    auto *aboutAct = new QAction(QIcon::fromTheme("kerything"), "About Kerything", this);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);

#ifdef KERYTHING_ENABLE_MEMORY_STATS
    auto* memoryStatsAct = new QAction(
        QIcon::fromTheme(QStringLiteral("utilities-system-monitor")),
        QStringLiteral("Memory Statistics..."),
        this
    );
    memoryStatsAct->setStatusTip(QStringLiteral("Show debug memory statistics for the in-memory index and current results"));
    connect(memoryStatsAct, &QAction::triggered, this, &MainWindow::showMemoryStats);
#endif

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

    // Reset Search
    auto* resetSearchAct = new QAction(
        QIcon::fromTheme(QStringLiteral("edit-clear")),
        QStringLiteral("Reset Search"),
        this
    );
    resetSearchAct->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_Escape),
        QKeySequence(Qt::ShiftModifier | Qt::AltModifier | Qt::Key_Backspace),
    });
    resetSearchAct->setStatusTip(
        QStringLiteral("Clear the search text, active filter, and search options, then focus the search box")
    );
    connect(resetSearchAct, &QAction::triggered, this, &MainWindow::resetSearchStateAndFocus);
    addAction(resetSearchAct);

    // Match Case
    matchCaseAct_ = new QAction(
        QIcon::fromTheme(
            QStringLiteral("format-text-uppercase"),
            QIcon::fromTheme(QStringLiteral("format-text-bold"))
        ),
        QStringLiteral("Match Case"),
        this
    );
    matchCaseAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    matchCaseAct_->setCheckable(true);
    matchCaseAct_->setChecked(matchCaseEnabled_);
    matchCaseAct_->setStatusTip(QStringLiteral("Match uppercase and lowercase letters exactly in file names"));
    connect(matchCaseAct_, &QAction::toggled, this, &MainWindow::setMatchCaseEnabled);
    addAction(matchCaseAct_);

    // Match Whole Word
    matchWholeWordAct_ = new QAction(
        QIcon::fromTheme(
            QStringLiteral("tools-check-spelling"),
            QIcon::fromTheme(QStringLiteral("format-text-bold"))
        ),
        QStringLiteral("Match Whole Word"),
        this
    );
    matchWholeWordAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    matchWholeWordAct_->setCheckable(true);
    matchWholeWordAct_->setChecked(matchWholeWordEnabled_);
    matchWholeWordAct_->setStatusTip(QStringLiteral("Match complete words in file names"));
    connect(matchWholeWordAct_, &QAction::toggled, this, &MainWindow::setMatchWholeWordEnabled);
    addAction(matchWholeWordAct_);

    // Regex
    regexAct_ = new QAction(
        QIcon::fromTheme(
            QStringLiteral("code-context"),
            QIcon::fromTheme(QStringLiteral("code-variable"))
        ),
        QStringLiteral("Regex"),
        this
    );
    regexAct_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    regexAct_->setCheckable(true);
    regexAct_->setChecked(regexEnabled_);
    regexAct_->setStatusTip(QStringLiteral("Interpret the search text as an RE2 regular expression"));
    regexAct_->setToolTip(
        QStringLiteral(
            "Interpret the search text as an RE2 regular expression.\n"
            "RE2 is fast and safe for interactive search, but does not support every PCRE feature."
        )
    );
    connect(regexAct_, &QAction::toggled, this, &MainWindow::setRegexEnabled);
    addAction(regexAct_);

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

    // View Menu
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("View"));
    viewMenu->addAction(showFiltersDropdownAct_);

    // Search Menu
    searchMenu_ = menuBar()->addMenu(QStringLiteral("Search"));
    searchMenu_->addAction(matchCaseAct_);
    searchMenu_->addAction(matchWholeWordAct_);
    searchMenu_->addAction(regexAct_);
    searchMenu_->addSeparator();
    searchMenu_->addAction(resetSearchAct);
    updateSearchMenuTitle();

    // Filter Menu
    filterMenu_ = menuBar()->addMenu(QStringLiteral("Filter"));
    rebuildFilterMenu();

    // Index Menu
    auto* indexMenu = menuBar()->addMenu(QStringLiteral("Index"));
    indexMenu->addAction(refreshIndexesAct);
    indexMenu->addSeparator();
    indexMenu->addAction(autoRefreshLiveUpdatesAct_);

    // Settings Menu
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("Settings"));
    settingsMenu->addAction(configureAct);

    // Help Menu
    auto* helpMenu = menuBar()->addMenu("Help");
#ifdef KERYTHING_ENABLE_MEMORY_STATS
    helpMenu->addAction(memoryStatsAct);
    helpMenu->addSeparator();
#endif
    helpMenu->addAction(aboutAct);
    // ---------------------

    if (controller_) {
        connect(controller_, &AppController::searchFiltersChanged, this, [this]() {
            rebuildFilterMenu();
            rebuildFilterDropdown();
            updateSearch(searchLine_->text());
        });

        connect(controller_, &AppController::autoRefreshResultsForLiveUpdatesChanged,
                this, [this](bool enabled) {
                    if (!autoRefreshLiveUpdatesAct_) {
                        return;
                    }

                    const QSignalBlocker blocker(autoRefreshLiveUpdatesAct_);
                    autoRefreshLiveUpdatesAct_->setChecked(enabled);
                });

        connect(controller_, &AppController::showFiltersDropdownChanged,
                this, [this](bool enabled) {
                    setFiltersDropdownVisible(enabled);
                });
    }

    updateActionStates();

    // Handle double-click on item in table view to open file
    connect(tableView_, &SearchResultTableView::doubleClicked, this, &MainWindow::openFile);

    // Initialize initial search chip/menu state
    rebuildFilterDropdown();
    updateSearchOptionChips();
    updateSearchMenuTitle();

    if (initialState) {
        applyNewWindowState(*initialState);
    } else {
        // Start with a full list, sorted by name ascending
        tableView_->horizontalHeader()->setSortIndicator(SearchResultColumn::Name, Qt::AscendingOrder);
        lastSortSection_ = SearchResultColumn::Name;
        updateSearch(QString());
    }
}

void MainWindow::updateSearch(const QString &text) {
    if (shouldDeferLiveRefresh()) {
        liveStructuralRefreshDirty_ = true;
        return;
    }

    if (model_) {
        const QString regexHighlightPattern = regexEnabled_
            ? regexHighlightPatternForSearchText(
                text,
                controller_ ? controller_->searchFilters() : std::vector<SearchFilterPreference>{}
            )
            : QString();

        const QStringList highlightTerms =
            regexEnabled_
                ? (regexHighlightPattern.isEmpty()
                    ? QStringList{}
                    : QStringList{ regexHighlightPattern })
                : highlightTermsForSearchText(text);

        model_->setSearchHighlightTerms(
            highlightTerms,
            controller_ && controller_->showHighlightedSearchTerms(),
            matchCaseEnabled_,
            matchWholeWordEnabled_,
            regexEnabled_
        );
    }

    auto start1 = std::chrono::steady_clock::now();

    const std::vector<IndexController::RecordHandle> selectedHandles =
        captureSelectedRecordHandles();
    const std::optional<IndexController::RecordHandle> currentHandle =
        captureCurrentRecordHandle();

    QString rawEffectiveQuery = text;

    if (!activeSearchFilter_.isEmpty()) {
        if (!rawEffectiveQuery.trimmed().isEmpty()) {
            rawEffectiveQuery += QLatin1Char(' ');
        }

        rawEffectiveQuery += activeSearchFilter_;
    }

    const QString effectiveQuery = effectiveSearchQueryWithFilterMacros(
        rawEffectiveQuery,
        controller_ ? controller_->searchFilters() : std::vector<SearchFilterPreference>{}
    );

    std::vector<IndexController::RecordHandle> results;

    if (regexEnabled_) {
        const IndexController::SearchOptions regexOptions{
            .matchCase = matchCaseEnabled_,
            .matchWholeWord = false,
            .useRegex = true
        };

        IndexController::RegexSearchResult regexResult =
            controller_->indexController()->performRegexSearchWithError(
                effectiveQuery.toStdString(),
                regexOptions
            );

        if (regexResult.errorText) {
            auto end1 = std::chrono::steady_clock::now();

            if (model_) {
                model_->setSearchHighlightTerms(
                    {},
                    false,
                    matchCaseEnabled_,
                    false,
                    true
                );
            }

            model_->setSortedSearchResults(
                {},
                tableView_->horizontalHeader()->sortIndicatorSection(),
                tableView_->horizontalHeader()->sortIndicatorOrder()
            );

            restoreSelectedRecordHandles(selectedHandles, currentHandle);

            std::chrono::duration<double> elapsed1 = end1 - start1;

            statusBar()->showMessage(
                QStringLiteral("Invalid regex: %1 (checked in %2s)")
                    .arg(*regexResult.errorText)
                    .arg(elapsed1.count(), 0, 'f', 4)
            );

            return;
        }

        results = std::move(regexResult.records);
    } else {
        results = controller_->indexController()->performTrigramSearch(
            effectiveQuery.toStdString(),
            IndexController::SearchOptions{
                .matchCase = matchCaseEnabled_,
                .matchWholeWord = matchWholeWordEnabled_,
                .useRegex = false
            }
        );
    }

    auto end1 = std::chrono::steady_clock::now();

    auto start2 = std::chrono::steady_clock::now();
    const int sortCol = tableView_->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = tableView_->horizontalHeader()->sortIndicatorOrder();

    model_->setSortedSearchResults(
        std::move(results),
        sortCol,
        sortOrder
    );
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

int MainWindow::hoveredRow() const {
    return hoveredRow_;
}

int MainWindow::resultCount() const
{
    return model_ ? model_->rowCount() : 0;
}

MainWindow::NewWindowState MainWindow::newWindowState() const
{
    const auto* header = tableView_ ? tableView_->horizontalHeader() : nullptr;

    return NewWindowState{
        .activeSearchFilterId = activeSearchFilterId_,
        .activeSearchFilterName = activeSearchFilterName_,
        .activeSearchFilter = activeSearchFilter_,
        .searchText = searchLine_ ? searchLine_->text() : QString(),
        .sortColumn = header ? header->sortIndicatorSection() : SearchResultColumn::Name,
        .sortOrder = header ? header->sortIndicatorOrder() : Qt::AscendingOrder,
        .matchCaseEnabled = matchCaseEnabled_,
        .matchWholeWordEnabled = matchWholeWordEnabled_,
        .regexEnabled = regexEnabled_,
    };
}

void MainWindow::applyNewWindowState(const NewWindowState& state)
{
    activeSearchFilterId_ = state.activeSearchFilterId;
    activeSearchFilterName_ = state.activeSearchFilterName;
    activeSearchFilter_ = state.activeSearchFilter;

    matchCaseEnabled_ = state.matchCaseEnabled;
    matchWholeWordEnabled_ = state.matchWholeWordEnabled;
    regexEnabled_ = state.regexEnabled;

    if (matchCaseAct_) {
        const QSignalBlocker blocker(matchCaseAct_);
        matchCaseAct_->setChecked(matchCaseEnabled_);
    }

    if (matchWholeWordAct_) {
        const QSignalBlocker blocker(matchWholeWordAct_);
        matchWholeWordAct_->setChecked(matchWholeWordEnabled_);
        matchWholeWordAct_->setEnabled(!regexEnabled_);
        matchWholeWordAct_->setStatusTip(
            regexEnabled_
                ? QStringLiteral("Match Whole Word is unavailable while Regex is enabled")
                : QStringLiteral("Match complete words in file names")
        );
    }

    if (regexAct_) {
        const QSignalBlocker blocker(regexAct_);
        regexAct_->setChecked(regexEnabled_);
    }

    rebuildFilterMenu();
    syncFilterDropdownSelection();
    updateSearchOptionChips();
    updateSearchMenuTitle();
    updateSearchLineFilterHint();

    if (tableView_ && tableView_->horizontalHeader()) {
        tableView_->horizontalHeader()->setSortIndicator(state.sortColumn, state.sortOrder);
        lastSortSection_ = state.sortColumn;
    }

    if (!searchLine_) {
        updateSearch(QString());
        return;
    }

    if (searchLine_->text() == state.searchText) {
        updateSearch(state.searchText);
    } else {
        searchLine_->setText(state.searchText);
    }
}

int MainWindow::preferredLiveRefreshIntervalMs() const
{
    const int rows = resultCount();

    if (rows >= 2'000'000) {
        return 2000;
    }

    if (rows >= 1'000'000) {
        return 1500;
    }

    return 1000;
}

bool MainWindow::shouldDeferLiveRefresh() const
{
    if (!isVisible() ||
        isMinimized() ||
        windowState().testFlag(Qt::WindowMinimized)) {
        return true;
        }

    const QWindow* nativeWindow = windowHandle();

    if (!nativeWindow) {
        return false;
    }

    const QWindow::Visibility visibility = nativeWindow->visibility();

    if (visibility == QWindow::Hidden ||
        visibility == QWindow::Minimized) {
        return true;
        }

    return !nativeWindow->isExposed();
}

void MainWindow::refresh() {
    if (shouldDeferLiveRefresh()) {
        markLiveStructuralRefreshDirty();
        return;
    }

    liveStructuralRefreshDirty_ = false;
    liveMetadataRefreshDirty_ = false;

    updateSearch(searchLine_->text());
}

void MainWindow::refreshLiveMetadata()
{
    if (shouldDeferLiveRefresh()) {
        markLiveMetadataRefreshDirty();
        return;
    }

    liveMetadataRefreshDirty_ = false;

    if (!model_ || !tableView_ || model_->rowCount() <= 0) {
        return;
    }

    const int sortColumn = tableView_->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = tableView_->horizontalHeader()->sortIndicatorOrder();

    if (sortColumn == SearchResultColumn::Size ||
        sortColumn == SearchResultColumn::DateModified) {
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

void MainWindow::refreshSearchHighlighting()
{
    if (!model_ || !searchLine_) {
        return;
    }

    const QString searchText = searchLine_->text();
    const QString regexHighlightPattern = regexEnabled_
        ? regexHighlightPatternForSearchText(
            searchText,
            controller_ ? controller_->searchFilters() : std::vector<SearchFilterPreference>{}
        )
        : QString();

    const QStringList highlightTerms =
        regexEnabled_
            ? (regexHighlightPattern.isEmpty()
                ? QStringList{}
                : QStringList{ regexHighlightPattern })
            : highlightTermsForSearchText(searchText);

    model_->setSearchHighlightTerms(
        highlightTerms,
        controller_ && controller_->showHighlightedSearchTerms(),
        matchCaseEnabled_,
        matchWholeWordEnabled_,
        regexEnabled_
    );

    if (tableView_ && tableView_->viewport()) {
        tableView_->viewport()->update();
    }
}

void MainWindow::markLiveStructuralRefreshDirty()
{
    liveStructuralRefreshDirty_ = true;
}

void MainWindow::markLiveMetadataRefreshDirty()
{
    liveMetadataRefreshDirty_ = true;
}

void MainWindow::trimSortScratch()
{
    if (model_) {
        model_->trimSortScratch();
    }
}

void MainWindow::resetSearchStateAndFocus()
{
    activeSearchFilterId_.clear();
    activeSearchFilterName_.clear();
    activeSearchFilter_.clear();

    matchCaseEnabled_ = false;
    matchWholeWordEnabled_ = false;
    regexEnabled_ = false;

    if (matchCaseAct_) {
        const QSignalBlocker blocker(matchCaseAct_);
        matchCaseAct_->setChecked(false);
    }

    if (matchWholeWordAct_) {
        const QSignalBlocker blocker(matchWholeWordAct_);
        matchWholeWordAct_->setChecked(false);
        matchWholeWordAct_->setEnabled(true);
        matchWholeWordAct_->setStatusTip(QStringLiteral("Match complete words in file names"));
    }

    if (regexAct_) {
        const QSignalBlocker blocker(regexAct_);
        regexAct_->setChecked(false);
    }

    rebuildFilterMenu();
    syncFilterDropdownSelection();
    updateSearchOptionChips();
    updateSearchMenuTitle();
    updateSearchLineFilterHint();

    if (!searchLine_) {
        updateSearch(QString());
        return;
    }

    if (searchLine_->text().isEmpty()) {
        updateSearch(QString());
    } else {
        searchLine_->clear();
    }

    searchLine_->setFocus();
    showTemporaryStatus(QStringLiteral("Search reset"), 2500);
}

void MainWindow::openNewWindowFromThisWindow()
{
    if (controller_) {
        controller_->openNewWindow(this);
    }
}

bool MainWindow::event(QEvent* event)
{
    const bool handled = QMainWindow::event(event);

    switch (event->type()) {
        case QEvent::Show:
        case QEvent::WindowActivate:
        case QEvent::Expose:
            refreshDirtyLiveUpdatesIfNeeded();
            break;

        default:
            break;
    }

    return handled;
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);

    if (event->type() != QEvent::WindowStateChange) {
        return;
    }

    refreshDirtyLiveUpdatesIfNeeded();
}

void MainWindow::rebuildFilterMenu()
{
    if (!filterMenu_) {
        return;
    }

    filterMenu_->clear();

    auto* filterActionGroup = new QActionGroup(filterMenu_);
    filterActionGroup->setExclusive(true);

    auto* allFilterAction = new QAction(QStringLiteral("All"), filterMenu_);
    allFilterAction->setCheckable(true);
    allFilterAction->setChecked(activeSearchFilterId_.isEmpty());
    filterActionGroup->addAction(allFilterAction);
    filterMenu_->addAction(allFilterAction);

    connect(allFilterAction, &QAction::triggered, this, [this]() {
        applySearchFilter(QString(), QString(), QString());
    });

    auto* foldersFilterAction = new QAction(QStringLiteral("Folders"), filterMenu_);
    foldersFilterAction->setCheckable(true);
    foldersFilterAction->setStatusTip(QStringLiteral("folder:"));
    foldersFilterAction->setToolTip(QStringLiteral("folder:"));
    foldersFilterAction->setChecked(activeSearchFilterId_ == QStringLiteral("builtin-folders"));
    filterActionGroup->addAction(foldersFilterAction);
    filterMenu_->addAction(foldersFilterAction);

    connect(foldersFilterAction, &QAction::triggered, this, [this]() {
        applySearchFilter(
            QStringLiteral("builtin-folders"),
            QStringLiteral("Folders"),
            QStringLiteral("folder:")
        );
    });

    auto* filesFilterAction = new QAction(QStringLiteral("Files"), filterMenu_);
    filesFilterAction->setCheckable(true);
    filesFilterAction->setStatusTip(QStringLiteral("files:"));
    filesFilterAction->setToolTip(QStringLiteral("files:"));
    filesFilterAction->setChecked(activeSearchFilterId_ == QStringLiteral("builtin-files"));
    filterActionGroup->addAction(filesFilterAction);
    filterMenu_->addAction(filesFilterAction);

    connect(filesFilterAction, &QAction::triggered, this, [this]() {
        applySearchFilter(
            QStringLiteral("builtin-files"),
            QStringLiteral("Files"),
            QStringLiteral("files:")
        );
    });

    filterMenu_->addSeparator();

    bool activeFilterStillExists =
        activeSearchFilterId_.isEmpty() ||
        activeSearchFilterId_ == QStringLiteral("builtin-folders") ||
        activeSearchFilterId_ == QStringLiteral("builtin-files");

    const std::vector<SearchFilterPreference> filters =
        controller_ ? controller_->searchFilters() : std::vector<SearchFilterPreference>{};

    for (const SearchFilterPreference& filter : filters) {
        auto* filterAction = new QAction(menuTextFromUserText(filter.name), filterMenu_);
        filterAction->setCheckable(true);

        const QString macroTip = filter.macro.trimmed().isEmpty()
            ? QString()
            : QStringLiteral("\nMacro: %1:").arg(filter.macro.trimmed());

        filterAction->setStatusTip(filter.query + macroTip);
        filterAction->setToolTip(filter.query + macroTip);

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
        allFilterAction->setChecked(true);
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
    rebuildFilterDropdown();
}

void MainWindow::rebuildFilterDropdown()
{
    if (!filterDropdown_) {
        return;
    }

    const QSignalBlocker blocker(filterDropdown_);

    filterDropdown_->clear();

    filterDropdown_->addItem(QStringLiteral("All"));
    filterDropdown_->setItemData(0, QString(), FilterDropdownIdRole);
    filterDropdown_->setItemData(0, QString(), FilterDropdownNameRole);
    filterDropdown_->setItemData(0, QString(), FilterDropdownQueryRole);
    filterDropdown_->setItemData(0, FilterDropdownKindFilter, FilterDropdownKindRole);

    filterDropdown_->addItem(QStringLiteral("Folders"));
    filterDropdown_->setItemData(1, QStringLiteral("builtin-folders"), FilterDropdownIdRole);
    filterDropdown_->setItemData(1, QStringLiteral("Folders"), FilterDropdownNameRole);
    filterDropdown_->setItemData(1, QStringLiteral("folder:"), FilterDropdownQueryRole);
    filterDropdown_->setItemData(1, QStringLiteral("folder:"), Qt::ToolTipRole);
    filterDropdown_->setItemData(1, FilterDropdownKindFilter, FilterDropdownKindRole);

    filterDropdown_->addItem(QStringLiteral("Files"));
    filterDropdown_->setItemData(2, QStringLiteral("builtin-files"), FilterDropdownIdRole);
    filterDropdown_->setItemData(2, QStringLiteral("Files"), FilterDropdownNameRole);
    filterDropdown_->setItemData(2, QStringLiteral("files:"), FilterDropdownQueryRole);
    filterDropdown_->setItemData(2, QStringLiteral("files:"), Qt::ToolTipRole);
    filterDropdown_->setItemData(2, FilterDropdownKindFilter, FilterDropdownKindRole);

    const std::vector<SearchFilterPreference> filters =
        controller_ ? controller_->searchFilters() : std::vector<SearchFilterPreference>{};

    for (const SearchFilterPreference& filter : filters) {
        filterDropdown_->addItem(filter.name);

        const QString macroTip = filter.macro.trimmed().isEmpty()
            ? filter.query
            : QStringLiteral("%1\nMacro: %2:").arg(filter.query, filter.macro.trimmed());

        const int index = filterDropdown_->count() - 1;
        filterDropdown_->setItemData(index, filter.id, FilterDropdownIdRole);
        filterDropdown_->setItemData(index, filter.name, FilterDropdownNameRole);
        filterDropdown_->setItemData(index, filter.query, FilterDropdownQueryRole);
        filterDropdown_->setItemData(index, macroTip, Qt::ToolTipRole);
        filterDropdown_->setItemData(index, FilterDropdownKindFilter, FilterDropdownKindRole);
    }

    filterDropdown_->addItem(QStringLiteral("  Manage Filters..."));

    const int manageIndex = filterDropdown_->count() - 1;
    filterDropdown_->setItemData(manageIndex, FilterDropdownKindManageFilters, FilterDropdownKindRole);
    filterDropdown_->setItemData(
        manageIndex,
        QStringLiteral("Open the Filters page in Preferences"),
        Qt::ToolTipRole
    );

    QFont manageFont = filterDropdown_->font();
    manageFont.setItalic(true);
    filterDropdown_->setItemData(manageIndex, manageFont, Qt::FontRole);
    filterDropdown_->setItemData(
        manageIndex,
        filterDropdown_->palette().brush(QPalette::PlaceholderText),
        Qt::ForegroundRole
    );

    syncFilterDropdownSelection();
}

void MainWindow::setFiltersDropdownVisible(bool visible)
{
    if (showFiltersDropdownAct_ && showFiltersDropdownAct_->isChecked() != visible) {
        const QSignalBlocker blocker(showFiltersDropdownAct_);
        showFiltersDropdownAct_->setChecked(visible);
    }

    if (filterDropdown_) {
        filterDropdown_->setVisible(visible);
    }
}

void MainWindow::syncFilterDropdownSelection()
{
    if (!filterDropdown_) {
        return;
    }

    for (int index = 0; index < filterDropdown_->count(); ++index) {
        if (filterDropdown_->itemData(index, FilterDropdownIdRole).toString() == activeSearchFilterId_) {
            filterDropdown_->setCurrentIndex(index);
            return;
        }
    }

    filterDropdown_->setCurrentIndex(0);
}

void MainWindow::applySearchFilter(const QString& filterId, const QString& filterName, const QString& queryFragment)
{
    activeSearchFilterId_ = filterId;
    activeSearchFilterName_ = filterName;
    activeSearchFilter_ = queryFragment;
    rebuildFilterMenu();
    syncFilterDropdownSelection();
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
        searchLine_->setPlaceholderText(
            regexEnabled_
                ? QStringLiteral("Search with regex...")
                : QStringLiteral("Search...")
        );
        searchLine_->setToolTip(
            regexEnabled_
                ? QStringLiteral(
                    "Search indexed file names using RE2 regular expressions.\n"
                    "The Match Case option controls regex case sensitivity.\n\n"
                    "Examples:\n"
                    "  \\.cpp$                  files ending in .cpp\n"
                    "  screenshot[0-9]{4}      screenshot followed by four digits\n"
                    "  holiday\\.(png|jpg)$     holiday image files\n"
                    "  .*(draft|final)\\.pdf$   draft or final PDFs\n\n"
                    "Filters such as files:, folders:, ext:mp4, ext:wav;mp3, or a custom filter macro such as audio: can still be used."
                )
                : QStringLiteral(
                    "Search indexed file names. You can use filters such as files:, folders:, ext:mp4, ext:wav;mp3, or a custom filter macro such as audio:."
                )
        );
        updateFilterChip();
        updateFilterMenuTitle();
        return;
    }

    if (activeSearchFilterId_ == QStringLiteral("builtin-folders")) {
        searchLine_->setPlaceholderText(
            regexEnabled_
                ? QStringLiteral("Search folders with regex...")
                : QStringLiteral("Search folders...")
        );
    } else if (activeSearchFilterId_ == QStringLiteral("builtin-files")) {
        searchLine_->setPlaceholderText(
            regexEnabled_
                ? QStringLiteral("Search files with regex...")
                : QStringLiteral("Search files...")
        );
    } else {
        searchLine_->setPlaceholderText(
            regexEnabled_
                ? QStringLiteral("Search files in %1 with regex...").arg(activeSearchFilterName_)
                : QStringLiteral("Search files in %1...").arg(activeSearchFilterName_)
        );
    }

    searchLine_->setToolTip(
        regexEnabled_
            ? QStringLiteral(
                "Active filter: %1\n"
                "Query fragment: %2\n\n"
                "Search text is interpreted as an RE2 regular expression."
            ).arg(
                activeSearchFilterName_,
                activeSearchFilter_
            )
            : QStringLiteral(
                "Active filter: %1\n"
                "Query fragment: %2"
            ).arg(
                activeSearchFilterName_,
                activeSearchFilter_
            )
    );

    updateFilterChip();
    updateFilterMenuTitle();
}

void MainWindow::updateFilterChip()
{
    if (!filterChip_) {
        updateChipSpacing();
        return;
    }

    if (activeSearchFilter_.isEmpty()) {
        filterChip_->hide();
        filterChip_->setText(QString());
        filterChip_->setToolTip(QString());
        filterChip_->setStatusTip(QStringLiteral("No filter is active"));
        updateChipSpacing();
        return;
    }

    filterChip_->setText(QStringLiteral("%1  ×").arg(menuTextFromUserText(activeSearchFilterName_)));
    filterChip_->setToolTip(
        QStringLiteral(
            "Active filter: %1\n"
            "Query fragment: %2\n\n"
            "Click to clear this filter."
        ).arg(
            activeSearchFilterName_,
            activeSearchFilter_
        )
    );
    filterChip_->setStatusTip(QStringLiteral("Click to clear filter: %1").arg(activeSearchFilterName_));
    filterChip_->show();

    updateChipSpacing();
}

void MainWindow::updateFilterMenuTitle()
{
    if (!filterMenu_) {
        return;
    }

    if (activeSearchFilter_.isEmpty()) {
        filterMenu_->setTitle(QStringLiteral("Filter"));
        filterMenu_->menuAction()->setToolTip(QString());
        filterMenu_->menuAction()->setStatusTip(QString());
        return;
    }

    filterMenu_->setTitle(QStringLiteral("Filter ●"));
    filterMenu_->menuAction()->setToolTip(
        QStringLiteral("Active filter: %1").arg(activeSearchFilterName_)
    );
    filterMenu_->menuAction()->setStatusTip(
        QStringLiteral("Active filter: %1").arg(activeSearchFilterName_)
    );
}

void MainWindow::updateSearchOptionChips()
{
    if (matchCaseChip_) {
        if (matchCaseEnabled_) {
            matchCaseChip_->setText(QStringLiteral("Case  ×"));
            matchCaseChip_->setToolTip(
                QStringLiteral(
                    "Search option enabled: Match Case\n\n"
                    "Click to turn off Match Case."
                )
            );
            matchCaseChip_->setStatusTip(QStringLiteral("Click to turn off Match Case"));
            matchCaseChip_->show();
        } else {
            matchCaseChip_->hide();
            matchCaseChip_->setText(QString());
            matchCaseChip_->setToolTip(QString());
            matchCaseChip_->setStatusTip(QStringLiteral("Match Case is off"));
        }
    }

    if (matchWholeWordChip_) {
        if (matchWholeWordEnabled_) {
            matchWholeWordChip_->setText(QStringLiteral("Whole Word  ×"));
            matchWholeWordChip_->setToolTip(
                QStringLiteral(
                    "Search option enabled: Match Whole Word\n\n"
                    "Click to turn off Match Whole Word."
                )
            );
            matchWholeWordChip_->setStatusTip(QStringLiteral("Click to turn off Match Whole Word"));
            matchWholeWordChip_->show();
        } else {
            matchWholeWordChip_->hide();
            matchWholeWordChip_->setText(QString());
            matchWholeWordChip_->setToolTip(QString());
            matchWholeWordChip_->setStatusTip(QStringLiteral("Match Whole Word is off"));
        }
    }

    if (regexChip_) {
        if (regexEnabled_) {
            regexChip_->setText(QStringLiteral("Regex  ×"));
            regexChip_->setToolTip(
                QStringLiteral(
                    "Search option enabled: Regex\n\n"
                    "Click to turn off Regex."
                )
            );
            regexChip_->setStatusTip(QStringLiteral("Click to turn off Regex"));
            regexChip_->show();
        } else {
            regexChip_->hide();
            regexChip_->setText(QString());
            regexChip_->setToolTip(QString());
            regexChip_->setStatusTip(QStringLiteral("Regex is off"));
        }
    }

    updateChipSpacing();
}

void MainWindow::updateSearchMenuTitle()
{
    if (!searchMenu_) {
        return;
    }

    QStringList activeOptions;

    if (matchCaseEnabled_) {
        activeOptions << QStringLiteral("Match Case");
    }

    if (matchWholeWordEnabled_) {
        activeOptions << QStringLiteral("Match Whole Word");
    }

    if (regexEnabled_) {
        activeOptions << QStringLiteral("Regex");
    }

    if (activeOptions.isEmpty()) {
        searchMenu_->setTitle(QStringLiteral("Search"));
        searchMenu_->menuAction()->setToolTip(QString());
        searchMenu_->menuAction()->setStatusTip(QString());
        return;
    }

    const QString activeText = activeOptions.join(QStringLiteral(", "));

    searchMenu_->setTitle(QStringLiteral("Search ●"));
    searchMenu_->menuAction()->setToolTip(
        QStringLiteral("Active search options: %1").arg(activeText)
    );
    searchMenu_->menuAction()->setStatusTip(
        QStringLiteral("Active search options: %1").arg(activeText)
    );
}

void MainWindow::updateChipSpacing()
{
    const QList<QToolButton*> chips = {
        matchCaseChip_,
        matchWholeWordChip_,
        regexChip_,
        filterChip_
    };

    const int chipHeight = std::max(
        12,
        (statusLabel_ ? statusLabel_->fontMetrics().height() : fontMetrics().height()) - 2
    );

    bool anyShownChip = false;

    for (QToolButton* chip : chips) {
        if (!chip) {
            continue;
        }

        anyShownChip = anyShownChip || !chip->isHidden();

        chip->setStyleSheet(QStringLiteral(
            "QToolButton {"
            "  background: palette(midlight);"
            "  color: palette(window-text);"
            "  border: 1px solid palette(midlight);"
            "  border-radius: 4px;"
            "  padding: 0px 7px;"
            "  margin-left: 0px;"
            "  margin-right: 0px;"
            "  min-height: %1px;"
            "  max-height: %1px;"
            "}"
            "QToolButton:hover {"
            "  background: palette(midlight);"
            "  color: palette(window-text);"
            "  border: 1px solid palette(highlight);"
            "}"
            "QToolButton:pressed {"
            "  background: palette(alternate-base);"
            "  color: palette(window-text);"
            "  border: 1px solid palette(highlight);"
            "}"
        ).arg(chipHeight));
    }

    if (chipContainer_) {
        chipContainer_->setVisible(anyShownChip);
    }

    statusLabel_->setStyleSheet(
        anyShownChip
            ? QStringLiteral(
                "QLabel {"
                "  margin-right: 0px;"
                "}"
            )
            : QStringLiteral(
                "QLabel {"
                "  margin-right: 6px;"
                "}"
            )
    );
}

void MainWindow::refreshDirtyLiveUpdatesIfNeeded()
{
    if (shouldDeferLiveRefresh()) {
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

void MainWindow::handleSortSectionClicked(int section)
{
    if (!tableView_ || !model_ || !controller_) {
        return;
    }

    auto* header = tableView_->horizontalHeader();
    if (!header) {
        return;
    }

    const bool switchedColumn = section != lastSortSection_;
    lastSortSection_ = section;

    if (!switchedColumn) {
        return;
    }

    const bool shouldPreferDescending =
        (section == SearchResultColumn::Size && controller_->sortSizeDescendingFirst()) ||
        (section == SearchResultColumn::DateModified && controller_->sortDateDescendingFirst());

    if (!shouldPreferDescending) {
        return;
    }

    /*
     * Qt's default sorting makes the first click on a newly selected column
     * ascending. For Size and Date Modified, descending is usually the desired
     * initial direction, so correct only that first click after switching columns.
     *
     * Subsequent clicks on the same column are left alone so normal toggling works.
     */
    if (header->sortIndicatorSection() == section &&
        header->sortIndicatorOrder() == Qt::AscendingOrder) {
        header->setSortIndicator(section, Qt::DescendingOrder);
        model_->sort(section, Qt::DescendingOrder);
    }
}

void MainWindow::setMatchCaseEnabled(bool enabled)
{
    if (matchCaseEnabled_ == enabled) {
        return;
    }

    matchCaseEnabled_ = enabled;

    if (matchCaseAct_ && matchCaseAct_->isChecked() != enabled) {
        const QSignalBlocker blocker(matchCaseAct_);
        matchCaseAct_->setChecked(enabled);
    }

    updateSearchOptionChips();
    updateSearchMenuTitle();
    updateSearch(searchLine_ ? searchLine_->text() : QString());
}

void MainWindow::setMatchWholeWordEnabled(bool enabled)
{
    if (regexEnabled_ && enabled) {
        return;
    }

    if (matchWholeWordEnabled_ == enabled) {
        return;
    }

    matchWholeWordEnabled_ = enabled;

    if (matchWholeWordAct_ && matchWholeWordAct_->isChecked() != enabled) {
        const QSignalBlocker blocker(matchWholeWordAct_);
        matchWholeWordAct_->setChecked(enabled);
    }

    updateSearchOptionChips();
    updateSearchMenuTitle();
    updateSearch(searchLine_ ? searchLine_->text() : QString());
}

void MainWindow::setRegexEnabled(bool enabled)
{
    if (regexEnabled_ == enabled) {
        return;
    }

    regexEnabled_ = enabled;

    if (regexAct_ && regexAct_->isChecked() != enabled) {
        const QSignalBlocker blocker(regexAct_);
        regexAct_->setChecked(enabled);
    }

    if (regexEnabled_ && matchWholeWordEnabled_) {
        matchWholeWordEnabled_ = false;

        if (matchWholeWordAct_) {
            const QSignalBlocker blocker(matchWholeWordAct_);
            matchWholeWordAct_->setChecked(false);
        }
    }

    if (matchWholeWordAct_) {
        matchWholeWordAct_->setEnabled(!regexEnabled_);
        matchWholeWordAct_->setStatusTip(
            regexEnabled_
                ? QStringLiteral("Match Whole Word is unavailable while Regex is enabled")
                : QStringLiteral("Match complete words in file names")
        );
    }

    updateSearchOptionChips();
    updateSearchMenuTitle();
    updateSearchLineFilterHint();
    refreshSearchHighlighting();
    updateSearch(searchLine_ ? searchLine_->text() : QString());
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

#ifdef KERYTHING_ENABLE_MEMORY_STATS
void MainWindow::showMemoryStats()
{
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Kerything Memory Statistics"));
    dialog->resize(1000, 750);

    auto* layout = new QVBoxLayout(dialog);

    auto* textEdit = new QPlainTextEdit(dialog);
    textEdit->setReadOnly(true);
    textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    textEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    auto buildStatsText = [this]() {
        QString text;
        QTextStream out(&text);

        out << "Kerything Memory Statistics\n";
        out << "===========================\n\n";
        out << "Notes:\n";
        out << "  - Vector figures use capacity, not just size.\n";
        out << "  - Hash-map payload figures exclude STL allocator/node overhead.\n";
        out << "  - Hash-map node-overhead estimates are rough what-if ranges, not exact measurements.\n";
        out << "  - glibc mallinfo2 figures describe the allocator, not individual containers.\n";
        out << "  - /proc smaps_rollup is usually the best breakdown of RSS/PSS/private memory.\n";
        out << "  - Process RSS is the best top-level number; category subtotals are attribution aids.\n";
        out << "  - Some temporary allocations from sorting/searching may not be retained here.\n\n";

        if (controller_ && controller_->indexController()) {
            out << controller_->indexController()->memoryStatsText();
        } else {
            out << "IndexController: unavailable\n";
        }

        out << "\n";

        if (model_) {
            out << model_->memoryStatsText();
        } else {
            out << "FileModel: unavailable\n";
        }

        return text;
    };

    textEdit->setPlainText(buildStatsText());

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), dialog);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), dialog);

    buttonLayout->addWidget(refreshButton);
    buttonLayout->addWidget(closeButton);

    layout->addWidget(textEdit);
    layout->addLayout(buttonLayout);

    connect(refreshButton, &QPushButton::clicked, dialog, [textEdit, buildStatsText]() {
        textEdit->setPlainText(buildStatsText());
    });

    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);

    dialog->show();
}
#endif

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

    if (index.column() == SearchResultColumn::Path &&
        controller_ &&
        controller_->showInFileManagerOnPathDoubleClick()) {
        openSelectedLocation();
        return;
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
        const QString fileName = model_->data(
            model_->index(index.row(), SearchResultColumn::Name),
            Qt::DisplayRole
        ).toString();

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

        const QString displayPath = model_->data(
            model_->index(index.row(), SearchResultColumn::Path),
            Qt::DisplayRole
        ).toString();

        const QString fileName = model_->data(
            model_->index(index.row(), SearchResultColumn::Name),
            Qt::DisplayRole
        ).toString();

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

        const QString displayPath = model_->data(
            model_->index(index.row(), SearchResultColumn::Path),
            Qt::DisplayRole
        ).toString();

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
