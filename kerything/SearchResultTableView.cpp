// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SearchResultTableView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextLayout>

#include <memory>

#include <re2/re2.h>
#include <re2/stringpiece.h>

#include "FileModel.h"
#include "SearchResultColumns.h"

namespace {
    constexpr const char* HoveredRowProperty = "_kerything_hovered_row";

    /*
     * Draw result text and decorations inside stable rects instead of using the
     * style's internal text/elision and decoration layout paths. Some styles make
     * item sub-rects depend on current text width or available column width, which
     * can shift text/icons when content barely fits or starts eliding.
     */
    constexpr int ResultTextLeftInset = 2;
    constexpr int ResultTextRightInset = 6;
    constexpr int ResultDecorationLeftInset = 3;
    constexpr int ResultDecorationTextSpacing = 4;

    void drawTextLayoutLine(
        QPainter* painter,
        const QRect& contentRect,
        const QString& text,
        const QFont& font,
        const QColor& textColor,
        const QList<QTextLayout::FormatRange>& formats = {}
    ) {
        if (contentRect.width() <= 0 || contentRect.height() <= 0 || text.isEmpty()) {
            return;
        }

        QTextLayout layout(text, font);
        layout.setFormats(formats);

        layout.beginLayout();
        QTextLine line = layout.createLine();
        layout.endLayout();

        if (!line.isValid()) {
            return;
        }

        painter->setPen(textColor);
        painter->setClipRect(contentRect);

        const qreal y = contentRect.top() + (contentRect.height() - line.height()) / 2.0;
        line.draw(
            painter,
            QPointF(contentRect.left(), y)
        );
    }

    QString elidePlainText(
        const QString& text,
        const QFontMetrics& fontMetrics,
        Qt::TextElideMode elideMode,
        int width
    ) {
        if (text.isEmpty() || width <= 0) {
            return QString();
        }

        return fontMetrics.elidedText(text, elideMode, width);
    }

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

