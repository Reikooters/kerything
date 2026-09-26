// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "PreferencesDialog.h"

#include <algorithm>
#include <cmath>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextOption>
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include "BlockDeviceDisplayUtils.h"
#include "DeviceCapabilities.h"
#include "HoverRowHighlight.h"
#include "PreferencesDialogPage.h"
#include "SearchResultColumns.h"

namespace {
    constexpr int DeviceIdRole = Qt::UserRole + 1;
    constexpr int InitialEnabledRole = Qt::UserRole + 2;
    constexpr int ScanWhenUnmountedRole = Qt::UserRole + 3;
    constexpr int ShowOfflineResultsRole = Qt::UserRole + 4;
    constexpr int LiveUpdatesEnabledRole = Qt::UserRole + 5;

    constexpr int IndexDeviceIdRole = Qt::UserRole + 20;
    constexpr int IndexIdRole = Qt::UserRole + 21;

    constexpr int FilterIdRole = Qt::UserRole + 40;

    QIcon themedIcon(const QString& iconName, const QString& fallbackIconName)
    {
        return QIcon::fromTheme(
            iconName,
            QIcon::fromTheme(fallbackIconName)
        );
    }

    void setButtonIcon(QPushButton* button, const QString& iconName, const QString& fallbackIconName)
    {
        if (!button) {
            return;
        }

        button->setIcon(themedIcon(iconName, fallbackIconName));
        button->setIconSize(QSize(16, 16));
    }

    bool moveTableSelectionToEdge(QTableWidget* table, QKeyEvent* keyEvent)
    {
        if (!table || table->rowCount() <= 0 || keyEvent->modifiers() != Qt::NoModifier) {
            return false;
        }

        if (keyEvent->key() != Qt::Key_Home && keyEvent->key() != Qt::Key_End) {
            return false;
        }

        const int row = keyEvent->key() == Qt::Key_Home
            ? 0
            : table->rowCount() - 1;

        const int column = table->currentColumn() >= 0
            ? table->currentColumn()
            : 0;

        table->setCurrentCell(row, column);
        table->scrollToItem(table->item(row, column), QAbstractItemView::PositionAtCenter);
        return true;
    }
}

PreferencesDialog::PreferencesDialog(
    Preferences& preferences,
    const std::vector<BlockDevice>& knownDevices,
    const std::vector<IndexSummary>& indexSummaries,
    QWidget* parent
)
    : QDialog(parent),
      preferences_(preferences),
      knownDevices_(knownDevices),
      indexSummaries_(indexSummaries)
{
    setWindowTitle(QStringLiteral("Configure Kerything"));
    resize(980, 700);

    for (const BlockDevice& blockDevice : knownDevices_) {
        if (!blockDevice.deviceId.isEmpty()) {
            knownDeviceById_.insert(blockDevice.deviceId, blockDevice);
        }
    }

    for (const IndexedDevicePreference& preference : preferences_.indexedDevicePreferences()) {
        if (!preference.deviceId.isEmpty()) {
            originalPreferencesByDeviceId_.insert(preference.deviceId, preference);
        }
    }

    originalSearchFilters_ = preferences_.searchFilters();

    auto* rootLayout = new QVBoxLayout(this);
    auto* contentLayout = new QHBoxLayout();

    navigation_ = new QListWidget(this);
    navigation_->setFixedWidth(180);
    navigation_->setSelectionMode(QAbstractItemView::SingleSelection);

    pages_ = new QStackedWidget(this);

    populateNavigation();

    contentLayout->addWidget(navigation_);
    contentLayout->addWidget(pages_, 1);

    rootLayout->addLayout(contentLayout, 1);

    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
        this
    );

    applyButton_ = buttonBox_->button(QDialogButtonBox::Apply);
    applyButton_->setEnabled(false);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, [this]() {
        if (applyChanges()) {
            accept();
        }
    });

    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        applyChanges();
    });

    rootLayout->addWidget(buttonBox_);

    if (navigation_->count() > 0) {
        navigation_->setCurrentRow(0);
    }

    updateApplyButtonEnabled();
}

bool PreferencesDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = dynamic_cast<QKeyEvent*>(event);

        if (watched == deviceTable_) {
            if (keyEvent->key() == Qt::Key_Space && keyEvent->modifiers() == Qt::NoModifier) {
                toggleDeviceRowChecked(deviceTable_->currentRow());
                return true;
            }

            if (moveTableSelectionToEdge(deviceTable_, keyEvent)) {
                return true;
            }
        }
        else if (watched == filterTable_) {
            if (moveTableSelectionToEdge(filterTable_, keyEvent)) {
                return true;
            }
        }
    }

    return QDialog::eventFilter(watched, event);
}

void PreferencesDialog::setCurrentPage(PreferencesDialogPage page)
{
    if (!navigation_ || !pages_) {
        return;
    }

    int row = 0;

    switch (page) {
        case PreferencesDialogPage::Devices:
            row = 0;
            break;
        case PreferencesDialogPage::Indexes:
            row = 1;
            break;
        case PreferencesDialogPage::Filters:
            row = 2;
            break;
        case PreferencesDialogPage::Windows:
            row = 3;
            break;
        case PreferencesDialogPage::UI:
            row = 4;
            break;
        case PreferencesDialogPage::Advanced:
            row = 5;
            break;
    }

    if (row >= 0 && row < navigation_->count()) {
        navigation_->setCurrentRow(row);
    }

    if (page == PreferencesDialogPage::Devices) {
        scheduleSelectedDeviceDetailsHeightUpdate();
    }
}

void PreferencesDialog::setAutoRefreshResultsForLiveUpdates(bool enabled)
{
    if (!autoRefreshLiveUpdatesCheckBox_) {
        return;
    }

    const QSignalBlocker blocker(autoRefreshLiveUpdatesCheckBox_);
    autoRefreshLiveUpdatesCheckBox_->setChecked(enabled);
    updateApplyButtonEnabled();
}

void PreferencesDialog::setShowFiltersDropdown(bool enabled)
{
    if (!showFiltersDropdownCheckBox_) {
        return;
    }

    const QSignalBlocker blocker(showFiltersDropdownCheckBox_);
    showFiltersDropdownCheckBox_->setChecked(enabled);
    updateApplyButtonEnabled();
}

void PreferencesDialog::setShowPreviewPane(bool enabled)
{
    if (!showPreviewPaneCheckBox_) {
        return;
    }

    const QSignalBlocker blocker(showPreviewPaneCheckBox_);
    showPreviewPaneCheckBox_->setChecked(enabled);
    updateApplyButtonEnabled();
}

void PreferencesDialog::setKnownDevices(const std::vector<BlockDevice>& knownDevices)
{
    QString selectedDeviceId;

    struct CurrentDeviceState {
        bool enabled = false;
        bool scanWhenUnmounted = true;
        bool showOfflineResults = true;
        bool liveUpdatesEnabled = true;
    };

    QHash<QString, CurrentDeviceState> currentStateByDeviceId;

    if (deviceTable_) {
        const int currentRow = deviceTable_->currentRow();
        if (currentRow >= 0 && currentRow < deviceTable_->rowCount()) {
            if (const auto* item = deviceTable_->item(currentRow, DeviceEnabledColumn)) {
                selectedDeviceId = item->data(DeviceIdRole).toString();
            }
        }

        for (int row = 0; row < deviceTable_->rowCount(); ++row) {
            const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
            if (!item) {
                continue;
            }

            const QString deviceId = item->data(DeviceIdRole).toString();
            if (deviceId.isEmpty()) {
                continue;
            }

            currentStateByDeviceId.insert(deviceId, CurrentDeviceState{
                .enabled = item->checkState() == Qt::Checked,
                .scanWhenUnmounted = item->data(ScanWhenUnmountedRole).toBool(),
                .showOfflineResults = item->data(ShowOfflineResultsRole).toBool(),
                .liveUpdatesEnabled = item->data(LiveUpdatesEnabledRole).toBool(),
            });
        }
    }

    knownDevices_ = knownDevices;

    knownDeviceById_.clear();
    for (const BlockDevice& blockDevice : knownDevices_) {
        if (!blockDevice.deviceId.isEmpty()) {
            knownDeviceById_.insert(blockDevice.deviceId, blockDevice);
        }
    }

    if (!deviceTable_) {
        updateApplyButtonEnabled();
        return;
    }

    populateDeviceTable();

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        const auto it = currentStateByDeviceId.constFind(deviceId);
        if (it == currentStateByDeviceId.constEnd()) {
            continue;
        }

        item->setCheckState(it->enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(ScanWhenUnmountedRole, it->scanWhenUnmounted);
        item->setData(ShowOfflineResultsRole, it->showOfflineResults);
        item->setData(LiveUpdatesEnabledRole, it->liveUpdatesEnabled);
    }

    int rowToSelect = -1;
    if (!selectedDeviceId.isEmpty()) {
        for (int row = 0; row < deviceTable_->rowCount(); ++row) {
            const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
            if (item && item->data(DeviceIdRole).toString() == selectedDeviceId) {
                rowToSelect = row;
                break;
            }
        }
    }

    if (rowToSelect < 0 && deviceTable_->rowCount() > 0) {
        rowToSelect = 0;
    }

    if (rowToSelect >= 0) {
        deviceTable_->setCurrentCell(rowToSelect, DeviceNameColumn);
    }

    updateApplyButtonEnabled();
}

void PreferencesDialog::setIndexSummaries(const std::vector<IndexSummary>& indexSummaries)
{
    QString selectedDeviceId;

    if (indexTable_) {
        const int currentRow = indexTable_->currentRow();
        if (currentRow >= 0 && currentRow < indexTable_->rowCount()) {
            if (const auto* item = indexTable_->item(currentRow, IndexNameColumn)) {
                selectedDeviceId = item->data(IndexDeviceIdRole).toString();
            }
        }
    }

    indexSummaries_ = indexSummaries;

    if (!indexTable_) {
        return;
    }

    populateIndexTable();

    int rowToSelect = -1;
    if (!selectedDeviceId.isEmpty()) {
        for (int row = 0; row < indexTable_->rowCount(); ++row) {
            const auto* item = indexTable_->item(row, IndexNameColumn);
            if (item && item->data(IndexDeviceIdRole).toString() == selectedDeviceId) {
                rowToSelect = row;
                break;
            }
        }
    }

    if (rowToSelect < 0 && indexTable_->rowCount() > 0) {
        rowToSelect = 0;
    }

    if (rowToSelect >= 0) {
        indexTable_->setCurrentCell(rowToSelect, IndexNameColumn);
    }
}

void PreferencesDialog::updateSelectedDeviceDetailsHeight()
{
    if (!selectedDeviceDetailsText_) {
        return;
    }

    static constexpr int MaxDetailsHeight = 190;

    QTextDocument* document = selectedDeviceDetailsText_->document();
    if (!document) {
        return;
    }

    document->setDocumentMargin(0);

    QTextOption textOption = document->defaultTextOption();
    textOption.setWrapMode(QTextOption::WordWrap);
    document->setDefaultTextOption(textOption);

    const int viewportWidth = selectedDeviceDetailsText_->viewport()
        ? selectedDeviceDetailsText_->viewport()->width()
        : selectedDeviceDetailsText_->width();

    if (viewportWidth > 0) {
        document->setTextWidth(viewportWidth);
    }

    const int contentHeight =
        std::max(1, static_cast<int>(std::ceil(document->size().height())));

    const int detailsHeight = std::min(contentHeight, MaxDetailsHeight);

    selectedDeviceDetailsText_->setMinimumHeight(detailsHeight);
    selectedDeviceDetailsText_->setMaximumHeight(detailsHeight);

    if (selectedDeviceDetailsText_->verticalScrollBar()) {
        selectedDeviceDetailsText_->verticalScrollBar()->setValue(0);
    }
}

void PreferencesDialog::scheduleSelectedDeviceDetailsHeightUpdate()
{
    if (!selectedDeviceDetailsText_) {
        return;
    }

    QTimer::singleShot(0, this, [this]() {
        updateSelectedDeviceDetailsHeight();
    });
}

void PreferencesDialog::populateNavigation()
{
    pages_->addWidget(createDevicesPage());
    navigation_->addItem(QStringLiteral("Devices"));

    pages_->addWidget(createIndexesPage());
    navigation_->addItem(QStringLiteral("Indexes"));

    pages_->addWidget(createFiltersPage());
    navigation_->addItem(QStringLiteral("Filters"));

    pages_->addWidget(createWindowsPage());
    navigation_->addItem(QStringLiteral("Windows"));

    pages_->addWidget(createUiPage());
    navigation_->addItem(QStringLiteral("UI"));

    pages_->addWidget(createAdvancedPage());
    navigation_->addItem(QStringLiteral("Advanced"));

    connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);

    connect(pages_, &QStackedWidget::currentChanged, this, [this](int index) {
        if (index == static_cast<int>(PreferencesDialogPage::Devices)) {
            scheduleSelectedDeviceDetailsHeightUpdate();
        }
    });
}

