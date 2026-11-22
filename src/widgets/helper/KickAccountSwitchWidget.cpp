#include "widgets/helper/KickAccountSwitchWidget.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "singletons/Settings.hpp"

namespace chatterino {

KickAccountSwitchWidget::KickAccountSwitchWidget(QWidget *parent)
    : QListWidget(parent)
{
    this->setObjectName("KickAccountList");
    this->setMouseTracking(true);
    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->kick.userListUpdated, [this]() {
            QSignalBlocker b(this);

            this->refreshItems();
            this->refresh();
        });

    this->refreshItems();
    this->refresh();

    QObject::connect(this, &QListWidget::clicked, [this] {
        if (this->selectedItems().isEmpty())
        {
            return;
        }

        getApp()->getAccounts()->kick.currentUsername =
            this->currentItem()->data(Qt::UserRole).toString();

        std::ignore = getSettings()->requestSave();
    });
}

void KickAccountSwitchWidget::refresh()
{
    QSignalBlocker b(this);

    if (this->count() <= 0)
    {
        return;
    }

    auto currentUser = getApp()->getAccounts()->kick.current();

    if (currentUser->isAnonymous())
    {
        this->setCurrentRow(0);
        return;
    }

    QString currentUsername = currentUser->username();
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

void KickAccountSwitchWidget::refreshItems()
{
    this->clear();

    auto *anonymous = new QListWidgetItem(uiText("Anonymous"), this);
    anonymous->setData(Qt::UserRole, QString{});

    for (const auto &userName : getApp()->getAccounts()->kick.usernames())
    {
        auto *item = new QListWidgetItem(userName, this);
        item->setData(Qt::UserRole, userName);
    }
}

}  // namespace chatterino
