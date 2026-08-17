// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SearchResultTableView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextLayout>

#include "FileModel.h"
#include "SearchResultColumns.h"

namespace {
    constexpr const char* HoveredRowProperty = "_kerything_hovered_row";

    /*
     * QStyledItemDelegate/style text drawing applies small style-private spacing
     * that QTextLine::draw() does not reproduce exactly. These values align the
     * custom highlighted text path with the normal, non-highlighted item text
     * placement for the current table style.
     */
    constexpr int HighlightedTextHorizontalInsetLeft = 2;
    constexpr int HighlightedTextHorizontalInsetRight = 3;
    constexpr int HighlightedTextDrawCorrection = 1;

    int hoveredRowForView(const QWidget* view)
    {
        if (!view) {
            return -1;
        }

        return view->property(HoveredRowProperty).toInt();
    }

    void applyHoverRowStyle(
        QStyleOptionViewItem& opt,
        QPainter* painter,
        const QModelIndex& index
    ) {
        const bool hoveredRow =
            opt.widget &&
            index.row() == hoveredRowForView(opt.widget);
        const bool selected = opt.state & QStyle::State_Selected;

        if (!hoveredRow || selected) {
            return;
        }

        opt.features &= ~QStyleOptionViewItem::Alternate;
        opt.state &= ~QStyle::State_MouseOver;

        QColor hover = opt.palette.color(QPalette::Highlight);
        hover.setAlpha(40);
        painter->fillRect(opt.rect, hover);

        opt.backgroundBrush = Qt::NoBrush;
    }

    struct HighlightSpan {
        qsizetype start = 0;
        qsizetype length = 0;
    };

    QList<HighlightSpan> highlightSpansForText(
        const QString& text,
        const QStringList& terms,
        bool matchCase
    ) {
        QList<HighlightSpan> spans;
        const QString searchText = matchCase
            ? text
            : text.toCaseFolded();

        for (const QString& term : terms) {
            const QString trimmedTerm = term.trimmed();
            if (trimmedTerm.isEmpty()) {
                continue;
            }

            const QString searchTerm = matchCase
                ? trimmedTerm
                : trimmedTerm.toCaseFolded();

            qsizetype pos = 0;

            while ((pos = searchText.indexOf(searchTerm, pos)) >= 0) {
                spans.append(HighlightSpan{
                    .start = pos,
                    .length = searchTerm.size(),
                });

                pos += std::max<qsizetype>(searchTerm.size(), 1);
            }
        }

        if (spans.isEmpty()) {
            return spans;
        }

        std::ranges::sort(spans, [](const HighlightSpan& lhs, const HighlightSpan& rhs) {
            if (lhs.start == rhs.start) {
                return lhs.length > rhs.length;
            }

            return lhs.start < rhs.start;
        });

        QList<HighlightSpan> merged;

        for (const HighlightSpan& span : spans) {
            if (merged.isEmpty() ||
                span.start > merged.last().start + merged.last().length) {
                merged.append(span);
                continue;
            }

            HighlightSpan& last = merged.last();
            const qsizetype lastEnd = last.start + last.length;
            const qsizetype spanEnd = span.start + span.length;
            last.length = std::max(lastEnd, spanEnd) - last.start;
        }

        return merged;
    }

    QList<QTextLayout::FormatRange> formatRangesForSpans(
        const QList<HighlightSpan>& spans,
        const QFont& font,
        const QColor& color
    ) {
        QList<QTextLayout::FormatRange> ranges;
        ranges.reserve(spans.size());

        QFont boldFont = font;
        boldFont.setBold(true);

        QTextCharFormat boldFormat;
        boldFormat.setFont(boldFont);
        boldFormat.setForeground(color);

        for (const HighlightSpan& span : spans) {
            QTextLayout::FormatRange range;
            range.start = static_cast<int>(span.start);
            range.length = static_cast<int>(span.length);
            range.format = boldFormat;
            ranges.append(range);
        }

        return ranges;
    }

    qreal singleLineTextWidth(
        const QString& text,
        const QFont& font,
        const QList<QTextLayout::FormatRange>& formats
    ) {
        QTextLayout layout(text, font);
        layout.setFormats(formats);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        layout.endLayout();

        if (!line.isValid()) {
            return 0.0;
        }

        return line.naturalTextWidth();
    }

