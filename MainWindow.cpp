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
#include <QComboBox>
#include <QMimeData>
#include <QClipboard>
#include <QShortcut>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QMimeType>
#include <QTimer>
#include <QStyledItemDelegate>
#include <QEvent>
#include <QPainter>
#include <QDateTime>
#include <QLocale>
#include <QSettings>
#include <QtDBus/QDBusConnection>
#include <KFileItemActions>
#include <KFileItemListProperties>
#include <KFileItem>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KTerminalLauncherJob>
#include <KService>
#include <KApplicationTrader>
#include <KAboutData>
#include <KAboutApplicationDialog>
#include <chrono>
#include <algorithm>
#include <numeric>
#include "MainWindow.h"
#include "FileModel.h"
#include "GuiUtils.h"
#include "PartitionDialog.h"
#include "SettingsDialog.h"
#include "ScannerManager.h"
#include "RemoteFileModel.h"
#include "DbusIndexerClient.h"

namespace {
    class HoverRowDelegate final : public QStyledItemDelegate {
    public:
        explicit HoverRowDelegate(const MainWindow* owner)
            : QStyledItemDelegate(const_cast<MainWindow*>(owner)), m_owner(owner) {}

        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
        {
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);

            const bool isHoveredRow = (m_owner && index.row() == m_owner->hoveredRow());
            const bool isSelected = (opt.state & QStyle::State_Selected);

            if (isHoveredRow && !isSelected) {
                // alternatingRowColors: prevent "alternate row" painting from overriding our hover
                opt.features &= ~QStyleOptionViewItem::Alternate;

                QColor hover = opt.palette.color(QPalette::Highlight);
                hover.setAlpha(40);
                painter->fillRect(opt.rect, hover);

                // Also ensure the style doesn't paint another background on top
                opt.backgroundBrush = Qt::NoBrush;
            }

            QStyledItemDelegate::paint(painter, opt, index);
        }

