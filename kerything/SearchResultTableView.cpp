// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SearchResultTableView.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QKeyEvent>
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

void SearchResultTableView::keyPressEvent(QKeyEvent* event)
{
    if (!model() || model()->rowCount() <= 0) {
        QTableView::keyPressEvent(event);
        return;
    }

    if (event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)) {
        const int row = event->key() == Qt::Key_Home
            ? 0
            : model()->rowCount() - 1;

        const int column = currentIndex().isValid()
            ? currentIndex().column()
            : 0;

        const QModelIndex index = model()->index(row, column);
        setCurrentIndex(index);

        if (selectionModel()) {
            selectionModel()->select(
                index,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows
            );
        }

        scrollTo(index, QAbstractItemView::PositionAtCenter);
        event->accept();
        return;
        }

    QTableView::keyPressEvent(event);
}