    QString elideHighlightedText(
        const QString& text,
        const QStringList& terms,
        bool matchCase,
        const QFont& font,
        int width
    ) {
        if (text.isEmpty() || width <= 0) {
            return QString();
        }

        const QList<HighlightSpan> fullSpans = highlightSpansForText(text, terms, matchCase);
        const QList<QTextLayout::FormatRange> fullFormats =
            formatRangesForSpans(fullSpans, font, Qt::black);

        if (singleLineTextWidth(text, font, fullFormats) <= width) {
            return text;
        }

        static constexpr QChar Ellipsis = QChar(0x2026);

        int low = 0;
        int high = text.size();
        QString best = QString(Ellipsis);

        while (low <= high) {
            const int mid = low + (high - low) / 2;
            const QString candidate = text.left(mid) + Ellipsis;

            const QList<HighlightSpan> candidateSpans =
                highlightSpansForText(candidate, terms, matchCase);
            const QList<QTextLayout::FormatRange> candidateFormats =
                formatRangesForSpans(candidateSpans, font, Qt::black);

            if (singleLineTextWidth(candidate, font, candidateFormats) <= width) {
                best = candidate;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }

        return best;
    }

    class HighlightedSearchTermDelegate final : public QStyledItemDelegate {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);

            painter->save();

            applyHoverRowStyle(opt, painter, index);

            const QStringList terms = index.data(FileModel::HighlightTermsRole).toStringList();
            const bool matchCase = index.data(FileModel::HighlightMatchCaseRole).toBool();

            if (terms.isEmpty()) {
                QStyledItemDelegate::paint(painter, opt, index);
                painter->restore();
                return;
            }

            const QString plainText = opt.text;

            QStyle* style = opt.widget
                ? opt.widget->style()
                : QApplication::style();

            /*
             * Compute the text rectangle while opt.text still contains the real text.
             * Some styles use the text-bearing option when calculating item sub-rects.
             * Clearing opt.text before this can shift the custom QTextLayout drawing
             * slightly compared with the default delegate.
             */
            const QRect textRect = style->subElementRect(
                QStyle::SE_ItemViewItemText,
                &opt,
                opt.widget
            );

            const qreal drawX = textRect.left() + HighlightedTextHorizontalInsetLeft;
            const int availableWidth = std::max(
                0,
                textRect.width() - HighlightedTextHorizontalInsetLeft - HighlightedTextHorizontalInsetRight - HighlightedTextDrawCorrection
            );

            /*
             * Keep the left clip edge at the style-provided text rect. Some glyphs have
             * negative left bearing/antialiasing overhang; clipping at the shifted draw
             * origin can make a shifted QTextLayout still appear visually too far left.
             * The right edge is inset so elided text still respects the same practical
             * right padding.
             */
            const QRect clipRect = textRect.adjusted(
                0,
                0,
                -HighlightedTextHorizontalInsetLeft,
                0
            );

            QStyleOptionViewItem backgroundOpt = opt;
            backgroundOpt.text.clear();

            style->drawControl(
                QStyle::CE_ItemViewItem,
                &backgroundOpt,
                painter,
                backgroundOpt.widget
            );

            const bool selected = opt.state & QStyle::State_Selected;
            const QColor textColor = selected
                ? opt.palette.color(QPalette::Active, QPalette::HighlightedText)
                : opt.palette.color(QPalette::Active, QPalette::Text);

            const QString elidedText = elideHighlightedText(
                plainText,
                terms,
                matchCase,
                opt.font,
                availableWidth
            );

            QList<HighlightSpan> spans = highlightSpansForText(elidedText, terms, matchCase);
            QList<QTextLayout::FormatRange> formats =
                formatRangesForSpans(spans, opt.font, textColor);

            QTextLayout layout(elidedText, opt.font);
            layout.setFormats(formats);

            layout.beginLayout();
            QTextLine line = layout.createLine();
            layout.endLayout();

            painter->setPen(textColor);
            painter->setClipRect(clipRect);

            if (line.isValid()) {
                const qreal y = textRect.top() + (textRect.height() - line.height()) / 2.0;
                line.draw(
                    painter,
                    QPointF(drawX + HighlightedTextDrawCorrection, y)
                );
            }

            painter->restore();
        }
    };
}

SearchResultTableView::SearchResultTableView(QWidget* parent)
    : QTableView(parent)
{
    setItemDelegateForColumn(
        SearchResultColumn::Name,
        new HighlightedSearchTermDelegate(this)
    );
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