    private:
        const MainWindow* m_owner = nullptr;
    };
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Restore persisted UI preferences (e.g. device scope selection).
    loadUiSettings();

    auto *centralWidget = new QWidget(this);
    auto *layout = new QVBoxLayout(centralWidget);

    searchLine = new QLineEdit(centralWidget);
    searchLine->setPlaceholderText("Search files...");
    searchLine->setClearButtonEnabled(true);

    // Add magnifying glass icon to the search bar
    searchLine->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);

    // --- Device Scope + Search Row ---
    auto* topRow = new QHBoxLayout();
    layout->addLayout(topRow);

    m_deviceScopeCombo = new QComboBox(centralWidget);
    m_deviceScopeCombo->setMinimumContentsLength(22);
    m_deviceScopeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_deviceScopeCombo->setToolTip(QStringLiteral("Limit search to a specific indexed device"));
    topRow->addWidget(m_deviceScopeCombo);

    topRow->addWidget(searchLine, 1);

    connect(m_deviceScopeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (!m_useDaemonSearch || !remoteModel || !m_deviceScopeCombo) return;

        const QString deviceId = m_deviceScopeCombo->itemData(idx).toString();
        m_selectedDeviceScopeId = deviceId;

        saveUiSettings();

        if (deviceId.isEmpty()) {
            remoteModel->setDeviceIds({});
        } else {
            remoteModel->setDeviceIds(QStringList{deviceId});

            const bool present = m_deviceScopeCombo->itemData(idx, Qt::UserRole + 1).toBool();
            if (!present) {
                statusBar()->showMessage(
                    QStringLiteral("Index available for this device, but it is not currently attached."),
                    5000
                );
            }
        }

        // Keep the existing query text; just refresh results under the new scope.
        updateSearch(searchLine ? searchLine->text() : QString());
    });
    // --- End Device Scope + Search Row ---

    // --- Burger Menu Setup ---
    auto *menu = new QMenu(this);

    m_settingsAction = new QAction(QIcon::fromTheme("settings-configure"), "Settings", this);
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::openSettings);
    menu->addAction(m_settingsAction);

    menu->addSeparator();

    m_changePartitionAction = new QAction(QIcon::fromTheme("drive-harddisk"), "Change Partition", this);
    connect(m_changePartitionAction, &QAction::triggered, this, &MainWindow::changePartition);
    m_changePartitionAction->setShortcut(QKeySequence::Open);
    menu->addAction(m_changePartitionAction);
    addAction(m_changePartitionAction); // Register with window for shortcuts

    m_rescanPartitionAction = new QAction(QIcon::fromTheme("view-refresh"), "Rescan Partition", this);
    connect(m_rescanPartitionAction, &QAction::triggered, this, &MainWindow::rescanPartition);
    m_rescanPartitionAction->setShortcut(QKeySequence::Refresh);
    menu->addAction(m_rescanPartitionAction);
    addAction(m_rescanPartitionAction); // Register with window for shortcuts

    menu->addSeparator();

    auto *aboutAct = new QAction(QIcon::fromTheme("kerything"), "About Kerything", this);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
    menu->addAction(aboutAct);

    auto *quitAct = new QAction(QIcon::fromTheme("application-exit"), "Quit", this);
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, qApp, &QCoreApplication::quit);
    menu->addAction(quitAct);
    addAction(quitAct); // Register with window for shortcuts

    // Add the menu to a button inside the search line
    auto *menuAction = searchLine->addAction(QIcon::fromTheme("application-menu"), QLineEdit::TrailingPosition);
    connect(menuAction, &QAction::triggered, [this, menu, menuAction]() {
        // Show the menu just below the icon
        menu->exec(QCursor::pos());
    });
    // ---------------------

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
    tableView->setWordWrap(false);

    // Full-row hover
    tableView->setItemDelegate(new HoverRowDelegate(this));
    tableView->setMouseTracking(true);
    tableView->viewport()->setMouseTracking(true);
    connect(tableView, &QAbstractItemView::entered, this, &MainWindow::onTableHovered);
    connect(tableView, &QAbstractItemView::viewportEntered, this, &MainWindow::onTableViewportHovered);
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

    // Daemon Status Bar
    daemonStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(daemonStatusLabel);

    // Create D-Bus client
    m_dbus = std::make_unique<DbusIndexerClient>(this);

    // Watch daemon presence on the system bus (event-driven "connected/disconnected")
    constexpr const char* kService = "net.reikooters.Kerything1";
    m_daemonWatcher = new QDBusServiceWatcher(
        QString::fromLatin1(kService),
        QDBusConnection::systemBus(),
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration,
        this
    );
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceRegistered,
            this, &MainWindow::onDaemonServiceRegistered);
    connect(m_daemonWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &MainWindow::onDaemonServiceUnregistered);

    // Initial daemon setup
    {
        QString err;
        auto ping = m_dbus->ping(&err);
        if (ping) {
            // Enable daemon-backed search mode
            m_useDaemonSearch = true;
            remoteModel = new RemoteFileModel(m_dbus.get(), this);
            tableView->setModel(remoteModel);

            // Surface daemon paging failures to the user (without making the UI noisy).
            connect(remoteModel, &RemoteFileModel::transientError, this, [this](const QString& msg) {
                statusBar()->showMessage(msg, 5000);
            });

            // Update baseline status with search results count from daemon when search completes
            connect(remoteModel, &RemoteFileModel::searchCompleted, this,
                    [this](quint64 totalHits, double elapsedSeconds) {
                        showBaselineStatus(
                            QString("%L1 objects found (daemon) in %2s")
                                .arg(totalHits)
                                .arg(elapsedSeconds, 0, 'f', 4)
                        );
                    });

            tableView->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);
            remoteModel->setSort(0, Qt::AscendingOrder);

            // Listen for index updates to refresh UI hints / counts
            constexpr const char* kPath    = "/net/reikooters/Kerything1";
            constexpr const char* kIface   = "net.reikooters.Kerything1.Indexer";

            const bool ok = QDBusConnection::systemBus().connect(
                QString::fromLatin1(kService),
                QString::fromLatin1(kPath),
                QString::fromLatin1(kIface),
                QStringLiteral("DeviceIndexUpdated"),
                this,
                SLOT(onDeviceIndexUpdated(QString,quint64,quint64))
            );

            if (!ok) {
                daemonStatusLabel->setText("Warning: failed to connect to daemon DeviceIndexUpdated signal.");
            }

            // Permanent daemon/index summary in the label
            refreshDaemonStatusLabel();
            refreshDeviceScopeCombo();

            // Initial: if nothing is indexed yet, tell the user what to do.
            QString idxErr;
            auto indexedOpt = m_dbus->listIndexedDevices(&idxErr);
            if (indexedOpt && indexedOpt->isEmpty()) {
                statusBar()->showMessage("No partitions are indexed yet. Use “Change Partition” to index one.", 0);
            } else {
                // Start with empty query = "everything"
                updateSearch("");
            }
        } else {
            //daemonStatusLabel->setText(QStringLiteral("Daemon: disconnected • live updates paused"));
            daemonStatusLabel->setText(QString("Daemon: disconnected • live updates paused (%1)").arg(err));
        }
    }

    // Debounce index update refreshes (fanotify-friendly)
    m_indexUpdateDebounceTimer = new QTimer(this);
    m_indexUpdateDebounceTimer->setSingleShot(true);
    m_indexUpdateDebounceTimer->setInterval(250);

    connect(m_indexUpdateDebounceTimer, &QTimer::timeout, this, [this]() {
        if (!m_indexUpdatePending) return;
        m_indexUpdatePending = false;

        if (m_useDaemonSearch && remoteModel) {
            remoteModel->invalidate();
            statusBar()->showMessage(
                QString("%L1 objects found").arg(remoteModel->totalHits()),
                0
            );
        }
    });

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

    updateLegacyPartitionActions();

    // Only open partition dialog in local mode
    if (!m_useDaemonSearch) {
        // Keep the initial search stats in the *message* area,
        // so we don't overwrite the permanent daemon label.
        statusBar()->showMessage("Select a partition to begin.", 0);

        // At the end of the constructor, trigger the partition selection
        // We use a QTimer to call it after the window is shown
        QTimer::singleShot(0, this, &MainWindow::changePartition);
    }
}

void MainWindow::loadUiSettings() {
    QSettings s;
    s.beginGroup(QStringLiteral("ui"));
    m_selectedDeviceScopeId = s.value(QStringLiteral("deviceScopeDeviceId"), QString()).toString();
    s.endGroup();
}

void MainWindow::saveUiSettings() const {
    QSettings s;
    s.beginGroup(QStringLiteral("ui"));
    s.setValue(QStringLiteral("deviceScopeDeviceId"), m_selectedDeviceScopeId);
    s.endGroup();
}

