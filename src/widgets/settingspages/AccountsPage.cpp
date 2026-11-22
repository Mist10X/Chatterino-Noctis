// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/AccountsPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/accounts/AccountModel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "singletons/Theme.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

void styleAccountTable(QTableView *table)
{
    auto *app = getApp();
    const auto scale = SettingsPage::interfaceScale();
    table->setFont(app->getFonts()->getFont(FontStyle::ChatMedium, scale));
    table->verticalHeader()->setDefaultSectionSize(
        std::max(qRound(32 * scale), table->fontMetrics().height() + 10));
    const auto *theme = app->getThemes();
    const auto surface = theme->splits.input.background;
    const auto ink = theme->window.text;
    const auto accent = theme->accent;
    const auto blend = [surface, accent](qreal weight) {
        return QColor::fromRgbF(surface.redF() * (1 - weight) + accent.redF() * weight,
                                surface.greenF() * (1 - weight) + accent.greenF() * weight,
                                surface.blueF() * (1 - weight) + accent.blueF() * weight).name();
    };
    table->setStyleSheet(QStringLiteral(R"(
        QTableView#NoctisAccountsTable {
            background: %1; color: %2; border: 1px solid %3;
            border-radius: %4px; outline: none;
        }
        QTableView#NoctisAccountsTable::item {
            padding: 3px %5px; border: none; border-radius: %4px;
        }
        QTableView#NoctisAccountsTable::item:hover { background: %6; }
        QTableView#NoctisAccountsTable::item:selected { background: %7; color: %2; }
    )").arg(surface.name(), ink.name(), theme->splits.header.border.name())
          .arg(qRound(6 * scale)).arg(qRound(9 * scale))
          .arg(blend(.10), blend(.22)));
}

}  // namespace

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    LayoutCreator<AccountsPage> layoutCreator(this);
    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();

    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                app->getAccounts()->createModel(nullptr), false)
            .getElement();

    view->getTableView()->horizontalHeader()->setVisible(false);
    view->getTableView()->horizontalHeader()->setStretchLastSection(true);

    // We can safely ignore this signal connection since we own the view
    std::ignore = view->addButtonPressed.connect([this] {
        LoginDialog d(this);
        d.exec();
    });

    auto *table = view->getTableView();
    table->setObjectName("NoctisAccountsTable");
    table->setShowGrid(false);
    table->setMouseTracking(true);
    table->setAlternatingRowColors(false);
    const auto fitRows = [table] {
        const auto count = std::clamp(table->model()->rowCount(), 1, 8);
        table->setFixedHeight(count * table->verticalHeader()->defaultSectionSize() + 2);
    };
    QObject::connect(table->model(), &QAbstractItemModel::rowsInserted, table, fitRows);
    QObject::connect(table->model(), &QAbstractItemModel::rowsRemoved, table, fitRows);
    QObject::connect(table->model(), &QAbstractItemModel::modelReset, table, fitRows);
    const auto refresh = [table, fitRows] {
        styleAccountTable(table);
        fitRows();
    };
    this->managedConnections_.managedConnect(app->getFonts()->fontChanged, refresh);
    this->managedConnections_.managedConnect(app->getThemes()->updated, refresh);
    getSettings()->uiScale.connect(refresh, this->managedConnections_);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addStretch(1);
}

}  // namespace chatterino
