// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_FILERECORD_H
#define KERYTHING_FILERECORD_H

struct FileRecord {
    QString path;
    quint64 size = 0;
    quint64 mtime = 0;
};

#endif // KERYTHING_FILERECORD_H