void MainWindow::refreshDaemonStatusLabel() {
    if (!daemonStatusLabel) return;

    if (!m_dbus) {
        daemonStatusLabel->setText(QStringLiteral("Daemon: disconnected • live updates paused"));
        daemonStatusLabel->setToolTip(QString());
        return;
    }

    QString pingErr;
    const auto ping = m_dbus->ping(&pingErr);
    if (!ping) {
        daemonStatusLabel->setText(QString("Daemon: disconnected • live updates paused (%1)").arg(pingErr));
        daemonStatusLabel->setToolTip(QString());
        return;
    }

    QString idxErr;
    const auto indexedOpt = m_dbus->listIndexedDevices(&idxErr);
    if (!indexedOpt) {
        daemonStatusLabel->setText(QString("Daemon: connected • indexes unavailable (%1)").arg(idxErr));
        daemonStatusLabel->setToolTip(QString());
        return;
    }

    quint64 totalEntries = 0;
    QStringList tooltipLines;
    tooltipLines << QStringLiteral("Indexed partitions:");

    for (const QVariant& v : *indexedOpt) {
        const QVariantMap m = v.toMap();

        const QString deviceId = m.value(QStringLiteral("deviceId")).toString();
        const quint64 entryCount = m.value(QStringLiteral("entryCount")).toULongLong();
        const qint64 t = m.value(QStringLiteral("lastIndexedTime")).toLongLong();

        totalEntries += entryCount;

        QString when;
        if (t > 0) {
            // const QDateTime dt = QDateTime::fromSecsSinceEpoch(t);
            // when = QLocale().toString(dt, QLocale::ShortFormat);
            when = QString::fromStdString(GuiUtils::uint64ToFormattedTime(t));
        } else {
            when = QStringLiteral("unknown");
        }

        tooltipLines << QStringLiteral("%1 • %2 entries • %3")
                            .arg(deviceId)
                            .arg(QLocale().toString(static_cast<qulonglong>(entryCount)))
                            .arg(when);
    }

    daemonStatusLabel->setText(
        QString("Daemon: connected • %1 partition(s) • %2 objects")
            .arg(indexedOpt->size())
            .arg(QLocale().toString(static_cast<qulonglong>(totalEntries)))
    );
    daemonStatusLabel->setToolTip(tooltipLines.join('\n'));
}

