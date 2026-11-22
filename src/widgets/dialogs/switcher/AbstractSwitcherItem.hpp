// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/listview/GenericListItem.hpp"

#include <algorithm>

namespace chatterino {

class AbstractSwitcherItem : public GenericListItem
{
public:
    using GenericListItem::GenericListItem;

    QSize sizeHint(const QRect &rect) const override
    {
        const auto scale = std::max(1.F, getSettings()->getClampedUiScale());
        const auto font = getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale);
        return {rect.width(), std::max(qRound(ICON_SIZE.height() * scale),
                                       QFontMetrics(font).height() + qRound(4 * scale))};
    }

protected:
    void paintRow(QPainter *painter, const QRect &rect, const QString &title,
                  const QString &detail = {}) const
    {
        const auto scale = std::max(1.F, getSettings()->getClampedUiScale());
        auto *fonts = getApp()->getFonts();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(getApp()->getThemes()->splits.header.text);
        painter->setFont(fonts->getFont(FontStyle::UiMediumBold, scale));
        const QSize iconSize = ICON_SIZE * scale;
        const QRect iconRect(rect.x(), rect.y() + (rect.height() - iconSize.height()) / 2,
                             iconSize.width(), iconSize.height());
        this->icon_.paint(painter, iconRect);
        auto textRect = rect.adjusted(iconSize.width() + qRound(4 * scale), 0,
                                      -qRound(4 * scale), 0);
        const auto titleText = painter->fontMetrics().elidedText(
            title, Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, titleText);
        if (!detail.isEmpty())
        {
            textRect.adjust(painter->fontMetrics().horizontalAdvance(titleText) +
                                qRound(6 * scale), 0, 0, 0);
            if (textRect.width() > 0)
            {
                painter->setFont(fonts->getFont(FontStyle::UiMedium, scale));
                painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter,
                                  painter->fontMetrics().elidedText(
                                      detail, Qt::ElideRight, textRect.width()));
            }
        }
        painter->restore();
    }
};

}
