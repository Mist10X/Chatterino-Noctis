// SPDX-License-Identifier: MIT
// Public card history exercised against synthetic loopback HTTP mirrors.
#pragma once

#include "PublicLogsChecks.hpp"
#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <functional>

namespace chatterino::preview {

inline void runHistoryChecks(
    UserInfoPopup &popup, std::function<UserInfoPreviewData(QString)> fixture,
    std::function<void(QString, bool)> check)
{
    using namespace publiclogchecks;
    FakeMirrors server;
    const bool listening = server.listen(QHostAddress::LocalHost, 0);
    check("public history fixture listens only on loopback", listening);
    if (!listening)
    {
        return;
    }

    auto first = fixture("a");
    auto second = fixture("b");
    const auto live = first.messages->getMessageSnapshot();
    check("fixture begins with actual rich chat messages", !live.empty());
    if (live.empty())
    {
        return;
    }
    const auto original = live.front();
    const auto start = original->serverReceivedTime.toMSecsSinceEpoch() - 86400000;
    const auto addRow = [&](QString id, qint64 at, const UserInfoPreviewData &data,
                            const QString &body) {
        auto raw = line(id, at, data.userId, data.login, body);
        raw.replace("#chan :", "#fixture_room :");
        for (auto &mirror : server.lines)
        {
            mirror.push_back({at, raw});
        }
    };
    addRow("public-first", start, first, "Older public chat message");
    addRow(original->id, original->serverReceivedTime.toMSecsSinceEpoch(), first,
           "Duplicate raw message must not replace richer live message");
    addRow("public-second", start - 1000, second, "Different Twitch identity");

    NetworkManager::init();
    publiclogs::setMirrorsForTesting(server.bases(), 300);
    first.allowPublicHistory = true;
    first.autoFillHistory = false;
    popup.setOfflinePreviewData(first);

    const auto find = [&](const QString &id) -> MessagePtr {
        if (const auto *view = popup.findChild<ChannelView *>())
        {
            for (const auto &message : view->channel()->getMessageSnapshot())
            {
                if (message->id == id)
                {
                    return message;
                }
            }
        }
        return {};
    };
    const auto load = [&] {
        auto *button = dynamic_cast<LabelButton *>(popup.findChild<QWidget *>("UsercardHistory"));
        check("public history button is available", button && button->enabled());
        if (button)
        {
            Q_EMIT button->leftClicked();
        }
    };

    // Opening the card must not contact history providers automatically.
    QEventLoop idle;
    QTimer::singleShot(100, &idle, &QEventLoop::quit);
    idle.exec();
    check("opening ordinary public card makes no history request", server.requests() == 0);
    load();
    check("manual history request imports older matching messages",
          waitUntil([&] { return bool(find("public-first")); }, 6000));
    const auto imported = find("public-first");
    check("public history preserves explicit capitalization and Twitch ID",
          imported && imported->displayName == first.login.toUpper() &&
              imported->userID == first.userId);
    check("public duplicate leaves the richer live message untouched",
          find(original->id) == original);
    check("another user sharing a login cannot enter the card history",
          !find("public-second"));

    second.allowPublicHistory = true;
    second.autoFillHistory = false;
    popup.setOfflinePreviewData(second);
    check("changing identity clears prior public results immediately",
          !find("public-first"));
    load();
    check("same login with different UID loads its own public history",
          waitUntil([&] { return bool(find("public-second")); }, 6000) &&
              !find("public-first"));

    const auto requests = server.requests();
    first.allowPublicHistory = false;
    popup.setOfflinePreviewData(first);
    check("offline fixture can disable history requests", server.requests() == requests);
    NetworkManager::deinit();
}

}  // namespace chatterino::preview
