#pragma once

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace chatterino::autoreplywords {

inline QStringList tokens(const QString &text)
{
    static const QRegularExpression word(QStringLiteral("[\\p{L}\\p{M}\\p{N}_]+"));
    auto matches = word.globalMatch(text);
    QStringList result;
    while (matches.hasNext())
        result.append(matches.next().captured().toCaseFolded());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

inline QString conflictKey(const QString &phrase, bool allWords)
{
    if (!allWords)
        return QStringLiteral("text:") + phrase.simplified().toCaseFolded();
    const auto words = tokens(phrase);
    return words.isEmpty() ? QString{} : QStringLiteral("words:") + words.join(u' ');
}

}  // namespace chatterino::autoreplywords
