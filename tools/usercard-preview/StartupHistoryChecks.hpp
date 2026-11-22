// SPDX-License-Identifier: MIT
#pragma once

#include "OwnTimeoutChecks.hpp"
#include "PublicLogsChecks.hpp"
#include "providers/recentmessages/Impl.hpp"

namespace chatterino::preview {

// Use NOTICE rows so this startup/ordering fixture does not initialize third-party
// badge and emote providers. The normal IRC parser and ChannelView are exercised.
inline int checkStartupHistory(const QString &report)
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
        return 40;
    const auto endpoint = QStringLiteral("http://127.0.0.1:%1/recent/").arg(server.serverPort()) + "%1";
    qputenv("CHATTERINO2_RECENT_MESSAGES_URL", endpoint.toUtf8());
    if (recentmessages::detail::recentMessagesApiUrlTemplate() != endpoint)
        return 41;  // Never fall through to a real provider.

    NetworkManager::init();
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
    const auto live = QStringLiteral("live-row-39");
    room->addSystemMessage(live);
    expected.push_back(live);

    int requests = 0;
    QUrl requestURL;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            if (!socket->canReadLine())
                return;
            ++requests;
            requestURL = QUrl::fromEncoded(socket->readLine().split(' ').value(1));
            auto body = QJsonDocument(QJsonObject{{"messages", rows}}).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
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
    QJsonObject checks{{"respectsDisabled", respectedDisabled}, {"completed", completed},
                       {"requests", requests}, {"expectedURL", correctURL}, {"prependSignals", prepends},
                       {"visibleRows", static_cast<int>(displayed.size())}, {"visibleOrder", displayed == expected}};
    QFile output(report);
    const bool saved = output.open(QIODevice::WriteOnly) &&
                       output.write(QJsonDocument(checks).toJson()) > 0;
    NetworkManager::deinit();
    return saved && respectedDisabled && completed && requests == 1 && correctURL &&
                   prepends == 1 && displayed == expected ? 0 : 42;
}

}  // namespace chatterino::preview