void MainWindow::refreshDeviceScopeCombo() {
    if (!m_deviceScopeCombo) return;

    // Only meaningful in daemon mode
    m_deviceScopeCombo->setVisible(m_useDaemonSearch);
    if (!m_useDaemonSearch) return;

    if (!m_dbus) {
        m_deviceScopeCombo->setEnabled(false);
        m_deviceScopeCombo->clear();
        m_deviceScopeCombo->addItem(QStringLiteral("All indexed devices"), QString());
        m_deviceScopeCombo->setToolTip(QStringLiteral("Daemon unavailable"));
        return;
    }

    QString errIdx;
    const auto indexedOpt = m_dbus->listIndexedDevices(&errIdx);
    if (!indexedOpt) {
        m_deviceScopeCombo->setEnabled(false);
        m_deviceScopeCombo->clear();
        m_deviceScopeCombo->addItem(QStringLiteral("All indexed devices"), QString());
        m_deviceScopeCombo->setToolTip(QStringLiteral("Failed to list indexed devices"));
        return;
    }

    // Build a lookup for currently-present devices with dev node + mount info
    struct KnownInfo {
        QString devNode;
        QString primaryMountPoint;
        bool mounted = false;
        bool present = false;
    };

    QHash<QString, KnownInfo> knownById;
    {
        QString errKnown;
        const auto knownOpt = m_dbus->listKnownDevices(&errKnown);
        if (knownOpt) {
            knownById.reserve(knownOpt->size());
            for (const QVariant& v : *knownOpt) {
                const QVariantMap m = v.toMap();
                const QString deviceId = m.value(QStringLiteral("deviceId")).toString();
                if (deviceId.isEmpty()) continue;

                KnownInfo ki;
                ki.devNode = m.value(QStringLiteral("devNode")).toString();
                ki.primaryMountPoint = m.value(QStringLiteral("primaryMountPoint")).toString();
                ki.mounted = m.value(QStringLiteral("mounted")).toBool();
                ki.present = true;

                knownById.insert(deviceId, ki);
            }
        }
    }

    struct IndexedRow {
        QString deviceId;
        QString label;
        QString fsType;
        quint64 entryCount = 0;
        bool present = false;
        QString devNode;
        bool mounted = false;
        QString primaryMountPoint;
    };

    std::vector<IndexedRow> rows;
    rows.reserve(static_cast<size_t>(indexedOpt->size()));

    quint64 totalAllObjects = 0;

    for (const QVariant& v : *indexedOpt) {
        const QVariantMap m = v.toMap();

        IndexedRow r;
        r.deviceId = m.value(QStringLiteral("deviceId")).toString();
        r.label = m.value(QStringLiteral("label")).toString().trimmed();
        r.fsType = m.value(QStringLiteral("fsType")).toString().trimmed();
        r.entryCount = m.value(QStringLiteral("entryCount")).toULongLong();

        totalAllObjects += r.entryCount;

        const auto it = knownById.find(r.deviceId);
        if (it != knownById.end()) {
            r.present = it->present;
            r.devNode = it->devNode;
            r.mounted = it->mounted;
            r.primaryMountPoint = it->primaryMountPoint;
        }

        rows.push_back(std::move(r));
    }

    auto sortKey = [](const IndexedRow& r) -> QString {
        if (!r.label.isEmpty()) return r.label;
        if (!r.fsType.isEmpty()) return r.fsType;
        return r.deviceId;
    };

    std::sort(rows.begin(), rows.end(), [&](const IndexedRow& a, const IndexedRow& b) {
        const int c = QString::compare(sortKey(a), sortKey(b), Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        return a.deviceId < b.deviceId;
    });

    m_deviceScopeCombo->setEnabled(true);

    const QString prev = m_selectedDeviceScopeId;

    m_deviceScopeCombo->blockSignals(true);
    m_deviceScopeCombo->clear();

    // Entry 0: all devices (empty deviceId)
    m_deviceScopeCombo->addItem(QStringLiteral("All indexed devices"), QString());
    m_deviceScopeCombo->setItemData(0, true, Qt::UserRole + 1);

    {
        const QString countStr = QLocale().toString(static_cast<qulonglong>(totalAllObjects));
        const QString tip = QStringLiteral("Objects (all indexed): %1").arg(countStr);
        m_deviceScopeCombo->setItemData(0, tip, Qt::ToolTipRole);
    }

    int restoreIndex = 0;

    for (const auto& r : rows) {
        const QString countStr = QLocale().toString(static_cast<qulonglong>(r.entryCount));

        QString base;
        if (!r.label.isEmpty()) base = r.label;
        else if (!r.fsType.isEmpty()) base = r.fsType;
        else base = QStringLiteral("Device");

        // Visible text: short, scannable
        QString text = QStringLiteral("%1 • %2").arg(base, countStr);
        if (!r.present) {
            text += QStringLiteral(" (not present)");
        }

        QString mountStr;
        if (!r.present) {
            mountStr = QStringLiteral("(not present)");
        } else if (r.mounted) {
            mountStr = r.primaryMountPoint.trimmed().isEmpty()
                ? QStringLiteral("(mounted)")
                : r.primaryMountPoint.trimmed();
        } else {
            mountStr = QStringLiteral("(not mounted)");
        }

        // Tooltip: include additional information for clarity
        const QString tip = QStringLiteral("Device ID: %1\nDev node: %2\nMount: %3\nObjects: %4")
                                .arg(r.deviceId,
                                     r.devNode.trimmed().isEmpty() ? QStringLiteral("—") : r.devNode.trimmed(),
                                     mountStr,
                                     countStr);

        m_deviceScopeCombo->addItem(text, r.deviceId);

        const int itemIndex = m_deviceScopeCombo->count() - 1;
        m_deviceScopeCombo->setItemData(itemIndex, tip, Qt::ToolTipRole);
        m_deviceScopeCombo->setItemData(itemIndex, r.present, Qt::UserRole + 1);

        if (!prev.isEmpty() && r.deviceId == prev) {
            restoreIndex = itemIndex;
        }
    }

    m_deviceScopeCombo->setCurrentIndex(restoreIndex);
    m_deviceScopeCombo->blockSignals(false);

    // If the previous device no longer exists, revert to "all" and apply filter.
    const QString now = m_deviceScopeCombo->currentData().toString();
    m_selectedDeviceScopeId = now;

    if (m_useDaemonSearch && remoteModel) {
        if (now.isEmpty()) {
            remoteModel->setDeviceIds({});
        } else {
            remoteModel->setDeviceIds(QStringList{now});
        }
    }
}

void MainWindow::onDaemonServiceRegistered(const QString& serviceName) {
    Q_UNUSED(serviceName);

    // Daemon appeared (or restarted). Refresh summary, device dropdown and refresh the current query page.
    refreshDaemonStatusLabel();
    refreshDeviceScopeCombo();

    if (m_useDaemonSearch && remoteModel) {
        remoteModel->setOffline(false);
        updateSearch(searchLine->text());
    }
}

void MainWindow::onDaemonServiceUnregistered(const QString& serviceName) {
    Q_UNUSED(serviceName);

    // Daemon vanished. Update the permanent label so users know background updates can't occur.
    if (daemonStatusLabel) {
        daemonStatusLabel->setText(QStringLiteral("Daemon: disconnected • live updates paused"));
    }

    // Also disable the device dropdown.
    if (m_deviceScopeCombo) {
        m_deviceScopeCombo->setEnabled(false);
    }

    if (m_useDaemonSearch && remoteModel) {
        remoteModel->setOffline(true); // empty table
    }

    statusBar()->showMessage(QStringLiteral("Daemon disconnected • live updates paused"), 5000);
}

void MainWindow::onDeviceIndexUpdated(const QString& deviceId, quint64 generation, quint64 entryCount) {
    Q_UNUSED(deviceId);
    Q_UNUSED(generation);
    Q_UNUSED(entryCount);

    // Keep the permanent label and device dropdown in sync with daemon state.
    refreshDaemonStatusLabel();
    refreshDeviceScopeCombo();

    if (!(m_useDaemonSearch && remoteModel)) {
        return;
    }

    // Coalesce bursts (multiple partitions, rapid fanotify batches, etc.)
    m_indexUpdatePending = true;
    if (!m_indexUpdateDebounceTimer->isActive()) {
        m_indexUpdateDebounceTimer->start();
    }
}

void MainWindow::onTableHovered(const QModelIndex& index) {
    const int newRow = index.isValid() ? index.row() : -1;
    if (newRow == m_hoveredRow) {
        return;
    }

    m_hoveredRow = newRow;
    tableView->viewport()->update();
}

void MainWindow::onTableViewportHovered() {
    if (m_hoveredRow != -1) {
        m_hoveredRow = -1;
        tableView->viewport()->update();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tableView->viewport()) {
        if (event->type() == QEvent::Leave) {
            if (m_hoveredRow != -1) {
                m_hoveredRow = -1;
                tableView->viewport()->update();
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setDatabase(ScannerEngine::SearchDatabase&& database, QString mountPath, QString devicePath, const QString& fsType) {
    db = std::move(database);

    // Remove UI placeholder when partition is not mounted
    if (mountPath == "Not Mounted") {
        mountPath.clear();
    }

    m_mountPath = std::move(mountPath);
    m_devicePath = std::move(devicePath);

    const QString fsTypeNormalized = GuiUtils::normalizeFsTypeForHelper(fsType);
    if (fsTypeNormalized.isEmpty()) {
        m_fsType = fsType;
    } else {
        m_fsType = fsTypeNormalized;
    }

    // Refresh search results with new data
    updateSearch(searchLine->text());
}

void MainWindow::openSettings() {
    if (!m_dbus) {
        QMessageBox::warning(this, QStringLiteral("Daemon unavailable"),
                             QStringLiteral("No D-Bus client available."));
        return;
    }

    SettingsDialog dlg(m_dbus.get(), this);
    dlg.exec();

    // After settings changes (new indexing), refresh daemon label and device dropdown + refresh results
    refreshDaemonStatusLabel();
    refreshDeviceScopeCombo();
    if (m_useDaemonSearch && remoteModel) {
        remoteModel->invalidate();
    }
}

void MainWindow::updateLegacyPartitionActions() {
    if (!m_changePartitionAction || !m_rescanPartitionAction) return;

    if (m_useDaemonSearch) {
        // Legacy actions become entry points to the new settings UX.
        m_changePartitionAction->setText("Manage Indexes…");
        m_rescanPartitionAction->setText("Rescan / Index…");
    } else {
        m_changePartitionAction->setText("Change Partition");
        m_rescanPartitionAction->setText("Rescan Partition");
    }
}

void MainWindow::showBaselineStatus(const QString& msg) {
    m_statusBaseline = msg;
    statusBar()->showMessage(msg, 0);
}

void MainWindow::showTransientStatus(const QString& msg, int timeoutMs) {
    const quint64 token = ++m_statusMessageToken;
    statusBar()->showMessage(msg, timeoutMs);

    QTimer::singleShot(timeoutMs, this, [this, token]() {
        if (token != m_statusMessageToken) return; // newer message happened
        if (!m_statusBaseline.isEmpty()) {
            statusBar()->showMessage(m_statusBaseline, 0);
        } else {
            statusBar()->clearMessage();
        }
    });
}

void MainWindow::changePartition() {
    // In daemon mode, "Change Partition" is legacy UI.
    // Redirect to the new Settings dialog where partitions/indexes are managed.
    if (m_useDaemonSearch) {
        openSettings();
        return;
    }

    PartitionDialog dlg(this);

    if (dlg.exec() == QDialog::Accepted) {
        auto newDb = dlg.takeDatabase();
        auto selected = dlg.getSelected();

        if (newDb) {
            setDatabase(std::move(*newDb), selected.mountPoint, selected.devicePath, selected.fsType);

            QString title = QString("[%1] %2 (%3) - %4")
                .arg(selected.fsType, selected.name, selected.devicePath, selected.mountPoint);

            setWindowTitle(title);
        }
    }
}

void MainWindow::rescanPartition() {
    // In daemon mode, redirect to Settings (user can pick partition and index/rescan).
    if (m_useDaemonSearch) {
        openSettings();
        return;
    }

    if (m_devicePath.isEmpty()) {
        changePartition();
        return;
    }

    const QString t = m_fsType.trimmed().toLower();
    if (t != QStringLiteral("ntfs") && t != QStringLiteral("ext4")) {
        QMessageBox::warning(
            this,
            "Unsupported filesystem",
            QString("Cannot rescan because the filesystem type is not supported for raw scanning.\n\nDetected: %1")
                .arg(m_fsType)
        );
        statusBar()->showMessage("Rescan unavailable (unsupported filesystem).", 0);
        return;
    }

    ScannerManager manager(this);

    // Connect progress messages to the status bar
    connect(&manager, &ScannerManager::progressMessage,
            this, [this](const QString& msg) {
                statusBar()->showMessage(msg, 0); // 0 = show until replaced/cleared
            });

    // Connect error messages to show a critical message box
    connect(&manager, &ScannerManager::errorMessage, this, [](const QString &title, const QString &msg) {
        QMessageBox::critical(nullptr, title, msg);
    });

    auto newDb = manager.scanDevice(m_devicePath, m_fsType);

    if (newDb) {
        setDatabase(std::move(*newDb), m_mountPath, m_devicePath, m_fsType);
    } else {
        statusBar()->showMessage("Rescan failed or cancelled.", 0);
    }
}

void MainWindow::showAbout() {
    auto *dialog = new KAboutApplicationDialog(KAboutData::applicationData(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
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

    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    QMenu menu(this);
    KFileItemActions menuActions;

    // ---- Daemon mode: resolve entries and build a KFileItemList for mounted items only ----
    QList<QUrl> mountedUrls;
    bool anyUnmounted = false;

    if (m_useDaemonSearch && remoteModel && m_dbus) {
        QList<quint64> entryIds;
        entryIds.reserve(selectedRows.size());

        for (const QModelIndex& idx : selectedRows) {
            auto idOpt = remoteModel->entryIdAtRow(idx.row());
            if (idOpt) entryIds.push_back(*idOpt);
        }

        QString err;
        auto resolved = m_dbus->resolveEntries(entryIds, &err);
        if (resolved) {
            for (const QVariant& v : *resolved) {
                const QVariantMap m = v.toMap();
                const bool mounted = m.value(QStringLiteral("mounted")).toBool();
                const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
                const QString internalPath = m.value(QStringLiteral("internalPath")).toString();

                if (mounted && !mp.isEmpty() && !internalPath.isEmpty()) {
                    mountedUrls.push_back(QUrl::fromLocalFile(QDir::cleanPath(mp + internalPath)));
                } else {
                    anyUnmounted = true;
                }
            }
        } else {
            // If resolve fails, we still show basic menu; “Open With” etc. will be limited.
            statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
        }

        if (!mountedUrls.isEmpty()) {
            KFileItemList items;
            items.reserve(mountedUrls.size());
            for (const QUrl& url : mountedUrls) {
                KFileItem fileItem(url);
                fileItem.determineMimeType();
                items.append(fileItem);
            }
            menuActions.setItemListProperties(KFileItemListProperties(items));
            menuActions.insertOpenWithActionsTo(nullptr, &menu, QStringList());
        }
    }

    // ---- Local mode ----
    if (!m_useDaemonSearch) {
        const bool isMounted = !m_mountPath.isEmpty();

        if (isMounted) {
            KFileItemList items;

            // Process all selected items
            for (const QModelIndex &index : selectedRows) {
                uint32_t recordIdx = model->getRecordIndex(index.row());
                const auto& rec = db.records[recordIdx];

                QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
                QString internalPath = QString::fromStdString(db.getFullPath(rec.parentRecordIdx));

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
    }

    // ---- Common actions ----

    // Add the actions we defined in the constructor
    // This automatically handles the text, icons, and showing the shortcuts

    // Add "Open" with grouping logic
    menu.addAction(findChild<QAction*>("openAction"));
    menu.addAction(findChild<QAction*>("openLocationAction"));
    menu.addAction(findChild<QAction*>("openTerminalAction"));

    menu.addSeparator();
    menu.addAction(findChild<QAction*>("copyFilesAction"));
    menu.addAction(findChild<QAction*>("copyFileNamesAction"));

    // Copy path actions: daemon mode gets split actions when unmounted is involved
    QAction* copyPathsAction = findChild<QAction*>("copyPathsAction");
    if (m_useDaemonSearch && anyUnmounted) {
        // Hide the single “Copy Full Path” semantics in mixed/unmounted case:
        // expose two explicit options.
        auto* copyDisplay = menu.addAction(QIcon::fromTheme("edit-copy"), "Copy Display Path");
        connect(copyDisplay, &QAction::triggered, this, [this]() {
            const QModelIndexList rows = tableView->selectionModel()->selectedRows();
            if (!remoteModel || !m_dbus || rows.isEmpty()) return;

            QList<quint64> entryIds;
            entryIds.reserve(rows.size());
            for (const auto& idx : rows) {
                auto idOpt = remoteModel->entryIdAtRow(idx.row());
                if (idOpt) entryIds.push_back(*idOpt);
            }

            QString err;
            auto resolved = m_dbus->resolveEntries(entryIds, &err);
            if (!resolved) {
                statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
                return;
            }

            QStringList lines;
            for (const QVariant& v : *resolved) {
                const QVariantMap m = v.toMap();
                const QString s = m.value(QStringLiteral("displayPath")).toString();
                if (!s.isEmpty()) lines << s;
            }
            QGuiApplication::clipboard()->setText(lines.join('\n'));
        });

        auto* copyInternal = menu.addAction(QIcon::fromTheme("edit-copy"), "Copy Internal Path");
        connect(copyInternal, &QAction::triggered, this, [this]() {
            const QModelIndexList rows = tableView->selectionModel()->selectedRows();
            if (!remoteModel || !m_dbus || rows.isEmpty()) return;

            QList<quint64> entryIds;
            entryIds.reserve(rows.size());
            for (const auto& idx : rows) {
                auto idOpt = remoteModel->entryIdAtRow(idx.row());
                if (idOpt) entryIds.push_back(*idOpt);
            }

            QString err;
            auto resolved = m_dbus->resolveEntries(entryIds, &err);
            if (!resolved) {
                statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
                return;
            }

            QStringList lines;
            for (const QVariant& v : *resolved) {
                const QVariantMap m = v.toMap();
                const QString s = m.value(QStringLiteral("internalPath")).toString();
                if (!s.isEmpty()) lines << s;
            }
            QGuiApplication::clipboard()->setText(lines.join('\n'));
        });

        if (copyPathsAction) copyPathsAction->setVisible(false);
    } else {
        if (copyPathsAction) {
            copyPathsAction->setVisible(true);
            menu.addAction(copyPathsAction);
        }
    }

    menu.addSeparator();

    // KDE “Open With” and other actions: only meaningful when we have real file URLs
    if (m_useDaemonSearch) {
        if (!mountedUrls.isEmpty()) {
            menuActions.addActionsTo(&menu);
        } else {
            menu.addAction("Metadata/Actions unavailable (Unmounted)")->setEnabled(false);
        }
    } else {
        const bool isMounted = !m_mountPath.isEmpty();
        if (isMounted) {
            menuActions.addActionsTo(&menu);
        } else {
            menu.addAction("Metadata/Actions unavailable (Unmounted)")->setEnabled(false);
        }
    }

    menu.exec(event->globalPos());
}

void MainWindow::openFile(const QModelIndex &index) {
    if (!index.isValid()) {
        return;
    }

    // Update the selection to just this item and trigger the selected files opener
    tableView->setCurrentIndex(index);
    openSelectedFiles();
}

void MainWindow::openSelectedFiles() {
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    // If nothing is selected, don't open anything
    if (selectedRows.isEmpty()) {
        return;
    }

    // ---- Daemon mode ----
    if (m_useDaemonSearch) {
        if (!remoteModel || !m_dbus) return;

        QList<quint64> entryIds;
        entryIds.reserve(selectedRows.size());
        for (const auto& idx : selectedRows) {
            auto idOpt = remoteModel->entryIdAtRow(idx.row());
            if (idOpt) entryIds.push_back(*idOpt);
        }

        QString err;
        auto resolved = m_dbus->resolveEntries(entryIds, &err);
        if (!resolved) {
            statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
            return;
        }

        QList<QUrl> urls;
        bool skipped = false;

        for (const QVariant& v : *resolved) {
            const QVariantMap m = v.toMap();
            const bool mounted = m.value(QStringLiteral("mounted")).toBool();
            const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
            const QString internalPath = m.value(QStringLiteral("internalPath")).toString();

            if (mounted && !mp.isEmpty() && !internalPath.isEmpty()) {
                urls.push_back(QUrl::fromLocalFile(QDir::cleanPath(mp + internalPath)));
            } else {
                skipped = true;
            }
        }

        if (urls.isEmpty()) {
            QMessageBox::information(this, "Drive Not Mounted",
                                     "These results are from partitions that are not currently mounted.\n\n"
                                     "Mount the partition to open files.");
            return;
        }

        if (skipped) {
            statusBar()->showMessage("Some selected items were skipped because they are unmounted.", 5000);
        }

        // Group by MIME type and launch
        QMimeDatabase mimeDb;
        QMap<QString, QList<QUrl>> mimeGroups;

        for (const QUrl& url : urls) {
            const QString mimeType = mimeDb.mimeTypeForUrl(url).name();
            mimeGroups[mimeType].append(url);
        }

        for (auto it = mimeGroups.begin(); it != mimeGroups.end(); ++it) {
            const QString &mimeType = it.key();
            const QList<QUrl> &groupUrls = it.value();

            KService::Ptr service = KApplicationTrader::preferredService(mimeType);

            auto *job = service ? new KIO::ApplicationLauncherJob(service)
                                : new KIO::ApplicationLauncherJob();

            job->setUrls(groupUrls);
            job->setAutoDelete(true);
            job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
            job->start();
        }

        return;
    }

    // ---- Local mode ----

    // Handle unmounted drives
    if (m_mountPath.isEmpty()) {
        QMessageBox::information(this, "Drive Not Mounted",
            "This partition is not currently mounted in Linux.\n\n"
            "Please mount it first then rescan the partition to open files.");
        return;
    }

    // Group URLs by their MIME type
    QMimeDatabase mimeDb;
    QMap<QString, QList<QUrl>> mimeGroups;

    for (const QModelIndex &index : selectedRows) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];

        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        QString internalPath = QString::fromStdString(db.getFullPath(rec.parentRecordIdx));

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
        job->setAutoDelete(true);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
        job->start();
    }
}

void MainWindow::openSelectedLocation() {
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    // ---- Daemon mode ----
    if (m_useDaemonSearch) {
        if (!remoteModel || !m_dbus) return;

        auto idOpt = remoteModel->entryIdAtRow(selectedRows.first().row());
        if (!idOpt) return;

        QString err;
        auto resolved = m_dbus->resolveEntries(QList<quint64>{*idOpt}, &err);
        if (!resolved || resolved->isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
            return;
        }

        const QVariantMap m = resolved->first().toMap();
        const bool mounted = m.value(QStringLiteral("mounted")).toBool();
        const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
        const QString internalDir = m.value(QStringLiteral("internalDir")).toString();

        if (!mounted || mp.isEmpty() || internalDir.isEmpty()) return;

        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::cleanPath(mp + internalDir)));
        return;
    }

    // ---- Local mode ----

    // Opening the selected file's directory only makes sense when the partition is mounted
    if (m_mountPath.isEmpty()) {
        return;
    }

    uint32_t recordIdx = model->getRecordIndex(selectedRows.first().row());
    const auto& rec = db.records[recordIdx];
    QString internalPath = QString::fromStdString(db.getFullPath(rec.parentRecordIdx));

    QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::cleanPath(m_mountPath + "/" + internalPath)));
}

void MainWindow::copyFileNames() {
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    // In daemon mode, Name is already in the model, so the existing approach works
    // if we just read column 0.
    if (m_useDaemonSearch) {
        QStringList names;
        names.reserve(selectedRows.size());
        for (const auto& idx : selectedRows) {
            names << tableView->model()->data(tableView->model()->index(idx.row(), 0), Qt::DisplayRole).toString();
        }
        QGuiApplication::clipboard()->setText(names.join('\n'));
        return;
    }

    QStringList fileNames;
    fileNames.reserve(selectedRows.size());

    for (const QModelIndex &index : selectedRows) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];
        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        fileNames.append(fileName);
    }

    QGuiApplication::clipboard()->setText(fileNames.join('\n'));
}

void MainWindow::copyPaths() {
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    // ---- Daemon mode: “Copy Full Path” means real paths for mounted items only ----
    if (m_useDaemonSearch) {
        if (!remoteModel || !m_dbus) return;

        QList<quint64> entryIds;
        entryIds.reserve(selectedRows.size());
        for (const auto& idx : selectedRows) {
            auto idOpt = remoteModel->entryIdAtRow(idx.row());
            if (idOpt) entryIds.push_back(*idOpt);
        }

        QString err;
        auto resolved = m_dbus->resolveEntries(entryIds, &err);
        if (!resolved) {
            statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
            return;
        }

        QStringList paths;
        bool skipped = false;

        for (const QVariant& v : *resolved) {
            const QVariantMap m = v.toMap();
            const bool mounted = m.value(QStringLiteral("mounted")).toBool();
            const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
            const QString internalPath = m.value(QStringLiteral("internalPath")).toString();

            if (mounted && !mp.isEmpty() && !internalPath.isEmpty()) {
                paths << QDir::cleanPath(mp + internalPath);
            } else {
                skipped = true;
            }
        }

        if (paths.isEmpty()) return;

        if (skipped) {
            statusBar()->showMessage("Some selected items were skipped because they are unmounted.", 5000);
        }

        QGuiApplication::clipboard()->setText(paths.join('\n'));
        return;
    }

    // ---- Local mode ----

    QStringList paths;
    paths.reserve(selectedRows.size());

    for (const QModelIndex &index : selectedRows) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];
        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        QString internalPath = QString::fromStdString(db.getFullPath(rec.parentRecordIdx));
        paths.append(QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName));
    }

    QGuiApplication::clipboard()->setText(paths.join('\n'));
}

