// SPDX-License-Identifier: MIT
#pragma once

#include "OwnTimeoutChecks.hpp"
#include "PublicLogsChecks.hpp"
#include "HistoryGapChecks.hpp"
#include "HistoryIncidentReplay.hpp"
#include "ReconnectHistoryChecks.hpp"
#include "providers/recentmessages/Impl.hpp"

#include <QUrlQuery>

namespace chatterino::preview {

// Check both ordinary chat and historical system rows in the actual ChannelView.
inline int checkStartupHistory(const QString &report)
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
        return 40;
    const auto endpoint = QStringLiteral("http://127.0.0.1:%1/recent/").arg(server.serverPort()) + "%1";
    const auto backupEndpoint = QStringLiteral("http://127.0.0.1:%1/backup/").arg(server.serverPort()) + "%1";
    qputenv("CHATTERINO2_RECENT_MESSAGES_URL", endpoint.toUtf8());
    qputenv("CHATTERINO2_RECENT_MESSAGES_FALLBACK_URL", backupEndpoint.toUtf8());
    if (recentmessages::detail::recentMessagesApiUrlTemplate() != endpoint ||
        recentmessages::detail::fallbackRecentMessagesApiUrlTemplate() != backupEndpoint)
        return 41;  // Never fall through to a real provider.

    NetworkManager::init();
    getSettings()->displaySevenTVPaints = false;
    getSettings()->showBadgesSevenTV = false;
    getSettings()->enableSevenTVPersonalEmotes = false;
    getSettings()->showBadgesHomiesSupporter = false;
    getSettings()->showBadgesHomiesCustom = false;
    auto room = std::make_shared<TwitchChannel>("startup_history_fixture");
    ChannelView view(nullptr);
    view.setChannel(room);
    std::vector<QString> expected;
    QJsonArray rows;
    const auto day = QDate::currentDate();
    for (int minute : {21, 30, 38})
    {
        const auto time = QDateTime(day, QTime(21, minute)).toMSecsSinceEpoch();
        auto text = QStringLiteral("history-row-%1").arg(minute);
        expected.push_back(text);
        rows.append(QStringLiteral("@historical=1;tmi-sent-ts=%1 :tmi.twitch.tv NOTICE #startup_history_fixture :%2")
                        .arg(time).arg(text));
    }
    // Simulate a live row already received when the history response arrives.
    rows.append(QStringLiteral("@historical=1;id=healthy;tmi-sent-ts=%1;user-id=910000001;display-name=Fixture;room-id=910000002;badges=;emotes= :fixture!fixture@fixture.tmi.twitch.tv PRIVMSG #startup_history_fixture :fixture-healthy")
                    .arg(QDateTime::currentMSecsSinceEpoch() - 1000));
    const auto live = QStringLiteral("live-row-39");
    room->addSystemMessage(live);
    expected.push_back(live);

