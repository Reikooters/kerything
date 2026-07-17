// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_DEVICECHANGEMONITOR_H
#define KERYTHINGD_DEVICECHANGEMONITOR_H

#include <QObject>
#include <QSocketNotifier>

#include <libudev.h>
#include <libmount/libmount.h>

class DeviceChangeMonitor final : public QObject {
    Q_OBJECT

public:
    explicit DeviceChangeMonitor(QObject* parent = nullptr);
    ~DeviceChangeMonitor() override;

    [[nodiscard]] bool isValid() const noexcept;

Q_SIGNALS:
    void devicesMayHaveChanged();

private Q_SLOTS:
    void onUdevActivated();
    void onMountActivated();

private:
    bool initUdev();
    bool initMountMonitor();

    udev* udev_ = nullptr;
    udev_monitor* udevMonitor_ = nullptr;
    QSocketNotifier* udevNotifier_ = nullptr;

    libmnt_monitor* mountMonitor_ = nullptr;
    QSocketNotifier* mountNotifier_ = nullptr;

    bool valid_ = false;
};

#endif // KERYTHINGD_DEVICECHANGEMONITOR_H
