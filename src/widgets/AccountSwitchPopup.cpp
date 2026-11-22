// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/UiLanguage.hpp"
#include "widgets/AccountSwitchPopup.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/AccountSwitchWidget.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/KickAccountSwitchWidget.hpp"
#include "widgets/helper/MicroNotebook.hpp"

#include <QLayout>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QScreen>

#include <algorithm>

namespace chatterino {

using namespace literals;

AccountSwitchPopup::AccountSwitchPopup(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::TopMost,
              BaseWindow::Frameless,
              BaseWindow::DisableLayoutSave,
              BaseWindow::LinuxPopup,
          },
          parent)
{
    this->focusOutAction = FocusOutAction::Hide;

    this->setObjectName("NoctisAccountSwitchPopup");
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setAutoFillBackground(false);
    this->setContentsMargins(0, 0, 0, 0);

    auto *notebook = new MicroNotebook(this);
    this->notebook_ = notebook;

    this->ui_.accountSwitchWidget = new AccountSwitchWidget(this);
    this->ui_.accountSwitchWidget->setFocusPolicy(Qt::NoFocus);
    this->ui_.kickAccountSwitcher = new KickAccountSwitchWidget(this);
    this->ui_.kickAccountSwitcher->setFocusPolicy(Qt::NoFocus);

    auto updateNotebook = [this, notebook] {
        if (getApp()->getAccounts()->kick.accounts.empty())
        {
            notebook->setShowHeader(false);
            notebook->select(this->ui_.accountSwitchWidget);
        }
        else
        {
            notebook->setShowHeader(true);
        }
    };
    notebook->addPage(this->ui_.accountSwitchWidget, "Twitch");
    notebook->addPage(this->ui_.kickAccountSwitcher, "Kick");
    updateNotebook();
    QVBoxLayout *vbox = new QVBoxLayout(this);
    vbox->addWidget(notebook);

    this->manageAccountsButton_ = new QPushButton(uiText("Manage Accounts"), this);
    this->manageAccountsButton_->setObjectName("ManageAccountsButton");
    this->manageAccountsButton_->setFocusPolicy(Qt::NoFocus);
    this->manageAccountsButton_->setCursor(Qt::PointingHandCursor);
    vbox->addWidget(this->manageAccountsButton_);

    connect(this->manageAccountsButton_, &QPushButton::clicked, [this]() {
        SettingsDialog::showDialog(this->parentWidget(),
                                   SettingsDialogPreference::Accounts);
    });

    this->getLayoutContainer()->setLayout(vbox);

    this->signalHolder_.addConnection(
        getApp()->getAccounts()->kick.userListUpdated.connect([this, updateNotebook] {
            updateNotebook();
            this->fitContents();
        }));
    this->signalHolder_.addConnection(
        getApp()->getAccounts()->twitch.userListUpdated.connect([this] {
            this->fitContents();
        }));
    this->signalHolder_.addConnection(
        getApp()->getAccounts()->twitch.currentUserNameChanged.connect([this] {
            this->fitContents();
        }));
    this->themeChangedEvent();
}

void AccountSwitchPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    const auto *t = getTheme();
    const auto scale = this->scale();
    const auto metric = [scale](int value) { return std::max(1, qRound(value * scale)); };
    const auto blend = [](const QColor &from, const QColor &to, qreal weight) {
        return QColor::fromRgbF(from.redF() * (1 - weight) + to.redF() * weight,
                                from.greenF() * (1 - weight) + to.greenF() * weight,
                                from.blueF() * (1 - weight) + to.blueF() * weight).name();
    };
    const auto surface = t->splits.input.background;
    const auto text = t->window.text;
    const auto accent = t->accent;
    const auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, scale);
    this->setFont(font);
    for (auto *list : {static_cast<QListWidget *>(this->ui_.accountSwitchWidget),
                      static_cast<QListWidget *>(this->ui_.kickAccountSwitcher)})
    {
        list->setFont(font);
        list->setSpacing(metric(2));
        list->setFrameShape(QFrame::NoFrame);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->verticalScrollBar()->setStyleSheet(QStringLiteral(
            "QScrollBar:vertical { background: transparent; width: %1px; }"
            "QScrollBar::handle:vertical { background: %2; border-radius: %3px; min-height: %4px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
            .arg(metric(5)).arg(blend(surface, text, .25)).arg(metric(3)).arg(metric(18)));
    }
    this->manageAccountsButton_->setFont(font);
    this->layout()->setContentsMargins(metric(8), metric(8), metric(8), metric(8));
    this->layout()->setSpacing(metric(5));
    this->setStyleSheet(uR"(
        QListView {
            color: %1;
            background: transparent;
            border: 0;
            outline: none;
        }
        QListView::item {
            padding: %2px %3px;
            border-radius: %4px;
        }
        QListView::item:hover {
            background: %5;
        }
        QListView::item:selected {
            color: %1;
            background: %6;
        }

        QPushButton#ManageAccountsButton {
            background: %7;
            color: %1;
            border: 1px solid %8;
            border-radius: %4px;
            padding: %2px %3px;
        }
        QPushButton#ManageAccountsButton:hover {
            background: %6;
            border-color: %9;
        }
        QPushButton#ManageAccountsButton:pressed {
            background: %6;
        }
    )"_s.arg(text.name()).arg(metric(4)).arg(metric(9)).arg(metric(6))
          .arg(blend(surface, accent, .10), blend(surface, accent, .22),
               surface.name(), blend(surface, text, .15), accent.name()));
    this->fitContents();
    this->update();
}

void AccountSwitchPopup::scaleChangedEvent(float scale)
{
    BaseWindow::scaleChangedEvent(scale);
    if (this->manageAccountsButton_)
    {
        this->themeChangedEvent();
    }
}

void AccountSwitchPopup::fitContents()
{
    if (!this->manageAccountsButton_)
    {
        return;
    }
    const auto scale = this->scale();
    const auto margin = this->layout()->contentsMargins();
    int contentWidth = this->manageAccountsButton_->sizeHint().width();
    int rowsHeight = 0;
    for (auto *list : {static_cast<QListWidget *>(this->ui_.accountSwitchWidget),
                      static_cast<QListWidget *>(this->ui_.kickAccountSwitcher)})
    {
        if (list == this->ui_.kickAccountSwitcher &&
            getApp()->getAccounts()->kick.accounts.empty())
        {
            continue;
        }
        const auto visibleRows = std::min(list->count(), 8);
        int height = 2 * list->spacing();
        for (int row = 0; row < list->count(); ++row)
        {
            contentWidth = std::max(contentWidth,
                list->fontMetrics().horizontalAdvance(list->item(row)->text()) +
                    qRound(30 * scale));
            if (row < visibleRows)
            {
                height += list->sizeHintForRow(row) + 2 * list->spacing();
            }
        }
        rowsHeight = std::max(rowsHeight, height);
    }
    this->ui_.accountSwitchWidget->setFixedHeight(rowsHeight);
    this->ui_.kickAccountSwitcher->setFixedHeight(rowsHeight);
    const int notebookHeight = rowsHeight + this->notebook_->headerHeightHint();
    this->notebook_->setFixedHeight(notebookHeight);
    this->layout()->invalidate();
    this->layout()->activate();
    const int width = std::max(qRound(184 * scale),
                              contentWidth + margin.left() + margin.right());
    const int height = notebookHeight + this->manageAccountsButton_->sizeHint().height() +
                       this->layout()->spacing() + margin.top() + margin.bottom();
    this->setFixedSize(width, height);
    if (this->isVisible())
    {
        this->applyLastBoundsCheck();
    }
}

void AccountSwitchPopup::refresh()
{
    this->ui_.accountSwitchWidget->refresh();
    this->ui_.kickAccountSwitcher->refresh();
    this->fitContents();
}

void AccountSwitchPopup::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(getTheme()->splits.header.border);
    painter.setBrush(getTheme()->window.background);
    const auto radius = 9 * this->scale();
    painter.drawRoundedRect(QRectF(this->rect()).adjusted(.5, .5, -.5, -.5),
                            radius, radius);
}

}  // namespace chatterino
