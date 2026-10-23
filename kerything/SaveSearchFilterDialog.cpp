// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SaveSearchFilterDialog.h"

#include <QAction>
#include <QColor>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "SearchFilterUtils.h"

namespace {
    QAction* addValidationWarningAction(QLineEdit* edit)
    {
        auto* action = new QAction(
            QIcon::fromTheme(
                QStringLiteral("dialog-warning"),
                QIcon::fromTheme(QStringLiteral("emblem-warning"))
            ),
            QString(),
            edit
        );
        edit->addAction(action, QLineEdit::TrailingPosition);
        action->setVisible(false);
        return action;
    }

    void clearFieldValidation(QLineEdit* edit, QAction* action, const QPalette& palette, const QString& toolTip)
    {
        edit->setPalette(palette);
        edit->setToolTip(toolTip);
        action->setVisible(false);
        action->setToolTip(QString());
    }

    void markFieldInvalid(QLineEdit* edit, QAction* action, const QString& message)
    {
        const QPalette palette = edit->palette();
        const bool darkMode = palette.color(QPalette::Base).lightness() < 128;
        QColor errorBackground = darkMode
            ? QColor(170, 35, 35)
            : QColor(255, 210, 210);

        errorBackground.setAlpha(darkMode ? 115 : 190);

        QPalette invalidPalette = palette;
        invalidPalette.setColor(QPalette::Base, errorBackground);
        invalidPalette.setColor(
            QPalette::Text,
            darkMode ? QColor(255, 235, 235) : QColor(110, 0, 0)
        );
        edit->setPalette(invalidPalette);
        edit->setToolTip(message);
        action->setToolTip(message);
        action->setVisible(true);
    }
}

SaveSearchFilterDialog::SaveSearchFilterDialog(
    const QString& searchText,
    const std::vector<SearchFilterPreference>& existingFilters,
    QWidget* parent
)
    : QDialog(parent),
      existingFilters_(existingFilters)
{
    setWindowTitle(QStringLiteral("Save Search as Filter"));
    setModal(true);
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(
        QStringLiteral("Save the current search as a reusable filter. You can edit the query before saving."),
        this
    );
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* formLayout = new QFormLayout();

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setText(uniqueSearchFilterName(QStringLiteral("New Filter"), existingFilters_));
    nameWarningAction_ = addValidationWarningAction(nameEdit_);
    nameEditPalette_ = nameEdit_->palette();
    formLayout->addRow(QStringLiteral("Name:"), nameEdit_);

    macroEdit_ = new QLineEdit(this);
    macroEdit_->setPlaceholderText(QStringLiteral("Optional"));
    macroEdit_->setToolTip(QStringLiteral("Optional keyword macro; type it followed by a colon in the search box."));
    macroWarningAction_ = addValidationWarningAction(macroEdit_);
    macroEditPalette_ = macroEdit_->palette();
    formLayout->addRow(QStringLiteral("Macro:"), macroEdit_);

    queryEdit_ = new QLineEdit(this);
    queryEdit_->setText(searchText.trimmed());
    queryWarningAction_ = addValidationWarningAction(queryEdit_);
    queryEditPalette_ = queryEdit_->palette();
    formLayout->addRow(QStringLiteral("Query:"), queryEdit_);

    layout->addLayout(formLayout);

    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        this
    );
    buttonBox->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &SaveSearchFilterDialog::acceptIfValid);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);

    connect(nameEdit_, &QLineEdit::textChanged, this, &SaveSearchFilterDialog::updateValidationState);
    connect(macroEdit_, &QLineEdit::textChanged, this, &SaveSearchFilterDialog::updateValidationState);
    connect(queryEdit_, &QLineEdit::textChanged, this, &SaveSearchFilterDialog::updateValidationState);

    nameEdit_->setFocus();
    nameEdit_->selectAll();
}

SearchFilterPreference SaveSearchFilterDialog::searchFilter() const
{
    return searchFilter_;
}

void SaveSearchFilterDialog::acceptIfValid()
{
    std::vector<SearchFilterPreference> filters = existingFilters_;
    SearchFilterPreference candidate;
    candidate.id = newCustomSearchFilterId();
    candidate.name = nameEdit_->text();
    candidate.macro = macroEdit_->text();
    candidate.query = queryEdit_->text();
    filters.push_back(candidate);

    const std::vector<SearchFilterValidationIssue> issues = validateAndNormalizeSearchFilters(filters);
    if (!issues.empty()) {
        updateValidationState();
        QMessageBox::warning(
            this,
            QStringLiteral("Invalid Filter"),
            issues.front().message
        );

        for (const SearchFilterValidationIssue& issue : issues) {
            if (issue.row != static_cast<int>(existingFilters_.size())) {
                continue;
            }

            switch (issue.column) {
                case SearchFilterColumn::Name:
                    nameEdit_->setFocus();
                    return;
                case SearchFilterColumn::Macro:
                    macroEdit_->setFocus();
                    return;
                case SearchFilterColumn::Query:
                    queryEdit_->setFocus();
                    return;
            }
        }

        return;
    }

    searchFilter_ = filters.back();
    accept();
}

void SaveSearchFilterDialog::updateValidationState()
{
    clearValidationState();

    std::vector<SearchFilterPreference> filters = existingFilters_;
    SearchFilterPreference candidate;
    candidate.id = QStringLiteral("save-search-filter-candidate");
    candidate.name = nameEdit_->text();
    candidate.macro = macroEdit_->text();
    candidate.query = queryEdit_->text();
    filters.push_back(std::move(candidate));

    const std::vector<SearchFilterValidationIssue> issues = validateAndNormalizeSearchFilters(filters);
    const int candidateRow = static_cast<int>(existingFilters_.size());

    for (const SearchFilterValidationIssue& issue : issues) {
        if (issue.row != candidateRow) {
            continue;
        }

        switch (issue.column) {
            case SearchFilterColumn::Name:
                markFieldInvalid(nameEdit_, nameWarningAction_, issue.message);
                break;
            case SearchFilterColumn::Macro:
                markFieldInvalid(macroEdit_, macroWarningAction_, issue.message);
                break;
            case SearchFilterColumn::Query:
                markFieldInvalid(queryEdit_, queryWarningAction_, issue.message);
                break;
        }
    }
}

void SaveSearchFilterDialog::clearValidationState()
{
    clearFieldValidation(nameEdit_, nameWarningAction_, nameEditPalette_, QString());
    clearFieldValidation(
        macroEdit_,
        macroWarningAction_,
        macroEditPalette_,
        QStringLiteral("Optional keyword macro; type it followed by a colon in the search box.")
    );
    clearFieldValidation(queryEdit_, queryWarningAction_, queryEditPalette_, QString());
}