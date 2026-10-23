// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SEARCHFILTERUTILS_H
#define KERYTHING_SEARCHFILTERUTILS_H

#include <QString>
#include <vector>

#include "Preferences.h"

enum class SearchFilterColumn {
    Name,
    Macro,
    Query,
};

struct SearchFilterValidationIssue {
    int row = -1;
    SearchFilterColumn column = SearchFilterColumn::Name;
    QString message;
};

[[nodiscard]] QString normalizedSearchFilterMacro(QString macro);
[[nodiscard]] QString normalizedSearchFilterQuery(QString query);
[[nodiscard]] QString uniqueSearchFilterName(
    const QString& baseName,
    const std::vector<SearchFilterPreference>& filters
);
[[nodiscard]] QString newCustomSearchFilterId();
[[nodiscard]] std::vector<SearchFilterValidationIssue> validateAndNormalizeSearchFilters(
    std::vector<SearchFilterPreference>& filters
);

#endif // KERYTHING_SEARCHFILTERUTILS_H
