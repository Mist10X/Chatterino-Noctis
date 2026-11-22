// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/UiLanguage.hpp"
#include "widgets/dialogs/ReplyThreadPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageThread.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/UsercardResizeGrip.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>

const QString TEXT_TITLE("Reply Thread - @%1 in %2");

namespace chatterino {

ReplyThreadPopup::ReplyThreadPopup(bool closeAutomatically, Split *split)
    : DraggablePopup(closeAutomatically, split)
    , split_(split)
    , frameless_(closeAutomatically)
{
    assert(split != nullptr);

    this->setWindowTitle(uiText(QStringLiteral("Reply Thread")));
    if (this->frameless_)
    {
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->setAutoFillBackground(false);
    }

    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](std::vector<QString>) -> QString {
             this->deleteLater();
             return "";
         }},
        {"scrollPage",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             auto direction = arguments.at(0);

             auto &scrollbar = this->ui_.threadView->getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"pin",
         [this](std::vector<QString> /*arguments*/) -> QString {
             this->togglePinned();
             return "";
         }},

        // these actions make no sense in the context of a reply thread, so they aren't implemented
        {"execModeratorAction", nullptr},
        {"openProfilePictureMenu", nullptr},
        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"search", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);

    // initialize UI
    this->ui_.threadView =
        new ChannelView(this, this->split_, ChannelView::Context::ReplyThread);
    this->ui_.threadView->setMinimumSize(250, 90);
    this->ui_.threadView->setSizePolicy(QSizePolicy::Expanding,
                                        QSizePolicy::Expanding);
    // We can safely ignore this signal's connection since threadView will always be deleted before
    // the ReplyThreadPopup
    std::ignore =
        this->ui_.threadView->mouseDown.connect([this](QMouseEvent *) {
            this->giveFocus(Qt::MouseFocusReason);
        });

    // Let the parent split handle twitch link actions
    std::ignore = this->ui_.threadView->openChannelIn.connect(
        [this](QString channelName, FromTwitchLinkOpenChannelIn openIn) {
            this->split_->getChannelView().openChannelIn.invoke(
                std::move(channelName), openIn);
        });

    // Create SplitInput with inline replying disabled
    this->ui_.replyInput =
        new SplitInput(this, this->split_, this->ui_.threadView, false);

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->updateInputUI();
        }));

    // We can safely ignore this signal's connection since threadView will always be deleted before
    // the ReplyThreadPopup
    std::ignore = this->ui_.threadView->selectionChanged.connect([this]() {
        // clear SplitInput selection when selecting in ChannelView
        if (this->ui_.replyInput->hasSelection())
        {
            this->ui_.replyInput->clearSelection();
        }
    });

    // clear ChannelView selection when selecting in SplitInput
    // We can safely ignore this signal's connection since replyInput will always be deleted before
    // the ReplyThreadPopup
    std::ignore = this->ui_.replyInput->selectionChanged.connect([this]() {
        if (this->ui_.threadView->hasSelection())
        {
            this->ui_.threadView->clearSelection();
        }
    });

    auto layers = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QGridLayout>()
                      .withoutMargin();
    auto layout = layers.emplace<QVBoxLayout>();

    layout->setSpacing(6);
    auto marginPx = closeAutomatically ? 10 : 7;
    layout->setContentsMargins(marginPx, marginPx, marginPx, marginPx);

    // Top Row
    bool addCheckbox = getSettings()->enableThreadHighlight;
    {
        auto *hbox = new QHBoxLayout();
        this->ui_.title = new QLabel(uiText("Reply Thread"), this);
        hbox->addWidget(this->ui_.title);
        hbox->addStretch(1);

        if (addCheckbox)
        {
            this->ui_.notificationCheckbox =
                new QCheckBox(uiText("Subscribe to thread"), this);
            QObject::connect(this->ui_.notificationCheckbox,
                             &QCheckBox::toggled, [this](bool checked) {
                                 if (!this->thread_ ||
                                     this->thread_->subscribed() == checked)
                                 {
                                     return;
                                 }

                                 if (checked)
                                 {
                                     this->thread_->markSubscribed();
                                 }
                                 else
                                 {
                                     this->thread_->markUnsubscribed();
                                 }
                             });
            hbox->addWidget(this->ui_.notificationCheckbox);
            this->ui_.notificationCheckbox->setFocusPolicy(Qt::NoFocus);
        }

        if (closeAutomatically)
        {
            auto *pin = this->createPinButton();
            pin->setObjectName("ReplyThreadPin");
            hbox->addWidget(pin, 0, Qt::AlignRight);
            hbox->setContentsMargins(0, 0, 0, 5);
        }
        else
        {
            hbox->setContentsMargins(10, 0, 0, 4);
        }

        layout->addLayout(hbox);
    }

    layout->addWidget(this->ui_.threadView, 1);
    layout->addWidget(this->ui_.replyInput);

    if (closeAutomatically)
    {
        this->resizeGrip_ = new UsercardResizeGrip(this);
        this->resizeGrip_->syncGeometry(this->scale());
    }

    // Roomier than the old minimum, and the size the user last chose.
    const int width = getSettings()->replyThreadPopupWidth.getValue();
    const int height = getSettings()->replyThreadPopupHeight.getValue();
    // setScaleIndependentSize() fixes both min and max dimensions. A resize
    // grip cannot change a fixed-size window, even when its cursor is visible.
    this->setMinimumSize(qRound(320 * this->scale()), qRound(200 * this->scale()));
    this->resize(qRound((width >= 320 ? width : 520) * this->scale()),
                 qRound((height >= 200 ? height : 420) * this->scale()));
    this->refreshStyle();
}

