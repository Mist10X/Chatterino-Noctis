// SPDX-License-Identifier: MIT
#pragma once

#include "OwnTimeoutChecks.hpp"
#include "PublicLogsChecks.hpp"

#include <QUrlQuery>

namespace chatterino::preview {

inline bool checkHistoryGaps(
    QJsonObject &checks,
    std::function<bool(const QUrl &, QTcpSocket *)> &responder, quint16 port)
{
    const QString name = QStringLiteral("startup_history_gap");
    const auto rawURL = QStringLiteral("http://127.0.0.1:%1/raw/").arg(port) + "%1";
    qputenv("CHATTERINO2_RECENT_MESSAGES_RAW_URL", rawURL.toUtf8());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 minute = 60'000;
    const qint64 left = now - 88 * minute;
    const qint64 right = now - minute;
    const auto row = [&](const QString &id, qint64 stamp, QString room = {},
                         const QString &extraTags = {}) {
        return QStringLiteral("@%1id=%2;tmi-sent-ts=%3;user-id=910000001;display-name=Fixture;room-id=910000002;badges=;emotes= :fixture!fixture@fixture.tmi.twitch.tv PRIVMSG #%4 :fixture-%2")
            .arg(extraTags, id).arg(stamp).arg(room.isEmpty() ? name : room);
    };
    const auto json = [](QJsonArray rows) {
        return QJsonDocument(QJsonObject{{"messages", rows}}).toJson(QJsonDocument::Compact);
    };
    const auto earlier = row("earlier", left);
    const auto later = row("later", right);
    const auto overlap = row("overlap", left + 10 * minute);
    const auto hidden = row("raw", left + 35 * minute);

    QByteArray primary, secondary, raw;
    int primaryStatus = 200, secondaryStatus = 200;
    QList<QUrl> requests;
    responder = [&](const QUrl &url, QTcpSocket *socket) {
        if (!url.path().endsWith("/" + name))
            return false;
        requests.push_back(url);
        const bool first = url.path().startsWith("/recent/");
        const bool second = url.path().startsWith("/backup/");
        const auto &body = first ? primary : second ? secondary : raw;
        const int status = first ? primaryStatus : second ? secondaryStatus : 200;
        socket->write("HTTP/1.1 " + QByteArray::number(status) +
                      " Fixture\r\nConnection: close\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
        return true;
    };
    bool passed = true;
    auto check = [&](const QString &key, const QStringList &ids, int expectedRequests,
                     bool scoped = false) {
        requests.clear();
        auto room = std::make_shared<TwitchChannel>(name);
        ChannelView view(nullptr);
        view.setChannel(room);
        room->addSystemMessage("joined fixture");
        TwitchChannelTestAccess::loadStartupHistory(*room);
        const bool finished = publiclogchecks::waitUntil(
            [&] { return !room->isLoadingRecentMessages(); }, 7000);
        QStringList shown;
        bool historical = true;
        for (const auto &layout : view.getMessagesSnapshot())
        {
            const auto &message = layout->getMessage();
            if (!message->id.isEmpty())
            {
                shown.push_back(message->id);
                historical &= message->flags.has(MessageFlag::RecentMessage);
            }
        }
        bool bounds = true;
        QStringList paths;
        for (const auto &url : requests)
        {
            paths.push_back(url.path().section('/', 1, 1));
            const QUrlQuery q(url);
            if (scoped && url.path().startsWith("/backup/"))
                bounds &= q.queryItemValue("after").toLongLong() >= left - 1 &&
                          q.queryItemValue("before").toLongLong() <= right + 1;
            if (scoped && url.path().startsWith("/raw/"))
            {
                auto from = QDateTime::fromString(q.queryItemValue("from"), Qt::ISODateWithMs);
                auto to = QDateTime::fromString(q.queryItemValue("to"), Qt::ISODateWithMs);
                bounds &= q.hasQueryItem("raw") && from.isValid() && to.isValid() &&
                          from.toMSecsSinceEpoch() >= left - 1 &&
                          to.toMSecsSinceEpoch() <= right + 1;
            }
        }
        const bool ok = finished && shown == ids && bounds && historical &&
                        requests.size() == expectedRequests;
        checks.insert(key, QJsonObject{{"ok", ok}, {"finished", finished},
            {"visibleIds", QJsonArray::fromStringList(shown)}, {"historical", historical},
            {"requestPaths", QJsonArray::fromStringList(paths)}, {"bounds", bounds}});
        passed &= ok;
    };

    // Both HTTP responses claim success. The second source returns an overlap,
    // the last source an extra message, a duplicate and unrelated data.
    primary = json({earlier, later});
    secondary = json({earlier, overlap, later});
    raw = QStringList{hidden, row("wrong", left + 15 * minute, "unrelated"),
                      hidden, row("future", now + minute), overlap}.join('\n').toUtf8();
    check("silentInterior87Minutes", {"earlier", "overlap", "raw", "later"}, 3, true);

    // A busy period after a long silence must not make the client ignore the
    // interior of an apparently healthy response.
    primary = json({earlier, row("mid", left + 45 * minute), later});
    secondary = json({overlap, row("second-gap", right - 10 * minute)});
    raw.clear();
    check("twoInteriorGaps", {"earlier", "overlap", "mid", "second-gap", "later"}, 3, true);

    primary = json({row("recent", right)});
    secondary = json({});
    check("recentHealthyNoRecovery", {"recent"}, 1);

    primary = json({});
    secondary = json({later});
    check("emptySuccessfulPrimary", {"later"}, 2);

    primary = json({QStringLiteral("@historical=1;rm-received-ts=%1 :tmi.twitch.tv ROOMSTATE #%2")
                        .arg(now - minute).arg(name)});
    secondary = json({});
    raw = hidden.toUtf8();
    check("serviceRowsAndEmptyBackup", {"raw"}, 3);

    primary = "{";
    secondary = json({later});
    raw.clear();
    check("malformedSuccessfulPrimary", {"later"}, 2);

    QJsonArray busy;
    QStringList busyExpected;
    for (int i = 0; i < 24; ++i)
    {
        const QString id = QStringLiteral("busy-%1").arg(i);
        busy.append(row(id, now - 110'000 + i * 1000));
        busyExpected.push_back(id);
    }
    busy.append(row("busy-late", now - 2000));
    busyExpected.append("missing-busy");
    busyExpected.append("busy-late");
    primary = json(busy);
    secondary = json({row("missing-busy", now - 44'000)});
    check("shorterGapInBusyChat", busyExpected, 2);

    // Recover original Twitch timestamps when the mirror's reception timestamp
    // is stale, without displaying an out-of-window source row.
    primary = json({earlier});
    const auto skewed = row("recovered", right, {},
                            QStringLiteral("historical=1;rm-received-ts=%1;").arg(now + minute));
    secondary = json({skewed, row("out-of-range", now + minute)});
    raw.clear();
    check("staleTail", {"earlier", "recovered"}, 3);

    // Explicit provider refusal on a channel must end secondary lookups.
    primary = json({earlier, later});
    secondaryStatus = 403;
    check("refusalStopsRaw", {"earlier", "later"}, 2);
    secondaryStatus = 200;
    primaryStatus = 403;
    check("primaryRefusal", {}, 1);
    primaryStatus = 429;
    check("primaryRateLimit", {}, 1);
    responder = {};
    qunsetenv("CHATTERINO2_RECENT_MESSAGES_RAW_URL");
    return passed;
}

}  // namespace chatterino::preview
