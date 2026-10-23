// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SAVESEARCHFILTERDIALOG_H
#define KERYTHING_SAVESEARCHFILTERDIALOG_H

#include <QDialog>
#include <QPalette>

#include <vector>

#include "Preferences.h"

class QLineEdit;
class QAction;

class SaveSearchFilterDialog final : public QDialog {
public:
    explicit SaveSearchFilterDialog(
        const QString& searchText,
        const std::vector<SearchFilterPreference>& existingFilters,
        QWidget* parent = nullptr
    );

    [[nodiscard]] SearchFilterPreference searchFilter() const;

private:
    void acceptIfValid();
    void updateValidationState();
    void clearValidationState();

    std::vector<SearchFilterPreference> existingFilters_;
    SearchFilterPreference searchFilter_;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* macroEdit_ = nullptr;
    QLineEdit* queryEdit_ = nullptr;
    QAction* nameWarningAction_ = nullptr;
    QAction* macroWarningAction_ = nullptr;
    QAction* queryWarningAction_ = nullptr;
    QPalette nameEditPalette_;
    QPalette macroEditPalette_;
    QPalette queryEditPalette_;
};

#endif // KERYTHING_SAVESEARCHFILTERDIALOG_H