QWidget* PreferencesDialog::createDevicesPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(
        QStringLiteral(
            "<h2>Devices</h2>"
            "<p>Choose which devices Kerything indexes. Enabling a mounted device starts indexing immediately. "
            "Unmounted indexing is available only for filesystems with low-level raw scanners, currently EXT4 and NTFS. "
            "Other filesystems are indexed through Linux’s mounted filesystem APIs and must be mounted to be scanned.</p>"
        ),
        page
    );
    title->setWordWrap(true);
    layout->addWidget(title);

    deviceTable_ = new QTableWidget(page);
    deviceTable_->setColumnCount(DeviceColumnCount);
    deviceTable_->setHorizontalHeaderLabels({
        QStringLiteral("Index"),
        QStringLiteral("Name"),
        QStringLiteral("Status"),
        QStringLiteral("Filesystem"),
        QStringLiteral("Mount point"),
        QStringLiteral("Device"),
        QStringLiteral("Model"),
    });

    deviceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    deviceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    deviceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    deviceTable_->setAlternatingRowColors(true);
    deviceTable_->setShowGrid(false);
    deviceTable_->setWordWrap(false);
    deviceTable_->verticalHeader()->setVisible(false);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceEnabledColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceNameColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceStatusColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceFsTypeColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceMountPointColumn, QHeaderView::Stretch);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceNodeColumn, QHeaderView::ResizeToContents);
    deviceTable_->horizontalHeader()->setSectionResizeMode(DeviceModelColumn, QHeaderView::ResizeToContents);
    installHoverRowHighlight(deviceTable_);
    deviceTable_->installEventFilter(this);

    populateDeviceTable();

    layout->addWidget(deviceTable_, 1);

    auto* optionsGroup = new QGroupBox(QStringLiteral("Selected device options"), page);
    auto* optionsLayout = new QVBoxLayout(optionsGroup);
    optionsLayout->setSpacing(3);

    selectedDeviceDetailsText_ = new QTextBrowser(optionsGroup);
    selectedDeviceDetailsText_->setReadOnly(true);
    selectedDeviceDetailsText_->setOpenLinks(false);
    selectedDeviceDetailsText_->setFrameShape(QFrame::NoFrame);
    selectedDeviceDetailsText_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    selectedDeviceDetailsText_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    selectedDeviceDetailsText_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    selectedDeviceDetailsText_->setFocusPolicy(Qt::NoFocus);
    selectedDeviceDetailsText_->setTextInteractionFlags(Qt::NoTextInteraction);
    selectedDeviceDetailsText_->setAutoFillBackground(false);
    selectedDeviceDetailsText_->viewport()->setAutoFillBackground(false);
    selectedDeviceDetailsText_->setStyleSheet(QStringLiteral(
        "QTextBrowser {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QTextBrowser viewport {"
        "  background: transparent;"
        "}"
    ));
    selectedDeviceDetailsText_->document()->setDocumentMargin(0);

    scanWhenUnmountedCheckBox_ = new QCheckBox(
        QStringLiteral("Scan this device even when it is not mounted"),
        optionsGroup
    );

    showOfflineResultsCheckBox_ = new QCheckBox(
        QStringLiteral("Keep this device searchable when it is not mounted"),
        optionsGroup
    );

    optionsLayout->addWidget(selectedDeviceDetailsText_);
    optionsLayout->addWidget(scanWhenUnmountedCheckBox_);
    optionsLayout->addWidget(showOfflineResultsCheckBox_);

    auto* liveUpdatesRow = new QWidget(optionsGroup);
    auto* liveUpdatesRowLayout = new QHBoxLayout(liveUpdatesRow);
    liveUpdatesRowLayout->setContentsMargins(0, 0, 0, 0);

    liveUpdatesWarningIconLabel_ = new QLabel(liveUpdatesRow);
    liveUpdatesWarningIconLabel_->setPixmap(
        QIcon::fromTheme(QStringLiteral("dialog-warning")).pixmap(16, 16)
    );
    liveUpdatesWarningIconLabel_->setVisible(false);

    liveUpdatesEnabledCheckBox_ = new QCheckBox(
        QStringLiteral("Watch this device for live updates when mounted"),
        liveUpdatesRow
    );

    liveUpdatesRowLayout->addWidget(liveUpdatesWarningIconLabel_);
    liveUpdatesRowLayout->addWidget(liveUpdatesEnabledCheckBox_);
    liveUpdatesRowLayout->addStretch();

    optionsLayout->addWidget(liveUpdatesRow);

    auto* selectedDeviceActionsRow = new QWidget(optionsGroup);
    auto* selectedDeviceActionsLayout = new QHBoxLayout(selectedDeviceActionsRow);
    selectedDeviceActionsLayout->setContentsMargins(0, 3, 0, 0);

    refreshSelectedDeviceIndexButton_ = new QPushButton(
        QStringLiteral("Refresh Selected Device Index"),
        selectedDeviceActionsRow
    );
    setButtonIcon(
        refreshSelectedDeviceIndexButton_,
        QStringLiteral("view-refresh"),
        QStringLiteral("reload")
    );

    selectedDeviceActionsLayout->addWidget(refreshSelectedDeviceIndexButton_);
    selectedDeviceActionsLayout->addStretch();

    optionsLayout->addWidget(selectedDeviceActionsRow);

    layout->addWidget(optionsGroup);

    auto updateSelectedDeviceOptions = [this]() {
        if (!deviceTable_ ||
            !scanWhenUnmountedCheckBox_ ||
            !showOfflineResultsCheckBox_ ||
            !liveUpdatesEnabledCheckBox_) {
            return;
        }

        const int row = deviceTable_->currentRow();
        const bool validRow = row >= 0 && row < deviceTable_->rowCount();
        auto* enabledItem = validRow ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        const QString deviceId = enabledItem ? enabledItem->data(DeviceIdRole).toString() : QString();

        const QSignalBlocker scanBlocker(scanWhenUnmountedCheckBox_);
        const QSignalBlocker offlineBlocker(showOfflineResultsCheckBox_);
        const QSignalBlocker liveUpdatesBlocker(liveUpdatesEnabledCheckBox_);

        if (deviceId.isEmpty()) {
            selectedDeviceDetailsText_->setHtml(QStringLiteral("No device selected."));

            scanWhenUnmountedCheckBox_->setEnabled(false);
            scanWhenUnmountedCheckBox_->setChecked(false);
            showOfflineResultsCheckBox_->setEnabled(false);
            showOfflineResultsCheckBox_->setChecked(false);
            liveUpdatesEnabledCheckBox_->setEnabled(false);
            liveUpdatesEnabledCheckBox_->setChecked(false);

            if (refreshSelectedDeviceIndexButton_) {
                refreshSelectedDeviceIndexButton_->setEnabled(false);
                refreshSelectedDeviceIndexButton_->setToolTip(QStringLiteral("No device selected."));
            }

            scheduleSelectedDeviceDetailsHeightUpdate();
            return;
        }

        const auto knownDeviceIt = knownDeviceById_.constFind(deviceId);

        if (knownDeviceIt != knownDeviceById_.constEnd()) {
            selectedDeviceDetailsText_->setHtml(
                BlockDeviceDisplayUtils::selectedDeviceDetailsHtml(knownDeviceIt.value())
            );
            scheduleSelectedDeviceDetailsHeightUpdate();
        }
        else {
            QString deviceName = deviceTable_->item(row, DeviceNameColumn)->text().toHtmlEscaped();

            selectedDeviceDetailsText_->setHtml(QStringLiteral(
                "Options for <b>%1</b><br>"
                "Device ID: %2"
            ).arg(
                deviceName,
                deviceId.toHtmlEscaped()
            ));
            scheduleSelectedDeviceDetailsHeightUpdate();
        }

        scanWhenUnmountedCheckBox_->setEnabled(true);
        showOfflineResultsCheckBox_->setEnabled(true);

        const bool deviceEnabled =
            enabledItem && enabledItem->checkState() == Qt::Checked;

        const bool deviceKnown =
            knownDeviceIt != knownDeviceById_.constEnd();

        if (refreshSelectedDeviceIndexButton_) {
            refreshSelectedDeviceIndexButton_->setEnabled(deviceKnown && deviceEnabled);
            refreshSelectedDeviceIndexButton_->setToolTip(
                deviceKnown && deviceEnabled
                    ? QStringLiteral("Refresh this device's index by rescanning it.")
                    : QStringLiteral("Enable this known device before refreshing its index.")
            );
        }

        const bool unmountedScanningSupported = unmountedScanningSupportedForDevice(deviceId);
        const bool liveUpdatesSupported = liveUpdatesSupportedForDevice(deviceId);
        const bool mountedAsFuseblk =
            knownDeviceIt != knownDeviceById_.constEnd() &&
            knownDeviceIt->mounted &&
            knownDeviceIt->mountedFsType.trimmed().toLower() == QStringLiteral("fuseblk");

        scanWhenUnmountedCheckBox_->setEnabled(unmountedScanningSupported);
        liveUpdatesEnabledCheckBox_->setEnabled(liveUpdatesSupported);

        scanWhenUnmountedCheckBox_->setChecked(
            unmountedScanningSupported && scanWhenUnmountedForDevice(deviceId)
        );
        showOfflineResultsCheckBox_->setChecked(showOfflineResultsForDevice(deviceId));
        liveUpdatesEnabledCheckBox_->setChecked(
            liveUpdatesSupported && liveUpdatesEnabledForDevice(deviceId)
        );

        if (unmountedScanningSupported) {
            scanWhenUnmountedCheckBox_->setText(
                QStringLiteral("Scan this device even when it is not mounted")
            );
            scanWhenUnmountedCheckBox_->setToolTip(
                QStringLiteral(
                    "Kerything can scan this filesystem while unmounted because it has a low-level\n"
                    "raw scanner for this filesystem type."
                )
            );
        } else {
            scanWhenUnmountedCheckBox_->setText(
                QStringLiteral("Scan this device even when it is not mounted (EXT4/NTFS only)")
            );

            if (isBtrfsDevice(deviceId)) {
                scanWhenUnmountedCheckBox_->setToolTip(
                    QStringLiteral(
                        "Btrfs is indexed with a dedicated mounted Btrfs scanner so Kerything can handle\n"
                        "subvolumes correctly. It still requires the filesystem to be mounted.\n\n"
                        "Low-level offline scanning is currently available only for EXT4 and NTFS."
                    )
                );
            } else {
                scanWhenUnmountedCheckBox_->setToolTip(
                    QStringLiteral(
                        "This filesystem is indexed with the generic mounted-device scanner,\n"
                        "which uses Linux filesystem APIs and requires the device to be mounted.\n\n"
                        "Low-level offline scanning is currently available only for EXT4 and NTFS."
                    )
                );
            }
        }

        liveUpdatesEnabledCheckBox_->setText(
            QStringLiteral("Watch this device for live updates when mounted")
        );

        if (mountedAsFuseblk) {
            const QString fuseblkLiveUpdatesToolTip = QStringLiteral(
                "You can keep this preference enabled, but this device is currently mounted as fuseblk,\n"
                "which usually means it is using a FUSE driver such as ntfs-3g.\n\n"
                "Live updates are not expected to work for this mount. For NTFS, use a kernel driver\n"
                "such as ntfs3 or the newer kernel ntfs driver."
            );

            liveUpdatesEnabledCheckBox_->setToolTip(fuseblkLiveUpdatesToolTip);

            if (liveUpdatesWarningIconLabel_) {
                liveUpdatesWarningIconLabel_->setToolTip(fuseblkLiveUpdatesToolTip);
                liveUpdatesWarningIconLabel_->setVisible(true);
            }
        }
        else if (liveUpdatesSupported) {
            if (knownDeviceIt != knownDeviceById_.constEnd() &&
                BlockDeviceDisplayUtils::isBtrfsDevice(knownDeviceIt.value())) {
                liveUpdatesEnabledCheckBox_->setToolTip(
                    QStringLiteral(
                        "<qt>"
                        "<p>"
                        "Kerything will try to keep this filesystem up to date using fanotify whenever it is mounted."
                        "</p>"
                        "<p>"
                        "For Btrfs, Kerything may create a private, read-only helper mount so it can watch "
                        "the filesystem root reliably, even on systems where your normal mounts are subvolumes "
                        "such as <tt>/@</tt> or <tt>/@home</tt>."
                        "</p>"
                        "<p>"
                        "This helper mount will be located under <tt>/run/kerythingd/btrfs-live</tt> and is "
                        "used only by the Kerything daemon for change monitoring. It is mounted read-only "
                        "with restricted options and is not intended for browsing."
                        "</p>"
                        "<p>Discovered mounted Btrfs subvolumes:</p>"
                        "%1"
                        "</qt>"
                    ).arg(BlockDeviceDisplayUtils::btrfsMountedSubvolumesTableHtml(knownDeviceIt.value()))
                );
            }
            else {
                liveUpdatesEnabledCheckBox_->setToolTip(
                    QStringLiteral(
                        "Kerything will try to keep this filesystem up to date using fanotify whenever it is mounted.\n"
                        "If the mounted filesystem or kernel driver does not support the required file-handle features,\n"
                        "Kerything will report that live updates are unavailable or that a rescan is needed."
                    )
                );
            }

            if (liveUpdatesWarningIconLabel_) {
                liveUpdatesWarningIconLabel_->setToolTip(QString());
                liveUpdatesWarningIconLabel_->setVisible(false);
            }
        } else {
            liveUpdatesEnabledCheckBox_->setToolTip(
                QStringLiteral("Live updates require a known filesystem type.")
            );

            if (liveUpdatesWarningIconLabel_) {
                liveUpdatesWarningIconLabel_->setToolTip(QString());
                liveUpdatesWarningIconLabel_->setVisible(false);
            }
        }
    };

    connect(deviceTable_, &QTableWidget::currentCellChanged, this, updateSelectedDeviceOptions);

    connect(deviceTable_, &QTableWidget::itemChanged, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(scanWhenUnmountedCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        if (item) {
            item->setData(ScanWhenUnmountedRole, checked);
        }

        updateApplyButtonEnabled();
    });

    connect(showOfflineResultsCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        if (item) {
            item->setData(ShowOfflineResultsRole, checked);
        }

        updateApplyButtonEnabled();
    });

    connect(liveUpdatesEnabledCheckBox_, &QCheckBox::toggled, this, [this](bool checked) {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;
        if (item) {
            item->setData(LiveUpdatesEnabledRole, checked);
        }

        updateApplyButtonEnabled();
    });

    connect(refreshSelectedDeviceIndexButton_, &QPushButton::clicked, this, [this]() {
        const int row = deviceTable_ ? deviceTable_->currentRow() : -1;
        auto* item = row >= 0 ? deviceTable_->item(row, DeviceEnabledColumn) : nullptr;

        if (!item || item->checkState() != Qt::Checked) {
            return;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        if (!deviceId.isEmpty()) {
            Q_EMIT refreshIndexRequested(deviceId);
        }
    });

    if (deviceTable_->rowCount() > 0) {
        deviceTable_->setCurrentCell(0, DeviceNameColumn);
    }

    updateSelectedDeviceOptions();
    scheduleSelectedDeviceDetailsHeightUpdate();

    return page;
}

