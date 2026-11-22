// SPDX-License-Identifier: MIT

#pragma once

// Exercise the production role page and provider against fictional HTTP
// fixtures on loopback. No Twitch credentials, real accounts or API calls.

#include "common/network/NetworkManager.hpp"
#include "common/UiLanguage.hpp"
#include "providers/twitch/ModdexRoles.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/UsercardRolesPage.hpp"
#include "widgets/splits/Split.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <functional>
#include <memory>
#include <optional>

namespace roleschecks {

using chatterino::moddex::CurrentRoles;
using chatterino::moddex::ResultStatus;
using chatterino::moddex::RoleKind;
using chatterino::moddex::RolePage;
using chatterino::moddex::RoleProvider;

namespace {

constexpr auto FIRST_UID = "1001";
constexpr auto SECOND_UID = "1002";

inline QJsonObject role(const QString &channel, const QString &display,
                        const QString &id, bool past = false)
{
    QJsonObject row{{"id", id},
                    {"login", channel},
                    {"name", display},
                    {"avatar", ""},
                    {"grantedAt", "2024-01-02T00:00:00.000Z"}};
    if (past)
    {
        row["endedAt"] = "2025-02-03T00:00:00.000Z";
    }
    return row;
}

inline QJsonObject page(QJsonArray rows, qint64 total,
                        const QString &cursor = {})
{
    return {{"items", rows},
            {"limit", 25},
            {"hasMore", !cursor.isEmpty()},
            {"cursor", cursor.isEmpty() ? QJsonValue(QJsonValue::Null)
                                        : QJsonValue(cursor)},
            {"total", total < 0 ? QJsonValue(QJsonValue::Null)
                               : QJsonValue(double(total))}};
}

inline QJsonObject profile(const QString &id, QJsonObject mod,
                           QJsonObject vip)
{
    return {{"id", id},
            {"login", "fixture_user"},
            {"name", "Fixture_User"},
            {"roles", QJsonObject{{"mod", mod.value("total")},
                                  {"vip", vip.value("total")}}},
            {"roleLists", QJsonObject{{"mods", mod}, {"vips", vip}}}};
}

class FakeRoleServer final : public QTcpServer
{
public:
    FakeRoleServer()
    {
        QObject::connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = this->nextPendingConnection())
            {
                auto bytes = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    if (!bytes->contains("\r\n\r\n"))
                    {
                        return;
                    }
                    if (!socket->property("answered").toBool())
                    {
                        socket->setProperty("answered", true);
                        this->answer(socket, *bytes);
                    }
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
            }
        });
    }

    QUrl baseUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1")
                        .arg(this->serverPort()));
    }

    int count(const QString &id, const QString &suffix = {},
              const QString &cursor = {}) const
    {
        return this->requests.value(
            QStringLiteral("/v1/users/by-id/") + id + suffix + u'|' + cursor);
    }

    int totalRequests() const
    {
        int total = 0;
        for (int value : this->requests)
        {
            total += value;
        }
        return total;
    }

    int unexpected = 0;
    bool sawCredentials = false;

