// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/listview/GenericItemDelegate.hpp"

#include "widgets/listview/GenericListItem.hpp"

#include <QAbstractItemView>

#include <algorithm>

namespace chatterino {

SwitcherItemDelegate::SwitcherItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

SwitcherItemDelegate::~SwitcherItemDelegate()
{
}

void SwitcherItemDelegate::paint(QPainter *painter,
                                 const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    auto *item = GenericListItem::fromVariant(index.data());

    if (item)
    {
        const bool completion = this->parent() &&
            this->parent()->property("noctisInputCompletion").toBool();
        painter->save();
        painter->setFont(option.font);
        painter->setPen(option.palette.text().color());
        if (option.state & QStyle::State_Selected)
        {
            if (completion)
            {
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(Qt::NoPen);
                painter->setBrush(option.palette.highlight());
                painter->drawRoundedRect(QRectF(option.rect).adjusted(1, 1, -1, -1), 6, 6);
                painter->setPen(option.palette.text().color());
            }
            else
            {
                painter->fillRect(option.rect, option.palette.highlight());
            }
        }

        item->paint(painter, option.rect);
        painter->restore();
    }
    else
    {
        QStyledItemDelegate::paint(painter, option, index);
    }
}

QSize SwitcherItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                     const QModelIndex &index) const
{
    auto *item = GenericListItem::fromVariant(index.data());

    if (item)
    {
        QRect rect = option.rect;
        if (auto *view =
                qobject_cast<const QAbstractItemView *>(this->parent()))
        {
            const int viewportWidth = view->viewport()->width();
            if (viewportWidth > 0)
            {
                rect.setWidth(viewportWidth);
            }
        }

        auto size = item->sizeHint(rect);
        if (this->parent() && this->parent()->property("noctisInputCompletion").toBool())
            size.setHeight(std::max(size.height(), option.fontMetrics.height() + 8));
        return size;
    }

    return QStyledItemDelegate::sizeHint(option, index);
}

}  // namespace chatterino