QWidget* PreferencesDialog::createIndexesPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(
        QStringLiteral(
            "<h2>Indexes</h2>"
            "<p>View the in-memory search indexes currently held by Kerything. "
            "You can refresh a single available index, or forget an index so its files no longer appear in search results.</p>"
        ),
        page
    );
    title->setWordWrap(true);
    layout->addWidget(title);

    indexTable_ = new QTableWidget(page);
    indexTable_->setColumnCount(IndexColumnCount);
    indexTable_->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Status"),
        QStringLiteral("Records"),
        QStringLiteral("Filesystem"),
        QStringLiteral("Mount point"),
        QStringLiteral("Device"),
        QStringLiteral("Last indexed"),
    });

    indexTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    indexTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    indexTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    indexTable_->setAlternatingRowColors(true);
    indexTable_->setShowGrid(false);
    indexTable_->setWordWrap(false);
    indexTable_->verticalHeader()->setVisible(false);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexNameColumn, QHeaderView::ResizeToContents);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexStatusColumn, QHeaderView::ResizeToContents);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexRecordsColumn, QHeaderView::ResizeToContents);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexFilesystemColumn, QHeaderView::ResizeToContents);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexMountPointColumn, QHeaderView::Stretch);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexDeviceColumn, QHeaderView::ResizeToContents);
    indexTable_->horizontalHeader()->setSectionResizeMode(IndexLastIndexedColumn, QHeaderView::ResizeToContents);
    installHoverRowHighlight(indexTable_);

    populateIndexTable();

    layout->addWidget(indexTable_, 1);

    auto* buttonLayout = new QHBoxLayout();

    refreshIndexButton_ = new QPushButton(QStringLiteral("Refresh Index"), page);
    forgetIndexButton_ = new QPushButton(QStringLiteral("Forget Index"), page);

    setButtonIcon(refreshIndexButton_, QStringLiteral("view-refresh"), QStringLiteral("reload"));
    setButtonIcon(forgetIndexButton_, QStringLiteral("edit-delete"), QStringLiteral("list-remove"));

    refreshIndexButton_->setEnabled(false);
    forgetIndexButton_->setEnabled(false);

    buttonLayout->addWidget(refreshIndexButton_);
    buttonLayout->addWidget(forgetIndexButton_);
    buttonLayout->addStretch();

    layout->addLayout(buttonLayout);

    auto updateIndexButtonStates = [this]() {
        const int row = indexTable_ ? indexTable_->currentRow() : -1;
        const bool hasSelection = row >= 0 && row < indexTable_->rowCount();

        QString deviceId;
        if (hasSelection) {
            if (const auto* item = indexTable_->item(row, IndexNameColumn)) {
                deviceId = item->data(IndexDeviceIdRole).toString();
            }
        }

        const bool knownDeviceAvailable =
            !deviceId.isEmpty() &&
            knownDeviceById_.contains(deviceId);

        if (refreshIndexButton_) {
            refreshIndexButton_->setEnabled(knownDeviceAvailable);
            refreshIndexButton_->setToolTip(
                knownDeviceAvailable
                    ? QStringLiteral("Refresh this index by rescanning its device.")
                    : QStringLiteral("This index cannot be refreshed because its device is no longer available.")
            );
        }

        if (forgetIndexButton_) {
            forgetIndexButton_->setEnabled(hasSelection && !deviceId.isEmpty());
            forgetIndexButton_->setToolTip(
                QStringLiteral("Forget this in-memory index and remove its files from search results.")
            );
        }
    };

    connect(indexTable_, &QTableWidget::currentCellChanged, this, updateIndexButtonStates);

    connect(refreshIndexButton_, &QPushButton::clicked, this, [this]() {
        if (!indexTable_) {
            return;
        }

        const int row = indexTable_->currentRow();
        const auto* item = row >= 0 ? indexTable_->item(row, IndexNameColumn) : nullptr;
        const QString deviceId = item ? item->data(IndexDeviceIdRole).toString() : QString();

        if (!deviceId.isEmpty()) {
            Q_EMIT refreshIndexRequested(deviceId);
        }
    });

    connect(forgetIndexButton_, &QPushButton::clicked, this, [this]() {
        if (!indexTable_) {
            return;
        }

        const int row = indexTable_->currentRow();
        const auto* item = row >= 0 ? indexTable_->item(row, IndexNameColumn) : nullptr;
        const QString deviceId = item ? item->data(IndexDeviceIdRole).toString() : QString();
        const QString name = item ? item->text() : QStringLiteral("this index");

        if (deviceId.isEmpty()) {
            return;
        }

        const bool knownDevice = knownDeviceById_.contains(deviceId);

        const QString rescanText = knownDevice
            ? QStringLiteral(
                "You can rescan this device later from the Devices page, or by pressing F5."
            )
            : QStringLiteral(
                "This device is not currently available. If you reconnect or mount it again, "
                "Kerything can index it again once the device is discovered."
            );

        QMessageBox confirmBox(this);
        confirmBox.setIcon(QMessageBox::Question);
        confirmBox.setWindowTitle(QStringLiteral("Forget Index?"));
        confirmBox.setText(
            QStringLiteral(
                "Forget the index for %1?\n\n"
                "Its files will be removed from search results. "
                "This does not delete files from disk and does not change device preferences.\n\n"
                "%2"
            ).arg(name, rescanText)
        );
        confirmBox.setStandardButtons(QMessageBox::Discard | QMessageBox::Cancel);
        confirmBox.setDefaultButton(QMessageBox::Cancel);
        confirmBox.button(QMessageBox::Discard)->setText(QStringLiteral("Forget"));

        if (confirmBox.exec() != QMessageBox::Discard) {
            return;
        }

        Q_EMIT forgetIndexRequested(deviceId);
    });

    if (indexTable_->rowCount() > 0) {
        indexTable_->setCurrentCell(0, IndexNameColumn);
    }

    QTimer::singleShot(0, this, updateIndexButtonStates);

    return page;
}

QWidget* PreferencesDialog::createFiltersPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(
        QStringLiteral(
            "<h2>Filters</h2>"
            "<p>Create and manage the filter presets shown in the Filter menu. "
            "Filters are saved search shortcuts that are added to your current search text. "
            "Optional macros let you type a keyword followed by a colon, such as <tt>audio:</tt>, "
            "directly in the search box to apply that filter to the search query. "
            "To edit a filter, double-click a name, macro, or query cell, or select a cell and press F2.</p>"
        ),
        page
    );
    title->setWordWrap(true);
    layout->addWidget(title);

    filterTable_ = new QTableWidget(page);
    filterTable_->setColumnCount(FilterColumnCount);
    filterTable_->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Macro"),
        QStringLiteral("Query"),
    });

    filterTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    filterTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    filterTable_->setEditTriggers(
        QAbstractItemView::DoubleClicked |
        QAbstractItemView::EditKeyPressed |
        QAbstractItemView::SelectedClicked
    );
    filterTable_->setAlternatingRowColors(true);
    filterTable_->setShowGrid(false);
    filterTable_->setWordWrap(false);
    filterTable_->verticalHeader()->setVisible(false);
    filterTable_->horizontalHeader()->setSectionResizeMode(FilterNameColumn, QHeaderView::ResizeToContents);
    filterTable_->horizontalHeader()->setSectionResizeMode(FilterMacroColumn, QHeaderView::ResizeToContents);
    filterTable_->horizontalHeader()->setSectionResizeMode(FilterQueryColumn, QHeaderView::Stretch);
    installHoverRowHighlight(filterTable_);
    filterTable_->installEventFilter(this);

    populateFilterTable();

    layout->addWidget(filterTable_, 1);

    auto* buttonLayout = new QHBoxLayout();

    addFilterButton_ = new QPushButton(QStringLiteral("Add"), page);
    duplicateFilterButton_ = new QPushButton(QStringLiteral("Duplicate"), page);
    removeFilterButton_ = new QPushButton(QStringLiteral("Remove"), page);
    moveFilterUpButton_ = new QPushButton(QStringLiteral("Move Up"), page);
    moveFilterDownButton_ = new QPushButton(QStringLiteral("Move Down"), page);
    restoreDefaultFiltersButton_ = new QPushButton(QStringLiteral("Restore Defaults"), page);

    setButtonIcon(addFilterButton_, QStringLiteral("list-add"), QStringLiteral("document-new"));
    setButtonIcon(duplicateFilterButton_, QStringLiteral("edit-copy"), QStringLiteral("document-duplicate"));
    setButtonIcon(removeFilterButton_, QStringLiteral("list-remove"), QStringLiteral("edit-delete"));
    setButtonIcon(moveFilterUpButton_, QStringLiteral("go-up"), QStringLiteral("arrow-up"));
    setButtonIcon(moveFilterDownButton_, QStringLiteral("go-down"), QStringLiteral("arrow-down"));
    setButtonIcon(restoreDefaultFiltersButton_, QStringLiteral("document-revert"), QStringLiteral("edit-undo"));

    duplicateFilterButton_->setEnabled(false);
    removeFilterButton_->setEnabled(false);
    moveFilterUpButton_->setEnabled(false);
    moveFilterDownButton_->setEnabled(false);

    buttonLayout->addWidget(addFilterButton_);
    buttonLayout->addWidget(duplicateFilterButton_);
    buttonLayout->addWidget(removeFilterButton_);
    buttonLayout->addWidget(moveFilterUpButton_);
    buttonLayout->addWidget(moveFilterDownButton_);
    buttonLayout->addStretch();
    buttonLayout->addWidget(restoreDefaultFiltersButton_);

    layout->addLayout(buttonLayout);

    connect(filterTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const bool hadValidationState = hasFilterValidationState();

        if (item) {
            clearFilterValidationState();

            const QSignalBlocker blocker(filterTable_);

            if (item->column() == FilterMacroColumn) {
                const QString macro = normalizedFilterMacro(item->text());
                item->setToolTip(
                    macro.isEmpty()
                        ? QStringLiteral("Optional macro. Example: audio lets you type audio: in the search box.")
                        : QStringLiteral("%1:").arg(macro)
                );
            } else if (item->column() == FilterQueryColumn) {
                item->setToolTip(item->text().trimmed());
            }
        }

        if (hadValidationState) {
            validateFilters(nullptr, false);
        }

        updateApplyButtonEnabled();
    });

    connect(filterTable_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        updateFilterButtonStates();
    });

    connect(addFilterButton_, &QPushButton::clicked, this, [this]() {
        const int row = filterTable_->rowCount();
        filterTable_->insertRow(row);

        auto* nameItem = new QTableWidgetItem(uniqueFilterName(QStringLiteral("New Filter")));
        nameItem->setData(FilterIdRole, newCustomFilterId());

        auto* macroItem = new QTableWidgetItem();
        macroItem->setToolTip(QStringLiteral("Optional macro, such as audio. Type audio: in search to use this filter."));

        auto* queryItem = new QTableWidgetItem(QStringLiteral("ext:"));

        filterTable_->setItem(row, FilterNameColumn, nameItem);
        filterTable_->setItem(row, FilterMacroColumn, macroItem);
        filterTable_->setItem(row, FilterQueryColumn, queryItem);
        filterTable_->setCurrentCell(row, FilterNameColumn);
        filterTable_->editItem(nameItem);

        updateFilterButtonStates();
        updateApplyButtonEnabled();
    });

    connect(duplicateFilterButton_, &QPushButton::clicked, this, [this]() {
        const QModelIndexList selectedRows = filterTable_->selectionModel()->selectedRows();
        if (selectedRows.isEmpty()) {
            return;
        }

        const int sourceRow = selectedRows.first().row();
        const auto* sourceNameItem = filterTable_->item(sourceRow, FilterNameColumn);
        const auto* sourceMacroItem = filterTable_->item(sourceRow, FilterMacroColumn);
        const auto* sourceQueryItem = filterTable_->item(sourceRow, FilterQueryColumn);

        if (!sourceNameItem || !sourceQueryItem) {
            return;
        }

        const int row = filterTable_->rowCount();
        filterTable_->insertRow(row);

        auto* nameItem = new QTableWidgetItem(uniqueFilterName(sourceNameItem->text() + QStringLiteral(" Copy")));
        nameItem->setData(FilterIdRole, newCustomFilterId());

        auto* macroItem = new QTableWidgetItem();
        macroItem->setToolTip(QStringLiteral("Optional macro, such as audio. Type audio: in search to use this filter."));

        auto* queryItem = new QTableWidgetItem(sourceQueryItem->text());

        filterTable_->setItem(row, FilterNameColumn, nameItem);
        filterTable_->setItem(row, FilterMacroColumn, macroItem);
        filterTable_->setItem(row, FilterQueryColumn, queryItem);
        filterTable_->setCurrentCell(row, FilterNameColumn);
        filterTable_->editItem(nameItem);

        updateFilterButtonStates();
        updateApplyButtonEnabled();
    });

    connect(removeFilterButton_, &QPushButton::clicked, this, [this]() {
        QModelIndexList selectedRows = filterTable_->selectionModel()->selectedRows();
        if (selectedRows.isEmpty()) {
            return;
        }

        std::ranges::sort(selectedRows, [](const QModelIndex& lhs, const QModelIndex& rhs) {
            return lhs.row() > rhs.row();
        });

        const QString message = selectedRows.size() == 1
            ? QStringLiteral("Remove the selected filter?")
            : QStringLiteral("Remove %1 selected filters?").arg(selectedRows.size());

        const QString title = selectedRows.size() == 1
            ? QStringLiteral("Remove Filter?")
            : QStringLiteral("Remove Filters?");

        QMessageBox confirmBox(this);
        confirmBox.setIcon(QMessageBox::Question);
        confirmBox.setWindowTitle(title);
        confirmBox.setText(message);
        confirmBox.setStandardButtons(QMessageBox::Discard | QMessageBox::Cancel);
        confirmBox.setDefaultButton(QMessageBox::Cancel);
        confirmBox.button(QMessageBox::Discard)->setText(QStringLiteral("Remove"));

        if (confirmBox.exec() != QMessageBox::Discard) {
            return;
        }

        for (const QModelIndex& index : selectedRows) {
            filterTable_->removeRow(index.row());
        }

        updateFilterButtonStates();
        updateApplyButtonEnabled();
    });

    connect(moveFilterUpButton_, &QPushButton::clicked, this, [this]() {
        moveSelectedFilters(-1);
    });

    connect(moveFilterDownButton_, &QPushButton::clicked, this, [this]() {
        moveSelectedFilters(1);
    });

    connect(restoreDefaultFiltersButton_, &QPushButton::clicked, this, [this]() {
        QMessageBox messageBox(this);
        messageBox.setIcon(QMessageBox::Question);
        messageBox.setWindowTitle(QStringLiteral("Restore Default Filters?"));
        messageBox.setText(QStringLiteral(
            "This will restore the default filters and keep any custom filters you created.\n\n"
            "Existing default filters will be reset to their original names and queries.\n\n"
            "The change will not be saved until you click Apply or OK."
        ));

        QPushButton* restoreButton = messageBox.addButton(
            QStringLiteral("Restore Defaults"),
            QMessageBox::AcceptRole
        );
        messageBox.addButton(QMessageBox::Cancel);
        messageBox.setDefaultButton(QMessageBox::Cancel);
        messageBox.setEscapeButton(QMessageBox::Cancel);

        messageBox.exec();

        if (messageBox.clickedButton() != restoreButton) {
            return;
        }

        std::vector<SearchFilterPreference> filters = filtersFromTable();
        const std::vector<SearchFilterPreference> defaults = Preferences::defaultSearchFilters();

        for (const SearchFilterPreference& defaultFilter : defaults) {
            auto existing = std::ranges::find_if(
                filters,
                [&](const SearchFilterPreference& filter) {
                    return filter.id == defaultFilter.id;
                }
            );

            if (existing == filters.end()) {
                filters.push_back(defaultFilter);
            } else {
                *existing = defaultFilter;
            }
        }

        populateFilterTable(filters);
        updateApplyButtonEnabled();
        updateFilterButtonStates();
    });

    updateFilterButtonStates();

    return page;
}