private:
    QHash<QString, int> requests;

    static void send(QTcpSocket *socket, int status, const QJsonObject &body)
    {
        if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
        {
            return;
        }
        const auto json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) +
                      " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                      QByteArray::number(json.size()) + "\r\n\r\n" + json);
        socket->disconnectFromHost();
    }

    void answer(QTcpSocket *socket, const QByteArray &request)
    {
        const auto lower = request.toLower();
        if (lower.contains("authorization:") || lower.contains("oauth") ||
            lower.contains("client-id:"))
        {
            this->sawCredentials = true;
        }
        const auto target = QString::fromUtf8(request.split(' ').value(1));
        const QUrl url(QStringLiteral("http://fixture") + target);
        const auto path = url.path();
        const QUrlQuery query(url);
        const auto cursor = query.queryItemValue("cursor", QUrl::FullyDecoded);
        const int seen = ++this->requests[path + u'|' + cursor];
        const auto parts = path.split(u'/', Qt::SkipEmptyParts);
        if (parts.size() < 4 || parts[0] != "v1" || parts[1] != "users" ||
            parts[2] != "by-id")
        {
            ++this->unexpected;
            send(socket, 404, {{"error", "unexpected fixture route"}});
            return;
        }

        const auto id = parts[3];
        if (parts.size() == 4 && query.queryItemValue("include") == "roles")
        {
            auto mods = page({}, 0);
            auto vips = page({}, 0);
            if (id == FIRST_UID)
            {
                mods = page({role("alpha_channel", "AlphaChannel", "2001"),
                             role("very_long_channel_12345",
                                  "Very_Long_Channel_12345", "2002")},
                            3, "next/mod");
                vips = page({role("kappa_stream", "KappaStream", "2003")}, 1);
            }
            else if (id == SECOND_UID)
            {
                mods = page({role("second_channel", "SecondChannel", "2010")}, 1);
            }
            else if (id == "1003")
            {
                send(socket, 404, {{"error", "not in index"}});
                return;
            }
            else if (id == "1004" && seen == 1)
            {
                send(socket, 503, {{"error", "temporary failure"}});
                return;
            }
            else if (id == "1005" || id == "1007")
            {
                // Keep the request in flight across a target switch/destruction.
                const auto body = profile(id, page({role("stale_channel",
                    "StaleChannel", "2020")}, 1), vips);
                QTimer::singleShot(550, socket,
                                   [socket, body] { send(socket, 200, body); });
                return;
            }
            else if (id == "1006")
            {
                send(socket, 200, profile("9006", mods, vips));
                return;
            }
            else if (id == "1008")
            {
                send(socket, 200, {{"id", id}, {"roleLists", "malformed"}});
                return;
            }
            else if (id != "1004")
            {
                ++this->unexpected;
                send(socket, 404, {});
                return;
            }
            send(socket, 200, profile(id, mods, vips));
            return;
        }

        if (id == FIRST_UID && parts.size() >= 5 &&
            query.queryItemValue("limit") == "25")
        {
            if (parts.size() == 5 && parts[4] == "mods" && cursor == "next/mod")
            {
                send(socket, 200, page({role("omega_channel", "OmegaChannel",
                                            "2004")}, -1));
                return;
            }
            if (parts.size() == 6 && parts[5] == "past" && cursor.isEmpty())
            {
                if (parts[4] == "mods")
                {
                    send(socket, 200, page({role("former_moderator",
                        "FormerModerator", "2090", true)}, 1));
                    return;
                }
                if (parts[4] == "vips")
                {
                    send(socket, 200, page({role("former_vip", "FormerVip",
                                                "2091", true)}, 1));
                    return;
                }
            }
        }

        ++this->unexpected;
        send(socket, 404, {{"error", "unexpected fixture page"}});
    }
};

inline bool waitUntil(const std::function<bool()> &done, int ms = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

inline void settle(int ms)
{
    waitUntil([] { return false; }, ms);
}

inline QStringList channels(QWidget &page)
{
    QStringList names;
    for (auto *label : page.findChildren<QLabel *>("usercardRoleChannel"))
    {
        if (label->isVisible())
        {
            names.push_back(label->property("channelLogin").toString());
        }
    }
    names.sort();
    return names;
}

inline QStringList dates(QWidget &page)
{
    QStringList values;
    for (auto *label : page.findChildren<QLabel *>("usercardRoleDate"))
    {
        if (label->isVisible())
        {
            values.push_back(label->text());
        }
    }
    return values;
}

inline void mouseClick(QWidget *widget)
{
    if (!widget)
    {
        return;
    }
    const auto point = widget->rect().center();
    const auto global = widget->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                        QPointF(global), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
    QCoreApplication::sendEvent(widget, &release);
}

}  // namespace

