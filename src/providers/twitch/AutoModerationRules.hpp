#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
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
    QString moduleAction; // optional signed action preset; empty = legacy action
    QJsonObject moduleParameters;
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
enum class RoleSnapshot { Unknown, Ordinary, Protected };

// The same public channel roster used by /mods and /vips. Only a complete,
// valid response that also agrees with the actor's moderator role may
// substitute for a missing recent badge observation. Never persist it.
inline RoleSnapshot checkRoleSnapshot(const QJsonArray &mods, const QJsonArray &vips,
                                      const QString &actorId, const QString &roomId,
                                      const QString &targetId, const QString &targetLogin)
{
    if (!validId(actorId) || !validId(roomId) || !validId(targetId) ||
        login(targetLogin) != targetLogin || targetLogin.isEmpty())
        return RoleSnapshot::Unknown;
    QSet<QString> ids, names;
    bool actorVerified = actorId == roomId;
    bool targetProtected = false;
    for (const auto *list : {&mods, &vips})
    {
        if (list->size() > 10000) return RoleSnapshot::Unknown;
        for (const auto &value : *list)
        {
            if (!value.isObject()) return RoleSnapshot::Unknown;
            const auto row = value.toObject();
            const auto id = row.value("id").toString();
            const auto name = login(row.value("login").toString());
            if (!validId(id) || name.isEmpty() || ids.contains(id) || names.contains(name))
                return RoleSnapshot::Unknown;
            ids.insert(id); names.insert(name);
            if (id == actorId && list == &mods) actorVerified = true;
            if (id == targetId) targetProtected = true;
            if (name == targetLogin && id != targetId) return RoleSnapshot::Unknown;
        }
    }
    if (targetProtected) return RoleSnapshot::Protected;
    return actorVerified ? RoleSnapshot::Ordinary : RoleSnapshot::Unknown;
}
inline bool validRule(const Rule &r)
{
    static const QRegularExpression profile(QStringLiteral("\\A[a-z][a-z0-9-]{0,39}\\z"));
    if(!r.moduleAction.isEmpty() &&
       (r.kind==Kind::RewardUntimeout || !profile.match(r.moduleAction).hasMatch() ||
        r.moduleParameters.size()>24 || QJsonDocument(r.moduleParameters).toJson(QJsonDocument::Compact).size()>8192))return false;
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
    {
        QJsonObject object{{"kind", int(r.kind)}, {"trigger",r.trigger}, {"title",r.title},
            {"seconds",r.seconds}, {"reason",r.reason}, {"enabled",r.enabled}};
        if(!r.moduleAction.isEmpty())
        {
            // A client predating presets must not silently execute a preset as
            // a fixed timeout. Old rules retain exactly their old representation.
            object["enabled"]=false;
            object["moduleAction"]=r.moduleAction;
            object["moduleEnabled"]=r.enabled;
            if(!r.moduleParameters.isEmpty())object["moduleParameters"]=r.moduleParameters;
        }
        rules.append(object);
    }
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
        const auto profile=r.value("moduleAction").toString();
        c.rules.append(Rule{Kind(kind),r.value("trigger").toString(),r.value("title").toString(),
            r.value("seconds").toInt(600),r.value("reason").toString(),
            profile.isEmpty()?r.value("enabled").toBool(true):r.value("moduleEnabled").toBool(false),profile,
            r.value("moduleParameters").toObject()});
    }
    return validate(c) ? std::optional<Config>(std::move(c)) : std::nullopt;
}
inline bool freshEvent(qint64 eventMs, qint64 nowMs)
{
    return eventMs > 0 && eventMs <= nowMs + 5000 && nowMs - eventMs <= 30000;
}
// Only consider a confirmed finite timeout that has not essentially expired.
inline bool canRemoveTimeout(qint64 until, qint64 now)
{
    return until > now + 1000;
}
} // namespace chatterino::automoderation
