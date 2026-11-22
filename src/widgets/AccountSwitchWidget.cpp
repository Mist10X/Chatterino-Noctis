// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/AccountSwitchWidget.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "singletons/Settings.hpp"

#include <QSignalBlocker>

namespace chatterino {

AccountSwitchWidget::AccountSwitchWidget(QWidget *parent)
    : QListWidget(parent)
{
    auto *app = getApp();
    this->setObjectName("TwitchAccountList");
    this->setMouseTracking(true);
    this->managedConnections_.managedConnect(
        app->getAccounts()->twitch.userListUpdated, [this]() {
            this->refreshItems();
            this->refreshSelection();
        });
    this->managedConnections_.managedConnect(
        app->getAccounts()->twitch.currentUserNameChanged, [this] {
            this->refreshItems();
            this->refreshSelection();
        });
    this->refreshItems();
    this->refreshSelection();

    QObject::connect(this, &QListWidget::itemClicked, this,
        [app](QListWidgetItem *item) {
            app->getAccounts()->twitch.currentUsername =
                item->data(Qt::UserRole).toString();
            getSettings()->requestSave();
        });
}

void AccountSwitchWidget::refresh()
{
    this->refreshItems();
    this->refreshSelection();
    getApp()->getAccounts()->twitch.refreshDisplayNames();
}

void AccountSwitchWidget::refreshItems()
{
    const QSignalBlocker blocker(this);
    this->clear();
    auto *anonymous = new QListWidgetItem(uiText("Anonymous"), this);
    anonymous->setData(Qt::UserRole, QString{});

    auto &accounts = getApp()->getAccounts()->twitch;
    for (const auto &login : accounts.getUsernames())
    {
        const auto account = accounts.findUserByUsername(login);
        auto *item = new QListWidgetItem(
            account ? account->getDisplayName() : login, this);
        item->setData(Qt::UserRole, login);
    }
}

void AccountSwitchWidget::refreshSelection()
{
    const QSignalBlocker blocker(this);

    if (this->count() > 0)
    {
        auto *app = getApp();

        auto currentUser = app->getAccounts()->twitch.getCurrent();

        if (currentUser->isAnon())
        {
            this->setCurrentRow(0);
        }
        else
        {
            const QString &currentUsername = currentUser->getUserName();
            for (int i = 0; i < this->count(); ++i)
            {
                const auto itemText = this->item(i)->data(Qt::UserRole).toString();

                if (itemText.compare(currentUsername, Qt::CaseInsensitive) == 0)
                {
                    this->setCurrentRow(i);
                    break;
                }
            }
        }
    }

}

}  // namespace chatterino