QWidget* PreferencesDialog::createWindowsPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);

    auto* label = new QLabel(
        QStringLiteral(
            "<h2>Windows</h2>"
            "<p>Configure how Kerything opens and initializes search windows.</p>"
        ),
        page
    );
    label->setWordWrap(true);

    pageLayout->addWidget(label);

    auto* scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* launchGroup = new QGroupBox(QStringLiteral("Launch"), content);
    auto* launchLayout = new QVBoxLayout(launchGroup);

    createNewWindowOnLaunchCheckBox_ = new QCheckBox(
        QStringLiteral("Create a new window when running Kerything"),
        launchGroup
    );
    createNewWindowOnLaunchCheckBox_->setChecked(preferences_.createNewWindowOnLaunch());
    createNewWindowOnLaunchCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, launching Kerything while it is already running opens another search window.\n"
            "When disabled, Kerything tries to present the last active window instead."
        )
    );

    auto* createNewWindowOnLaunchDescription = new QLabel(
        QStringLiteral(
            "When enabled, launching Kerything while it is already running opens another search window. "
            "When disabled, Kerything tries to present the last active window instead.\n"
            "Some Wayland compositors may ignore requests to raise or activate existing windows."
        ),
        launchGroup
    );
    createNewWindowOnLaunchDescription->setWordWrap(true);

    launchLayout->addWidget(createNewWindowOnLaunchCheckBox_);
    launchLayout->addWidget(createNewWindowOnLaunchDescription);

    layout->addWidget(launchGroup);

    auto* newWindowsGroup = new QGroupBox(QStringLiteral("New windows"), content);
    auto* newWindowsLayout = new QVBoxLayout(newWindowsGroup);

    carryFilterToNewWindowsCheckBox_ = new QCheckBox(
        QStringLiteral("Carry over the active filter to new windows"),
        newWindowsGroup
    );
    carryFilterToNewWindowsCheckBox_->setChecked(preferences_.carryFilterToNewWindows());
    carryFilterToNewWindowsCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, File > New Window and Ctrl+N copy the current window’s active filter\n"
            "into the new search window."
        )
    );

    carrySearchOptionsToNewWindowsCheckBox_ = new QCheckBox(
        QStringLiteral("Carry over search options to new windows"),
        newWindowsGroup
    );
    carrySearchOptionsToNewWindowsCheckBox_->setChecked(preferences_.carrySearchOptionsToNewWindows());
    carrySearchOptionsToNewWindowsCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, File > New Window and Ctrl+N copy the current window's Match Case,\n"
            "Match Whole Word, and Regex into the new search window."
        )
    );

    carrySearchTextToNewWindowsCheckBox_ = new QCheckBox(
        QStringLiteral("Carry over the search text to new windows"),
        newWindowsGroup
    );
    carrySearchTextToNewWindowsCheckBox_->setChecked(preferences_.carrySearchTextToNewWindows());
    carrySearchTextToNewWindowsCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, File > New Window and Ctrl+N copy the current search text\n"
            "into the new search window."
        )
    );

    carryResultSortingToNewWindowsCheckBox_ = new QCheckBox(
        QStringLiteral("Carry over result sorting to new windows"),
        newWindowsGroup
    );
    carryResultSortingToNewWindowsCheckBox_->setChecked(preferences_.carryResultSortingToNewWindows());
    carryResultSortingToNewWindowsCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, File > New Window and Ctrl+N copy the current result sort column\n"
            "and direction into the new search window."
        )
    );

    carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_ = new QCheckBox(
        QStringLiteral("Carry over window size and column widths to new windows"),
        newWindowsGroup
    );
    carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_->setChecked(preferences_.carryWindowSizeAndColumnWidthsToNewWindows());
    carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, File > New Window and Ctrl+N use the current window's width,\n"
            "height, and search result column widths for the new search window."
        )
    );

    auto* newWindowStateDescription = new QLabel(
        QStringLiteral(
            "These options control what is copied from the current window when you open another window "
            "using File > New Window or Ctrl+N."
        ),
        newWindowsGroup
    );
    newWindowStateDescription->setWordWrap(true);

    newWindowsLayout->addWidget(carryFilterToNewWindowsCheckBox_);
    newWindowsLayout->addWidget(carrySearchOptionsToNewWindowsCheckBox_);
    newWindowsLayout->addWidget(carrySearchTextToNewWindowsCheckBox_);
    newWindowsLayout->addWidget(carryResultSortingToNewWindowsCheckBox_);
    newWindowsLayout->addWidget(carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_);
    newWindowsLayout->addWidget(newWindowStateDescription);

    layout->addWidget(newWindowsGroup);
    layout->addStretch();

    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea, 1);

    connect(createNewWindowOnLaunchCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(carryFilterToNewWindowsCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(carrySearchOptionsToNewWindowsCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(carrySearchTextToNewWindowsCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(carryResultSortingToNewWindowsCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    return page;
}

QWidget* PreferencesDialog::createUiPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);

    auto* label = new QLabel(
        QStringLiteral(
            "<h2>UI</h2>"
            "<p>Configure user-interface behavior.</p>"
        ),
        page
    );
    label->setWordWrap(true);

    pageLayout->addWidget(label);

    auto* scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* filtersGroup = new QGroupBox(QStringLiteral("Filters"), content);
    auto* filtersLayout = new QVBoxLayout(filtersGroup);

    showFiltersDropdownCheckBox_ = new QCheckBox(
        QStringLiteral("Show filter dropdown next to the search box"),
        filtersGroup
    );
    showFiltersDropdownCheckBox_->setChecked(preferences_.showFiltersDropdown());
    showFiltersDropdownCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, each search window shows a filter dropdown on the right side of the search box."
        )
    );

    auto* filtersDescription = new QLabel(
        QStringLiteral(
            "The dropdown contains the same filter choices as the Filter menu and lets you switch filters without opening the menu."
        ),
        filtersGroup
    );
    filtersDescription->setWordWrap(true);

    filtersLayout->addWidget(showFiltersDropdownCheckBox_);
    filtersLayout->addWidget(filtersDescription);

    layout->addWidget(filtersGroup);

    // --- Preview Pane Group ---
    auto* previewGroup = new QGroupBox(QStringLiteral("Preview Pane"), content);
    auto* previewLayout = new QVBoxLayout(previewGroup);

    showPreviewPaneCheckBox_ = new QCheckBox(
        QStringLiteral("Show preview pane by default"),
        previewGroup
    );
    showPreviewPaneCheckBox_->setChecked(preferences_.showPreviewPane());

    previewLayout->addWidget(showPreviewPaneCheckBox_);

    layout->addWidget(previewGroup);

    auto* sortingGroup = new QGroupBox(QStringLiteral("Sorting"), content);
    auto* sortingLayout = new QVBoxLayout(sortingGroup);

    auto* defaultSortRow = new QWidget(sortingGroup);
    auto* defaultSortLayout = new QHBoxLayout(defaultSortRow);
    defaultSortLayout->setContentsMargins(0, 0, 0, 0);

    auto* defaultSortColumnLabel = new QLabel(QStringLiteral("Default sort column:"), defaultSortRow);

    defaultSortColumnComboBox_ = new QComboBox(defaultSortRow);
    defaultSortColumnComboBox_->addItem(QStringLiteral("Name"), SearchResultColumn::Name);
    defaultSortColumnComboBox_->addItem(QStringLiteral("Path"), SearchResultColumn::Path);
    defaultSortColumnComboBox_->addItem(QStringLiteral("Size"), SearchResultColumn::Size);
    defaultSortColumnComboBox_->addItem(QStringLiteral("Date Modified"), SearchResultColumn::DateModified);

    const int defaultSortColumnIndex = defaultSortColumnComboBox_->findData(preferences_.defaultSortColumn());
    defaultSortColumnComboBox_->setCurrentIndex(defaultSortColumnIndex >= 0 ? defaultSortColumnIndex : 0);
    defaultSortColumnComboBox_->setToolTip(
        QStringLiteral("Choose the sort column used when a new search window does not carry sorting from another window.")
    );

    auto* defaultSortOrderLabel = new QLabel(QStringLiteral("Direction:"), defaultSortRow);

    defaultSortOrderComboBox_ = new QComboBox(defaultSortRow);
    defaultSortOrderComboBox_->addItem(
        QStringLiteral("Ascending"),
        static_cast<int>(Qt::AscendingOrder)
    );
    defaultSortOrderComboBox_->addItem(
        QStringLiteral("Descending"),
        static_cast<int>(Qt::DescendingOrder)
    );

    const int defaultSortOrderIndex = defaultSortOrderComboBox_->findData(
        static_cast<int>(preferences_.defaultSortOrder())
    );
    defaultSortOrderComboBox_->setCurrentIndex(defaultSortOrderIndex >= 0 ? defaultSortOrderIndex : 0);
    defaultSortOrderComboBox_->setToolTip(
        QStringLiteral("Choose the sort direction used when a new search window does not carry sorting from another window.")
    );

    defaultSortLayout->addWidget(defaultSortColumnLabel);
    defaultSortLayout->addWidget(defaultSortColumnComboBox_);
    defaultSortLayout->addSpacing(12);
    defaultSortLayout->addWidget(defaultSortOrderLabel);
    defaultSortLayout->addWidget(defaultSortOrderComboBox_);
    defaultSortLayout->addStretch();

    sortDateDescendingFirstCheckBox_ = new QCheckBox(
        QStringLiteral("Sort Date Modified descending (newest files) first"),
        sortingGroup
    );
    sortDateDescendingFirstCheckBox_->setChecked(preferences_.sortDateDescendingFirst());
    sortDateDescendingFirstCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, the first click on the Date Modified column sorts newest files first."
        )
    );

    sortSizeDescendingFirstCheckBox_ = new QCheckBox(
        QStringLiteral("Sort Size descending (largest files) first"),
        sortingGroup
    );
    sortSizeDescendingFirstCheckBox_->setChecked(preferences_.sortSizeDescendingFirst());
    sortSizeDescendingFirstCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, the first click on the Size column sorts largest files first."
        )
    );

    sortingLayout->addWidget(defaultSortRow);
    sortingLayout->addWidget(sortDateDescendingFirstCheckBox_);
    sortingLayout->addWidget(sortSizeDescendingFirstCheckBox_);

    auto* sortingDescription = new QLabel(
        QStringLiteral(
            "The default sort is used for new search windows unless result sorting is carried over from an existing window. "
            "The first-click options only affect the first click when changing to the Size or Date Modified column. "
            "Further clicks continue toggling the sort direction normally."
        ),
        sortingGroup
    );
    sortingDescription->setWordWrap(true);
    sortingLayout->addWidget(sortingDescription);

    layout->addWidget(sortingGroup);

    auto* resultsGroup = new QGroupBox(QStringLiteral("Search results"), content);
    auto* resultsLayout = new QVBoxLayout(resultsGroup);

    showInFileManagerOnPathDoubleClickCheckBox_ = new QCheckBox(
        QStringLiteral("Show in file manager when double-clicking the Path column"),
        resultsGroup
    );
    showInFileManagerOnPathDoubleClickCheckBox_->setChecked(
        preferences_.showInFileManagerOnPathDoubleClick()
    );
    showInFileManagerOnPathDoubleClickCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, double-clicking the Path cell of a search result opens the item’s location\n"
            "in the file manager instead of opening the file."
        )
    );

    auto* showInFileManagerOnPathDoubleClickDescription = new QLabel(
        QStringLiteral(
            "When enabled, the Path column can be used as a shortcut for locating files in your file manager."
        ),
        resultsGroup
    );
    showInFileManagerOnPathDoubleClickDescription->setWordWrap(true);

    resultsLayout->addWidget(showInFileManagerOnPathDoubleClickCheckBox_);
    resultsLayout->addWidget(showInFileManagerOnPathDoubleClickDescription);

    showHighlightedSearchTermsCheckBox_ = new QCheckBox(
        QStringLiteral("Show highlighted search terms in result names"),
        resultsGroup
    );
    showHighlightedSearchTermsCheckBox_->setChecked(
        preferences_.showHighlightedSearchTerms()
    );
    showHighlightedSearchTermsCheckBox_->setToolTip(
        QStringLiteral(
            "When enabled, matching search text is shown in bold in the Name column."
        )
    );

    auto* showHighlightedSearchTermsDescription = new QLabel(
        QStringLiteral(
            "When enabled, matching search text is shown in bold in the Name column. Search term highlighting "
            "only affects visible result text and does not change sorting or matching."
        ),
        resultsGroup
    );
    showHighlightedSearchTermsDescription->setWordWrap(true);

    resultsLayout->addWidget(showHighlightedSearchTermsCheckBox_);
    resultsLayout->addWidget(showHighlightedSearchTermsDescription);

    layout->addWidget(resultsGroup);
    layout->addStretch();

    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea, 1);

    connect(showFiltersDropdownCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(defaultSortColumnComboBox_, &QComboBox::currentIndexChanged, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(defaultSortOrderComboBox_, &QComboBox::currentIndexChanged, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(sortDateDescendingFirstCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(sortSizeDescendingFirstCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(showInFileManagerOnPathDoubleClickCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    connect(showHighlightedSearchTermsCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    return page;
}

QWidget* PreferencesDialog::createAdvancedPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);

    auto* label = new QLabel(
        QStringLiteral(
            "<h2>Advanced</h2>"
            "<p>Advanced maintenance actions and application-wide preferences for Kerything.</p>"
        ),
        page
    );
    label->setWordWrap(true);

    pageLayout->addWidget(label);

    auto* scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* liveUpdatesGroup = new QGroupBox(QStringLiteral("Live updates"), content);
    auto* liveUpdatesLayout = new QVBoxLayout(liveUpdatesGroup);

    autoRefreshLiveUpdatesCheckBox_ = new QCheckBox(
        QStringLiteral("Automatically refresh and re-sort visible search results as live updates arrive"),
        liveUpdatesGroup
    );
    autoRefreshLiveUpdatesCheckBox_->setChecked(preferences_.autoRefreshResultsForLiveUpdates());
    autoRefreshLiveUpdatesCheckBox_->setToolTip(
        QStringLiteral(
            "When disabled, Kerything still keeps the internal index up to date,\n"
            "but visible search results are not automatically refreshed or re-sorted\n"
            "until you search again or re-enable this option."
        )
    );

    auto* liveUpdatesDescription = new QLabel(
        QStringLiteral(
            "Disable this if many filesystem changes are occurring and you want the visible results list to stay still while you browse it. "
            "The internal index will continue receiving live updates."
        ),
        liveUpdatesGroup
    );
    liveUpdatesDescription->setWordWrap(true);

    liveUpdatesLayout->addWidget(autoRefreshLiveUpdatesCheckBox_);
    liveUpdatesLayout->addWidget(liveUpdatesDescription);

    layout->addWidget(liveUpdatesGroup);

    auto* firstRunGroup = new QGroupBox(QStringLiteral("First-run setup"), content);
    auto* firstRunLayout = new QVBoxLayout(firstRunGroup);

    auto* firstRunDescription = new QLabel(
        QStringLiteral(
            "Resetting first-run setup will make Kerything show the initial device selection "
            "setup again the next time it starts. Your saved device preferences are not deleted."
        ),
        firstRunGroup
    );
    firstRunDescription->setWordWrap(true);

    auto* resetFirstRunButton = new QPushButton(QStringLiteral("Reset first-run setup"), firstRunGroup);

    firstRunLayout->addWidget(firstRunDescription);
    firstRunLayout->addWidget(resetFirstRunButton, 0, Qt::AlignLeft);

    layout->addWidget(firstRunGroup);
    layout->addStretch();

    scrollArea->setWidget(content);
    pageLayout->addWidget(scrollArea, 1);

    connect(resetFirstRunButton, &QPushButton::clicked, this, [this]() {
        const QMessageBox::StandardButton result = QMessageBox::question(
            this,
            QStringLiteral("Reset first-run setup?"),
            QStringLiteral(
                "Kerything will show the initial device selection setup again the next time it starts.\n\n"
                "This will not delete your saved device preferences.\n\n"
                "Do you want to continue?"
            ),
            QMessageBox::Reset | QMessageBox::Cancel,
            QMessageBox::Cancel
        );

        if (result != QMessageBox::Reset) {
            return;
        }

        preferences_.setInitialDeviceSelectionCompleted(false);

        QMessageBox::information(
            this,
            QStringLiteral("First-run setup reset"),
            QStringLiteral("First-run setup has been reset and will appear again the next time Kerything starts.")
        );
    });

    connect(autoRefreshLiveUpdatesCheckBox_, &QCheckBox::toggled, this, [this]() {
        updateApplyButtonEnabled();
    });

    return page;
}

void PreferencesDialog::populateDeviceTable()
{
    if (!deviceTable_) {
        return;
    }

    const QSignalBlocker blocker(deviceTable_);

    std::vector<BlockDevice> sortedDevices = knownDevices_;
    std::sort(
        sortedDevices.begin(),
        sortedDevices.end(),
        BlockDeviceDisplayUtils::deviceLessThan
    );

    deviceTable_->clearContents();
    deviceTable_->setRowCount(static_cast<int>(sortedDevices.size()));

    const QPalette palette = deviceTable_->palette();
    const QBrush unmountedForeground = palette.brush(QPalette::PlaceholderText);

    int row = 0;
    for (const BlockDevice& blockDevice : sortedDevices) {
        const IndexedDevicePreference preference =
            preferences_.indexedDevicePreference(blockDevice.deviceId).value_or(IndexedDevicePreference{
                .deviceId = blockDevice.deviceId
            });

        auto* enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enabledItem->setCheckState(preference.enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setData(DeviceIdRole, blockDevice.deviceId);
        enabledItem->setData(InitialEnabledRole, preference.enabled);
        enabledItem->setData(
            ScanWhenUnmountedRole,
            DeviceCapabilities::deviceSupportsUnmountedScanning(blockDevice) && preference.scanWhenUnmounted
        );
        enabledItem->setData(ShowOfflineResultsRole, preference.showOfflineResults);
        enabledItem->setData(
            LiveUpdatesEnabledRole,
            DeviceCapabilities::deviceSupportsLiveUpdates(blockDevice) && preference.liveUpdatesEnabled
        );
        deviceTable_->setItem(row, DeviceEnabledColumn, enabledItem);

        auto* nameItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayNameForBlockDevice(blockDevice));
        nameItem->setToolTip(blockDevice.deviceId);
        deviceTable_->setItem(row, DeviceNameColumn, nameItem);

        auto* statusItem = new QTableWidgetItem(blockDevice.mounted ? QStringLiteral("Mounted") : QStringLiteral("Not mounted"));

        if (blockDevice.mounted &&
            blockDevice.mountedFsType.trimmed().toLower() == QStringLiteral("fuseblk")) {
            if (deviceTable_ && deviceTable_->style()) {
                statusItem->setData(
                    Qt::DecorationRole,
                    deviceTable_->style()
                        ->standardIcon(QStyle::SP_MessageBoxWarning)
                        .pixmap(16, 16)
                );
            }
            statusItem->setToolTip(
                QStringLiteral(
                    "This device is mounted as fuseblk, which usually means it is using a FUSE driver such as ntfs-3g.\n\n"
                    "Kerything can index the mounted files, but live updates are not expected to work for this mount.\n\n"
                    "For NTFS, live updates require a kernel driver such as ntfs3 or the newer kernel ntfs driver."
                )
            );
        }
        else if (blockDevice.mounted && !blockDevice.mountedFsType.trimmed().isEmpty()) {
            statusItem->setToolTip(
                QStringLiteral("Mounted as %1").arg(blockDevice.mountedFsType.trimmed())
            );
        }

        deviceTable_->setItem(row, DeviceStatusColumn, statusItem);

        auto* fsTypeItem = new QTableWidgetItem(
            BlockDeviceDisplayUtils::filesystemDisplayTextForBlockDevice(blockDevice)
        );
        fsTypeItem->setToolTip(
            BlockDeviceDisplayUtils::filesystemToolTipForBlockDevice(blockDevice)
        );
        deviceTable_->setItem(row, DeviceFsTypeColumn, fsTypeItem);

        const QString mountPointText =
            BlockDeviceDisplayUtils::mountPointSummaryForBlockDevice(blockDevice);

        auto* mountPointItem = new QTableWidgetItem(mountPointText);
        mountPointItem->setToolTip(
            BlockDeviceDisplayUtils::mountPointToolTipForBlockDevice(blockDevice)
        );
        deviceTable_->setItem(row, DeviceMountPointColumn, mountPointItem);

        auto* devNodeItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.devNode));
        devNodeItem->setToolTip(blockDevice.deviceId);
        deviceTable_->setItem(row, DeviceNodeColumn, devNodeItem);

        auto* modelItem = new QTableWidgetItem(BlockDeviceDisplayUtils::displayOrDash(blockDevice.diskModel));
        deviceTable_->setItem(row, DeviceModelColumn, modelItem);

        if (!blockDevice.mounted) {
            for (int column = 0; column < DeviceColumnCount; ++column) {
                if (auto* item = deviceTable_->item(row, column)) {
                    item->setForeground(unmountedForeground);
                }
            }
        }

        ++row;
    }
}

void PreferencesDialog::populateIndexTable()
{
    if (!indexTable_) {
        return;
    }

    const QSignalBlocker blocker(indexTable_);

    indexTable_->clearContents();
    indexTable_->setRowCount(static_cast<int>(indexSummaries_.size()));

    const QLocale locale;
    const QPalette palette = indexTable_->palette();
    const QBrush unavailableForeground = palette.brush(QPalette::PlaceholderText);

    int row = 0;
    for (const IndexSummary& summary : indexSummaries_) {
        const bool knownDevice = !summary.deviceId.isEmpty() && knownDeviceById_.contains(summary.deviceId);

        QString status;
        if (!summary.ready) {
            status = QStringLiteral("Indexing");
        }
        else if (!knownDevice) {
            status = summary.searchable
                ? QStringLiteral("Device unavailable")
                : QStringLiteral("Unavailable");
        }
        else if (!summary.mounted && summary.searchable) {
            status = QStringLiteral("Ready, offline");
        }
        else if (!summary.searchable) {
            status = QStringLiteral("Hidden offline");
        }
        else {
            status = QStringLiteral("Ready");
        }

        auto* nameItem = new QTableWidgetItem(summary.displayName);
        nameItem->setData(IndexDeviceIdRole, summary.deviceId);
        nameItem->setData(IndexIdRole, summary.indexId);
        nameItem->setToolTip(summary.deviceId);
        indexTable_->setItem(row, IndexNameColumn, nameItem);

        auto* statusItem = new QTableWidgetItem(status);
        if (!knownDevice) {
            if (indexTable_ && indexTable_->style()) {
                statusItem->setData(
                    Qt::DecorationRole,
                    indexTable_->style()
                        ->standardIcon(QStyle::SP_MessageBoxWarning)
                        .pixmap(16, 16)
                );
            }

            statusItem->setToolTip(
                QStringLiteral(
                    "The indexed device is no longer visible in the current device list. "
                    "You can keep using offline results or forget this index."
                )
            );
        }
        indexTable_->setItem(row, IndexStatusColumn, statusItem);

        const qsizetype liveRecords = std::max<qsizetype>(
            0,
            summary.recordCount - summary.deletedRecordCount
        );

        auto* recordsItem = new QTableWidgetItem(locale.toString(liveRecords));
        recordsItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        recordsItem->setToolTip(
            summary.deletedRecordCount > 0
                ? QStringLiteral("%1 live records, %2 deleted/tombstoned records")
                    .arg(locale.toString(liveRecords))
                    .arg(locale.toString(summary.deletedRecordCount))
                : QStringLiteral("%1 records").arg(locale.toString(liveRecords))
        );
        indexTable_->setItem(row, IndexRecordsColumn, recordsItem);

        indexTable_->setItem(
            row,
            IndexFilesystemColumn,
            new QTableWidgetItem(summary.fsType.trimmed().isEmpty() ? QStringLiteral("—") : summary.fsType)
        );

        const QString mountPoint = summary.primaryMountPoint.trimmed().isEmpty()
            ? QStringLiteral("—")
            : summary.primaryMountPoint.trimmed();

        auto* mountPointItem = new QTableWidgetItem(mountPoint);
        if (!summary.mountPoints.isEmpty()) {
            mountPointItem->setToolTip(summary.mountPoints.join(QLatin1Char('\n')));
        }
        indexTable_->setItem(row, IndexMountPointColumn, mountPointItem);

        indexTable_->setItem(
            row,
            IndexDeviceColumn,
            new QTableWidgetItem(summary.devNode.trimmed().isEmpty() ? QStringLiteral("—") : summary.devNode)
        );

        QString lastIndexedText = QStringLiteral("—");
        if (summary.lastIndexedTime > 0) {
            const QDateTime lastIndexedDateTime = QDateTime::fromSecsSinceEpoch(
                summary.lastIndexedTime,
                QTimeZone::UTC
            ).toLocalTime();

            lastIndexedText = locale.toString(
                lastIndexedDateTime,
                QLocale::ShortFormat
            );
        }

        indexTable_->setItem(row, IndexLastIndexedColumn, new QTableWidgetItem(lastIndexedText));

        if (!knownDevice) {
            for (int column = 0; column < IndexColumnCount; ++column) {
                if (auto* item = indexTable_->item(row, column)) {
                    item->setForeground(unavailableForeground);
                }
            }
        }

        ++row;
    }
}

void PreferencesDialog::populateFilterTable()
{
    populateFilterTable(preferences_.searchFilters());
}

void PreferencesDialog::populateFilterTable(const std::vector<SearchFilterPreference>& filters)
{
    if (!filterTable_) {
        return;
    }

    const QSignalBlocker blocker(filterTable_);

    filterTable_->clearContents();
    filterTable_->setRowCount(static_cast<int>(filters.size()));

    int row = 0;
    for (const SearchFilterPreference& filter : filters) {
        auto* nameItem = new QTableWidgetItem(filter.name);
        nameItem->setData(FilterIdRole, filter.id);

        auto* macroItem = new QTableWidgetItem(filter.macro);
        macroItem->setToolTip(
            filter.macro.trimmed().isEmpty()
                ? QStringLiteral("Optional macro. Example: audio lets you type audio: in the search box.")
                : QStringLiteral("%1:").arg(filter.macro.trimmed())
        );

        auto* queryItem = new QTableWidgetItem(filter.query);
        queryItem->setToolTip(filter.query);

        filterTable_->setItem(row, FilterNameColumn, nameItem);
        filterTable_->setItem(row, FilterMacroColumn, macroItem);
        filterTable_->setItem(row, FilterQueryColumn, queryItem);

        ++row;
    }

    if (filterTable_->rowCount() > 0) {
        filterTable_->setCurrentCell(0, FilterNameColumn);
    }

    updateFilterButtonStates();
}

QStringList PreferencesDialog::enabledDeviceIdsFromTable() const
{
    QStringList out;

    if (!deviceTable_) {
        return out;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item || item->checkState() != Qt::Checked) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        if (!deviceId.isEmpty()) {
            out << deviceId;
        }
    }

    return out;
}

bool PreferencesDialog::scanWhenUnmountedForDevice(const QString& deviceId) const
{
    if (!unmountedScanningSupportedForDevice(deviceId)) {
        return false;
    }

    if (!deviceTable_) {
        return true;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (item && item->data(DeviceIdRole).toString() == deviceId) {
            return item->data(ScanWhenUnmountedRole).toBool();
        }
    }

    return true;
}

bool PreferencesDialog::showOfflineResultsForDevice(const QString& deviceId) const
{
    if (!deviceTable_) {
        return true;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (item && item->data(DeviceIdRole).toString() == deviceId) {
            return item->data(ShowOfflineResultsRole).toBool();
        }
    }

    return true;
}

bool PreferencesDialog::isBtrfsDevice(const QString& deviceId) const
{
    const auto knownDeviceIt = knownDeviceById_.constFind(deviceId);
    if (knownDeviceIt != knownDeviceById_.constEnd()) {
        return BlockDeviceDisplayUtils::isBtrfsDevice(knownDeviceIt.value());
    }

    const IndexedDevicePreference preference =
        originalPreferencesByDeviceId_.value(
            deviceId,
            IndexedDevicePreference{ .deviceId = deviceId }
        );

    return preference.fsType.trimmed().compare(
        QStringLiteral("btrfs"),
        Qt::CaseInsensitive
    ) == 0;
}

bool PreferencesDialog::unmountedScanningSupportedForDevice(const QString& deviceId) const
{
    const auto knownDeviceIt = knownDeviceById_.constFind(deviceId);
    if (knownDeviceIt != knownDeviceById_.constEnd()) {
        return DeviceCapabilities::deviceSupportsUnmountedScanning(knownDeviceIt.value());
    }

    const IndexedDevicePreference preference =
        originalPreferencesByDeviceId_.value(
            deviceId,
            IndexedDevicePreference{ .deviceId = deviceId }
        );

    return DeviceCapabilities::preferenceSupportsUnmountedScanning(preference);
}

bool PreferencesDialog::liveUpdatesEnabledForDevice(const QString& deviceId) const
{
    if (!liveUpdatesSupportedForDevice(deviceId)) {
        return false;
    }

    if (!deviceTable_) {
        return true;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (item && item->data(DeviceIdRole).toString() == deviceId) {
            return item->data(LiveUpdatesEnabledRole).toBool();
        }
    }

    return true;
}

bool PreferencesDialog::liveUpdatesSupportedForDevice(const QString& deviceId) const
{
    const auto knownDeviceIt = knownDeviceById_.constFind(deviceId);
    if (knownDeviceIt != knownDeviceById_.constEnd()) {
        return DeviceCapabilities::deviceSupportsLiveUpdates(knownDeviceIt.value());
    }

    const IndexedDevicePreference preference =
        originalPreferencesByDeviceId_.value(
            deviceId,
            IndexedDevicePreference{ .deviceId = deviceId }
        );

    return DeviceCapabilities::preferenceSupportsLiveUpdates(preference);
}

void PreferencesDialog::toggleDeviceRowChecked(int row)
{
    if (!deviceTable_ || row < 0 || row >= deviceTable_->rowCount()) {
        return;
    }

    auto* item = deviceTable_->item(row, DeviceEnabledColumn);
    if (!item) {
        return;
    }

    item->setCheckState(
        item->checkState() == Qt::Checked
            ? Qt::Unchecked
            : Qt::Checked
    );

    updateApplyButtonEnabled();
}

std::vector<SearchFilterPreference> PreferencesDialog::filtersFromTable() const
{
    std::vector<SearchFilterPreference> filters;

    if (!filterTable_) {
        return filters;
    }

    filters.reserve(static_cast<std::size_t>(filterTable_->rowCount()));

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        const auto* nameItem = filterTable_->item(row, FilterNameColumn);
        const auto* macroItem = filterTable_->item(row, FilterMacroColumn);
        const auto* queryItem = filterTable_->item(row, FilterQueryColumn);

        if (!nameItem || !queryItem) {
            continue;
        }

        SearchFilterPreference filter;
        filter.id = nameItem->data(FilterIdRole).toString().trimmed();

        if (filter.id.isEmpty()) {
            filter.id = QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        filter.name = nameItem->text().trimmed();
        filter.macro = normalizedFilterMacro(macroItem ? macroItem->text() : QString());
        filter.query = normalizedFilterQuery(queryItem->text());

        filters.push_back(std::move(filter));
    }

    return filters;
}

QString PreferencesDialog::uniqueFilterName(const QString& baseName) const
{
    QStringList existingNames;

    if (filterTable_) {
        for (int row = 0; row < filterTable_->rowCount(); ++row) {
            if (const auto* item = filterTable_->item(row, FilterNameColumn)) {
                existingNames << item->text().trimmed().toCaseFolded();
            }
        }
    }

    QString candidate = baseName.trimmed().isEmpty()
        ? QStringLiteral("New Filter")
        : baseName.trimmed();

    if (!existingNames.contains(candidate.toCaseFolded())) {
        return candidate;
    }

    int suffix = 2;
    while (true) {
        const QString numbered = QStringLiteral("%1 %2").arg(candidate).arg(suffix);
        if (!existingNames.contains(numbered.toCaseFolded())) {
            return numbered;
        }

        ++suffix;
    }
}

QString PreferencesDialog::newCustomFilterId() const
{
    return QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString PreferencesDialog::normalizedFilterMacro(QString macro)
{
    macro = macro.trimmed();

    while (macro.endsWith(QLatin1Char(':'))) {
        macro.chop(1);
        macro = macro.trimmed();
    }

    return macro;
}

QString PreferencesDialog::normalizedExtensionFilterToken(QString token)
{
    const qsizetype colon = token.indexOf(QLatin1Char(':'));

    if (colon <= 0) {
        return token;
    }

    const QString prefix = token.left(colon + 1);
    QString extensionList = token.mid(colon + 1);

    if (extensionList.isEmpty() || !extensionList.contains(QLatin1Char(','))) {
        return token;
    }

    extensionList.replace(QLatin1Char(','), QLatin1Char(';'));
    return prefix + extensionList;
}

bool PreferencesDialog::isBuiltInFilterKeyword(const QString& keyword)
{
    const QString folded = keyword.trimmed().toCaseFolded();

    return folded == QStringLiteral("ext") ||
           folded == QStringLiteral("extension") ||
           folded == QStringLiteral("file") ||
           folded == QStringLiteral("files") ||
           folded == QStringLiteral("folder") ||
           folded == QStringLiteral("folders") ||
           folded == QStringLiteral("type");
}

bool PreferencesDialog::isKnownFilterKeyword(
    const QString& keyword,
    const QSet<QString>& macroKeywords
) {
    const QString folded = keyword.trimmed().toCaseFolded();

    return isBuiltInFilterKeyword(folded) || macroKeywords.contains(folded);
}

QStringList PreferencesDialog::filterMacroReferencesInQuery(
    const QString& query,
    const QSet<QString>& macroKeywords
) {
    QStringList references;

    const QStringList tokens = query.split(
        QRegularExpression(QStringLiteral("\\s+")),
        Qt::SkipEmptyParts
    );

    for (const QString& token : tokens) {
        const QString trimmedToken = token.trimmed();

        if (trimmedToken.size() <= 1 || !trimmedToken.endsWith(QLatin1Char(':'))) {
            continue;
        }

        const QString keyword = trimmedToken.left(trimmedToken.size() - 1).trimmed().toCaseFolded();

        if (macroKeywords.contains(keyword)) {
            references << keyword;
        }
    }

    references.removeDuplicates();
    return references;
}

QString PreferencesDialog::normalizedFilterQuery(QString query)
{
    query = query.trimmed();

    if (query.isEmpty()) {
        return query;
    }

    QStringList normalizedTokens;
    const QStringList tokens = query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    normalizedTokens.reserve(tokens.size());

    for (const QString& token : tokens) {
        QString trimmedToken = token.trimmed();
        const QString foldedToken = trimmedToken.toCaseFolded();

        if (foldedToken == QStringLiteral("ext;")) {
            trimmedToken = QStringLiteral("ext:");
        } else if (foldedToken == QStringLiteral("extension;")) {
            trimmedToken = QStringLiteral("extension:");
        } else if (foldedToken.startsWith(QStringLiteral("ext;"))) {
            trimmedToken = QStringLiteral("ext:") + trimmedToken.mid(4);
        } else if (foldedToken.startsWith(QStringLiteral("extension;"))) {
            trimmedToken = QStringLiteral("extension:") + trimmedToken.mid(10);
        } else if (foldedToken == QStringLiteral("file;")) {
            trimmedToken = QStringLiteral("file:");
        } else if (foldedToken == QStringLiteral("files;")) {
            trimmedToken = QStringLiteral("files:");
        } else if (foldedToken == QStringLiteral("folder;")) {
            trimmedToken = QStringLiteral("folder:");
        } else if (foldedToken == QStringLiteral("folders;")) {
            trimmedToken = QStringLiteral("folders:");
        } else if (foldedToken == QStringLiteral("type;file")) {
            trimmedToken = QStringLiteral("type:file");
        } else if (foldedToken == QStringLiteral("type;files")) {
            trimmedToken = QStringLiteral("type:files");
        } else if (foldedToken == QStringLiteral("type;folder")) {
            trimmedToken = QStringLiteral("type:folder");
        } else if (foldedToken == QStringLiteral("type;folders")) {
            trimmedToken = QStringLiteral("type:folders");
        }

        const QString normalizedFoldedToken = trimmedToken.toCaseFolded();

        if (normalizedFoldedToken.startsWith(QStringLiteral("ext:")) ||
            normalizedFoldedToken.startsWith(QStringLiteral("extension:"))) {
            normalizedTokens << normalizedExtensionFilterToken(trimmedToken);
            continue;
        }

        normalizedTokens << trimmedToken;
    }

    return normalizedTokens.join(QLatin1Char(' '));
}

bool PreferencesDialog::isValidFilterMacro(const QString& macro)
{
    if (macro.isEmpty()) {
        return true;
    }

    for (const QChar c : macro) {
        const ushort value = c.unicode();

        const bool valid =
            (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') ||
            value == '_';

        if (!valid) {
            return false;
        }
    }

    return true;
}

QList<int> PreferencesDialog::selectedFilterRows() const
{
    QList<int> rows;

    if (!filterTable_ || !filterTable_->selectionModel()) {
        return rows;
    }

    const QModelIndexList selectedRows = filterTable_->selectionModel()->selectedRows();
    rows.reserve(selectedRows.size());

    for (const QModelIndex& index : selectedRows) {
        rows << index.row();
    }

    std::ranges::sort(rows);
    return rows;
}

void PreferencesDialog::updateFilterButtonStates()
{
    const QList<int> rows = selectedFilterRows();
    const bool hasSelection = !rows.isEmpty();
    const int rowCount = filterTable_ ? filterTable_->rowCount() : 0;
    const bool canReorder = hasSelection && rowCount > 1;

    if (duplicateFilterButton_) {
        duplicateFilterButton_->setEnabled(hasSelection);
    }

    if (removeFilterButton_) {
        removeFilterButton_->setEnabled(hasSelection);
    }

    if (moveFilterUpButton_) {
        moveFilterUpButton_->setEnabled(canReorder && rows.first() > 0);
    }

    if (moveFilterDownButton_) {
        moveFilterDownButton_->setEnabled(canReorder && rows.last() < rowCount - 1);
    }
}

void PreferencesDialog::moveSelectedFilters(int direction)
{
    if (!filterTable_ || direction == 0) {
        return;
    }

    QList<int> rows = selectedFilterRows();
    if (rows.isEmpty()) {
        return;
    }

    const int rowCount = filterTable_->rowCount();

    if (direction < 0 && rows.first() <= 0) {
        return;
    }

    if (direction > 0 && rows.last() >= rowCount - 1) {
        return;
    }

    filterTable_->clearSelection();

    if (direction > 0) {
        std::ranges::reverse(rows);
    }

    QList<int> movedRows;
    movedRows.reserve(rows.size());

    for (const int row : rows) {
        const int destinationRow = row + direction;

        QList<QTableWidgetItem*> movedItems;
        movedItems.reserve(FilterColumnCount);

        for (int column = 0; column < FilterColumnCount; ++column) {
            movedItems << filterTable_->takeItem(row, column);
        }

        filterTable_->removeRow(row);
        filterTable_->insertRow(destinationRow);

        for (int column = 0; column < FilterColumnCount; ++column) {
            filterTable_->setItem(destinationRow, column, movedItems.at(column));
        }

        movedRows << destinationRow;
    }

    std::ranges::sort(movedRows);

    if (auto* selectionModel = filterTable_->selectionModel()) {
        const int currentRow = direction < 0 ? movedRows.first() : movedRows.last();
        const QModelIndex currentIndex = filterTable_->model()->index(currentRow, FilterNameColumn);

        selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);

        for (const int row : movedRows) {
            const QModelIndex left = filterTable_->model()->index(row, 0);
            const QModelIndex right = filterTable_->model()->index(row, FilterColumnCount - 1);

            selectionModel->select(
                QItemSelection(left, right),
                QItemSelectionModel::Select | QItemSelectionModel::Rows
            );
        }

        filterTable_->scrollTo(currentIndex, QAbstractItemView::EnsureVisible);
    }

    updateFilterButtonStates();
    updateApplyButtonEnabled();
}

void PreferencesDialog::clearFilterValidationState() const
{
    if (!filterTable_) {
        return;
    }

    const QSignalBlocker blocker(filterTable_);

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        for (int column = 0; column < FilterColumnCount; ++column) {
            auto* item = filterTable_->item(row, column);

            if (!item) {
                continue;
            }

            item->setData(Qt::BackgroundRole, QVariant());
            item->setData(Qt::ForegroundRole, QVariant());
            item->setIcon(QIcon());

            if (column == FilterMacroColumn) {
                const QString macro = normalizedFilterMacro(item->text());
                item->setToolTip(
                    macro.isEmpty()
                        ? QStringLiteral("Optional macro. Example: audio lets you type audio: in the search box.")
                        : QStringLiteral("%1:").arg(macro)
                );
            } else if (column == FilterQueryColumn) {
                item->setToolTip(item->text().trimmed());
            }
        }
    }
}

void PreferencesDialog::markFilterCellInvalid(int row, int column, const QString& message) const
{
    if (!filterTable_ ||
        row < 0 ||
        row >= filterTable_->rowCount() ||
        column < 0 ||
        column >= FilterColumnCount) {
        return;
    }

    const QSignalBlocker blocker(filterTable_);

    auto* item = filterTable_->item(row, column);

    if (!item) {
        item = new QTableWidgetItem();
        filterTable_->setItem(row, column, item);
    }

    const QIcon warningIcon = QIcon::fromTheme(
        QStringLiteral("dialog-warning"),
        QIcon::fromTheme(QStringLiteral("emblem-warning"))
    );

    const QPalette palette = filterTable_->palette();
    const QColor baseColor = palette.color(QPalette::Base);
    const bool darkMode = baseColor.lightness() < 128;

    QColor errorBackground = darkMode
        ? QColor(170, 35, 35)
        : QColor(255, 210, 210);

    QColor errorForeground = darkMode
        ? QColor(255, 235, 235)
        : QColor(110, 0, 0);

    errorBackground.setAlpha(darkMode ? 115 : 190);

    item->setIcon(warningIcon);
    item->setBackground(errorBackground);
    item->setForeground(errorForeground);
    item->setToolTip(message);
}

void PreferencesDialog::focusFilterCell(int row, int column) const
{
    if (!filterTable_ ||
        row < 0 ||
        row >= filterTable_->rowCount() ||
        column < 0 ||
        column >= FilterColumnCount) {
        return;
    }

    filterTable_->setCurrentCell(row, column);
    filterTable_->scrollToItem(
        filterTable_->item(row, column),
        QAbstractItemView::PositionAtCenter
    );
    filterTable_->setFocus();
}

bool PreferencesDialog::hasFilterValidationState() const
{
    if (!filterTable_) {
        return false;
    }

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        for (int column = 0; column < FilterColumnCount; ++column) {
            const auto* item = filterTable_->item(row, column);

            if (item && !item->icon().isNull()) {
                return true;
            }
        }
    }

    return false;
}