void ReplyThreadPopup::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
    if (this->resizeGrip_)
        this->resizeGrip_->syncGeometry(this->scale());
    if (this->isVisible())
    {
        const auto scale = std::max(0.1F, this->scale());
        getSettings()->replyThreadPopupWidth = int(this->width() / scale);
        getSettings()->replyThreadPopupHeight = int(this->height() / scale);
    }
}

void ReplyThreadPopup::paintEvent(QPaintEvent *event)
{
    if (!this->frameless_)
    {
        DraggablePopup::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(this->theme->splits.header.border);
    painter.setBrush(this->theme->window.background);
    const auto radius = 10 * this->scale();
    painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), radius, radius);
}

void ReplyThreadPopup::refreshStyle()
{
    if (!this->ui_.title)
        return;
    const auto fontScale = std::max(.85F, this->scale());
    auto *fonts = getApp()->getFonts();
    this->ui_.title->setFont(fonts->getFont(FontStyle::ChatMediumBold, fontScale));
    this->ui_.title->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                      .arg(this->theme->window.text.name()));
    if (auto *checkbox = this->ui_.notificationCheckbox)
    {
        checkbox->setPalette(moltorinoDialogPalette(*this->theme));
        checkbox->setFont(fonts->getFont(FontStyle::ChatMediumSmall, fontScale));
        checkbox->setStyleSheet(QStringLiteral(R"(
            QCheckBox { color: %1; background: transparent; spacing: 6px; }
            QCheckBox::indicator { width: 13px; height: 13px; border-radius: 3px;
                                    border: 1px solid %2; background: %3; }
            QCheckBox::indicator:checked { image: url(:/buttons/menu-check-%5.svg);
                                           border-color: %4; }
            QCheckBox::indicator:hover { border-color: %4; }
        )").arg(this->theme->window.text.name(), this->theme->tabs.dividerLine.name(),
                this->theme->splits.input.background.name(), this->theme->accent.name(),
                this->theme->isLightTheme() ? "light" : "dark"));
    }
    this->update();
}

void ReplyThreadPopup::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void ReplyThreadPopup::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->setMinimumSize(qRound(320 * scale), qRound(200 * scale));
    this->refreshStyle();
    if (this->resizeGrip_)
        this->resizeGrip_->syncGeometry(scale);
}

