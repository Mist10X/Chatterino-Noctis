// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/RatelimitBucket.hpp"
#include "providers/twitch/TwitchReadJoinLimiter.hpp"

#include "Test.hpp"

#include <QApplication>
#include <QDebug>
#include <QtConcurrent>

#include <chrono>
#include <thread>

using namespace chatterino;

TEST(RatelimitBucket, BatchTwoParts)
{
    const int cooldown = 100;
    int n = 0;
    auto cb = [&n](QString msg) {
        qDebug() << msg;
        ++n;
    };
    auto bucket = std::make_unique<RatelimitBucket>(5, cooldown, cb, nullptr);
    bucket->send("1");
    EXPECT_EQ(n, 1);

    bucket->send("2");
    EXPECT_EQ(n, 2);

    bucket->send("3");
    EXPECT_EQ(n, 3);

    bucket->send("4");
    EXPECT_EQ(n, 4);

    bucket->send("5");
    EXPECT_EQ(n, 5);

    bucket->send("6");
    // Rate limit reached, n will not have changed yet. If we wait for the cooldown to run, n should have changed
    EXPECT_EQ(n, 5);

    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds{cooldown});
    QCoreApplication::processEvents();

    EXPECT_EQ(n, 6);
}

TEST(TwitchReadJoinLimiter, NewAccountDoesNotWaitForPreviousAccount)
{
    using namespace std::chrono_literals;
    TwitchReadJoinLimiter limiter(18, 12500ms);
    const TwitchReadJoinLimiter::TimePoint start{};
    limiter.bind("uid:a");
    for (int i = 0; i < 18; ++i)
    {
        const auto name = QString::number(i);
        limiter.enqueue(name);
        EXPECT_EQ(limiter.takeNext(start), name);
    }
    limiter.enqueue("stale-a");
    EXPECT_EQ(limiter.nextDelay(start + 2s), 10500ms);
    EXPECT_FALSE(limiter.takeNext(start + 2s));

    limiter.bind("uid:b");
    EXPECT_FALSE(limiter.belongsTo("uid:a"));
    EXPECT_TRUE(limiter.belongsTo("uid:b"));
    EXPECT_FALSE(limiter.nextDelay(start + 2s));
    for (int i = 0; i < 18; ++i)
    {
        limiter.enqueue(QString::number(i));
        EXPECT_EQ(limiter.takeNext(start + 2s), QString::number(i));
    }
    limiter.enqueue("stale-b");
    EXPECT_FALSE(limiter.takeNext(start + 2s));

    // Rapidly switching back cannot reset Twitch's genuine per-user limit.
    limiter.bind("uid:a");
    limiter.enqueue("visible");
    limiter.enqueue("background");
    EXPECT_FALSE(limiter.takeNext(start + 3s));
    EXPECT_EQ(limiter.nextDelay(start + 3s), 9500ms);
    EXPECT_EQ(limiter.takeNext(start + 12500ms), QStringLiteral("visible"));
    EXPECT_EQ(limiter.takeNext(start + 12500ms), QStringLiteral("background"));
    EXPECT_FALSE(limiter.takeNext(start + 12500ms));
}

TEST(TwitchReadJoinLimiter, ReconnectClearsQueueWithoutResettingSpentBudget)
{
    using namespace std::chrono_literals;
    TwitchReadJoinLimiter limiter(2, 12500ms);
    const TwitchReadJoinLimiter::TimePoint start{};
    limiter.bind("uid:a");
    limiter.enqueue("first");
    EXPECT_EQ(limiter.takeNext(start), QStringLiteral("first"));
    limiter.enqueue("second");
    EXPECT_EQ(limiter.takeNext(start + 100ms), QStringLiteral("second"));
    limiter.enqueue("old pending channel");
    limiter.clearPending();
    limiter.bind("uid:a");
    EXPECT_FALSE(limiter.nextDelay(start + 1s));
    limiter.enqueue("visible");
    limiter.enqueue("visible"); // repeated JOIN request is coalesced
    limiter.enqueue("closed");
    limiter.remove("closed");
    limiter.enqueue("next");
    EXPECT_FALSE(limiter.takeNext(start + 12499ms));
    EXPECT_EQ(limiter.nextDelay(start + 12499ms), 1ms);
    EXPECT_EQ(limiter.takeNext(start + 12500ms), QStringLiteral("visible"));
    EXPECT_FALSE(limiter.takeNext(start + 12500ms));
    EXPECT_EQ(limiter.nextDelay(start + 12500ms), 100ms);
    EXPECT_EQ(limiter.takeNext(start + 12600ms), QStringLiteral("next"));
    EXPECT_FALSE(limiter.nextDelay(start + 12600ms));
}
