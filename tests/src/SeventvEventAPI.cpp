// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPaintLookupQueue.hpp"

#include "mocks/BaseApplication.hpp"
#include "messages/Emote.hpp"
#include "providers/liveupdates/Diag.hpp"
#include "providers/seventv/eventapi/Client.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/eventapi/Message.hpp"
#include "Test.hpp"

#include <QString>
#include <QtCore/qtestsupport_core.h>

#include <optional>

using namespace chatterino;
using namespace chatterino::seventv::eventapi;
using namespace std::chrono_literals;

TEST(SeventvEventAPI, CachedChannelIdentityRestoresExactSubscription)
{
    const QJsonObject json{
        {"user", QJsonObject{{"id","seven-user"},{"connections",QJsonArray{
            QJsonObject{{"platform","KICK"},{"id","888"}},
            QJsonObject{{"platform","TWITCH"},{"id","123"}},
            QJsonObject{{"platform","TWITCH"},{"id","456"}}}}}},
        {"emote_set", QJsonObject{{"id","seven-set"},{"emotes",QJsonArray{}}}}
    };
    const auto info = SeventvEmotes::twitchChannelInfo(json, "456");
    EXPECT_EQ(info.userID, QStringLiteral("seven-user"));
    EXPECT_EQ(info.emoteSetID, QStringLiteral("seven-set"));
    EXPECT_EQ(info.twitchConnectionIndex, 2u);
}

TEST(SeventvEventAPI, IncompleteLiveUpdateRequestsSnapshot)
{
    SeventvEventAPI api(QStringLiteral("wss://127.0.0.1:9050"));
    Client client(api, 25s);
    QString requested;
    int adds = 0;
    auto refresh = api.signals_.emoteSetNeedsRefresh.connect([&](const QString &id) {
        requested = id;
    });
    auto add = api.signals_.emoteAdded.connect([&](const auto &) { ++adds; });
    client.onMessage(R"({"op":0,"d":{"type":"emote_set.update","body":{"id":"set-that-changed","pushed":[{"key":"emotes","value":{"id":"new-id","name":"hero"}}]}}})");
    EXPECT_EQ(requested, QStringLiteral("set-that-changed"));
    EXPECT_EQ(adds, 0);
}

TEST(SeventvEventAPI, SnapshotDetectsOnlyActualMapChanges)
{
    auto emote = [](const QString &name, const QString &id) {
        auto value = std::make_shared<Emote>();
        value->name = EmoteName{name}; value->id = EmoteId{id};
        return value;
    };
    EmoteMap previous, current;
    previous[EmoteName{"old"}] = emote("old","1");
    previous[EmoteName{"unchanged"}] = emote("unchanged","2");
    current[EmoteName{"hero"}] = emote("hero","3");
    current[EmoteName{"unchanged"}] = emote("unchanged","2");
    const auto changes = seventv::detail::diffEmotes(previous,current);
    ASSERT_EQ(changes.added.size(),1u);
    ASSERT_EQ(changes.removed.size(),1u);
    EXPECT_EQ(changes.added.front()->name.string,QStringLiteral("hero"));
    EXPECT_EQ(changes.removed.front()->name.string,QStringLiteral("old"));
    const auto none = seventv::detail::diffEmotes(current,current);
    EXPECT_TRUE(none.added.empty());
    EXPECT_TRUE(none.removed.empty());
}

TEST(SeventvPaintLookup, LivePromotesHistoryAndOwnComesFirst)
{
    using namespace chatterino::seventv;
    PaintLookupQueue queue;
    for (int i = 0; i < 30; ++i)
    {
        ASSERT_TRUE(queue.enqueue(QString::number(i), QString::number(i),
                                  PaintLookupPriority::History));
    }
    ASSERT_TRUE(queue.enqueue("29", "29", PaintLookupPriority::Live));
    ASSERT_FALSE(queue.enqueue("29", "29", PaintLookupPriority::Live));
    ASSERT_TRUE(queue.enqueue("own", "100", PaintLookupPriority::Own));
    auto batch = queue.takeBatch();
    ASSERT_EQ(batch.size(), PaintLookupQueue::batchSize);
    EXPECT_EQ(batch[0].login, "own");
    EXPECT_EQ(batch[1].login, "29");
    EXPECT_FALSE(queue.enqueue("29", "29", PaintLookupPriority::Live));
    EXPECT_FALSE(queue.enqueue("own", "100", PaintLookupPriority::Own));
    queue.finish(batch);
    EXPECT_TRUE(queue.enqueue("29", "29", PaintLookupPriority::Live));
}

