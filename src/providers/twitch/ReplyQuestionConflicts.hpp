// SPDX-License-Identifier: MIT
#pragma once

#include "common/UiLanguage.hpp"

#include <QHash>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <optional>

namespace chatterino::replyquestions {

struct Location {
    qsizetype rule = -1;
    qsizetype line = -1;
    int channelOrdinal = 0;
    QString phrase;
};
struct Conflict {
    Location first, second;
};
struct Conflicts {
    std::optional<Conflict> first;
    qsizetype count = 0;
};

inline QString channelKey(QString channel)
{
    channel = channel.trimmed().toLower();
    if (channel.startsWith(u'#')) channel.remove(0, 1);
    return channel;
}

// Preserve the editor's real line numbers (including empty lines). Only
// identical normalized questions conflict; overlapping partial triggers
// remain valid and are resolved by the existing matching priority.
template <typename Rules, typename Include, typename KeyFor>
Conflicts findConflicts(const Rules &rules, qsizetype preferredRule, Include include, KeyFor keyFor)
{
    Conflicts result;
    if (rules.isEmpty()) return result;
    QHash<QString, QHash<QString, Location>> seen;
    QHash<QString, int> ordinals;
    QList<int> ordinalByRule;
    for (const auto &rule : rules)
        ordinalByRule.append(++ordinals[channelKey(rule.channel)]);
    const auto start = preferredRule >= 0 && preferredRule < rules.size() ? (preferredRule + 1) % rules.size() : 0;
    for (qsizetype offset = 0; offset < rules.size(); ++offset)
    {
        const auto row = (start + offset) % rules.size();
        const auto &rule = rules.at(row);
        if (!include(rule)) continue;
        auto &channel = seen[channelKey(rule.channel)];
        for (qsizetype line = 0; line < rule.triggers.size(); ++line)
        {
            const auto phrase = rule.triggers.at(line).simplified();
            if (phrase.isEmpty()) continue;
            const auto key = keyFor(rule, phrase);
            if (key.isEmpty()) continue;
            const Location current{row, line, ordinalByRule.at(row), phrase};
            const auto it = channel.constFind(key);
            if (it != channel.cend())
            {
                if (!result.first) result.first = Conflict{it.value(), current};
                ++result.count;
            }
            else channel.insert(key, current);
        }
    }
    return result;
}

template <typename Rules, typename Include>
Conflicts findConflicts(const Rules &rules, qsizetype preferredRule, Include include)
{
    return findConflicts(rules, preferredRule, include,
                         [](const auto &, const QString &phrase) { return phrase.toCaseFolded(); });
}

inline QString concise(QString text, qsizetype limit)
{
    return text.size() > limit ? text.left(limit - 1) + u'…' : text;
}

template <typename Rule>
QString groupLabel(const Rule &rule)
{
    if (!rule.title.trimmed().isEmpty()) return concise(rule.title.trimmed(), 70);
    for (const auto &line : rule.triggers)
        if (!line.simplified().isEmpty()) return concise(line.simplified(), 70);
    return uiText("No group name");
}

template <typename Rules>
QString describe(const Conflicts &conflicts, const Rules &rules)
{
    if (!conflicts.first) return {};
    const auto &c = *conflicts.first;
    const auto a = c.first;
    const auto b = c.second;
    auto result = uiText("Question conflict: “%1” — “%2” (rule %3), line %4 and “%5” (rule %6), line %7.")
        .arg(concise(b.phrase, 120), groupLabel(rules.at(a.rule)), QString::number(a.channelOrdinal),
             QString::number(a.line + 1), groupLabel(rules.at(b.rule)), QString::number(b.channelOrdinal), QString::number(b.line + 1));
    if (conflicts.count > 1)
        result += u' ' + uiText("Other conflicting lines: %1.").arg(conflicts.count - 1);
    return result;
}

inline void selectLine(QPlainTextEdit &editor, qsizetype line)
{
    const auto block = editor.document()->findBlockByNumber(static_cast<int>(line));
    if (block.isValid())
    {
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        editor.setTextCursor(cursor);
        editor.setFocus();
        editor.ensureCursorVisible();
    }
}

} // namespace chatterino::replyquestions
