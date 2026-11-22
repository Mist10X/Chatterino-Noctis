// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/switcher/NewPopupItem.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

namespace chatterino {

NewPopupItem::NewPopupItem(const QString &channelName)
    : AbstractSwitcherItem(QIcon(":/switcher/popup.svg"))
    , channelName_(channelName)
    , text_(uiText(TEXT_FORMAT).arg(channelName))
{
}

void NewPopupItem::action()
{
    auto channel = getApp()->getTwitch()->getOrAddChannel(this->channelName_);
    getApp()->getWindows()->openInPopup(channel);
}

void NewPopupItem::paint(QPainter *painter, const QRect &rect) const
{
    this->paintRow(painter, rect, this->text_);
}

}  // namespace chatterino
