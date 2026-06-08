// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SearchResultTableView.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QMouseEvent>

SearchResultTableView::SearchResultTableView(QWidget* parent)
    : QTableView(parent)
{
}

void SearchResultTableView::mousePressEvent(QMouseEvent* event)
{
    dragPressIndex_ = indexAt(event->pos());
    dragPressCanDrag_ =
        event->button() == Qt::LeftButton &&
        dragPressIndex_.isValid() &&
        model() &&
        (model()->flags(dragPressIndex_) & Qt::ItemIsDragEnabled);

    QTableView::mousePressEvent(event);
}

void SearchResultTableView::mouseMoveEvent(QMouseEvent* event)
{
    if ((event->buttons() & Qt::LeftButton) && !dragPressCanDrag_) {
        event->accept();
        return;
    }

    QTableView::mouseMoveEvent(event);
}

void SearchResultTableView::mouseReleaseEvent(QMouseEvent* event)
{
    dragPressIndex_ = QModelIndex();
    dragPressCanDrag_ = false;
    unsetCursor();

    QTableView::mouseReleaseEvent(event);
}

void SearchResultTableView::leaveEvent(QEvent* event)
{
    unsetCursor();
    QTableView::leaveEvent(event);
}