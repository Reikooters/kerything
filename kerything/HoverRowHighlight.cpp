// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "HoverRowHighlight.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QModelIndex>
#include <QPainter>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace {
    constexpr const char* HoveredRowProperty = "_kerything_hovered_row";

    int hoveredRowForView(const QAbstractItemView* view)
    {
        if (!view) {
            return -1;
        }

        return view->property(HoveredRowProperty).toInt();
    }

    void setHoveredRowForView(QAbstractItemView* view, const int row)
    {
        if (!view || hoveredRowForView(view) == row) {
            return;
        }

        view->setProperty(HoveredRowProperty, row);
        view->viewport()->update();
    }

    class HoverRowDelegate final : public QStyledItemDelegate {
    public:
        explicit HoverRowDelegate(QAbstractItemView* view)
            : QStyledItemDelegate(view),
              view_(view)
        {
        }

        void paint(
            QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index
        ) const override
        {
            QStyleOptionViewItem opt(option);
            initStyleOption(&opt, index);

            const bool hoveredRow = view_ && index.row() == hoveredRowForView(view_);
            const bool selected = (opt.state & QStyle::State_Selected);

            if (hoveredRow && !selected) {
                /*
                 * Draw our own full-row hover background. Clearing Alternate and
                 * State_MouseOver prevents the style from painting per-cell hover
                 * or alternating-row backgrounds over it.
                 */
                opt.features &= ~QStyleOptionViewItem::Alternate;
                opt.state &= ~QStyle::State_MouseOver;

                QColor hover = opt.palette.color(QPalette::Highlight);
                hover.setAlpha(40);
                painter->fillRect(opt.rect, hover);

                opt.backgroundBrush = Qt::NoBrush;
            }

            QStyledItemDelegate::paint(painter, opt, index);
        }

    private:
        QPointer<QAbstractItemView> view_;
    };

    class HoverRowController final : public QObject {
    public:
        explicit HoverRowController(QAbstractItemView* view)
            : QObject(view),
              view_(view)
        {
            view_->setMouseTracking(true);
            view_->viewport()->setMouseTracking(true);
            view_->viewport()->installEventFilter(this);

            connect(
                view_,
                &QAbstractItemView::entered,
                this,
                [this](const QModelIndex& index) {
                    setHoveredRow(index.isValid() ? index.row() : -1);
                }
            );

            connect(
                view_,
                &QAbstractItemView::viewportEntered,
                this,
                [this]() {
                    setHoveredRow(-1);
                }
            );
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (view_ &&
                watched == view_->viewport() &&
                event->type() == QEvent::Leave) {
                setHoveredRow(-1);
            }

            return QObject::eventFilter(watched, event);
        }

    private:
        void setHoveredRow(const int row)
        {
            if (view_) {
                setHoveredRowForView(view_, row);
            }
        }

        QPointer<QAbstractItemView> view_;
    };
}

void installHoverRowHighlight(QAbstractItemView* view)
{
    if (!view) {
        return;
    }

    view->setProperty(HoveredRowProperty, -1);
    view->setItemDelegate(new HoverRowDelegate(view));
    new HoverRowController(view);
}