    int requests = 0;
    QUrl requestURL;
    int primaryRecoveries = 0;
    int backupRecoveries = 0;
    bool backupURL = true;
    bool delayFallback = false;
    QJsonObject primaryResponse, backupResponse;
    int primaryStatus = 200;
    std::function<bool(const QUrl &, QTcpSocket *)> extraResponder;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            if (!socket->canReadLine() || socket->property("responded").toBool())
                return;
            socket->setProperty("responded", true);
            const auto url = QUrl::fromEncoded(socket->readLine().split(' ').value(1));
            if (extraResponder && extraResponder(url, socket))
                return;
            const bool original = url.path() == "/recent/startup_history_fixture";
            const bool backup = url.path().startsWith("/backup/");
            if (original)
            {
                ++requests;
                requestURL = url;
            }
            else if (backup)
            {
                ++backupRecoveries;
                const QUrlQuery query(url);
                backupURL &= url.path() == "/backup/startup_history_recovery" &&
                             query.queryItemValue("limit") == "77" &&
                             query.queryItemValue("before").toLongLong() > 0;
            }
            else
            {
                ++primaryRecoveries;
            }
            const auto body = QJsonDocument(original ? QJsonObject{{"messages", rows}} :
                (backup ? backupResponse : primaryResponse)).toJson(QJsonDocument::Compact);
            const auto status = (original || backup) ? 200 : primaryStatus;
            const QByteArray response = (status == 200 ? QByteArray("HTTP/1.1 200 OK") :
                QByteArray("HTTP/1.1 503 Service Unavailable")) +
                "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            const auto write = [target = QPointer<QTcpSocket>(socket), response] {
                if (target)
                {
                    target->write(response);
                    target->disconnectFromHost();
                }
            };
            if (backup && delayFallback)
                QTimer::singleShot(250, &server, write);
            else
                write();
        });
    });
    int prepends = 0;
    pajlada::Signals::SignalHolder holder;
    holder.managedConnect(room->messagesAddedAtStart, [&](const auto &) { ++prepends; });
    getSettings()->loadTwitchMessageHistoryOnConnect = false;
    TwitchChannelTestAccess::loadStartupHistory(*room);
    const bool respectedDisabled = !room->isLoadingRecentMessages();
    getSettings()->loadTwitchMessageHistoryOnConnect = true;
    getSettings()->twitchMessageHistoryLimit = 77;
    TwitchChannelTestAccess::loadStartupHistory(*room);
    TwitchChannelTestAccess::loadStartupHistory(*room);
    const bool completed = publiclogchecks::waitUntil([&] { return !room->isLoadingRecentMessages(); }, 7000);
    std::vector<QString> displayed;
    for (const auto &layout : view.getMessagesSnapshot())
    {
        const auto &text = layout->getMessage()->messageText;
        if (text.startsWith("history-row-") || text == live)
            displayed.push_back(text);
    }
    const bool correctURL = requestURL.path() == "/recent/startup_history_fixture" &&
                            requestURL.query() == "limit=77";
    const bool normalUnchanged = primaryRecoveries == 0 && backupRecoveries == 0;
    QJsonObject checks{{"respectsDisabled", respectedDisabled}, {"completed", completed},
                       {"requests", requests}, {"expectedURL", correctURL}, {"prependSignals", prepends},
                       {"visibleRows", static_cast<int>(displayed.size())}, {"visibleOrder", displayed == expected}};

    constexpr auto login = "startup_history_recovery";
    const auto epoch = QDateTime::currentMSecsSinceEpoch() - 60'000;
    const auto raw = [&](QString id, int second, QString channel = "startup_history_recovery") {
        return QStringLiteral("@historical=1;id=%1;tmi-sent-ts=%2;user-id=910000001;display-name=Fixture;room-id=910000002;badges=;emotes= :fixture!fixture@fixture.tmi.twitch.tv PRIVMSG #%3 :fixture-%1")
            .arg(id).arg(epoch + second * 1000).arg(channel);
    };
    const auto partial = [](QJsonArray messages) {
        return QJsonObject{{"messages", messages}, {"error_code", "channel_not_joined"}};
    };
    const auto idsInView = [](ChannelView &view) {
        QStringList ids;
        for (const auto &layout : view.getMessagesSnapshot())
        {
            if (const auto &id = layout->getMessage()->id; !id.isEmpty())
                ids.append(id);
        }
        return ids;
    };
    const auto warnings = [](const Channel &channel) {
        int count = 0;
        const auto warning = uiText("Message history service recovering, there may be gaps in the message history.");
        for (const auto &message : channel.getMessageSnapshot())
            count += message->messageText == warning;
        return count;
    };

    bool recoveryPassed = true;
    auto runCase = [&](const QString &name, QJsonObject primary, QJsonObject backup,
                       int status, bool live, bool delayed, int warningCount,
                       const QStringList &expectedIds) {
        primaryResponse = std::move(primary);
        backupResponse = std::move(backup);
        primaryStatus = status;
        delayFallback = delayed;
        const int initialPrimary = primaryRecoveries, initialBackup = backupRecoveries;
        auto recovering = std::make_shared<TwitchChannel>(login);
        ChannelView recoveringView(nullptr);
        recoveringView.setChannel(recovering);
        recovering->addSystemMessage("connected fixture");
        if (live)
        {
            MessageBuilder builder;
            builder->id = "live";
            builder->serverReceivedTime = QDateTime::fromMSecsSinceEpoch(epoch + 5000);
            builder->messageText = "fixture-live";
            builder->searchText = builder->messageText;
            recovering->addMessage(builder.release(), MessageContext::Original);
        }
        TwitchChannelTestAccess::loadStartupHistory(*recovering);
        const bool immediate = !delayed || publiclogchecks::waitUntil([&] {
            return recovering->isLoadingRecentMessages() &&
                idsInView(recoveringView).contains("first") && backupRecoveries > initialBackup;
        }, 3000);
        const bool finished = publiclogchecks::waitUntil([&] {
            return !recovering->isLoadingRecentMessages();
        }, 7000);
        const auto shown = idsInView(recoveringView);
        const bool oneEach = primaryRecoveries == initialPrimary + 1 &&
                             backupRecoveries == initialBackup + 1;
        const bool orderedAndUnique = shown == expectedIds;
        const bool good = immediate && finished && oneEach && orderedAndUnique &&
                          warnings(*recovering) == warningCount;
        checks.insert(name, QJsonObject{{"ok", good}, {"immediate", immediate},
            {"finished", finished}, {"oneRequestEach", oneEach},
            {"orderedAndUnique", orderedAndUnique}, {"warningCount", warnings(*recovering)},
            {"visibleIds", QJsonArray::fromStringList(shown)}});
        recoveryPassed &= good;
    };
    runCase("overlap", partial({raw("first", 1), raw("third", 3)}),
            QJsonObject{{"messages", QJsonArray{raw("fourth", 4), raw("first", 1),
                raw("second", 2), raw("second", 2), raw("live", 5), raw("future", 5000),
                raw("wrong", 2, "other_channel"), raw("", 2)}}}, 200, true, true, 0,
            {"first", "second", "third", "fourth", "live"});
    runCase("emptyPrimary", partial({}), QJsonObject{{"messages", QJsonArray{raw("third", 3), raw("second", 2)}}},
            200, false, false, 0, {"second", "third"});
    runCase("emptyBackup", partial({raw("first", 1)}), QJsonObject{{"messages", QJsonArray{}}},
            200, false, false, 1, {"first"});
    runCase("bothPartial", partial({raw("first", 1)}), partial({raw("second", 2)}),
            200, false, false, 1, {"first", "second"});
    runCase("transportFailure", QJsonObject{}, QJsonObject{{"messages", QJsonArray{raw("second", 2)}}},
            503, false, false, 0, {"second"});
    checks.insert("normalNoFallback", normalUnchanged);
    checks.insert("backupURLBounded", backupURL);
    recoveryPassed &= checkHistoryGaps(checks, extraResponder, server.serverPort());
    recoveryPassed &= checkHistoryIncidentReplay(checks, extraResponder, server.serverPort());
    recoveryPassed &= checkReconnectHistory(checks, extraResponder);
    qunsetenv("CHATTERINO2_RECENT_MESSAGES_FALLBACK_URL");
    const bool isolatedCustom = recentmessages::detail::fallbackRecentMessagesApiUrlTemplate().isEmpty();
    checks.insert("customProviderNeverUsesProduction", isolatedCustom);
    QFile output(report);
    const bool saved = output.open(QIODevice::WriteOnly) &&
                       output.write(QJsonDocument(checks).toJson()) > 0;
    NetworkManager::deinit();
    return saved && respectedDisabled && completed && requests == 1 && correctURL &&
                   prepends == 1 && displayed == expected && normalUnchanged &&
                   backupURL && recoveryPassed && isolatedCustom ? 0 : 42;
}

}  // namespace chatterino::preview
