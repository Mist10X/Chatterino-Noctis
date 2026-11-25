// SPDX-License-Identifier: MIT
#pragma once

#include "common/UiLanguage.hpp"
#include "providers/twitch/ModeratorModule.hpp"

#include <IrcTagsRef>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <optional>

namespace chatterino::moderator {

enum class SenderRole : unsigned { Unknown = 0, Viewer = 1, Vip = 2, Moderator = 4, Broadcaster = 8 };
struct RoleOption { SenderRole role; const char *key; const char *label; };
inline constexpr std::array ROLE_OPTIONS{
    RoleOption{SenderRole::Viewer, "viewer", "Viewers"},
    RoleOption{SenderRole::Vip, "vip", "VIP"},
    RoleOption{SenderRole::Moderator, "moderator", "Moderators"},
    RoleOption{SenderRole::Broadcaster, "owner", "Broadcaster"},
};
inline constexpr unsigned DEFAULT_SENDER_ROLES = static_cast<unsigned>(SenderRole::Viewer);

inline bool acceptsRole(unsigned mask, SenderRole role)
{
    return (mask & static_cast<unsigned>(role)) != 0;
}

inline SenderRole senderRole(Communi::TagsRef tags)
{
    const auto id = tags.getOrEmpty("user-id");
    if (!validUserId(id)) return SenderRole::Unknown;
    if (tags.getOrEmpty("room-id") == id) return SenderRole::Broadcaster;
    auto role = tags.has("badges") ? SenderRole::Viewer : SenderRole::Unknown;
    if (tags.getOrEmpty("vip") == "1") role = SenderRole::Vip;
    if (tags.getOrEmpty("mod") == "1" || tags.getOrEmpty("user-type") == "mod") role = SenderRole::Moderator;
    bool staff = false;
    for (const auto &badge : tags.getOrEmpty("badges").split(u','))
    {
        if (badge.startsWith("broadcaster/")) return SenderRole::Broadcaster;
        if (badge.startsWith("moderator/") || badge.startsWith("lead_moderator/")) role = SenderRole::Moderator;
        else if (badge.startsWith("vip/")) role = std::max(role, SenderRole::Vip);
        else if (badge.startsWith("staff/")) staff = true;
    }
    return staff && role == SenderRole::Viewer ? SenderRole::Unknown : role;
}

inline QJsonArray encodeRoles(unsigned mask)
{
    QJsonArray out;
    for (const auto &option : ROLE_OPTIONS)
        if (acceptsRole(mask, option.role)) out.append(QString::fromLatin1(option.key));
    return out;
}

inline unsigned decodeRoles(const QJsonValue &value)
{
    if (value.isUndefined()) return DEFAULT_SENDER_ROLES;  // old profiles
    unsigned mask = 0;
    if (value.isArray())
        for (const auto &option : ROLE_OPTIONS)
            if (value.toArray().contains(QString::fromLatin1(option.key))) mask |= static_cast<unsigned>(option.role);
    return mask;
}

inline QJsonObject encodeTimers(const QHash<QString, qint64> &timers)
{
    QJsonObject out;
    for (auto it = timers.cbegin(); it != timers.cend(); ++it)
        if (validChannel(it.key()) && it.value() > 0) out.insert(it.key(), it.value());
    return out;
}

inline QHash<QString, qint64> decodeTimers(const QJsonObject &obj, qint64 now)
{
    QHash<QString, qint64> out;
    for (auto it = obj.begin(); it != obj.end() && out.size() < 1000; ++it)
    {
        const auto start = it.value().toInteger();
        if (validChannel(it.key()) && start > 0 && start <= now) out.insert(it.key(), start);
    }
    return out;
}

// Confirmed observations from Twitch's existing live-status requests (30 s).
// Short restarts preserve the same segment; long unobserved gaps are never
// silently counted as continuous play. No background process or host API.
struct CategoryState {
    QString stream, category, name;
    qint64 since = 0, seen = 0;
    bool uncertainSegment = false, uncertainTotal = false;
    QHash<QString, qint64> completed;

    bool observe(const QString &streamId, const QString &categoryId,
                 const QString &title, qint64 streamStart, qint64 now)
    {
        if (!validUserId(streamId) || !validUserId(categoryId) || streamStart <= 0 || now < streamStart || now < seen)
            return false;
        if (stream != streamId || since < streamStart)
        {
            *this = {};
            stream = streamId;
            category = categoryId;
            uncertainSegment = now - streamStart > 45;
            uncertainTotal = uncertainSegment;
            since = uncertainSegment ? now : streamStart;
        }
        else if (category != categoryId || now - seen > 90)
        {
            completed[category] += std::max<qint64>(0, seen - since);
            uncertainSegment = now - seen > 90;
            uncertainTotal = uncertainTotal || uncertainSegment;
            category = categoryId;
            since = now;
        }
        name = title;
        seen = now;
        return true;
    }

