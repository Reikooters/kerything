// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QApplication>
#include <KAboutData>
#include "ScannerEngine.h"
#include "PartitionDialog.h"
#include "MainWindow.h"
#include "Version.h"

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            std::cout << "kerything v" << Version::VERSION << std::endl;
            return 0;
        }
    }

    QApplication app(argc, argv);

    KAboutData aboutData(
        QStringLiteral("kerything"),
        QStringLiteral("Kerything"),
        QString::fromUtf8(Version::VERSION),
        QStringLiteral("A fast NTFS file searcher."),
        KAboutLicense::GPL_V3,
        QStringLiteral("(c) 2026 Reikooters &lt;https://github.com/Reikooters&gt;")
    );

    aboutData.addAuthor("Reikooters", "Developer", "https://github.com/Reikooters");
    aboutData.setBugAddress("https://github.com/Reikooters/kerything/issues");
    aboutData.setHomepage("https://github.com/Reikooters/kerything");

    // This tells KDE to look for the icon named 'kerything' in the system theme
    aboutData.setProgramLogo(QIcon::fromTheme(QStringLiteral("kerything")));

    KAboutData::setApplicationData(aboutData);

    // This must match the .desktop filename exactly
    QApplication::setDesktopFileName(QStringLiteral("net.reikooters.kerything"));

    // Set window icon
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("kerything")));

    MainWindow window;
    window.show();

    return app.exec();
}