inline int runRolesChecks(chatterino::Split &split,
                          const chatterino::UserInfoPreviewData &data,
                          float scale, const QString &screenshot,
                          QJsonArray &checks)
{
    using namespace chatterino;
    int failed = 0;
    auto check = [&](const QString &name, bool ok) {
        failed += !ok;
        checks.append(QJsonObject{{"name", name}, {"ok", ok}});
    };

    // The production popup's header opens its own Roles column. This offline
    // preview keeps the production endpoint disabled by construction.
    {
        UserInfoPopup popup(false, &split, true);
        popup.setAttribute(Qt::WA_DontShowOnScreen);
        popup.setAttribute(Qt::WA_DeleteOnClose, false);
        popup.setOverrideScale(scale);
        popup.setOfflinePreviewData(data);
        popup.resize(860, 550);
        popup.show();
        settle(80);
        auto *tabs = popup.findChild<QTabWidget *>("UsercardContent");
        auto *roles = popup.findChild<UsercardRolesPage *>();
        auto *toggle = popup.findChild<QWidget *>("UsercardRolesToggle");
        check("real usercard has a distinct roles page and navigation",
              tabs && roles && toggle && toggle->isVisible() &&
                  tabs->indexOf(roles) >= 0 && tabs->currentWidget() != roles);
        mouseClick(toggle);
        settle(40);
        check("clicking Roles selects the actual card page",
              tabs && roles && tabs->currentWidget() == roles);
        if (!screenshot.isEmpty())
        {
            check("rendering the roles page inside the complete usercard succeeds",
                  popup.grab().save(screenshot + QStringLiteral(".card.png")));
        }
        auto *messages = popup.findChild<QWidget *>("UsercardMessagesToggle");
        mouseClick(messages);
        settle(40);
        check("usercard can return from roles to messages without networking",
              tabs && tabs->currentWidget() != roles &&
                  NetworkManager::workerThread == nullptr);
        popup.hide();
    }

    FakeRoleServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
    {
        check("fixture role server listens", false);
        return failed;
    }
    NetworkManager::init();
    {
        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen);
        host.resize(qRound(414 * scale), qRound(476 * scale));
        UsercardRolesPage page(&host, server.baseUrl());
        page.setScale(scale);
        page.setGeometry(host.rect());
        host.show();
        page.show();
        auto *active = page.findChild<QAbstractButton *>("usercardRolesActive");
        auto *past = page.findChild<QAbstractButton *>("usercardRolesPast");
        auto *mods = page.findChild<QAbstractButton *>("usercardRolesModerators");
        auto *vips = page.findChild<QAbstractButton *>("usercardRolesVips");
        auto *more = page.findChild<QAbstractButton *>("usercardRolesMore");
        auto *retry = page.findChild<QAbstractButton *>("usercardRolesRetry");
        auto *status = page.findChild<QLabel *>("usercardRolesStatus");
        check("role page presents both modes, both role filters and a load-more action",
              active && past && mods && vips && more && retry && status);
        if (active && past && mods && vips && more && retry && status)
        {
            check("current and removed mode labels follow the UI language",
                  active->text() == uiText("Current roles") &&
                      past->text() == uiText("Removed roles") &&
                      (activeUiLanguage() != "ru" ||
                       (active->text() != "Current roles" && past->text() != "Removed roles")));
            page.setTarget(FIRST_UID, "fixture_user");
            settle(100);
            check("unopened role page sends no request", server.totalRequests() == 0);
            page.setActive(true);
            const QStringList firstPage{"alpha_channel", "very_long_channel_12345"};
            check("active moderator list loads on activation",
                  waitUntil([&] { return channels(page) == firstPage; }));
            check("first request obtains active moderator and VIP counts",
                  server.count(FIRST_UID) == 1 && mods->text().contains("3") &&
                      vips->text().contains("1"));
            check("active list indicates an additional page", more->isVisible());
            if (!screenshot.isEmpty())
            {
                check("rendering active roles page succeeds",
                      page.grab().save(screenshot));
            }

            more->click();
            const QStringList allMods{"alpha_channel", "omega_channel",
                                      "very_long_channel_12345"};
            check("cursor adds only the next active role page and retains prior rows",
                  waitUntil([&] { return channels(page) == allMods; }) &&
                      server.count(FIRST_UID, "/mods", "next/mod") == 1 &&
                      !more->isVisible());
            const auto granted = dates(page).join(u' ');
            check("current rows show their granted date without a removal date",
                  granted.contains("2024") && !granted.contains("2025"));

            vips->click();
            check("VIP filter does not mix moderator rows",
                  waitUntil([&] { return channels(page) == QStringList{"kappa_stream"}; }) &&
                      server.count(FIRST_UID, "/vips") == 0);

            mods->click();
            past->click();
            check("removed moderator role is fetched separately",
                  waitUntil([&] { return channels(page) == QStringList{"former_moderator"}; }) &&
                      server.count(FIRST_UID, "/mods/past") == 1);
            check("former rows display granted and observed end dates",
                  dates(page).join(u' ').contains("2024") &&
                      dates(page).join(u' ').contains("2025"));
            check("former-role mode reports its own counts",
                  waitUntil([&] { return mods->text().contains("1") &&
                                           vips->text().contains("1"); }) &&
                      server.count(FIRST_UID, "/vips/past") == 1);
            if (!screenshot.isEmpty())
            {
                const auto pastPath = screenshot + QStringLiteral(".past.png");
                check("rendering removed roles page succeeds", page.grab().save(pastPath));
            }
            vips->click();
            check("removed VIP filter is independent of active VIPs",
                  waitUntil([&] { return channels(page) == QStringList{"former_vip"}; }));
            mods->click();
            active->click();
            check("switching back restores loaded active pages without another request",
                  waitUntil([&] { return channels(page) == allMods; }) &&
                      server.count(FIRST_UID) == 1 && mods->text().contains("3"));
            page.setTarget(FIRST_UID, "renamed_user");
            settle(70);
            check("renaming an account with the same UID retains the displayed page",
                  channels(page) == allMods && server.count(FIRST_UID) == 1);

            bool contained = true;
            const auto rows = page.findChildren<QWidget *>("usercardRoleRow");
            for (auto *row : rows)
            {
                if (!row->isVisible())
                {
                    continue;
                }
                auto *name = row->findChild<QLabel *>("usercardRoleChannel");
                auto *date = row->findChild<QLabel *>("usercardRoleDate");
                if (!name || !date || !row->rect().contains(QRect(name->mapTo(row, QPoint{}), name->size())) ||
                    !row->rect().contains(QRect(date->mapTo(row, QPoint{}), date->size())) ||
                    QRect(name->mapTo(row, QPoint{}), name->size()).intersects(
                        QRect(date->mapTo(row, QPoint{}), date->size())))
                {
                    contained = false;
                }
            }
            check("scaled channel and date labels remain within each role row",
                  !rows.isEmpty() && contained);

            page.setTarget("1003", "missing_user");
            check("unavailable/opted-out identity never inherits a prior result",
                  waitUntil([&] { return server.count("1003") == 1 &&
                                           status->isVisible() &&
                                           status->text() == uiText("No public role data for this user") &&
                                           channels(page).isEmpty(); }));
            past->click();
            settle(100);
            check("unavailable identity is not looked up by login or another route, even in past mode",
                  server.count("1003") == 1 && server.unexpected == 0);

            page.setTarget("1004", "flaky_user");
            check("temporary failure is visible and retryable",
                  waitUntil([&] { return server.count("1004") == 1 && retry->isVisible() &&
                                           channels(page).isEmpty(); }));
            retry->click();
            check("retry can recover to an explicitly empty result",
                  waitUntil([&] { return server.count("1004") == 2 && !retry->isVisible() &&
                                           mods->text().contains("0") && vips->text().contains("0"); }));

            page.setTarget("1005", "delayed_user");
            check("new role identity starts with an empty list while loading",
                  waitUntil([&] { return server.count("1005") == 1; }) &&
                      channels(page).isEmpty());
            page.setTarget(SECOND_UID, "second_user");
            const QStringList secondPage{"second_channel"};
            check("new target can finish before delayed previous identity",
                  waitUntil([&] { return channels(page) == secondPage; }));
            settle(700);
            check("late result cannot replace the current target's roles",
                  channels(page) == secondPage && server.count(SECOND_UID) == 1);

            page.setActive(false);
            const auto before = server.totalRequests();
            page.setTarget("1009", "hidden_user");
            settle(120);
            check("switching the hidden page to another identity stays offline",
                  server.totalRequests() == before);
        }

        RoleProvider provider(server.baseUrl());
        QObject lifetime;
        std::optional<ResultStatus> cached;
        CurrentRoles result;
        provider.requestCurrent(FIRST_UID, &lifetime,
                                [&](ResultStatus status, CurrentRoles value) {
            cached = status;
            result = std::move(value);
        });
        check("another provider gets the cached active result with a stable Twitch UID query",
              waitUntil([&] { return cached.has_value(); }) && cached == ResultStatus::Ok &&
                  result.moderators.items.size() == 2 && result.vips.total == 1 &&
                  result.moderators.items[0].channelId == "2001" &&
                  result.moderators.items[0].grantedAt.isValid() && server.count(FIRST_UID) == 1);

        std::optional<ResultStatus> mismatch;
        provider.requestCurrent("1006", &lifetime,
                                [&](ResultStatus status, CurrentRoles) { mismatch = status; });
        check("provider rejects a successful HTTP response for a different UID",
              waitUntil([&] { return mismatch.has_value(); }) && mismatch == ResultStatus::Error);

        std::optional<ResultStatus> malformed;
        provider.requestCurrent("1008", &lifetime,
                                [&](ResultStatus status, CurrentRoles) { malformed = status; });
        check("malformed current profile is an error instead of zero roles",
              waitUntil([&] { return malformed.has_value(); }) && malformed == ResultStatus::Error);

        bool called = false;
        auto canceled = std::make_unique<QObject>();
        provider.requestCurrent("1007", canceled.get(),
                                [&](ResultStatus, CurrentRoles) { called = true; });
        check("delayed fixture request starts before its context is destroyed",
              waitUntil([&] { return server.count("1007") == 1; }));
        canceled.reset();
        settle(700);
        check("destroyed request context is never called", !called);
        check("only documented ID-based routes were requested and no credentials were sent",
              server.unexpected == 0 && !server.sawCredentials);
    }
    NetworkManager::deinit();
    return failed;
}

}  // namespace roleschecks
