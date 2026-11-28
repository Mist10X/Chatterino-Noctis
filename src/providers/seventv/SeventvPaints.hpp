#pragma once

#include "providers/seventv/paints/Paint.hpp"
#include "providers/seventv/SeventvPaintLookupQueue.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}  // namespace seventv::eventapi

/// 7TV paints and who wears them.
///
/// Both are also kept on disk (Misc/seventv-paints.json), so names are
/// painted right after a start instead of only once 7TV sends each user's
/// entitlement again. 7TV's live updates still win; an assignment that 7TV
/// hasn't confirmed for two weeks is dropped.
///
/// Live events take precedence over a lookup already in flight. Active chatters
/// have a shorter refresh interval than history, within the same global batch
/// and request limit.
///
/// The same lookup brings the chatter's 7TV badge; badges learnt this way are
/// kept on disk too and are in the badge registry before the first message is
/// built, so a known badge (your own included) shows right after a start.
class SeventvPaints : public QObject
{
public:
    SeventvPaints();
    ~SeventvPaints() override;

    void addPaint(const QJsonObject &paintJson);
    void assignPaintToUsers(const QString &paintID,
                            std::span<const seventv::eventapi::User> users,
                            bool fromLookup = false);
    void clearPaintFromUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users,
                             bool fromLookup = false);

    std::shared_ptr<Paint> getPaint(const QString &userName, bool kick) const;

    /// A Twitch chatter was seen; look their paint up if it isn't known.
    void noteTwitchChatter(const QString &login, const QString &userID,
                          bool live = true);
    void noteTwitchMention(const QString &login);

    /// Reads what was saved on disk (once). Message builders call it before
    /// asking for 7TV badges.
    void ensureLoaded() const;

private:
    void load();
    /// Your own paint, again: a change on the 7TV site shows within a minute.
    void refreshOwn();
    void refreshActive();
    void queueLookup(const QString &login, const QString &userID,
                     bool live = true);
    void applyBadge(const QString &userID, const QJsonObject &badge);
    void save();
    void queueSave();
    void armLookup();
    void lookUpChatters();
    void resolveMentionLogins();
    void applyLookup(const std::vector<seventv::PaintLookupQueue::Entry> &batch,
                     const QJsonObject &data, bool partial);

    std::unique_ptr<QTimer> ownTimer_;
    std::mutex lookupMutex_;
    seventv::PaintLookupQueue lookupQueue_;
    // Recently visible live chatters only. History never enters this table.
    std::unordered_map<QString, std::pair<QString, qint64>> activeChatters_;
    bool lookupArmed_ = false;
    int lookupDelayMs_ = 1000;

    mutable std::once_flag loaded_;
    QString file_;
    std::atomic<bool> dirty_{false};
    std::atomic<bool> saveQueued_{false};

    /// Raw 7TV paint definitions, as they are written to disk.
    std::unordered_map<QString, QJsonObject> paintJson_;
    /// When 7TV last confirmed a user's paint (seconds since epoch).
    std::unordered_map<QString, qint64> twitchConfirmed_;
    std::unordered_map<QString, qint64> kickConfirmed_;
    /// 7TV badges learnt by lookups: definitions, and Twitch user ID => (badge
    /// ID, when 7TV confirmed it).
    std::unordered_map<QString, QJsonObject> badgeJson_;
    std::unordered_map<QString, std::pair<QString, qint64>> twitchBadges_;
    /// Login => when 7TV was last asked about this chatter.
    std::unordered_map<QString, qint64> lookupTimes_;
    // Only the current network batch, guarded by mutex_. A live assignment or
    // deletion invalidates a response started before that event.
    seventv::PaintLookupEvents paintLookupEvents_;

    mutable std::shared_mutex mutex_;

    std::unordered_map<QString, std::shared_ptr<Paint>> kickPaintMap_;

    std::unordered_map<QString, std::shared_ptr<Paint>> twitchPaintMap_;

    std::unordered_map<QString, std::shared_ptr<Paint>> knownPaints_;
};

}  // namespace chatterino