bool PreferencesDialog::validateFilters(QString* errorText, bool focusFirstInvalid) const
{
    if (!filterTable_) {
        return true;
    }

    clearFilterValidationState();

    QHash<QString, QList<int>> rowsByName;
    QHash<QString, QList<int>> rowsByMacro;
    QHash<QString, int> rowByMacro;
    QHash<QString, QString> queryByMacro;
    QSet<QString> macroKeywords;

    bool valid = true;
    int firstInvalidRow = -1;
    int firstInvalidColumn = -1;
    QString firstError;

    auto rememberFirstInvalid = [&](int row, int column, const QString& message) {
        if (firstInvalidRow >= 0) {
            return;
        }

        firstInvalidRow = row;
        firstInvalidColumn = column;
        firstError = message;
    };

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        const auto* macroItem = filterTable_->item(row, FilterMacroColumn);
        const QString macro = normalizedFilterMacro(macroItem ? macroItem->text() : QString());

        if (!macro.isEmpty() && isValidFilterMacro(macro)) {
            macroKeywords.insert(macro.toCaseFolded());
        }
    }

    auto normalizedQueryWithKnownSemicolonFixes = [&](QString query) {
        query = normalizedFilterQuery(query);

        if (query.isEmpty()) {
            return query;
        }

        QStringList normalizedTokens;
        const QStringList tokens = query.split(
            QRegularExpression(QStringLiteral("\\s+")),
            Qt::SkipEmptyParts
        );

        normalizedTokens.reserve(tokens.size());

        for (const QString& token : tokens) {
            QString trimmedToken = token.trimmed();

            if (trimmedToken.size() > 1 && trimmedToken.endsWith(QLatin1Char(';'))) {
                const QString keyword = trimmedToken.left(trimmedToken.size() - 1).trimmed();

                if (isKnownFilterKeyword(keyword, macroKeywords)) {
                    trimmedToken = keyword + QLatin1Char(':');
                }
            }

            normalizedTokens << trimmedToken;
        }

        return normalizedTokens.join(QLatin1Char(' '));
    };

    for (int row = 0; row < filterTable_->rowCount(); ++row) {
        const auto* nameItem = filterTable_->item(row, FilterNameColumn);
        auto* macroItem = filterTable_->item(row, FilterMacroColumn);
        auto* queryItem = filterTable_->item(row, FilterQueryColumn);

        const QString name = nameItem ? nameItem->text().trimmed() : QString();
        const QString rawQuery = queryItem ? queryItem->text() : QString();
        const QString query = normalizedQueryWithKnownSemicolonFixes(rawQuery);
        const QString rawMacro = macroItem ? macroItem->text() : QString();
        const QString macro = normalizedFilterMacro(rawMacro);
        const QString foldedMacro = macro.toCaseFolded();

        if (macroItem && macroItem->text() != macro) {
            const QSignalBlocker blocker(filterTable_);

            macroItem->setText(macro);
            macroItem->setToolTip(
                macro.isEmpty()
                    ? QStringLiteral("Optional macro. Example: audio lets you type audio: in the search box.")
                    : QStringLiteral("%1:").arg(macro)
            );
        }

        if (queryItem && queryItem->text() != query) {
            const QSignalBlocker blocker(filterTable_);

            queryItem->setText(query);
            queryItem->setToolTip(query);
        }

        if (name.isEmpty()) {
            const QString message = QStringLiteral("Filter names cannot be empty.");

            markFilterCellInvalid(row, FilterNameColumn, message);
            rememberFirstInvalid(row, FilterNameColumn, message);
            valid = false;
        } else {
            rowsByName[name.toCaseFolded()].append(row);
        }

        if (query.isEmpty()) {
            const QString message = QStringLiteral("Filter queries cannot be empty.");

            markFilterCellInvalid(row, FilterQueryColumn, message);
            rememberFirstInvalid(row, FilterQueryColumn, message);
            valid = false;
        }

        if (!isValidFilterMacro(macro)) {
            const QString message = QStringLiteral(
                "Filter macros can only contain letters, numbers, and underscores."
            );

            markFilterCellInvalid(row, FilterMacroColumn, message);
            rememberFirstInvalid(row, FilterMacroColumn, message);
            valid = false;
        } else if (!macro.isEmpty() && isBuiltInFilterKeyword(macro)) {
            const QString message = QStringLiteral(
                "Filter macros cannot use built-in filter keywords such as ext, extension, file, files, folder, folders, or type."
            );

            markFilterCellInvalid(row, FilterMacroColumn, message);
            rememberFirstInvalid(row, FilterMacroColumn, message);
            valid = false;
        } else if (!macro.isEmpty()) {
            rowsByMacro[foldedMacro].append(row);
            rowByMacro.insert(foldedMacro, row);
            queryByMacro.insert(foldedMacro, query);
        }

        const QStringList queryTokens = query.split(
            QRegularExpression(QStringLiteral("\\s+")),
            Qt::SkipEmptyParts
        );

        for (const QString& token : queryTokens) {
            const QString trimmedToken = token.trimmed();
            const QString foldedToken = trimmedToken.toCaseFolded();

            if (foldedToken.startsWith(QStringLiteral("ext:")) ||
                foldedToken.startsWith(QStringLiteral("extension:"))) {
                const qsizetype colon = trimmedToken.indexOf(QLatin1Char(':'));
                const QString extensionList = colon >= 0
                    ? trimmedToken.mid(colon + 1)
                    : QString();

                bool hasExtension = false;
                const QStringList extensionTokens = extensionList.split(
                    QRegularExpression(QStringLiteral("[;,]")),
                    Qt::SkipEmptyParts
                );

                for (const QString& extensionToken : extensionTokens) {
                    QString normalizedExtension = extensionToken.trimmed();

                    while (normalizedExtension.startsWith(QLatin1Char('.'))) {
                        normalizedExtension.remove(0, 1);
                    }

                    if (!normalizedExtension.isEmpty()) {
                        hasExtension = true;
                        break;
                    }
                }

                if (!hasExtension) {
                    const QString message = QStringLiteral(
                        "Extension filters must include at least one extension. Example: ext:7z or ext:7z;zip."
                    );

                    markFilterCellInvalid(row, FilterQueryColumn, message);
                    rememberFirstInvalid(row, FilterQueryColumn, message);
                    valid = false;
                    break;
                }
            }

            if (foldedToken.startsWith(QStringLiteral("type:"))) {
                if (foldedToken != QStringLiteral("type:file") &&
                    foldedToken != QStringLiteral("type:files") &&
                    foldedToken != QStringLiteral("type:folder") &&
                    foldedToken != QStringLiteral("type:folders")) {
                    const QString message = QStringLiteral(
                        "Unknown type filter \"%1\". Supported values are type:file, type:files, type:folder, and type:folders."
                    ).arg(trimmedToken);

                    markFilterCellInvalid(row, FilterQueryColumn, message);
                    rememberFirstInvalid(row, FilterQueryColumn, message);
                    valid = false;
                    break;
                }
            }

            if (trimmedToken.size() <= 1 || !trimmedToken.endsWith(QLatin1Char(':'))) {
                continue;
            }

            const QString keyword = trimmedToken.left(trimmedToken.size() - 1).trimmed();

            if (isKnownFilterKeyword(keyword, macroKeywords)) {
                continue;
            }

            const QString message = QStringLiteral(
                "Unknown filter keyword \"%1:\". Check for a typo, or add a filter macro named \"%1\"."
            ).arg(keyword);

            markFilterCellInvalid(row, FilterQueryColumn, message);
            rememberFirstInvalid(row, FilterQueryColumn, message);
            valid = false;
            break;
        }
    }

    const QString duplicateNameMessage = QStringLiteral("Filter names must be unique.");

    for (auto it = rowsByName.constBegin(); it != rowsByName.constEnd(); ++it) {
        const QList<int>& rows = it.value();

        if (rows.size() < 2) {
            continue;
        }

        for (const int row : rows) {
            markFilterCellInvalid(row, FilterNameColumn, duplicateNameMessage);
        }

        rememberFirstInvalid(rows.first(), FilterNameColumn, duplicateNameMessage);
        valid = false;
    }

    const QString duplicateMacroMessage = QStringLiteral("Filter macros must be unique.");

    for (auto it = rowsByMacro.constBegin(); it != rowsByMacro.constEnd(); ++it) {
        const QList<int>& rows = it.value();

        if (rows.size() < 2) {
            continue;
        }

        for (const int row : rows) {
            markFilterCellInvalid(row, FilterMacroColumn, duplicateMacroMessage);
        }

        rememberFirstInvalid(rows.first(), FilterMacroColumn, duplicateMacroMessage);
        valid = false;
    }

    QHash<QString, QStringList> macroReferencesByMacro;

    for (auto it = queryByMacro.constBegin(); it != queryByMacro.constEnd(); ++it) {
        macroReferencesByMacro.insert(
            it.key(),
            filterMacroReferencesInQuery(it.value(), macroKeywords)
        );
    }

    QSet<QString> visitingMacros;
    QSet<QString> visitedMacros;

    auto containsCycle = [&](const QString& macro, auto&& containsCycleRef) -> bool {
        if (visitingMacros.contains(macro)) {
            return true;
        }

        if (visitedMacros.contains(macro)) {
            return false;
        }

        visitingMacros.insert(macro);

        const QStringList references = macroReferencesByMacro.value(macro);
        for (const QString& referencedMacro : references) {
            if (containsCycleRef(referencedMacro, containsCycleRef)) {
                return true;
            }
        }

        visitingMacros.remove(macro);
        visitedMacros.insert(macro);
        return false;
    };

    for (auto it = rowByMacro.constBegin(); it != rowByMacro.constEnd(); ++it) {
        visitingMacros.clear();

        if (!containsCycle(it.key(), containsCycle)) {
            continue;
        }

        const QString message = QStringLiteral(
            "Filter macro \"%1:\" creates a circular filter reference."
        ).arg(it.key());

        markFilterCellInvalid(it.value(), FilterQueryColumn, message);
        rememberFirstInvalid(it.value(), FilterQueryColumn, message);
        valid = false;
    }

    if (valid) {
        return true;
    }

    if (focusFirstInvalid) {
        if (pages_) {
            pages_->setCurrentIndex(1);
        }

        if (navigation_ && navigation_->count() > 1) {
            navigation_->setCurrentRow(1);
        }

        focusFilterCell(firstInvalidRow, firstInvalidColumn);
    }

    if (errorText) {
        *errorText = firstError;
    }

    return false;
}

