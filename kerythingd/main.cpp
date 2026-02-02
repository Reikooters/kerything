// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

#include "IndexerService.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("kerythingd");
    QCoreApplication::setOrganizationDomain("reikooters.net");

    constexpr const char* kServiceName = "net.reikooters.Kerything1";
    constexpr const char* kObjectPath  = "/net/reikooters/Kerything1";

    auto conn = QDBusConnection::systemBus();
    if (!conn.isConnected()) {
        qCritical() << "Failed to connect to system bus:" << conn.lastError().message();
        return 1;
    }

    if (!conn.registerService(kServiceName)) {
        qCritical() << "Failed to register service" << kServiceName << ":" << conn.lastError().message();
        return 2;
    }

    IndexerService svc;

    if (!conn.registerObject(kObjectPath, &svc,
                             QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCritical() << "Failed to register object" << kObjectPath << ":" << conn.lastError().message();
        return 3;
    }

    qInfo() << "kerythingd running on system bus as" << kServiceName << "object" << kObjectPath;
    return app.exec();
}