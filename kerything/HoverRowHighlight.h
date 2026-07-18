// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_HOVERROWHIGHLIGHT_H
#define KERYTHING_HOVERROWHIGHLIGHT_H

class QAbstractItemView;

/**
 * Installs full-row hover highlighting on an item view.
 *
 * Works with QTableView and QTableWidget because both inherit QAbstractItemView.
 */
void installHoverRowHighlight(QAbstractItemView* view);

#endif // KERYTHING_HOVERROWHIGHLIGHT_H