void MainWindow::copyFiles() {
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    // ---- Daemon mode: copy URLs for mounted entries only ----
    if (m_useDaemonSearch) {
        if (!remoteModel || !m_dbus) return;

        QList<quint64> entryIds;
        entryIds.reserve(selectedRows.size());
        for (const auto& idx : selectedRows) {
            auto idOpt = remoteModel->entryIdAtRow(idx.row());
            if (idOpt) entryIds.push_back(*idOpt);
        }

        QString err;
        auto resolved = m_dbus->resolveEntries(entryIds, &err);
        if (!resolved) {
            statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
            return;
        }

        QList<QUrl> urls;
        bool skipped = false;

        for (const QVariant& v : *resolved) {
            const QVariantMap m = v.toMap();
            const bool mounted = m.value(QStringLiteral("mounted")).toBool();
            const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
            const QString internalPath = m.value(QStringLiteral("internalPath")).toString();

            if (mounted && !mp.isEmpty() && !internalPath.isEmpty()) {
                urls.push_back(QUrl::fromLocalFile(QDir::cleanPath(mp + internalPath)));
            } else {
                skipped = true;
            }
        }

        if (urls.isEmpty()) return;

        if (skipped) {
            statusBar()->showMessage("Some selected items were skipped because they are unmounted.", 5000);
        }

        auto *mimeData = new QMimeData();
        mimeData->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(mimeData);
        return;
    }

    // ---- Local mode ----

    // Copying file objects (URLs) only makes sense when the partition is mounted
    if (m_mountPath.isEmpty()) {
        return;
    }

    QList<QUrl> urls;
    urls.reserve(selectedRows.size());

    for (const QModelIndex &index : selectedRows) {
        uint32_t recordIdx = model->getRecordIndex(index.row());
        const auto& rec = db.records[recordIdx];
        QString fileName = QString::fromUtf8(&db.stringPool[rec.nameOffset], rec.nameLen);
        QString internalPath = QString::fromStdString(db.getFullPath(rec.parentRecordIdx));
        urls.append(QUrl::fromLocalFile(QDir::cleanPath(m_mountPath + "/" + internalPath + "/" + fileName)));
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);
    QGuiApplication::clipboard()->setMimeData(mimeData);
}

