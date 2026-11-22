// SPDX-License-Identifier: MIT
#pragma once

#include "OwnTimeoutChecks.hpp"
#include "PublicLogsChecks.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageThread.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"

#include <algorithm>

namespace chatterino::preview {

inline int checkHistoryBadges(const QString &report)
{
    QJsonArray checks;
    int failed = 0;
    auto check = [&](const QString &name, bool ok) {
        failed += !ok;
        checks.append(QJsonObject{{"name", name}, {"ok", ok}});
        std::fprintf(stderr, "history-badge check: %s %s\n",
                     ok ? "ok" : "FAIL", name.toUtf8().constData());
    };

    // This fixture uses the bundled backup for the initial global Twitch
    // badge. A hidden ChannelView observes actual replacements without
    // attempting to download images. Do not start any 7TV chatter lookups.
    getSettings()->displaySevenTVPaints = false;
    getSettings()->showBadgesSevenTV = false;
    getApp()->getTwitchBadges()->loadLocalBadges();
    const auto fallback = getApp()->getTwitchBadges()->badge("subscriber", "3");
    check("bundled subscriber fallback exists",
          fallback.has_value() &&
              !getApp()->getTwitchBadges()->badge("subscriber", "3009"));
    if (!fallback)
    {
        return 43;
    }

    auto room = std::make_shared<TwitchChannel>("history_badges_fixture");
    ChannelView view(nullptr);
    view.setChannel(room);
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::magenta);
    auto externalEmote = [&](const QString &name) {
        return std::make_shared<const Emote>(
            Emote{.name = EmoteName{name},
                  .images = ImageSet{Image::fromResourcePixmap(pixmap)},
                  .tooltip = Tooltip{name}});
    };
    const auto existingExternal = externalEmote("fixture:existing");
    const auto lateHomies = externalEmote("homies:fixture-custom");

    const auto makeMessage = [&](const QString &id, const QString &version,
                                 const QString &uid, int minute,
                                 bool historical, bool shared,
                                 bool existing) {
        MessageBuilder builder;
        builder->id = id;
        builder->loginName = "badge_fixture_user";
        builder->displayName = "Badge_Fixture_User";
        builder->userID = uid;
        builder->channelName = room->getName();
        builder->messageText = "Fixture " + id;
        builder->searchText = builder->displayName + ": " + builder->messageText;
        builder->serverReceivedTime =
            QDateTime(QDate::currentDate(), QTime(20, minute));
        builder->usernameColor = QColor("#a8bfde");
        builder->flags.set(MessageFlag::DoNotLog);
        builder->flags.set(MessageFlag::RecentMessage, historical);
        builder->flags.set(MessageFlag::SharedMessage, shared);
        builder->twitchBadges.emplace_back("subscriber", version);
        builder->twitchBadgeInfos.insert({"subscriber", "22"});
        builder.emplace<TimestampElement>(QTime(20, minute));
        if (version == "3")
        {
            auto *badge = builder.emplace<BadgeElement>(
                *fallback, MessageElementFlag::BadgeSubscription);
            badge->setTwitchBadge("subscriber", "3");
            badge->setTooltip((*fallback)->tooltip.string + " (22 months)");
        }
        if (existing)
        {
            builder.emplace<BadgeElement>(existingExternal,
                                           MessageElementFlag::BadgeBttv);
            builder->externalBadges.push_back(existingExternal->name.string);
        }
        builder.emplace<TextElement>(builder->displayName + ":",
                                     MessageElementFlag::Username)
            ->setLink({Link::UserInfo, builder->loginName});
        builder.emplace<TextElement>(builder->messageText,
                                     MessageElementFlag::Text);
        return builder.release();
    };