bool PreferencesDialog::hasChanges() const
{
    return hasDeviceChanges() ||
           hasFilterChanges() ||
           hasWindowChanges() ||
           hasUIChanges() ||
           hasGeneralChanges();
}

bool PreferencesDialog::hasGeneralChanges() const
{
    bool changed = false;

    if (autoRefreshLiveUpdatesCheckBox_) {
        changed = changed ||
            autoRefreshLiveUpdatesCheckBox_->isChecked() !=
            preferences_.autoRefreshResultsForLiveUpdates();
    }

    return changed;
}

bool PreferencesDialog::hasDeviceChanges() const
{
    if (!deviceTable_) {
        return false;
    }

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        const auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        const bool enabled = item->checkState() == Qt::Checked;
        const bool originalEnabled = item->data(InitialEnabledRole).toBool();

        if (enabled != originalEnabled) {
            return true;
        }

        const IndexedDevicePreference original =
            originalPreferencesByDeviceId_.value(deviceId, IndexedDevicePreference{ .deviceId = deviceId });

        const bool unmountedScanningSupported = unmountedScanningSupportedForDevice(deviceId);
        const bool scanWhenUnmounted =
            unmountedScanningSupported && item->data(ScanWhenUnmountedRole).toBool();

        if (scanWhenUnmounted != original.scanWhenUnmounted) {
            return true;
        }

        if (item->data(ShowOfflineResultsRole).toBool() != original.showOfflineResults) {
            return true;
        }

        const bool liveUpdatesSupported = liveUpdatesSupportedForDevice(deviceId);
        const bool liveUpdatesEnabled =
            liveUpdatesSupported && item->data(LiveUpdatesEnabledRole).toBool();

        if (liveUpdatesEnabled != original.liveUpdatesEnabled) {
            return true;
        }
    }

    return false;
}