    bool isAsciiWordCharacter(QChar c) noexcept
    {
        const ushort value = c.unicode();

        return (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') ||
               value == '_';
    }

    bool isWholeWordMatchAt(
        const QString& text,
        qsizetype pos,
        qsizetype length
    ) {
        if (length <= 0 || pos < 0 || pos + length > text.size()) {
            return false;
        }

        const bool leftBoundary =
            pos == 0 ||
            !isAsciiWordCharacter(text.at(pos - 1));

        const bool rightBoundary =
            pos + length >= text.size() ||
            !isAsciiWordCharacter(text.at(pos + length));

        return leftBoundary && rightBoundary;
    }

    QList<HighlightSpan> highlightSpansForText(
        const QString& text,
        const QStringList& terms,
        bool matchCase,
        bool matchWholeWord
    ) {
        QList<HighlightSpan> spans;
        const QString searchText = matchCase
            ? text
            : text.toCaseFolded();

        for (const QString& term : terms) {
            if (term.isEmpty()) {
                continue;
            }

            const QString searchTerm = matchCase
                ? term
                : term.toCaseFolded();

            qsizetype pos = 0;

            while ((pos = searchText.indexOf(searchTerm, pos)) >= 0) {
                if (!matchWholeWord || isWholeWordMatchAt(searchText, pos, searchTerm.size())) {
                    spans.append(HighlightSpan{
                        .start = pos,
                        .length = searchTerm.size(),
                    });
                }

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

    struct RegexHighlightCache {
        QString pattern;
        bool matchCase = false;
        bool valid = false;
        std::unique_ptr<re2::RE2> regex;

        void update(const QString& nextPattern, bool nextMatchCase)
        {
            if (regex &&
                pattern == nextPattern &&
                matchCase == nextMatchCase) {
                return;
            }

            pattern = nextPattern;
            matchCase = nextMatchCase;
            valid = false;
            regex.reset();

            const QString trimmedPattern = pattern.trimmed();
            if (trimmedPattern.isEmpty()) {
                return;
            }

            const QByteArray patternBytes = trimmedPattern.toUtf8();

            re2::RE2::Options options;
            options.set_log_errors(false);
            options.set_never_nl(true);
            options.set_case_sensitive(matchCase);

            auto compiled = std::make_unique<re2::RE2>(
                std::string_view(
                    patternBytes.constData(),
                    static_cast<std::size_t>(patternBytes.size())
                ),
                options
            );

            if (!compiled->ok()) {
                return;
            }

            valid = true;
            regex = std::move(compiled);
        }
    };

    QList<HighlightSpan> highlightSpansForRegexText(
        const QString& text,
        const re2::RE2* regex
    ) {
        QList<HighlightSpan> spans;

        if (text.isEmpty() || !regex) {
            return spans;
        }

        const QByteArray textBytes = text.toUtf8();

        re2::StringPiece input(
            textBytes.constData(),
            static_cast<int>(textBytes.size())
        );

        int searchStart = 0;

        while (searchStart <= textBytes.size()) {
            re2::StringPiece match;

            const bool matched = regex->Match(
                input,
                searchStart,
                static_cast<int>(textBytes.size()),
                re2::RE2::UNANCHORED,
                &match,
                1
            );

            if (!matched) {
                break;
            }

            const auto* matchBegin = match.data();
            const auto* inputBegin = textBytes.constData();

            const int byteStart = static_cast<int>(matchBegin - inputBegin);
            const int byteLength = static_cast<int>(match.size());

            if (byteStart < 0 || byteStart > textBytes.size()) {
                break;
            }

            const QString before = QString::fromUtf8(
                textBytes.constData(),
                byteStart
            );

            const QString matchedText = QString::fromUtf8(
                textBytes.constData() + byteStart,
                byteLength
            );

            if (!matchedText.isEmpty()) {
                spans.append(HighlightSpan{
                    .start = before.size(),
                    .length = matchedText.size(),
                });
            }

            const int nextStart = byteStart + std::max(byteLength, 1);

            if (nextStart <= searchStart) {
                break;
            }

            searchStart = nextStart;
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

    QList<HighlightSpan> highlightSpansForText(
        const QString& text,
        const QStringList& terms,
        bool matchCase,
        bool matchWholeWord,
        bool useRegex,
        const re2::RE2* regex = nullptr
    ) {
        if (useRegex) {
            return highlightSpansForRegexText(text, regex);
        }

        return highlightSpansForText(text, terms, matchCase, matchWholeWord);
    }

    QList<HighlightSpan> highlightSpansForElidedText(
        const QString& originalText,
        const QString& elidedText,
        const QStringList& terms,
        bool matchCase,
        bool matchWholeWord,
        bool useRegex,
        const re2::RE2* regex = nullptr
    ) {
        static constexpr QChar Ellipsis = QChar(0x2026);

        if (originalText == elidedText) {
            return highlightSpansForText(
                elidedText,
                terms,
                matchCase,
                matchWholeWord,
                useRegex,
                regex
            );
        }

        if (elidedText.isEmpty()) {
            return {};
        }

        /*
         * elideHighlightedText() only right-elides, so the visible text is a prefix
         * of originalText followed by U+2026. Highlight any full-text match that
         * intersects that visible prefix. This preserves partial highlighting for
         * cases like:
         *
         *   original: SettingsHandlers_AppControl.dll
         *   query:    AppControl
         *   elided:   SettingsHandlers_AppC…
         *
         * where "AppC" should still be highlighted.
         */
        const qsizetype visiblePrefixLength =
            elidedText.endsWith(Ellipsis)
                ? elidedText.size() - 1
                : elidedText.size();

        if (visiblePrefixLength <= 0) {
            return {};
        }

        const QList<HighlightSpan> originalSpans =
            highlightSpansForText(
                originalText,
                terms,
                matchCase,
                matchWholeWord,
                useRegex,
                regex
            );

        QList<HighlightSpan> clippedSpans;

        for (const HighlightSpan& span : originalSpans) {
            const qsizetype spanStart = span.start;
            const qsizetype spanEnd = span.start + span.length;

            const qsizetype clippedStart = std::max<qsizetype>(spanStart, 0);
            const qsizetype clippedEnd = std::min<qsizetype>(spanEnd, visiblePrefixLength);

            if (clippedEnd <= clippedStart) {
                continue;
            }

            clippedSpans.append(HighlightSpan{
                .start = clippedStart,
                .length = clippedEnd - clippedStart,
            });
        }

        return clippedSpans;
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
        bool matchWholeWord,
        bool useRegex,
        const re2::RE2* regex,
        const QFont& font,
        int width
    ) {
        if (text.isEmpty() || width <= 0) {
            return QString();
        }

        const QList<HighlightSpan> fullSpans =
            highlightSpansForText(text, terms, matchCase, matchWholeWord, useRegex, regex);
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
                highlightSpansForElidedText(
                    text,
                    candidate,
                    terms,
                    matchCase,
                    matchWholeWord,
                    useRegex,
                    regex
                );
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

    class SearchResultTextDelegate final : public QStyledItemDelegate {
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
            const bool matchWholeWord = index.data(FileModel::HighlightMatchWholeWordRole).toBool();
            const bool useRegex = index.data(FileModel::HighlightUseRegexRole).toBool();

            const QString plainText = opt.text;

            QStyle* style = opt.widget
                ? opt.widget->style()
                : QApplication::style();

            /*
             * Let the style draw only the item background, selection, focus, etc.
             * Text and decorations are drawn manually below from opt.rect-based
             * geometry so their positions do not change when content approaches the
             * elision threshold.
             */
            QStyleOptionViewItem backgroundOpt = opt;
            backgroundOpt.text.clear();

            const QIcon itemIcon = backgroundOpt.icon;
            const bool hasItemIcon = !itemIcon.isNull();

            const QSize iconSize = opt.decorationSize.isValid()
                ? opt.decorationSize
                : QSize(16, 16);

            QRect decorationRect;

            int contentLeft = opt.rect.left() + ResultTextLeftInset;

            if (hasItemIcon) {
                decorationRect = QRect(
                    opt.rect.left() + ResultDecorationLeftInset,
                    opt.rect.top() + (opt.rect.height() - iconSize.height()) / 2,
                    iconSize.width(),
                    iconSize.height()
                );

                contentLeft =
                    decorationRect.right() +
                    1 +
                    ResultDecorationTextSpacing +
                    ResultTextLeftInset;
            }

            const int contentRight = opt.rect.right() - ResultTextRightInset;

            const QRect contentRect(
                contentLeft,
                opt.rect.top(),
                std::max(0, contentRight - contentLeft + 1),
                opt.rect.height()
            );

            backgroundOpt.icon = QIcon();

            style->drawControl(
                QStyle::CE_ItemViewItem,
                &backgroundOpt,
                painter,
                backgroundOpt.widget
            );

            if (hasItemIcon && decorationRect.isValid()) {
                const QIcon::Mode iconMode =
                    opt.state & QStyle::State_Enabled
                        ? QIcon::Normal
                        : QIcon::Disabled;

                const QIcon::State iconState =
                    opt.state & QStyle::State_Open
                        ? QIcon::On
                        : QIcon::Off;

                painter->save();
                painter->setClipRect(opt.rect);

                itemIcon.paint(
                    painter,
                    decorationRect,
                    opt.decorationAlignment,
                    iconMode,
                    iconState
                );

                painter->restore();
            }

            const bool selected = opt.state & QStyle::State_Selected;
            const QColor textColor = selected
                ? opt.palette.color(QPalette::Active, QPalette::HighlightedText)
                : opt.palette.color(QPalette::Active, QPalette::Text);

            const int availableWidth = std::max(0, contentRect.width());

            if (terms.isEmpty()) {
                const QString elidedText = elidePlainText(
                    plainText,
                    opt.fontMetrics,
                    opt.textElideMode,
                    availableWidth
                );

                drawTextLayoutLine(
                    painter,
                    contentRect,
                    elidedText,
                    opt.font,
                    textColor
                );

                painter->restore();
                return;
            }

            const re2::RE2* highlightRegex = nullptr;

            if (useRegex) {
                regexHighlightCache_.update(
                    terms.first(),
                    matchCase
                );

                if (!regexHighlightCache_.valid) {
                    const QString elidedText = elidePlainText(
                        plainText,
                        opt.fontMetrics,
                        opt.textElideMode,
                        availableWidth
                    );

                    drawTextLayoutLine(
                        painter,
                        contentRect,
                        elidedText,
                        opt.font,
                        textColor
                    );

                    painter->restore();
                    return;
                }

                highlightRegex = regexHighlightCache_.regex.get();
            }

            const QString elidedText = elideHighlightedText(
                plainText,
                terms,
                matchCase,
                matchWholeWord,
                useRegex,
                highlightRegex,
                opt.font,
                availableWidth
            );

            QList<HighlightSpan> spans =
                highlightSpansForElidedText(
                    plainText,
                    elidedText,
                    terms,
                    matchCase,
                    matchWholeWord,
                    useRegex,
                    highlightRegex
                );

            QList<QTextLayout::FormatRange> formats =
                formatRangesForSpans(spans, opt.font, textColor);

            drawTextLayoutLine(
                painter,
                contentRect,
                elidedText,
                opt.font,
                textColor,
                formats
            );

            painter->restore();
        }

    private:
        mutable RegexHighlightCache regexHighlightCache_;
    };
}

SearchResultTableView::SearchResultTableView(QWidget* parent)
    : QTableView(parent)
{
    setItemDelegateForColumn(
        SearchResultColumn::Name,
        new SearchResultTextDelegate(this)
    );

    setItemDelegateForColumn(
        SearchResultColumn::Path,
        new SearchResultTextDelegate(this)
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
