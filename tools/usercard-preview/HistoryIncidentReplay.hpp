// SPDX-License-Identifier: MIT
#pragma once

#include "OwnTimeoutChecks.hpp"
#include "PublicLogsChecks.hpp"

#include <QUrlQuery>

#include <limits>

namespace chatterino::preview {

/// Optional offline replay of a real response envelope. The fixture stores only
/// command types, time offsets, deletion flags and synthetic shared identities.
inline bool checkHistoryIncidentReplay(
    QJsonObject &checks,
    std::function<bool(const QUrl &, QTcpSocket *)> &responder, quint16 port)
{
    const auto path = qEnvironmentVariable("NOCTIS_HISTORY_REPLAY_FIXTURE");
    if (path.isEmpty())
        return true;
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly))
        return false;
    const auto data = QJsonDocument::fromJson(source.readAll()).object();
    const QString channel = QStringLiteral("history_incident");
    const QString rawURL = QStringLiteral("http://127.0.0.1:%1/raw/%2").arg(port).arg("%1");
    const qint64 epoch = QDateTime::currentMSecsSinceEpoch();
    qputenv("CHATTERINO2_RECENT_MESSAGES_RAW_URL", rawURL.toUtf8());
    const auto previousLimit = getSettings()->twitchMessageHistoryLimit.getValue();
    getSettings()->twitchMessageHistoryLimit = 800;
    QHash<QString, int> requests;
    const auto idOf = [](int n) { return QStringLiteral("incident-%1").arg(n); };
    QSet<QString> expected, deletedInPrimary;
    for (const auto &id : data.value("expected").toArray())
        expected.insert(idOf(id.toInt()));
    bool validQuery = true;

    responder = [&](const QUrl &url, QTcpSocket *socket) {
        if (!url.path().endsWith("/" + channel))
            return false;
        const auto route = url.path().section('/', 1, 1);
        const auto key = route == "recent" ? QStringLiteral("primary") : route;
        ++requests[key];
        const auto values = data.value(key).toArray();
        const QUrlQuery query(url);
        qint64 from = 0, before = std::numeric_limits<qint64>::max();
        int limit = 800;
        if (key == "raw")
        {
            const auto timestamp = [&](const QString &name) {
                return QDateTime::fromString(query.queryItemValue(name),
                                             Qt::ISODateWithMs);
            };
            const auto left = timestamp("from"), right = timestamp("to");
            validQuery &= left.isValid() && right.isValid() &&
                          left < right && query.hasQueryItem("raw");
            from = left.toMSecsSinceEpoch();
            before = right.toMSecsSinceEpoch();
        }
        else
        {
            limit = query.queryItemValue("limit").toInt();
            validQuery &= limit > 0 && limit <= 800;
            if (key == "primary")
                validQuery &= limit == 800;
            if (query.hasQueryItem("after"))
                from = query.queryItemValue("after").toLongLong();
            if (query.hasQueryItem("before"))
                before = query.queryItemValue("before").toLongLong();
        }

        QStringList rows;
        for (const auto &value : values)
        {
            const auto row = value.toArray();
            const QString command = row.at(0).toString();
            const auto id = row.at(1).toInt();
            const auto sent = row.at(2), received = row.at(3);
            const qint64 t = epoch + (received.isNull() ? sent : received).toInteger();
            if (t <= from || t >= before)
                continue;
            QStringList tags{QStringLiteral("room-id=910000002")};
            if (!received.isNull())
                tags << "historical=1" << QStringLiteral("rm-received-ts=%1").arg(t);
            if (!sent.isNull())
                tags << QStringLiteral("tmi-sent-ts=%1").arg(epoch + sent.toInteger());
            if (id != 0)
                tags << "id=" + idOf(id);
            if (row.at(4).toInt())
            {
                tags << "rm-deleted=1";
                if (key == "primary" && command == "PRIVMSG")
                    deletedInPrimary.insert(idOf(id));
            }
            const bool chat = command == "PRIVMSG";
            const QString prefix = chat ? "fixture!fixture@fixture.tmi.twitch.tv" : "tmi.twitch.tv";
            if (chat)
                tags << "display-name=Fixture" << "user-id=910000001";
            else if (command == "USERNOTICE")
                tags << "msg-id=announcement" << "display-name=Fixture" << "login=fixture";
            QString line = "@" + tags.join(';') + " :" + prefix + " " + command + " #" + channel;
            if (chat || command == "NOTICE" || command == "USERNOTICE")
                line += " :fixture-" + QString::number(id);
            else if (command == "CLEARCHAT")
                line += " :absent_fixture_user";
            rows.push_back(std::move(line));
        }
        if (rows.size() > limit && limit > 0)
            rows = rows.last(limit);
        const auto body = key == "raw" ? rows.join('\n').toUtf8() :
            QJsonDocument(QJsonObject{{"messages", QJsonArray::fromStringList(rows)}})
                .toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
        return true;
    };

    auto room = std::make_shared<TwitchChannel>(channel);
    ChannelView view(nullptr);
    view.setChannel(room);
    room->addSystemMessage("joined fixture channel");
    TwitchChannelTestAccess::loadStartupHistory(*room);
    const bool completed = publiclogchecks::waitUntil(
        [&] { return !room->isLoadingRecentMessages(); }, 15000);
    QSet<QString> visible, found, disabled;
    bool unique = true, ordered = true;
    qint64 previous = 0;
    for (const auto &layout : view.getMessagesSnapshot())
    {
        const auto &message = layout->getMessage();
        if (!message->id.startsWith("incident-") ||
            !message->messageText.startsWith("fixture-"))
            continue;
        unique &= !visible.contains(message->id);
        visible.insert(message->id);
        if (message->flags.has(MessageFlag::Disabled))
            disabled.insert(message->id);
        if (!expected.contains(message->id))
            continue;
        const auto t = message->serverReceivedTime.toMSecsSinceEpoch();
        ordered &= t >= previous;
        previous = t;
        found.insert(message->id);
    }
    getSettings()->twitchMessageHistoryLimit = previousLimit;
    responder = {};
    qunsetenv("CHATTERINO2_RECENT_MESSAGES_RAW_URL");
    const bool keptDeletions = !deletedInPrimary.isEmpty() && disabled.contains(deletedInPrimary);
    const bool ok = completed && validQuery && requests.value("primary") == 1 &&
                    requests.value("backup") > 0 && found == expected &&
                    expected.size() == 23 && unique && ordered && keptDeletions;
    checks.insert("incidentReplay", QJsonObject{
        {"ok", ok}, {"finished", completed}, {"validQuery", validQuery},
        {"primaryRequests", requests.value("primary")},
        {"backupRequests", requests.value("backup")},
        {"intervalRequests", requests.value("raw")},
        {"primaryInputRows", data.value("primary").toArray().size()},
        {"backupInputRows", data.value("backup").toArray().size()},
        {"visibleChatRows", visible.size()}, {"expectedGapIds", expected.size()},
        {"shownGapIds", found.size()}, {"unique", unique}, {"chronologicalGap", ordered},
        {"primaryDeletedRows", deletedInPrimary.size()}, {"retainedDeletions", keptDeletions}});
    return ok;
}

}  // namespace chatterino::preview
