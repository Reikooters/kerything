// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_PREVIEWMETADATA_H
#define KERYTHING_PREVIEWMETADATA_H

#include <QtTypes>
#include <QString>

struct PreviewMetadata {
    QString fileName;
    QString displayPath;
    quint64 size = 0;
    quint64 modificationTime = 0;
    bool isDirectory = false;
    bool isSymlink = false;
};

#endif // KERYTHING_PREVIEWMETADATA_H