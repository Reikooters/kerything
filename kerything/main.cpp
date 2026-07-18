// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QApplication>
#include <QIcon>

#include "AppController.h"
#include "Version.h"

int main(int argc, char* argv[]) {
    // Qt application object: required for widgets, event processing, and app-wide state.
    QApplication app(argc, argv);

    // Basic metadata used by Qt features such as settings and platform integration.
    QApplication::setApplicationName(QStringLiteral("kerything"));
    QApplication::setOrganizationName(QStringLiteral("Reikooters"));
    QApplication::setApplicationVersion(KerythingVersion::Version);

    // This must match the .desktop filename exactly
    QApplication::setDesktopFileName(QStringLiteral("net.reikooters.kerything"));

    // Set window icon
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("kerything")));

    // Central coordinator for startup, windows, and single-instance handling.
    AppController controller(app);

    // If this process is not the primary instance and successfully notified the
    // already-running instance, it should exit immediately.
    if (!controller.start()) {
        return 0;
    }

    // Start Qt's event loop. This keeps the application alive until it quits.
    return app.exec();
}