void MainWindow::openTerminal() {
    const QModelIndexList selectedRows = tableView->selectionModel()->selectedRows();

    if (selectedRows.isEmpty()) {
        return;
    }

    // ---- Daemon mode ----
    if (m_useDaemonSearch) {
        if (!remoteModel || !m_dbus) return;

        auto idOpt = remoteModel->entryIdAtRow(selectedRows.first().row());
        if (!idOpt) return;

        QString err;
        auto resolved = m_dbus->resolveEntries(QList<quint64>{*idOpt}, &err);
        if (!resolved || resolved->isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Daemon error: ") + err, 5000);
            return;
        }

        const QVariantMap m = resolved->first().toMap();
        const bool mounted = m.value(QStringLiteral("mounted")).toBool();
        const QString mp = m.value(QStringLiteral("primaryMountPoint")).toString();
        const QString internalDir = m.value(QStringLiteral("internalDir")).toString();

        if (!mounted || mp.isEmpty() || internalDir.isEmpty()) return;

        const QString fullDirPath = QDir::cleanPath(mp + internalDir);

        auto *job = new KTerminalLauncherJob(QString());
        job->setWorkingDirectory(fullDirPath);
        job->setAutoDelete(true);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
        job->start();
        return;
    }

    // ---- Local mode ----

    // Opening the terminal in the selected file's directory only makes sense when the partition is mounted
    if (m_mountPath.isEmpty()) {
        return;
    }

    uint32_t recordIdx = model->getRecordIndex(selectedRows.first().row());
    const auto& rec = db.records[recordIdx];
    QString internalPath = QString::fromStdString(db.getFullPath(rec.parentRecordIdx));
    QString fullDirPath = QDir::cleanPath(m_mountPath + "/" + internalPath);

    // KTerminalLauncherJob automatically finds the preferred terminal
    // and handles the command-line arguments to set the working directory.
    auto *job = new KTerminalLauncherJob(QString()); // Empty string means "default terminal"
    job->setWorkingDirectory(fullDirPath);
    job->setAutoDelete(true);

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
    if (m_useDaemonSearch && remoteModel) {
        remoteModel->setQuery(text);
        return; // status will update on RemoteFileModel::searchCompleted
    }

    // Local search
    auto start = std::chrono::steady_clock::now();

    auto results = performTrigramSearch(text.toStdString());
    model->setResults(std::move(results), &db, m_mountPath, m_fsType);

    int sortCol = tableView->horizontalHeader()->sortIndicatorSection();
    model->sort(sortCol, tableView->horizontalHeader()->sortIndicatorOrder());

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    showBaselineStatus(QString("%L1 objects found in %2s")
        .arg(model->rowCount())
        .arg(elapsed.count(), 0, 'f', 4));
}