bool PreferencesDialog::hasFilterChanges() const
{
    const std::vector<SearchFilterPreference> current = filtersFromTable();

    if (current.size() != originalSearchFilters_.size()) {
        return true;
    }

    for (std::size_t i = 0; i < current.size(); ++i) {
        if (current[i].id != originalSearchFilters_[i].id ||
            current[i].name != originalSearchFilters_[i].name ||
            current[i].macro != originalSearchFilters_[i].macro ||
            current[i].query != originalSearchFilters_[i].query) {
            return true;
        }
    }

    return false;
}

bool PreferencesDialog::hasWindowChanges() const
{
    bool changed = false;

    if (createNewWindowOnLaunchCheckBox_) {
        changed = changed ||
            createNewWindowOnLaunchCheckBox_->isChecked() !=
            preferences_.createNewWindowOnLaunch();
    }

    if (carryFilterToNewWindowsCheckBox_) {
        changed = changed ||
            carryFilterToNewWindowsCheckBox_->isChecked() !=
            preferences_.carryFilterToNewWindows();
    }

    if (carrySearchOptionsToNewWindowsCheckBox_) {
        changed = changed ||
            carrySearchOptionsToNewWindowsCheckBox_->isChecked() !=
            preferences_.carrySearchOptionsToNewWindows();
    }

    if (carrySearchTextToNewWindowsCheckBox_) {
        changed = changed ||
            carrySearchTextToNewWindowsCheckBox_->isChecked() !=
            preferences_.carrySearchTextToNewWindows();
    }

    if (carryResultSortingToNewWindowsCheckBox_) {
        changed = changed ||
            carryResultSortingToNewWindowsCheckBox_->isChecked() !=
            preferences_.carryResultSortingToNewWindows();
    }

    if (carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_) {
        changed = changed ||
            carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_->isChecked() !=
            preferences_.carryWindowSizeAndColumnWidthsToNewWindows();
    }

    return changed;
}

