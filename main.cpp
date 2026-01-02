// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QApplication>
#include <KAboutData>
#include "ScannerEngine.h"
#include "PartitionDialog.h"
#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    KAboutData aboutData(
        QStringLiteral("kerything"),
        QStringLiteral("Kerything"),
        QStringLiteral("1.0"),
        QStringLiteral("A fast NTFS file scanner"),
        KAboutLicense::GPL_V3,
        QStringLiteral("(c) 2026  Reikooters <https://github.com/Reikooters>")
    );

    // This tells KDE to look for the icon named 'kerything' in the system theme
    aboutData.setProgramLogo(QIcon::fromTheme(QStringLiteral("kerything")));
    KAboutData::setApplicationData(aboutData);

    // This must match the .desktop filename exactly
    QApplication::setDesktopFileName(QStringLiteral("net.reikooters.kerything"));

    // Set window icon
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("kerything")));

    PartitionDialog dlg;

    // This will block here. The partition selection and scan happens inside the dlg
    if (dlg.exec() != QDialog::Accepted) {
        return 0;
    }

    std::optional<ScannerEngine::SearchDatabase> db = dlg.takeDatabase();
    if (!db) {
        return 0;
    }

    PartitionInfo selected = dlg.getSelected();

    MainWindow window(std::move(*db), selected.mountPoint);
    window.setWindowTitle(selected.name);
    window.show();

    return app.exec();
}
