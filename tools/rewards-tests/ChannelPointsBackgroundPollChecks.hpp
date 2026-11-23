// SPDX-License-Identifier: MIT
#pragma once

#include "controllers/channelpoints/ChannelPointsBackgroundPoll.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QStringList>

#include <memory>
#include <vector>

namespace chatterino {

// Set only the isolated channel's identity and clock, without connecting to
// IRC, fetching profile metadata, or sending GQL requests.
class TwitchChannelTestAccess
{
public:
    static void setPointsRoom(TwitchChannel &channel, const QString &room)
    {
        *channel.roomID_.access() = room;
    }
    static void agePointsObservation(TwitchChannel &channel, const QDateTime &time)
    {
        channel.lastChannelPointsObservationAt_ = time;
    }
    static void setPointsInFlight(TwitchChannel &channel, bool inFlight)
    {
        channel.channelPointsFetchInFlight_.store(inFlight);
    }
};

namespace preview {

template <typename Check>
void checkChannelPointsBackgroundPoll(Check &&check)
{
    using channelpoints::backgroundRefreshDue;
    using channelpoints::pollNextChannel;
    const auto now = QDateTime::fromString(
        QStringLiteral("2026-11-23T12:00:00.000Z"), Qt::ISODateWithMs);
    const QDateTime unknown;
    const auto stale = now.addSecs(-300);
    const auto oldAttempt = now.addSecs(-120);
    check(channelpoints::BackgroundPollTickMs == 5000 &&
              backgroundRefreshDue(now, unknown, unknown, false),
          QStringLiteral("background poll: first tick may refresh an unobserved channel"));
    check(!backgroundRefreshDue(now, stale.addMSecs(1), oldAttempt, false) &&
              backgroundRefreshDue(now, stale, oldAttempt, false),
          QStringLiteral("background poll: exactly five minutes since observation becomes stale"));
    check(!backgroundRefreshDue(now, stale, oldAttempt.addMSecs(1), false) &&
              backgroundRefreshDue(now, stale, oldAttempt, false),
          QStringLiteral("background poll: failed attempts back off for exactly two minutes"));
    check(!backgroundRefreshDue(now, unknown, unknown, true) &&
              !backgroundRefreshDue(now, stale, oldAttempt, true),
          QStringLiteral("background poll: in-flight refresh prevents duplicate requests"));
    check(!backgroundRefreshDue(now, now, oldAttempt, false) &&
              !backgroundRefreshDue(now.addSecs(120), now, now, false) &&
              backgroundRefreshDue(now.addSecs(300), now, now, false),
          QStringLiteral("background poll: an unchanged confirmed balance renews freshness"));
    check(backgroundRefreshDue(now, now.addSecs(60), oldAttempt, false) &&
              backgroundRefreshDue(now, stale, now.addSecs(60), false) &&
              backgroundRefreshDue(unknown, stale, oldAttempt, false),
          QStringLiteral("background poll: invalid and backwards-clock timestamps recover"));

    struct Channel {
        QString key;
        bool eligible;
        bool visible;
    };
    std::vector<Channel> channels{
        {QStringLiteral("alpha"), true, true},
        {QStringLiteral("bravo"), false, false},
        {QStringLiteral("charlie"), true, false},
        {QStringLiteral("delta"), true, false}};
    QString cursor;
    QStringList visited;
    QStringList started;
    const auto key = [](const Channel &channel) {
        return channel.key;
    };
    const auto tryStart = [&](const Channel &channel) {
        visited.append(channel.key);
        if (!channel.eligible)
        {
            return false;
        }
        started.append(channel.key);
        return true;
    };
    const auto tick = [&] {
        visited.clear();
        started.clear();
        return pollNextChannel(channels, cursor, key, tryStart);
    };

    check(tick() && cursor == QStringLiteral("alpha") &&
              visited == QStringList{QStringLiteral("alpha")} &&
              started.size() == 1,
          QStringLiteral("background poll: one start on the first eligible channel"));
    check(tick() && cursor == QStringLiteral("charlie") &&
              visited == QStringList{QStringLiteral("bravo"), QStringLiteral("charlie")} &&
              started.size() == 1 && !channels[2].visible,
          QStringLiteral("background poll: skip an ineligible channel and refresh a hidden channel"));
    check(tick() && cursor == QStringLiteral("delta") && started.size() == 1 &&
              tick() && cursor == QStringLiteral("alpha") && started.size() == 1,
          QStringLiteral("background poll: rotating cursor fairly visits all eligible channels and wraps"));
    check(tick() && cursor == QStringLiteral("charlie"),
          QStringLiteral("background poll: next rotation visits the hidden channel again"));
    channels.erase(channels.begin() + 2);
    check(tick() && cursor == QStringLiteral("delta") && started.size() == 1,
          QStringLiteral("background poll: closing the last started tab does not skip its successor"));

    channels[0].eligible = false;
    channels[2].eligible = false;
    const auto lastStarted = cursor;
    check(!tick() && started.isEmpty() &&
              visited == QStringList{QStringLiteral("alpha"), QStringLiteral("bravo"),
                                     QStringLiteral("delta")} &&
              cursor == lastStarted,
          QStringLiteral("background poll: none due scans all once without a request or cursor move"));
    channels.clear();
    check(!tick() && visited.isEmpty() && started.isEmpty() &&
              cursor == lastStarted,
          QStringLiteral("background poll: empty or entirely closed tabs retain only the cursor"));

    struct TimedChannel {
        QString key;
        bool visible;
        QDateTime observed;
        QDateTime attempted;
        bool inFlight;
    };
    std::vector<TimedChannel> timed{
        {QStringLiteral("active"), true, now.addSecs(-10), oldAttempt, false},
        {QStringLiteral("hidden-due"), false, stale, oldAttempt, false},
        {QStringLiteral("hidden-inflight"), false, stale, oldAttempt, true},
        {QStringLiteral("hidden-retry"), false, stale, now.addSecs(-10), false}};
    QString timedCursor;
    QStringList requests;
    const auto timedKey = [](const TimedChannel &channel) {
        return channel.key;
    };
    const auto timedTick = [&](const QDateTime &tickTime) {
        requests.clear();
        return pollNextChannel(timed, timedCursor, timedKey,
                               [&](const TimedChannel &channel) {
            if (!backgroundRefreshDue(tickTime, channel.observed,
                                      channel.attempted, channel.inFlight))
            {
                return false;
            }
            requests.append(channel.key);
            return true;
        });
    };
    check(timedTick(now) &&
              requests == QStringList{QStringLiteral("hidden-due")} &&
              !timed[1].visible,
          QStringLiteral("background poll: stale hidden channels refresh independently of selected chat"));
    timed[1].attempted = now;
    check(!timedTick(now.addMSecs(channelpoints::BackgroundPollTickMs)) &&
              requests.isEmpty(),
          QStringLiteral("background poll: the next tick makes no requests while every channel is fresh, backing off or in-flight"));
}

template <typename Check>
void checkChannelPointsBackgroundChannel(Check &&check)
{
    const bool wasEnabled = getSettings()->enableChannelPointsDisplay;
    getSettings()->enableChannelPointsDisplay = true;
    auto channel = std::make_shared<TwitchChannel>(QStringLiteral("points_poll_fixture"));
    const auto now = QDateTime::currentDateTimeUtc();
    check(!channel->isChannelPointsBackgroundRefreshDue(now),
          QStringLiteral("background poll: actual unjoined Twitch channel is ineligible"));
    const auto room = QStringLiteral("points-poll-fixture-room");
    TwitchChannelTestAccess::setPointsRoom(*channel, room);
    check(channel->isChannelPointsBackgroundRefreshDue(now),
          QStringLiteral("background poll: actual hidden joined Twitch channel is due without a Split"));
    check(!channel->refreshChannelPointsInBackground({}, now) &&
              !channel->isChannelPointsFetchInFlight(),
          QStringLiteral("background poll: missing personal token sends no request"));
    MoltorinoAuthToken own;
    own.token = QStringLiteral("fixture-no-network");
    own.userId = room;
    check(!channel->refreshChannelPointsInBackground(own, now) &&
              !channel->isChannelPointsFetchInFlight(),
          QStringLiteral("background poll: broadcaster's own room sends no points request"));
    TwitchChannelTestAccess::setPointsInFlight(*channel, true);
    check(!channel->isChannelPointsBackgroundRefreshDue(now),
          QStringLiteral("background poll: real in-flight flag blocks another request"));
    TwitchChannelTestAccess::setPointsInFlight(*channel, false);
    const auto payload = QJsonObject{
        {"type", "points-earned"},
        {"data", QJsonObject{{"balance", QJsonObject{{"channel_id", room}, {"balance", 123}}}}}};
    channel->handleUserPointsUpdate(payload, QStringLiteral("points-poll-fixture-account"));
    check(channel->channelPointBalance() == 123 &&
              !channel->isChannelPointsBackgroundRefreshDue(QDateTime::currentDateTimeUtc()),
          QStringLiteral("background poll: real PubSub observation prevents needless GQL polling"));
    TwitchChannelTestAccess::agePointsObservation(*channel, now.addSecs(-400));
    check(channel->isChannelPointsBackgroundRefreshDue(QDateTime::currentDateTimeUtc()),
          QStringLiteral("background poll: confirmed balance becoming stale makes channel due"));
    channel->handleUserPointsUpdate(payload, QStringLiteral("points-poll-fixture-account"));
    check(!channel->isChannelPointsBackgroundRefreshDue(QDateTime::currentDateTimeUtc()),
          QStringLiteral("background poll: unchanged real PubSub balance renews observation timestamp"));
    TwitchChannelTestAccess::agePointsObservation(*channel, now.addSecs(-400));
    getSettings()->enableChannelPointsDisplay = false;
    auto other = own;
    other.userId = QStringLiteral("points-poll-fixture-other-account");
    channel->handleUserPointsUpdate(payload, QStringLiteral("points-poll-fixture-account"));
    check(channel->isChannelPointsBackgroundRefreshDue(QDateTime::currentDateTimeUtc()) &&
              !channel->refreshChannelPointsInBackground(other, now) &&
              !channel->isChannelPointsFetchInFlight(),
          QStringLiteral("background poll: explicitly disabled points ignore queued observations and requests"));
    getSettings()->enableChannelPointsDisplay = wasEnabled;
}

}  // namespace preview
}  // namespace chatterino