TEST(SeventvPaintLookup, FullHistoryDoesNotBlockLive)
{
    using namespace chatterino::seventv;
    PaintLookupQueue queue;
    for (size_t i = 0; i < PaintLookupQueue::capacity; ++i)
    {
        const auto login = QString::number(i);
        ASSERT_TRUE(queue.enqueue(login, login, PaintLookupPriority::History));
    }
    ASSERT_FALSE(queue.enqueue("overflow", "9", PaintLookupPriority::History));
    ASSERT_TRUE(queue.enqueue("live", "999", PaintLookupPriority::Live));
    EXPECT_FALSE(queue.contains(QString::number(PaintLookupQueue::capacity - 1)));
    ASSERT_TRUE(queue.enqueue("owner", "9999", PaintLookupPriority::Own));
    auto batch = queue.takeBatch();
    ASSERT_EQ(batch.size(), PaintLookupQueue::batchSize);
    EXPECT_EQ(batch[0].login, "owner");
    EXPECT_EQ(batch[1].login, "live");
}

TEST(SeventvPaintLookup, HistoryStillProgressesDuringLiveTraffic)
{
    using namespace chatterino::seventv;
    PaintLookupQueue queue;
    for (size_t i = 0; i < 30; ++i)
    {
        const auto id = QString::number(i);
        ASSERT_TRUE(queue.enqueue("h" + id, id, PaintLookupPriority::History));
        ASSERT_TRUE(queue.enqueue("l" + id, id, PaintLookupPriority::Live));
    }
    for (size_t i = 0; i < 3; ++i)
    {
        const auto batch = queue.takeBatch();
        ASSERT_EQ(batch.size(), PaintLookupQueue::batchSize);
        EXPECT_EQ(batch.at(0).priority, PaintLookupPriority::Live);
        EXPECT_EQ(batch.at(8).priority, PaintLookupPriority::History);
        EXPECT_EQ(batch.at(9).priority, PaintLookupPriority::History);
        queue.finish(batch);
    }
}

TEST(SeventvPaintLookup, FreshnessDependsOnActivityAndPaint)
{
    using namespace chatterino::seventv;
    EXPECT_EQ(PaintLookupQueue::refreshAfter(PaintLookupPriority::Own, true), 20);
    EXPECT_EQ(PaintLookupQueue::refreshAfter(PaintLookupPriority::Live, true), 30);
    EXPECT_EQ(PaintLookupQueue::refreshAfter(PaintLookupPriority::Live, false), 120);
    EXPECT_EQ(PaintLookupQueue::refreshAfter(PaintLookupPriority::History, true), 300);
    EXPECT_EQ(PaintLookupQueue::refreshAfter(PaintLookupPriority::History, false), 300);
}

TEST(SeventvPaintLookup, NewLiveEventWinsOverDelayedReply)
{
    using namespace chatterino::seventv;
    PaintLookupQueue queue;
    queue.enqueue("wearer", "42", PaintLookupPriority::Live);
    queue.enqueue("other", "43", PaintLookupPriority::Live);
    const auto batch = queue.takeBatch();
    PaintLookupEvents events;
    events.begin(batch);
    ASSERT_TRUE(events.accept("wearer", false)); // actual new assignment/deletion
    EXPECT_FALSE(events.accept("wearer", true)); // old response must not undo it
    EXPECT_TRUE(events.accept("other", true));
    EXPECT_TRUE(events.accept("wearer", false)); // another newer event
    events.finish();
    events.begin(batch);
    EXPECT_TRUE(events.accept("wearer", true)); // subsequent fresh lookup
}

const QString EMOTE_SET_A = "60b39e943e203cc169dfc106";
const QString EMOTE_SET_B = "60bca831e7ecd2f892c9b9ab";
const QString TARGET_USER_ID = "60b39e943e203cc169dfc106";