    const auto first = makeMessage("history-first", "3", "123", 10, true, false, true);
    const auto missing = makeMessage("history-missing", "3009", "123", 11, true, false, false);
    const auto shared = makeMessage("history-shared", "3", "123", 12, true, true, false);
    const auto anotherUser = makeMessage("other-user", "3", "456", 13, true, false, false);
    const auto live = makeMessage("live-row", "3", "123", 20, false, false, false);
    first->flags.set(MessageFlag::Disabled, MessageFlag::Highlighted,
                    MessageFlag::ShowInMentions);
    first->highlightColor = std::make_shared<QColor>("#803060");
    auto parent = std::make_shared<Message>();
    parent->id = "reply-root";
    first->replyParent = parent;
    first->replyThread = std::make_shared<MessageThread>(parent);
    first->replyThread->addToThread(MessagePtr{first});
    const auto stubSystem = std::make_shared<Message>();
    stubSystem->id = "system-row";
    stubSystem->flags.set(MessageFlag::System, MessageFlag::RecentMessage,
                          MessageFlag::DoNotLog);
    stubSystem->messageText = "system fixture";
    stubSystem->serverReceivedTime = QDateTime(QDate::currentDate(), QTime(20, 14));
    stubSystem->userID = "123";
    stubSystem->elements.push_back(std::make_unique<TextElement>(
        stubSystem->messageText, MessageElementFlag::Text));
    room->addMessage(live, MessageContext::Original);
    room->addMessagesAtStart({first, missing, shared, anotherUser, stubSystem});
    const auto mentions = getApp()->getTwitch()->getMentionsChannel();
    mentions->addMessage(first, MessageContext::Repost);

    int replacements = 0;
    pajlada::Signals::SignalHolder holder;
    holder.managedConnect(room->messageReplaced,
                          [&](auto, const auto &, const auto &) { ++replacements; });
    bool providerArrived = false;
    bool sawSharedGlobalOnly = false;
    bool sawChannelLookup = false;
    QSet<QString> requestedUsers;
    room->historyBadgeProviderForTesting =
        [&](const QString &id, TwitchChannel *channel) -> MessagePtr {
            sawChannelLookup |= channel == room.get();
            sawSharedGlobalOnly |= channel == nullptr;
            requestedUsers.insert(id);
            MessageBuilder builder;
            if (id == "123" && providerArrived)
            {
                builder.emplace<BadgeElement>(lateHomies,
                    MessageElementFlag::BadgeHomiesCustom);
                builder->externalBadges.push_back(lateHomies->name.string);
            }
            return builder.release();
        };
    const auto waitForRefresh = [&] {
        return publiclogchecks::waitUntil([&] {
            return !TwitchChannelTestAccess::historyBadgeRefreshPending(*room);
        }, 7000);
    };
    const auto row = [&](const QString &id) -> MessagePtr {
        return room->findMessageByID(id);
    };
    const auto twitchBadge = [](const MessagePtr &message,
                                const QString &version) -> const BadgeElement * {
        if (!message)
        {
            return nullptr;
        }
        for (const auto &element : message->elements)
        {
            if (auto *badge = dynamic_cast<const BadgeElement *>(element.get());
                badge && badge->twitchBadgeSlug() == std::optional<QString>("subscriber") &&
                badge->twitchBadgeVersion() == std::optional<QString>(version))
            {
                return badge;
            }
        }
        return nullptr;
    };
    const auto countExternal = [](const MessagePtr &message, const QString &name) {
        if (!message)
        {
            return 0;
        }
        return static_cast<int>(std::ranges::count_if(message->elements,
            [&](const auto &element) {
                auto *badge = dynamic_cast<const BadgeElement *>(element.get());
                return badge && badge->getEmote()->name.string == name;
            }));
    };
    const auto nonBadges = [](const MessagePtr &message) {
        QJsonArray result;
        if (message)
        {
            for (const auto &element : message->elements)
            {
                if (!dynamic_cast<const BadgeElement *>(element.get()))
                {
                    result.append(element->toJson());
                }
            }
        }
        return result;
    };
    const auto unchangedPayload = [&](const MessagePtr &before, const MessagePtr &after) {
        return after && before->id == after->id &&
               before->messageText == after->messageText &&
               before->searchText == after->searchText &&
               before->displayName == after->displayName &&
               before->loginName == after->loginName &&
               before->userID == after->userID &&
               before->channelName == after->channelName &&
               before->usernameColor == after->usernameColor &&
               before->serverReceivedTime == after->serverReceivedTime &&
               before->flags == after->flags &&
               before->highlightColor == after->highlightColor &&
               before->replyThread == after->replyThread &&
               before->replyParent == after->replyParent &&
               before->twitchBadges == after->twitchBadges &&
               before->twitchBadgeInfos == after->twitchBadgeInfos &&
               nonBadges(before) == nonBadges(after);
    };

