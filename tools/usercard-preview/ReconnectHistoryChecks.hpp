// SPDX-License-Identifier: MIT
#pragma once

#include "OwnTimeoutChecks.hpp"
#include "PublicLogsChecks.hpp"

#include <QScopeGuard>

namespace chatterino::preview {

inline bool checkReconnectHistory(
    QJsonObject &checks,
    std::function<bool(const QUrl &, QTcpSocket *)> &extraResponder)
{
    const auto login = QStringLiteral("reconnect_history_fixture");
    const auto savedLimit = getSettings()->twitchMessageHistoryLimit.getValue();
    auto restore = qScopeGuard([&] {
        extraResponder = {};
        getSettings()->twitchMessageHistoryLimit = savedLimit;
        getSettings()->loadTwitchMessageHistoryOnConnect = true;
    });
    constexpr int limit = 120;
    constexpr int burst = 80;
    getSettings()->twitchMessageHistoryLimit = limit;
    const auto write = [](QTcpSocket *socket, bool success,
                          const QJsonArray &rows) {
        const auto body = QJsonDocument(QJsonObject{{"messages", rows}})
                              .toJson(QJsonDocument::Compact);
        const QByteArray head = success ? "200 OK" : "400 Bad Request";
        socket->write("HTTP/1.1 " + head + "\r\nConnection: close\r\n"
                      "Content-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    };
    bool allPassed = true;
    for (const auto &scenario : {QStringLiteral("startupSuccess"),
                                 QStringLiteral("startupFailure"),
                                 QStringLiteral("disabledDuringLoad")})
    {
        auto room = std::make_shared<TwitchChannel>(login);
        ChannelView view(nullptr);
        view.setChannel(room);
        getSettings()->loadTwitchMessageHistoryOnConnect = true;
        const auto firstHeartbeat = QDateTime::currentMSecsSinceEpoch() - 2200;
        const auto raw = [&](const QString &id, qint64 at) {
            return QStringLiteral("@historical=1;id=%1;tmi-sent-ts=%2;"
                                  "user-id=910000001;display-name=Fixture;"
                                  "room-id=910000002;badges=;emotes= "
                                  ":fixture!fixture@fixture.tmi.twitch.tv "
                                  "PRIVMSG #%3 :row-%1").arg(id).arg(at).arg(login);
        };
        QPointer<QTcpSocket> startup;
        std::vector<QUrlQuery> queries;
        int unexpected = 0;
        extraResponder = [&](const QUrl &url, QTcpSocket *socket) {
            if (!url.path().endsWith("/" + login))
                return false;
            if (!url.path().startsWith("/recent/"))
            {
                ++unexpected;
                write(socket, true, {});
                return true;
            }
            queries.emplace_back(url);
            if (queries.size() == 1)
            {
                startup = socket;  // Hold the response across two joins.
                return true;
            }
            QJsonArray rows;
            const int requested = queries.back().queryItemValue("limit").toInt();
            for (int index = std::max(0, burst - requested); index < burst; ++index)
                rows.append(raw(QStringLiteral("burst-%1").arg(index),
                                firstHeartbeat + 100 + index * 10));
            write(socket, true, rows);
            return true;
        };
        TwitchChannelTestAccess::loadStartupHistory(*room);
        const bool started = publiclogchecks::waitUntil([&] { return !startup.isNull(); }, 5000);
        qint64 lastJoin = 0;
        const bool disabled = scenario == "disabledDuringLoad";
        if (started)
        {
            const auto firstJoin = TwitchChannelTestAccess::rejoinAfterHeartbeat(
                *room, firstHeartbeat);
            publiclogchecks::waitUntil(
                [&] { return QDateTime::currentMSecsSinceEpoch() > firstJoin; }, 1000);
            lastJoin = TwitchChannelTestAccess::rejoinAfterHeartbeat(*room, firstJoin);
            // A subsequent heartbeat changes the live cursor. The saved
            // request must still use the original disconnection interval.
            room->markConnected();
            if (disabled)
                getSettings()->loadTwitchMessageHistoryOnConnect = false;
            write(startup, scenario != "startupFailure",
                  {raw("startup", firstHeartbeat - 100)});
        }
        const bool finished = publiclogchecks::waitUntil(
            [&] { return !room->isLoadingRecentMessages(); }, 5000);
        const bool bounded = queries.size() == 2 &&
            queries.back().queryItemValue("after").toLongLong() == firstHeartbeat &&
            queries.back().queryItemValue("before").toLongLong() == lastJoin &&
            queries.back().queryItemValue("limit").toInt() == limit;
        QStringList visible;
        for (const auto &layout : view.getMessagesSnapshot())
        {
            const auto &id = layout->getMessage()->id;
            if (id.startsWith("burst-"))
                visible.append(id);
        }
        QStringList expected;
        if (!disabled)
            for (int i = 0; i < burst; ++i)
                expected.append(QStringLiteral("burst-%1").arg(i));
        const bool passed = started && finished && unexpected == 0 &&
            (disabled ? queries.size() == 1 : bounded) && visible == expected;
        checks.insert("reconnect_" + scenario,
                      QJsonObject{{"ok", passed}, {"primaryRequests", int(queries.size())},
                                  {"strictIntervalAndLimit", bounded},
                                  {"visibleBurst", int(visible.size())},
                                  {"unexpectedRequests", unexpected}});
        allPassed &= passed;
        extraResponder = {};
    }
    return allPassed;
}

}  // namespace chatterino::preview