    QJsonObject encode() const
    {
        QJsonObject totals;
        for (auto it = completed.cbegin(); it != completed.cend(); ++it) totals.insert(it.key(), it.value());
        return {{"stream", stream}, {"category", category}, {"name", name}, {"since", since}, {"seen", seen},
                {"uncertainSegment", uncertainSegment}, {"uncertainTotal", uncertainTotal}, {"completed", totals}};
    }
    static CategoryState decode(const QJsonObject &data)
    {
        CategoryState s;
        s.stream = data.value("stream").toString();
        s.category = data.value("category").toString();
        s.name = data.value("name").toString();
        s.since = data.value("since").toInteger();
        s.seen = data.value("seen").toInteger();
        s.uncertainSegment = data.value("uncertainSegment").toBool(true);
        s.uncertainTotal = data.value("uncertainTotal").toBool(true);
        if (!validUserId(s.stream) || !validUserId(s.category) || s.since <= 0 || s.seen < s.since)
            return {};
        const auto totals = data.value("completed").toObject();
        for (auto it = totals.begin(); it != totals.end() && s.completed.size() < 1000; ++it)
        {
            const auto value = it.value().toInteger(-1);
            if (validUserId(it.key()) && value >= 0 && value <= s.seen) s.completed.insert(it.key(), value);
        }
        return s;
    }
};

inline CategoryState readCategory(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
    return CategoryState::decode(QJsonDocument::fromJson(file.readAll()).object());
}

inline CategoryState observeCategoryFile(const QDir &dir, const QString &channelId,
    const QString &streamId, const QString &categoryId, const QString &name, qint64 start, qint64 now)
{
    if (!validUserId(channelId) || !QDir().mkpath(dir.absolutePath())) return {};
    const auto path = dir.filePath(channelId + ".json");
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0)) return readCategory(path);
    const auto previous = readCategory(path);
    auto next = previous;
    if (!next.observe(streamId, categoryId, name, start, now)) return previous;
    if (next.encode() == previous.encode()) return previous;
    const auto bytes = QJsonDocument(next.encode()).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return previous;
    return next;
}

inline QString duration(qint64 seconds)
{
    const auto minutes = std::max<qint64>(0, seconds / 60);
    return minutes >= 60 ? uiText("%1 h %2 min").arg(minutes / 60).arg(minutes % 60)
                         : uiText("%1 min").arg(minutes);
}

inline std::optional<QString> expandReply(QString reply, bool live,
    const QString &streamId, const QString &categoryId, const QString &name,
    const CategoryState &state, qint64 timerStart, qint64 now)
{
    const bool usesGame = reply.contains("{game}") || reply.contains("{game_time}") ||
        reply.contains("{game_since}") || reply.contains("{game_total}");
    const bool usesTimer = reply.contains("{timer}");
    if (!usesGame && !usesTimer) return validReply(reply) ? std::optional<QString>(reply) : std::nullopt;
    if (!live && (usesGame || usesTimer)) return {};
    const bool justChatting = categoryId == "509658" ||
        (categoryId.isEmpty() && name.trimmed().compare("Just Chatting", Qt::CaseInsensitive) == 0);
    if (usesGame && (justChatting || !validUserId(categoryId))) return {};
    if (usesTimer && (timerStart <= 0 || now < timerStart)) return {};
    // Replace the literal tokens in a single pass. Expansion never recursively
    // interprets category names as another token.
    const bool known = live && state.stream == streamId && state.category == categoryId &&
        state.since > 0 && now >= state.seen && now - state.seen <= 90;
    const auto unknown = uiText("unknown");
    const auto playing = known ? now - state.since : 0;
    const auto game = known && !state.name.isEmpty() ? state.name : unknown;
    const auto time = known && !state.uncertainSegment ? duration(playing) : unknown;
    const auto since = known && !state.uncertainSegment ? QDateTime::fromSecsSinceEpoch(state.since).toLocalTime().toString("HH:mm") : unknown;
    const auto total = known && !state.uncertainTotal ? duration(playing + state.completed.value(state.category)) : unknown;
    const std::array<std::pair<QStringView, QString>, 5> replacements{{
        {u"{game}", game}, {u"{game_time}", time}, {u"{game_since}", since},
        {u"{game_total}", total}, {u"{timer}", usesTimer ? duration(now - timerStart) : QString{}}}};
    QString out;
    for (qsizetype i = 0; i < reply.size();)
    {
        bool replaced = false;
        for (const auto &pair : replacements)
        {
            if (QStringView(reply).mid(i).startsWith(pair.first))
            {
                out += pair.second; i += pair.first.size(); replaced = true; break;
            }
        }
        if (!replaced) out += reply.at(i++);
    }
    if (!validReply(out)) return {};
    return out;
}
} // namespace chatterino::moderator