    TwitchChannelTestAccess::noteLoadedHistoryForBadges(
        *room, {first, missing, shared, anotherUser, stubSystem});
    check("a pass with no new badge data leaves original rows intact",
          waitForRefresh() && replacements == 0 &&
              row(first->id) == first && row(missing->id) == missing);
    check("the existing subscription is a real fallback and unknown tier is absent",
          twitchBadge(first, "3") &&
              twitchBadge(first, "3")->getEmote() == *fallback &&
              !twitchBadge(missing, "3009"));

    const auto version = [](const QString &id) {
        // Never touched by the hidden view. Loopback also prevents this
        // synthetic template from ever going to a production host.
        const QString base = "http://127.0.0.1:9/fixture/" + id + "/";
        return QJsonObject{{"id", id}, {"title", "Fixture channel subscriber"},
                           {"image_url_1x", base + "1"},
                           {"image_url_2x", base + "2"},
                           {"image_url_4x", base + "3"}};
    };
    const auto channelBadges = HelixChannelBadges(QJsonObject{
        {"data", QJsonArray{QJsonObject{{"set_id", "subscriber"},
             {"versions", QJsonArray{version("3"), version("3009")}}}}}});
    room->addTwitchBadgeSets(channelBadges);
    check("channel template arrival schedules and completes repair", waitForRefresh());
    const auto fixedFirst = row(first->id);
    const auto fixedMissing = row(missing->id);
    const auto fixedOtherUser = row(anotherUser->id);
    const auto *fixedNormalBadge = twitchBadge(fixedFirst, "3");
    const auto *fixedTierBadge = twitchBadge(fixedMissing, "3009");
    check("historical fallback changes to the channel subscriber art",
          fixedFirst != first && fixedNormalBadge &&
              fixedNormalBadge->getEmote() == room->twitchBadge("subscriber", "3").value());
    check("original month information retained in badge tooltip",
          fixedNormalBadge && fixedNormalBadge->getTooltip().contains("22 months"));
    check("missing subscription appeared with correct higher tier",
          fixedMissing != missing && fixedTierBadge &&
              fixedTierBadge->getEmote() == room->twitchBadge("subscriber", "3009").value() &&
              fixedTierBadge->getTooltip().contains("Tier 3, 22 months"));
    check("historical repair uses channel templates for other users too",
          fixedOtherUser != anotherUser && twitchBadge(fixedOtherUser, "3") &&
              twitchBadge(fixedOtherUser, "3")->getEmote() ==
                  room->twitchBadge("subscriber", "3").value());
    check("a shared-source row never takes a native badge from the receiving channel",
          row(shared->id) == shared);
    check("live and system rows remain untouched",
          row(live->id) == live && row(stubSystem->id) == stubSystem);

