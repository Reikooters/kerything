// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SEARCHRESULTCOLUMNS_H
#define KERYTHING_SEARCHRESULTCOLUMNS_H

namespace SearchResultColumn {
    enum Column : int {
        Name = 0,
        Path,
        Size,
        DateModified,
        Count
    };
}

#endif // KERYTHING_SEARCHRESULTCOLUMNS_H