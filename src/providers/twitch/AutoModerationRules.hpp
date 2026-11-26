#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <optional>

namespace chatterino::automoderation {

enum class Kind { Words, RewardTimeout, RewardUntimeout };
struct Rule {
    Kind kind = Kind::Words;
    QString trigger; // literal phrase or confirmed custom reward ID
    QString title;
    int seconds = 600;
    QString reason;
    bool enabled = true;
};
struct Config {
    QString roomId;
    bool enabled = false;
    bool dryRun = true;
    QStringList exceptions;
    QList<Rule> rules;
};

inline QString login(QString value)
{
    value = value.trimmed();
    if (value.startsWith(u'@')) value.remove(0, 1);
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_]{2,25}\\z"));
    return re.match(value).hasMatch() ? value.toLower() : QString{};
}
inline bool validId(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("\\A[1-9][0-9]{0,19}\\z"));
    return re.match(value).hasMatch();
}
inline bool rewardId(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("\\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"));
    return re.match(value).hasMatch();
}
inline bool protectedBadges(const QString &badges)
{
    for (const auto &badge : badges.split(u',', Qt::SkipEmptyParts))
    {
        const auto name = badge.section(u'/', 0, 0);
        if (name == u"moderator" || name == u"lead_moderator" || name == u"broadcaster" ||
            name == u"staff" || name == u"admin" || name == u"global_mod" || name == u"vip")
            return true;
    }
    return false;
}
inline bool validRule(const Rule &r)
{
    return r.reason.size() <= 300 && r.title.size() <= 120 &&
        (r.kind == Kind::Words ? !r.trigger.simplified().isEmpty() && r.trigger.size() <= 200
                               : rewardId(r.trigger)) &&
        (r.kind == Kind::RewardUntimeout || (r.seconds >= 1 && r.seconds <= 86400));
}
inline std::optional<Rule> match(const Config &c, const QString &text, const QString &reward)
{
    if (!c.enabled) return {};
    // A reward message is never also interpreted as a word-rule violation.
    const auto key = text.toCaseFolded();
    for (const auto &rule : c.rules)
    {
        if (!rule.enabled || !validRule(rule)) continue;
        if (!reward.isEmpty())
        {
            if (rule.kind != Kind::Words && rule.trigger.compare(reward, Qt::CaseInsensitive) == 0)
                return rule;
        }
        else if (rule.kind == Kind::Words && key.contains(rule.trigger.toCaseFolded()))
            return rule;
    }
    return {};
}
inline bool validate(const Config &c)
{
    if (!validId(c.roomId) || c.rules.size() > 100 || c.exceptions.size() > 100) return false;
    QStringList seen;
    for (const auto &r : c.rules)
    {
        if (!validRule(r)) return false;
        const auto key = QString(r.kind == Kind::Words ? u"word:" : u"reward:") + r.trigger.toCaseFolded();
        if (seen.contains(key)) return false;
        seen.append(key);
    }
    for (const auto &name : c.exceptions)
        if (login(name) != name) return false;
    return true;
}
inline QJsonObject encode(const Config &c)
{
    QJsonArray rules;
    for (const auto &r : c.rules)
        rules.append(QJsonObject{{"kind", int(r.kind)}, {"trigger",r.trigger}, {"title",r.title},
            {"seconds",r.seconds}, {"reason",r.reason}, {"enabled",r.enabled}});
    return {{"roomId",c.roomId},{"enabled",c.enabled},{"dryRun",c.dryRun},
            {"exceptions",QJsonArray::fromStringList(c.exceptions)},{"rules",rules}};
}
inline std::optional<Config> decode(const QJsonObject &obj)
{
    Config c{obj.value("roomId").toString(), obj.value("enabled").toBool(false),
             obj.value("dryRun").toBool(true)};
    for (const auto &value : obj.value("exceptions").toArray())
        c.exceptions.append(value.toString());
    for (const auto &value : obj.value("rules").toArray())
    {
        auto r = value.toObject();
        const int kind = r.value("kind").toInt(-1);
        if (kind < 0 || kind > 2) return {};
        c.rules.append(Rule{Kind(kind),r.value("trigger").toString(),r.value("title").toString(),
            r.value("seconds").toInt(600),r.value("reason").toString(),r.value("enabled").toBool(true)});
    }
    return validate(c) ? std::optional<Config>(std::move(c)) : std::nullopt;
}
inline bool freshEvent(qint64 eventMs, qint64 nowMs)
{
    return eventMs > 0 && eventMs <= nowMs + 5000 && nowMs - eventMs <= 30000;
}
// A shortened finite timeout cannot remove a permanent ban. Seek explicit
// evidence of an active timeout before considering this action.
inline bool canShortenTimeout(qint64 until, qint64 now)
{
    return until > now + 1000;
}
} // namespace chatterino::automoderation