TEST(SeventvEventAPI, AllEvents)
{
    mock::BaseApplication app;
    const QString host("wss://127.0.0.1:9050/liveupdates/seventv/all-events");
    SeventvEventAPI eventAPI(host, std::chrono::milliseconds(1000));

    std::optional<EmoteAddDispatch> addDispatch;
    std::optional<EmoteUpdateDispatch> updateDispatch;
    std::optional<EmoteRemoveDispatch> removeDispatch;
    std::optional<UserConnectionUpdateDispatch> userDispatch;

    std::ignore = eventAPI.signals_.emoteAdded.connect([&](const auto &d) {
        addDispatch = d;
    });
    std::ignore = eventAPI.signals_.emoteUpdated.connect([&](const auto &d) {
        updateDispatch = d;
    });
    std::ignore = eventAPI.signals_.emoteRemoved.connect([&](const auto &d) {
        removeDispatch = d;
    });
    std::ignore = eventAPI.signals_.userUpdated.connect([&](const auto &d) {
        userDispatch = d;
    });

    eventAPI.subscribeUser("", EMOTE_SET_A);
    QTest::qWait(500);

    ASSERT_EQ(eventAPI.diag().connectionsOpened, 1);
    ASSERT_EQ(eventAPI.diag().connectionsClosed, 0);
    ASSERT_EQ(eventAPI.diag().connectionsFailed, 0);

    auto add = *addDispatch;
    ASSERT_EQ(add.emoteSetID, EMOTE_SET_A);
    ASSERT_EQ(add.actorName, QString("nerixyz"));
    ASSERT_EQ(add.emoteID, QString("621d13967cc2d4e1953838ed"));

    auto upd = *updateDispatch;
    ASSERT_EQ(upd.emoteSetID, EMOTE_SET_A);
    ASSERT_EQ(upd.actorName, QString("nerixyz"));
    ASSERT_EQ(upd.emoteID, QString("621d13967cc2d4e1953838ed"));
    ASSERT_EQ(upd.oldEmoteName, QString("Chatterinoge"));
    ASSERT_EQ(upd.emoteName, QString("Chatterino"));

    auto rem = *removeDispatch;
    ASSERT_EQ(rem.emoteSetID, EMOTE_SET_A);
    ASSERT_EQ(rem.actorName, QString("nerixyz"));
    ASSERT_EQ(rem.emoteName, QString("Chatterino"));
    ASSERT_EQ(rem.emoteID, QString("621d13967cc2d4e1953838ed"));

    ASSERT_EQ(userDispatch.has_value(), false);
    addDispatch = std::nullopt;
    updateDispatch = std::nullopt;
    removeDispatch = std::nullopt;

    eventAPI.subscribeUser(TARGET_USER_ID, "");
    QTest::qWait(50);

    ASSERT_EQ(addDispatch.has_value(), false);
    ASSERT_EQ(updateDispatch.has_value(), false);
    ASSERT_EQ(removeDispatch.has_value(), false);

    auto user = *userDispatch;
    ASSERT_EQ(user.userID, TARGET_USER_ID);
    ASSERT_EQ(user.actorName, QString("nerixyz"));
    ASSERT_EQ(user.oldEmoteSetID, EMOTE_SET_A);
    ASSERT_EQ(user.emoteSetID, EMOTE_SET_B);
    ASSERT_EQ(user.connectionIndex, 0);

    eventAPI.stop();
    // after exactly one event loop iteration, we should see updated counters
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    ASSERT_EQ(eventAPI.diag().connectionsOpened, 1);
    ASSERT_EQ(eventAPI.diag().connectionsClosed, 1);
    ASSERT_EQ(eventAPI.diag().connectionsFailed, 0);
}

TEST(SeventvEventAPI, NoHeartbeat)
{
    mock::BaseApplication app;
    const QString host("wss://127.0.0.1:9050/liveupdates/seventv/no-heartbeat");
    SeventvEventAPI eventApi(host, std::chrono::milliseconds(1000));

    eventApi.subscribeUser("", EMOTE_SET_A);
    QTest::qWait(1250);
    ASSERT_EQ(eventApi.diag().connectionsOpened, 2);
    ASSERT_EQ(eventApi.diag().connectionsClosed, 1);
    ASSERT_EQ(eventApi.diag().connectionsFailed, 0);

    eventApi.stop();
}
