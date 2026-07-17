// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_DEVICEPICKERDIALOG_H
#define KERYTHING_DEVICEPICKERDIALOG_H

#include <QDialog>
#include <QString>
#include <vector>

#include "BlockDevice.h"
#include "Preferences.h"

class QPushButton;
class QTableWidget;

class DevicePickerDialog final : public QDialog {
    Q_OBJECT

public:
    explicit DevicePickerDialog(
        const std::vector<BlockDevice>& blockDevices,
        const Preferences& preferences,
        QWidget* parent = nullptr
    );

    [[nodiscard]] QStringList selectedDeviceIds() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum Column {
        EnabledColumn = 0,
        NameColumn,
        FsTypeColumn,
        MountPointColumn,
        DevNodeColumn,
        DiskModelColumn,
        ColumnCount
    };

    static QString displayNameForBlockDevice(const BlockDevice& blockDevice);
    static QString displayOrDash(const QString& value);
    static bool shouldSelectByDefault(const BlockDevice& blockDevice);
    static bool deviceLessThan(const BlockDevice& lhs, const BlockDevice& rhs);

    [[nodiscard]] Qt::CheckState initialCheckStateForBlockDevice(const BlockDevice& blockDevice) const;

    void populateTable(const std::vector<BlockDevice>& blockDevices);
    void setAllChecked(Qt::CheckState checkState);
    void restoreDefaultSelection();
    void toggleRowChecked(int row);
    void updateStartButtonEnabled();

    [[nodiscard]] bool hasSelectedDevices() const;

    QTableWidget* table_ = nullptr;
    QPushButton* startButton_ = nullptr;

    const Preferences& preferences_;
};

#endif // KERYTHING_DEVICEPICKERDIALOG_H