bool PreferencesDialog::hasUIChanges() const
{
    bool changed = false;

    if (defaultSortColumnComboBox_) {
        changed = changed ||
            defaultSortColumnComboBox_->currentData().toInt() !=
            preferences_.defaultSortColumn();
    }

    if (defaultSortOrderComboBox_) {
        changed = changed ||
            defaultSortOrderComboBox_->currentData().toInt() !=
            static_cast<int>(preferences_.defaultSortOrder());
    }

    if (sortDateDescendingFirstCheckBox_) {
        changed = changed ||
            sortDateDescendingFirstCheckBox_->isChecked() !=
            preferences_.sortDateDescendingFirst();
    }

    if (sortSizeDescendingFirstCheckBox_) {
        changed = changed ||
            sortSizeDescendingFirstCheckBox_->isChecked() !=
            preferences_.sortSizeDescendingFirst();
    }

    if (showInFileManagerOnPathDoubleClickCheckBox_) {
        changed = changed ||
            showInFileManagerOnPathDoubleClickCheckBox_->isChecked() !=
            preferences_.showInFileManagerOnPathDoubleClick();
    }

    if (showHighlightedSearchTermsCheckBox_) {
        changed = changed ||
            showHighlightedSearchTermsCheckBox_->isChecked() !=
            preferences_.showHighlightedSearchTerms();
    }

    if (showFiltersDropdownCheckBox_) {
        changed = changed ||
            showFiltersDropdownCheckBox_->isChecked() !=
            preferences_.showFiltersDropdown();
    }

    if (showPreviewPaneCheckBox_) {
        changed = changed ||
            showPreviewPaneCheckBox_->isChecked() !=
            preferences_.showPreviewPane();
    }

    return changed;
}

void PreferencesDialog::updateApplyButtonEnabled()
{
    if (applyButton_) {
        applyButton_->setEnabled(hasChanges());
    }
}

bool PreferencesDialog::applyChanges()
{
    QString filterError;
    if (!validateFilters(&filterError)) {
        QMessageBox::warning(
            this,
            QStringLiteral("Invalid Filters"),
            filterError
        );
        return false;
    }

    updateApplyButtonEnabled();

    const bool autoRefreshChanged =
        autoRefreshLiveUpdatesCheckBox_ &&
        autoRefreshLiveUpdatesCheckBox_->isChecked() !=
        preferences_.autoRefreshResultsForLiveUpdates();

    const bool autoRefreshEnabled =
        autoRefreshLiveUpdatesCheckBox_
            ? autoRefreshLiveUpdatesCheckBox_->isChecked()
            : preferences_.autoRefreshResultsForLiveUpdates();

    const bool filtersChanged = hasFilterChanges();

    if (filtersChanged) {
        preferences_.saveSearchFilters(filtersFromTable());
        originalSearchFilters_ = preferences_.searchFilters();
        populateFilterTable();
    }

    if (showPreviewPaneCheckBox_) {
        preferences_.setShowPreviewPane(showPreviewPaneCheckBox_->isChecked());
    }

    const bool uiChanged = hasUIChanges();
    const bool windowChanged = hasWindowChanges();

    const bool searchResultHighlightingChanged =
        showHighlightedSearchTermsCheckBox_ &&
        showHighlightedSearchTermsCheckBox_->isChecked() !=
        preferences_.showHighlightedSearchTerms();

    const bool searchResultHighlightingEnabled =
        showHighlightedSearchTermsCheckBox_
            ? showHighlightedSearchTermsCheckBox_->isChecked()
            : preferences_.showHighlightedSearchTerms();

    const bool showFiltersDropdownChanged =
        showFiltersDropdownCheckBox_ &&
        showFiltersDropdownCheckBox_->isChecked() !=
        preferences_.showFiltersDropdown();

    const bool showFiltersDropdownEnabled =
        showFiltersDropdownCheckBox_
            ? showFiltersDropdownCheckBox_->isChecked()
            : preferences_.showFiltersDropdown();

    const bool showPreviewPaneChanged =
        showPreviewPaneCheckBox_ &&
        showPreviewPaneCheckBox_->isChecked() !=
        preferences_.showPreviewPane();

    const bool showPreviewPaneEnabled =
        showPreviewPaneCheckBox_
            ? showPreviewPaneCheckBox_->isChecked()
            : preferences_.showPreviewPane();

    /*
     * NOTE: The following preferences are only written to in AppController, NOT from here:
     * - UI - showFiltersDropdown
     * - Advanced - autoRefreshResultsForLiveUpdates
     *
     * This is because these preferences can be changed from the MainWindow OR from the PreferencesDialog,
     * so the AppController is used as a central location to save them. Therefore, we don't want to
     * overwrite them here.
     */

    if (windowChanged) {
        if (createNewWindowOnLaunchCheckBox_) {
            preferences_.setCreateNewWindowOnLaunch(
                createNewWindowOnLaunchCheckBox_->isChecked()
            );
        }

        if (carryFilterToNewWindowsCheckBox_) {
            preferences_.setCarryFilterToNewWindows(
                carryFilterToNewWindowsCheckBox_->isChecked()
            );
        }

        if (carrySearchOptionsToNewWindowsCheckBox_) {
            preferences_.setCarrySearchOptionsToNewWindows(
                carrySearchOptionsToNewWindowsCheckBox_->isChecked()
            );
        }

        if (carrySearchTextToNewWindowsCheckBox_) {
            preferences_.setCarrySearchTextToNewWindows(
                carrySearchTextToNewWindowsCheckBox_->isChecked()
            );
        }

        if (carryResultSortingToNewWindowsCheckBox_) {
            preferences_.setCarryResultSortingToNewWindows(
                carryResultSortingToNewWindowsCheckBox_->isChecked()
            );
        }

        if (carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_) {
            preferences_.setCarryWindowSizeToNewWindows(
                carryWindowSizeAndColumnWidthsToNewWindowsCheckBox_->isChecked()
            );
        }
    }

    if (uiChanged) {
        if (defaultSortColumnComboBox_) {
            preferences_.setDefaultSortColumn(
                defaultSortColumnComboBox_->currentData().toInt()
            );
        }

        if (defaultSortOrderComboBox_) {
            const Qt::SortOrder order =
                defaultSortOrderComboBox_->currentData().toInt() ==
                static_cast<int>(Qt::DescendingOrder)
                    ? Qt::DescendingOrder
                    : Qt::AscendingOrder;

            preferences_.setDefaultSortOrder(order);
        }

        if (sortDateDescendingFirstCheckBox_) {
            preferences_.setSortDateDescendingFirst(
                sortDateDescendingFirstCheckBox_->isChecked()
            );
        }

        if (sortSizeDescendingFirstCheckBox_) {
            preferences_.setSortSizeDescendingFirst(
                sortSizeDescendingFirstCheckBox_->isChecked()
            );
        }

        if (showInFileManagerOnPathDoubleClickCheckBox_) {
            preferences_.setShowInFileManagerOnPathDoubleClick(
                showInFileManagerOnPathDoubleClickCheckBox_->isChecked()
            );
        }

        if (showHighlightedSearchTermsCheckBox_) {
            preferences_.setShowHighlightedSearchTerms(
                showHighlightedSearchTermsCheckBox_->isChecked()
            );
        }
    }

    if (!deviceTable_) {
        if (filtersChanged) {
            Q_EMIT searchFiltersApplied();
        }

        if (autoRefreshChanged) {
            Q_EMIT autoRefreshResultsForLiveUpdatesApplied(autoRefreshEnabled);
        }

        if (searchResultHighlightingChanged) {
            Q_EMIT searchResultHighlightingApplied(searchResultHighlightingEnabled);
        }

        if (showFiltersDropdownChanged) {
            Q_EMIT showFiltersDropdownApplied(showFiltersDropdownEnabled);
        }

        if (showPreviewPaneChanged) {
            Q_EMIT showPreviewPaneApplied(showPreviewPaneEnabled);
        }

        updateApplyButtonEnabled();
        return true;
    }

    QList<DevicePreferenceChange> changes;

    for (int row = 0; row < deviceTable_->rowCount(); ++row) {
        auto* item = deviceTable_->item(row, DeviceEnabledColumn);
        if (!item) {
            continue;
        }

        const QString deviceId = item->data(DeviceIdRole).toString();
        if (deviceId.isEmpty()) {
            continue;
        }

        const auto blockDeviceIt = knownDeviceById_.constFind(deviceId);
        if (blockDeviceIt == knownDeviceById_.constEnd()) {
            continue;
        }

        const bool enabled = item->checkState() == Qt::Checked;

        const IndexedDevicePreference original =
            originalPreferencesByDeviceId_.value(deviceId, IndexedDevicePreference{ .deviceId = deviceId });

        const bool originallyEnabled = original.enabled;
        const bool unmountedScanningSupported = unmountedScanningSupportedForDevice(deviceId);
        const bool scanWhenUnmounted =
            unmountedScanningSupported && item->data(ScanWhenUnmountedRole).toBool();
        const bool showOfflineResults = item->data(ShowOfflineResultsRole).toBool();
        const bool liveUpdatesSupported = liveUpdatesSupportedForDevice(deviceId);
        const bool liveUpdatesEnabled =
            liveUpdatesSupported && item->data(LiveUpdatesEnabledRole).toBool();

        const BlockDevice& blockDevice = blockDeviceIt.value();

        IndexedDevicePreference preference =
            preferences_.indexedDevicePreference(deviceId).value_or(IndexedDevicePreference{ .deviceId = deviceId });

        preference.deviceId = blockDevice.deviceId;
        preference.enabled = enabled;
        preference.displayName = BlockDeviceDisplayUtils::displayNameForBlockDevice(blockDevice);
        preference.fsType = blockDevice.fsType;
        preference.uuid = blockDevice.uuid;
        preference.partuuid = blockDevice.partuuid;
        preference.lastKnownDevNode = blockDevice.devNode;
        preference.lastKnownPrimaryMountPoint = blockDevice.primaryMountPoint;
        preference.lastKnownMountPoints = blockDevice.mountPoints;
        preference.lastSeenAt = QDateTime::currentDateTimeUtc();
        preference.scanWhenUnmounted = scanWhenUnmounted;
        preference.showOfflineResults = showOfflineResults;
        preference.liveUpdatesEnabled = liveUpdatesEnabled;

        preferences_.saveIndexedDevicePreference(preference);

        if (enabled != original.enabled ||
            scanWhenUnmounted != original.scanWhenUnmounted ||
            showOfflineResults != original.showOfflineResults ||
            liveUpdatesEnabled != original.liveUpdatesEnabled) {
            changes.append(DevicePreferenceChange{
                .deviceId = deviceId,
                .wasEnabled = originallyEnabled,
                .enabled = enabled,
                .wasScanWhenUnmounted = original.scanWhenUnmounted,
                .scanWhenUnmounted = scanWhenUnmounted,
                .wasShowOfflineResults = original.showOfflineResults,
                .showOfflineResults = showOfflineResults,
                .wasLiveUpdatesEnabled = original.liveUpdatesEnabled,
                .liveUpdatesEnabled = liveUpdatesEnabled,
            });
            }

        item->setData(InitialEnabledRole, enabled);
        item->setData(ScanWhenUnmountedRole, scanWhenUnmounted);
        item->setData(LiveUpdatesEnabledRole, liveUpdatesEnabled);
    }

    originalPreferencesByDeviceId_.clear();
    for (const IndexedDevicePreference& preference : preferences_.indexedDevicePreferences()) {
        if (!preference.deviceId.isEmpty()) {
            originalPreferencesByDeviceId_.insert(preference.deviceId, preference);
        }
    }

    updateApplyButtonEnabled();

    if (filtersChanged) {
        Q_EMIT searchFiltersApplied();
    }

    if (!changes.isEmpty()) {
        Q_EMIT preferencesApplied(changes);
    }

    if (autoRefreshChanged) {
        Q_EMIT autoRefreshResultsForLiveUpdatesApplied(autoRefreshEnabled);
    }

    if (searchResultHighlightingChanged) {
        Q_EMIT searchResultHighlightingApplied(searchResultHighlightingEnabled);
    }

    if (showFiltersDropdownChanged) {
        Q_EMIT showFiltersDropdownApplied(showFiltersDropdownEnabled);
    }

    return true;
}