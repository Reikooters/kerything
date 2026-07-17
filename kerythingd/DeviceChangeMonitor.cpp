// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "DeviceChangeMonitor.h"

#include <iostream>

DeviceChangeMonitor::DeviceChangeMonitor(QObject* parent)
    : QObject(parent)
{
    const bool udevOk = initUdev();
    const bool mountOk = initMountMonitor();

    valid_ = udevOk || mountOk;

    if (!udevOk) {
        std::cerr << "DeviceChangeMonitor: udev monitoring unavailable\n";
    }

    if (!mountOk) {
        std::cerr << "DeviceChangeMonitor: mount monitoring unavailable\n";
    }
}

DeviceChangeMonitor::~DeviceChangeMonitor()
{
    delete udevNotifier_;
    udevNotifier_ = nullptr;

    delete mountNotifier_;
    mountNotifier_ = nullptr;

    if (udevMonitor_) {
        udev_monitor_unref(udevMonitor_);
        udevMonitor_ = nullptr;
    }

    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }

    if (mountMonitor_) {
        mnt_unref_monitor(mountMonitor_);
        mountMonitor_ = nullptr;
    }
}

bool DeviceChangeMonitor::isValid() const noexcept
{
    return valid_;
}

bool DeviceChangeMonitor::initUdev()
{
    udev_ = udev_new();
    if (!udev_) {
        return false;
    }

    udevMonitor_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (!udevMonitor_) {
        return false;
    }

    udev_monitor_filter_add_match_subsystem_devtype(udevMonitor_, "block", nullptr);

    if (udev_monitor_enable_receiving(udevMonitor_) < 0) {
        return false;
    }

    const int fd = udev_monitor_get_fd(udevMonitor_);
    if (fd < 0) {
        return false;
    }

    udevNotifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(udevNotifier_, &QSocketNotifier::activated,
            this, &DeviceChangeMonitor::onUdevActivated);

    return true;
}

bool DeviceChangeMonitor::initMountMonitor()
{
    mountMonitor_ = mnt_new_monitor();
    if (!mountMonitor_) {
        return false;
    }

    /*
     * Monitor kernel mount-table changes. This should catch ordinary mount and
     * unmount operations.
     */
    if (mnt_monitor_enable_kernel(mountMonitor_, true) != 0) {
        return false;
    }

    const int fd = mnt_monitor_get_fd(mountMonitor_);
    if (fd < 0) {
        return false;
    }

    mountNotifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(mountNotifier_, &QSocketNotifier::activated,
            this, &DeviceChangeMonitor::onMountActivated);

    return true;
}

void DeviceChangeMonitor::onUdevActivated()
{
    while (udev_device* device = udev_monitor_receive_device(udevMonitor_)) {
        const char* action = udev_device_get_action(device);
        const char* devnode = udev_device_get_devnode(device);

#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "udev block event action="
                  << (action ? action : "")
                  << " devnode="
                  << (devnode ? devnode : "")
                  << "\n";
#endif

        udev_device_unref(device);
        Q_EMIT devicesMayHaveChanged();
    }
}

void DeviceChangeMonitor::onMountActivated()
{
    /*
     * Drain pending mount-monitor events. We don't need event details because
     * the daemon already rebuilds the authoritative snapshot afterwards.
     */
    while (mnt_monitor_next_change(mountMonitor_, nullptr, nullptr) == 0) {
#ifdef KERYTHING_ENABLE_LOGGING
        std::cout << "mount table changed\n";
#endif
        Q_EMIT devicesMayHaveChanged();
    }
}