    providerArrived = true;
    room->scheduleHistoryBadgeRefresh();
    check("late external provider completion triggers another correction", waitForRefresh());
    const auto repaired = row(first->id);
    check("late Homies appears alongside the existing external badge exactly once",
          countExternal(repaired, lateHomies->name.string) == 1 &&
          countExternal(repaired, existingExternal->name.string) == 1 &&
          repaired->externalBadges.count(lateHomies->name.string) == 1 &&
          repaired->externalBadges.count(existingExternal->name.string) == 1);
    check("same UID receives late badge in other historical rows",
          countExternal(row(missing->id), lateHomies->name.string) == 1 &&
          countExternal(row(shared->id), lateHomies->name.string) == 1);
    check("provider lookups are scoped to the original user and source context",
          sawSharedGlobalOnly && sawChannelLookup &&
          countExternal(row(anotherUser->id), lateHomies->name.string) == 0 &&
          countExternal(row(live->id), lateHomies->name.string) == 0 &&
          twitchBadge(row(shared->id), "3") &&
          twitchBadge(row(shared->id), "3")->getEmote() == *fallback);
    check("history retains text, nickname case, metadata, timestamps and disabled flag",
          unchangedPayload(first, repaired) && unchangedPayload(missing, row(missing->id)) &&
              unchangedPayload(anotherUser, row(anotherUser->id)) &&
              unchangedPayload(shared, row(shared->id)));

    auto current = room->getMessageSnapshot();
    QStringList ids;
    for (const auto &message : current)
    {
        ids.push_back(message->id);
    }
    check("no duplicate or reordered chat messages",
          ids == QStringList{"history-first", "history-missing", "history-shared",
                             "other-user", "system-row", "live-row"});
    QStringList renderedOrder;
    bool renderedSame = true;
    for (const auto &layout : view.getMessagesSnapshot())
    {
        const auto &message = layout->getMessagePtr();
        if (ids.contains(message->id))
        {
            renderedOrder.push_back(message->id);
            renderedSame &= row(message->id) == message;
        }
    }
    check("real ChannelView replaced each original layout with corrected message",
          renderedSame && renderedOrder == ids);
    check("mentions and reply-thread references point to the repaired row",
          mentions->findMessageByID(first->id) == repaired &&
              first->replyThread->replies().size() == 1 &&
              first->replyThread->replies().front().lock() == repaired);

    // A reconnect batch can be built before a provider finishes. The regular
    // post-insertion callback must repair such a row, even after the previous
    // provider event was processed.
    const auto reconnect =
        makeMessage("reconnect-history", "3", "123", 15, true, false, false);
    room->fillInMissingMessages({reconnect});
    TwitchChannelTestAccess::noteLoadedHistoryForBadges(*room, {reconnect});
    check("reconnect batch built with stale badge is repaired after insertion",
          waitForRefresh() && row(reconnect->id) != reconnect &&
              twitchBadge(row(reconnect->id), "3") &&
              twitchBadge(row(reconnect->id), "3")->getEmote() ==
                  room->twitchBadge("subscriber", "3").value() &&
              countExternal(row(reconnect->id), lateHomies->name.string) == 1);

    const auto countBeforeReplay = replacements;
    room->addTwitchBadgeSets(channelBadges);
    room->scheduleHistoryBadgeRefresh();
    check("repeated identical Twitch and external data do not clone rows again",
          waitForRefresh() && replacements == countBeforeReplay);
    requestedUsers.clear();
    room->scheduleHistoryBadgeRefresh({"456"});
    check("per-user update does not query other historical chatters",
          waitForRefresh() && requestedUsers == QSet<QString>{"456"} &&
              replacements == countBeforeReplay);
    providerArrived = false;
    room->scheduleHistoryBadgeRefresh();
    check("temporarily missing provider data does not remove existing badges",
          waitForRefresh() && replacements == countBeforeReplay && row(first->id) == repaired);

    QFile output(report);
    const auto saved = output.open(QIODevice::WriteOnly) &&
        output.write(QJsonDocument(QJsonObject{{"checks", checks},
                                               {"failed", failed},
                                               {"passed", checks.size() - failed}}).toJson()) > 0;
    return saved && failed == 0 ? 0 : 44;
}

}  // namespace chatterino::preview