void ReplyThreadPopup::setThread(std::shared_ptr<MessageThread> thread,
                                 std::weak_ptr<Channel> channel)
{
    this->thread_ = std::move(thread);
    this->ui_.replyInput->setReply(this->thread_->root(), std::move(channel));
    this->addMessagesFromThread();
    this->updateInputUI();

    if (!this->thread_) [[unlikely]]
    {
        this->replySubscriptionSignal_ = boost::signals2::scoped_connection{};
        return;
    }

    auto updateCheckbox = [this]() {
        if (this->ui_.notificationCheckbox)
        {
            this->ui_.notificationCheckbox->setChecked(
                this->thread_->subscribed());
        }
    };
    updateCheckbox();

    this->replySubscriptionSignal_ =
        this->thread_->subscriptionUpdated.connect(updateCheckbox);
}

void ReplyThreadPopup::addMessagesFromThread()
{
    this->ui_.threadView->clearMessages();
    if (!this->thread_)
    {
        return;
    }

    const auto &sourceChannel = this->split_->getChannel();
    this->setWindowTitle(TEXT_TITLE.arg(this->thread_->root()->loginName,
                                        sourceChannel->getName()));

    if (sourceChannel->isTwitchChannel())
    {
        this->virtualChannel_ =
            std::make_shared<TwitchChannel>(sourceChannel->getName());
    }
    else
    {
        this->virtualChannel_ = std::make_shared<Channel>(
            sourceChannel->getName(), Channel::Type::None);
    }

    auto rootOverrideFlags =
        std::optional<MessageFlags>(this->thread_->root()->flags);
    rootOverrideFlags->set(MessageFlag::DoNotLog);

    this->virtualChannel_->addMessage(
        this->thread_->root(), MessageContext::Repost, rootOverrideFlags);
    for (const auto &msgRef : this->thread_->replies())
    {
        if (auto msg = msgRef.lock())
        {
            auto overrideFlags = std::optional<MessageFlags>(msg->flags);
            overrideFlags->set(MessageFlag::DoNotLog);

            this->virtualChannel_->addMessage(msg, MessageContext::Repost,
                                              overrideFlags);
        }
    }

    this->ui_.threadView->setChannel(this->virtualChannel_);
    this->ui_.threadView->setSourceChannel(sourceChannel);

    this->messageConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            sourceChannel->messageAppended.connect(
                [this](MessagePtr &message, auto) {
                    if (message->replyThread == this->thread_)
                    {
                        auto overrideFlags =
                            std::optional<MessageFlags>(message->flags);
                        overrideFlags->set(MessageFlag::DoNotLog);

                        // same reply thread, add message
                        this->virtualChannel_->addMessage(
                            message, MessageContext::Repost, overrideFlags);
                    }
                }));
}

void ReplyThreadPopup::updateInputUI()
{
    auto channel = this->split_->getSelectedChannel();
    // Bail out if not a twitch channel.
    // Special twitch channels will hide their reply input box.
    if (!channel || !channel->isTwitchOrKickChannel())
    {
        return;
    }

    this->ui_.replyInput->setVisible(channel->isWritable());

    QString name;
    if (channel->isTwitchChannel())
    {
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        if (!user->isAnon())
        {
            name = user->getDisplayName();
        }
    }
    else
    {
        auto user = getApp()->getAccounts()->kick.current();
        if (!user->isAnonymous())
        {
            name = user->username();
        }
    }
    QString placeholderText;
    if (name.isEmpty())
    {
        placeholderText = uiText(QStringLiteral("Log in to send messages..."));
    }
    else
    {
        placeholderText = uiText(QStringLiteral("Reply as %1...")).arg(name);
    }

    this->ui_.replyInput->setPlaceholderText(placeholderText);
}

void ReplyThreadPopup::giveFocus(Qt::FocusReason reason)
{
    this->ui_.replyInput->giveFocus(reason);
}

void ReplyThreadPopup::focusInEvent(QFocusEvent *event)
{
    this->giveFocus(event->reason());
}

}  // namespace chatterino
