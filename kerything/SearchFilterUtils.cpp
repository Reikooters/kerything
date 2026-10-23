// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SearchFilterUtils.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUuid>

namespace {
    QString normalizedExtensionFilterToken(QString token)
    {
        const qsizetype colon = token.indexOf(QLatin1Char(':'));

        if (colon <= 0) {
            return token;
        }

        const QString prefix = token.left(colon + 1);
        QString extensionList = token.mid(colon + 1);

        if (extensionList.isEmpty() || !extensionList.contains(QLatin1Char(','))) {
            return token;
        }

        extensionList.replace(QLatin1Char(','), QLatin1Char(';'));
        return prefix + extensionList;
    }

    bool isValidFilterMacro(const QString& macro)
    {
        if (macro.isEmpty()) {
            return true;
        }

        for (const QChar c : macro) {
            const ushort value = c.unicode();

            const bool valid =
                (value >= 'A' && value <= 'Z') ||
                (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') ||
                value == '_';

            if (!valid) {
                return false;
            }
        }

        return true;
    }

    bool isBuiltInFilterKeyword(const QString& keyword)
    {
        const QString folded = keyword.trimmed().toCaseFolded();

        return folded == QStringLiteral("ext") ||
               folded == QStringLiteral("extension") ||
               folded == QStringLiteral("file") ||
               folded == QStringLiteral("files") ||
               folded == QStringLiteral("folder") ||
               folded == QStringLiteral("folders") ||
               folded == QStringLiteral("type");
    }

    bool isKnownFilterKeyword(const QString& keyword, const QSet<QString>& macroKeywords)
    {
        const QString folded = keyword.trimmed().toCaseFolded();
        return isBuiltInFilterKeyword(folded) || macroKeywords.contains(folded);
    }

    QStringList filterMacroReferencesInQuery(
        const QString& query,
        const QSet<QString>& macroKeywords
    ) {
        QStringList references;

        const QStringList tokens = query.split(
            QRegularExpression(QStringLiteral("\\s+")),
            Qt::SkipEmptyParts
        );

        for (const QString& token : tokens) {
            const QString trimmedToken = token.trimmed();

            if (trimmedToken.size() <= 1 || !trimmedToken.endsWith(QLatin1Char(':'))) {
                continue;
            }

            const QString keyword = trimmedToken.left(trimmedToken.size() - 1).trimmed().toCaseFolded();

            if (macroKeywords.contains(keyword)) {
                references << keyword;
            }
        }

        references.removeDuplicates();
        return references;
    }
}

namespace SearchFilterUtils {

QString normalizedSearchFilterMacro(QString macro)
{
    macro = macro.trimmed();

    while (macro.endsWith(QLatin1Char(':'))) {
        macro.chop(1);
        macro = macro.trimmed();
    }

    return macro;
}

QString normalizedSearchFilterQuery(QString query)
{
    query = query.trimmed();

    if (query.isEmpty()) {
        return query;
    }

    QStringList normalizedTokens;
    const QStringList tokens = query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    normalizedTokens.reserve(tokens.size());

    for (const QString& token : tokens) {
        QString trimmedToken = token.trimmed();
        const QString foldedToken = trimmedToken.toCaseFolded();

        if (foldedToken == QStringLiteral("ext;")) {
            trimmedToken = QStringLiteral("ext:");
        } else if (foldedToken == QStringLiteral("extension;")) {
            trimmedToken = QStringLiteral("extension:");
        } else if (foldedToken.startsWith(QStringLiteral("ext;"))) {
            trimmedToken = QStringLiteral("ext:") + trimmedToken.mid(4);
        } else if (foldedToken.startsWith(QStringLiteral("extension;"))) {
            trimmedToken = QStringLiteral("extension:") + trimmedToken.mid(10);
        } else if (foldedToken == QStringLiteral("file;")) {
            trimmedToken = QStringLiteral("file:");
        } else if (foldedToken == QStringLiteral("files;")) {
            trimmedToken = QStringLiteral("files:");
        } else if (foldedToken == QStringLiteral("folder;")) {
            trimmedToken = QStringLiteral("folder:");
        } else if (foldedToken == QStringLiteral("folders;")) {
            trimmedToken = QStringLiteral("folders:");
        } else if (foldedToken == QStringLiteral("type;file")) {
            trimmedToken = QStringLiteral("type:file");
        } else if (foldedToken == QStringLiteral("type;files")) {
            trimmedToken = QStringLiteral("type:files");
        } else if (foldedToken == QStringLiteral("type;folder")) {
            trimmedToken = QStringLiteral("type:folder");
        } else if (foldedToken == QStringLiteral("type;folders")) {
            trimmedToken = QStringLiteral("type:folders");
        }

        const QString normalizedFoldedToken = trimmedToken.toCaseFolded();

        if (normalizedFoldedToken.startsWith(QStringLiteral("ext:")) ||
            normalizedFoldedToken.startsWith(QStringLiteral("extension:"))) {
            normalizedTokens << normalizedExtensionFilterToken(trimmedToken);
            continue;
        }

        normalizedTokens << trimmedToken;
    }

    return normalizedTokens.join(QLatin1Char(' '));
}

QString uniqueSearchFilterName(
    const QString& baseName,
    const std::vector<SearchFilterPreference>& filters
) {
    QSet<QString> existingNames;

    for (const SearchFilterPreference& filter : filters) {
        existingNames.insert(filter.name.trimmed().toCaseFolded());
    }

    QString candidate = baseName.trimmed().isEmpty()
        ? QStringLiteral("New Filter")
        : baseName.trimmed();

    if (!existingNames.contains(candidate.toCaseFolded())) {
        return candidate;
    }

    int suffix = 2;
    while (true) {
        const QString numbered = QStringLiteral("%1 %2").arg(candidate).arg(suffix);
        if (!existingNames.contains(numbered.toCaseFolded())) {
            return numbered;
        }

        ++suffix;
    }
}

QString newCustomSearchFilterId()
{
    return QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

std::vector<SearchFilterValidationIssue> validateAndNormalizeSearchFilters(
    std::vector<SearchFilterPreference>& filters
) {
    std::vector<SearchFilterValidationIssue> issues;

    auto addIssue = [&](int row, SearchFilterColumn column, const QString& message) {
        issues.push_back(SearchFilterValidationIssue{ .row = row, .column = column, .message = message });
    };

    QHash<QString, QList<int>> rowsByName;
    QHash<QString, QList<int>> rowsByMacro;
    QHash<QString, int> rowByMacro;
    QHash<QString, QString> queryByMacro;
    QSet<QString> macroKeywords;

    for (const SearchFilterPreference& filter : filters) {
        const QString macro = normalizedSearchFilterMacro(filter.macro);
        if (!macro.isEmpty() && isValidFilterMacro(macro)) {
            macroKeywords.insert(macro.toCaseFolded());
        }
    }

    auto normalizedQueryWithKnownSemicolonFixes = [&](QString query) {
        query = normalizedSearchFilterQuery(query);

        if (query.isEmpty()) {
            return query;
        }

        QStringList normalizedTokens;
        const QStringList tokens = query.split(
            QRegularExpression(QStringLiteral("\\s+")),
            Qt::SkipEmptyParts
        );

        normalizedTokens.reserve(tokens.size());

        for (const QString& token : tokens) {
            QString trimmedToken = token.trimmed();

            if (trimmedToken.size() > 1 && trimmedToken.endsWith(QLatin1Char(';'))) {
                const QString keyword = trimmedToken.left(trimmedToken.size() - 1).trimmed();

                if (isKnownFilterKeyword(keyword, macroKeywords)) {
                    trimmedToken = keyword + QLatin1Char(':');
                }
            }

            normalizedTokens << trimmedToken;
        }

        return normalizedTokens.join(QLatin1Char(' '));
    };

    for (std::size_t rowIndex = 0; rowIndex < filters.size(); ++rowIndex) {
        SearchFilterPreference& filter = filters[rowIndex];
        const int row = static_cast<int>(rowIndex);
        const QString name = filter.name.trimmed();
        const QString query = normalizedQueryWithKnownSemicolonFixes(filter.query);
        const QString macro = normalizedSearchFilterMacro(filter.macro);
        const QString foldedMacro = macro.toCaseFolded();

        filter.name = name;
        filter.query = query;
        filter.macro = macro;

        if (name.isEmpty()) {
            addIssue(row, SearchFilterColumn::Name, QStringLiteral("Filter names cannot be empty."));
        } else {
            rowsByName[name.toCaseFolded()].append(row);
        }

        if (query.isEmpty()) {
            addIssue(row, SearchFilterColumn::Query, QStringLiteral("Filter queries cannot be empty."));
        }

        if (!isValidFilterMacro(macro)) {
            addIssue(
                row,
                SearchFilterColumn::Macro,
                QStringLiteral("Filter macros can only contain letters, numbers, and underscores.")
            );
        } else if (!macro.isEmpty() && isBuiltInFilterKeyword(macro)) {
            addIssue(
                row,
                SearchFilterColumn::Macro,
                QStringLiteral(
                    "Filter macros cannot use built-in filter keywords such as ext, extension, file, files, folder, folders, or type."
                )
            );
        } else if (!macro.isEmpty()) {
            rowsByMacro[foldedMacro].append(row);
            rowByMacro.insert(foldedMacro, row);
            queryByMacro.insert(foldedMacro, query);
        }

        const QStringList queryTokens = query.split(
            QRegularExpression(QStringLiteral("\\s+")),
            Qt::SkipEmptyParts
        );

        for (const QString& token : queryTokens) {
            const QString trimmedToken = token.trimmed();
            const QString foldedToken = trimmedToken.toCaseFolded();

            if (foldedToken.startsWith(QStringLiteral("ext:")) ||
                foldedToken.startsWith(QStringLiteral("extension:"))) {
                const qsizetype colon = trimmedToken.indexOf(QLatin1Char(':'));
                const QString extensionList = colon >= 0
                    ? trimmedToken.mid(colon + 1)
                    : QString();

                bool hasExtension = false;
                const QStringList extensionTokens = extensionList.split(
                    QRegularExpression(QStringLiteral("[;,]")),
                    Qt::SkipEmptyParts
                );

                for (const QString& extensionToken : extensionTokens) {
                    QString normalizedExtension = extensionToken.trimmed();

                    while (normalizedExtension.startsWith(QLatin1Char('.'))) {
                        normalizedExtension.remove(0, 1);
                    }

                    if (!normalizedExtension.isEmpty()) {
                        hasExtension = true;
                        break;
                    }
                }

                if (!hasExtension) {
                    addIssue(
                        row,
                        SearchFilterColumn::Query,
                        QStringLiteral(
                            "Extension filters must include at least one extension. Example: ext:7z or ext:7z;zip."
                        )
                    );
                    break;
                }
            }

            if (foldedToken.startsWith(QStringLiteral("type:"))) {
                if (foldedToken != QStringLiteral("type:file") &&
                    foldedToken != QStringLiteral("type:files") &&
                    foldedToken != QStringLiteral("type:folder") &&
                    foldedToken != QStringLiteral("type:folders")) {
                    addIssue(
                        row,
                        SearchFilterColumn::Query,
                        QStringLiteral(
                            "Unknown type filter \"%1\". Supported values are type:file, type:files, type:folder, and type:folders."
                        ).arg(trimmedToken)
                    );
                    break;
                }
            }

            if (trimmedToken.size() <= 1 || !trimmedToken.endsWith(QLatin1Char(':'))) {
                continue;
            }

            const QString keyword = trimmedToken.left(trimmedToken.size() - 1).trimmed();

            if (isKnownFilterKeyword(keyword, macroKeywords)) {
                continue;
            }

            addIssue(
                row,
                SearchFilterColumn::Query,
                QStringLiteral(
                    "Unknown filter keyword \"%1:\". Check for a typo, or add a filter macro named \"%1\"."
                ).arg(keyword)
            );
            break;
        }
    }

    const QString duplicateNameMessage = QStringLiteral("Filter names must be unique.");
    for (auto it = rowsByName.constBegin(); it != rowsByName.constEnd(); ++it) {
        const QList<int>& rows = it.value();
        if (rows.size() < 2) {
            continue;
        }

        for (const int row : rows) {
            addIssue(row, SearchFilterColumn::Name, duplicateNameMessage);
        }
    }

    const QString duplicateMacroMessage = QStringLiteral("Filter macros must be unique.");
    for (auto it = rowsByMacro.constBegin(); it != rowsByMacro.constEnd(); ++it) {
        const QList<int>& rows = it.value();
        if (rows.size() < 2) {
            continue;
        }

        for (const int row : rows) {
            addIssue(row, SearchFilterColumn::Macro, duplicateMacroMessage);
        }
    }

    QHash<QString, QStringList> macroReferencesByMacro;
    for (auto it = queryByMacro.constBegin(); it != queryByMacro.constEnd(); ++it) {
        macroReferencesByMacro.insert(
            it.key(),
            filterMacroReferencesInQuery(it.value(), macroKeywords)
        );
    }

    QSet<QString> visitingMacros;
    QSet<QString> visitedMacros;

    auto containsCycle = [&](const QString& macro, auto&& containsCycleRef) -> bool {
        if (visitingMacros.contains(macro)) {
            return true;
        }

        if (visitedMacros.contains(macro)) {
            return false;
        }

        visitingMacros.insert(macro);

        const QStringList references = macroReferencesByMacro.value(macro);
        for (const QString& referencedMacro : references) {
            if (containsCycleRef(referencedMacro, containsCycleRef)) {
                return true;
            }
        }

        visitingMacros.remove(macro);
        visitedMacros.insert(macro);
        return false;
    };

    for (auto it = rowByMacro.constBegin(); it != rowByMacro.constEnd(); ++it) {
        visitingMacros.clear();

        if (!containsCycle(it.key(), containsCycle)) {
            continue;
        }

        addIssue(
            it.value(),
            SearchFilterColumn::Query,
            QStringLiteral("Filter macro \"%1:\" creates a circular filter reference.").arg(it.key())
        );
    }

    return issues;
}

}
