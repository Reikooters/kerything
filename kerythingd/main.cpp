// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QtCore/QCoreApplication>

#include "Server.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    Server server;
    return app.exec();
}
