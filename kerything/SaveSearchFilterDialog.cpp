// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SaveSearchFilterDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "SearchFilterUtils.h"

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
    formLayout->addRow(QStringLiteral("Name:"), nameEdit_);

    macroEdit_ = new QLineEdit(this);
    macroEdit_->setPlaceholderText(QStringLiteral("Optional"));
    macroEdit_->setToolTip(QStringLiteral("Optional keyword macro; type it followed by a colon in the search box."));
    formLayout->addRow(QStringLiteral("Macro:"), macroEdit_);

    queryEdit_ = new QLineEdit(this);
    queryEdit_->setText(searchText.trimmed());
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
        QMessageBox::warning(
            this,
            QStringLiteral("Invalid Filter"),
            issues.front().message
        );
        return;
    }

    searchFilter_ = filters.back();
    accept();
}