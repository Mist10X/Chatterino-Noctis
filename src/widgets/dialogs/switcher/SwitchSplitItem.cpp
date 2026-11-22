// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/switcher/SwitchSplitItem.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/splits/Split.hpp"

#include <QFontMetrics>

#include <algorithm>

namespace chatterino {

SwitchSplitItem::SwitchSplitItem(SplitContainer *container, Split *split)
    : AbstractSwitcherItem(QIcon(":switcher/switch.svg"))
    , container_(container)
    , split_(split)
{
    assert(this->container_ != nullptr);
}

void SwitchSplitItem::action()
{
    if (this->split_)
    {
        getApp()->getWindows()->select(this->split_);
    }
    else if (this->container_)
    {
        getApp()->getWindows()->select(this->container_);
    }
}

void SwitchSplitItem::paint(QPainter *painter, const QRect &rect) const
{
    if (this->split_)
    {
        this->paintRow(painter, rect, this->split_->getChannel()->getName(),
                       this->container_->getTab()->getTitle());
    }
    else
    {
        this->paintRow(painter, rect, this->container_->getTab()->getTitle());
    }
}

}  // namespace chatterino
