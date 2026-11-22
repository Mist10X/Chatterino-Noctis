// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/switcher/NewTabItem.hpp"

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

NewTabItem::NewTabItem(Window *window_, const QString &channelName)
    : AbstractSwitcherItem(QIcon(":/switcher/plus.svg"))
    , channelName_(channelName)
    , text_(uiText(TEXT_FORMAT).arg(channelName))
    , window(window_)
{
}

void NewTabItem::action()
{
    auto &nb = this->window->getNotebook();
    SplitContainer *container = nb.addPage(true);

    Split *split = new Split(container);
    split->setChannel(
        getApp()->getTwitch()->getOrAddChannel(this->channelName_));
    container->insertSplit(split);
}

void NewTabItem::paint(QPainter *painter, const QRect &rect) const
{
    this->paintRow(painter, rect, this->text_);
}

}  // namespace chatterino
