#include "widgets/splits/InputCompletionItem.hpp"

#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <algorithm>

namespace chatterino {

InputCompletionItem::InputCompletionItem(const EmotePtr &emote,
                                         const QString &text,
                                         ActionCallback action)
    : emote_(emote)
    , text_(text)
    , action_(action)
{
}

void InputCompletionItem::action()
{
    if (this->action_)
    {
        if (this->emote_)
        {
            this->action_(this->emote_->name.string);
        }
        else
        {
            this->action_(this->text_);
        }
    }
}

void InputCompletionItem::paint(QPainter *painter, const QRect &rect) const
{
    const auto margin = 7;
    QRect textRect;
    if (this->emote_)
    {
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->setRenderHint(QPainter::Antialiasing);

        const auto imageHeight = std::max(1, rect.height() - 8);

        QRect iconRect{
            rect.topLeft() + QPoint{margin, 4},
            QSize{imageHeight, imageHeight},
        };

        if (auto image = this->emote_->images.getImage(2))
        {
            if (auto pixmap = image->pixmapOrLoad())
            {
                if (image->height() > 0 && image->width() > 0)
                {
                    auto aspectRatio =
                        double(image->width()) / double(image->height());

                    const int width = std::clamp(int(imageHeight * aspectRatio), 1,
                                                  std::max(1, rect.width() / 2));
                    const int height = std::clamp(int(width / aspectRatio), 1, imageHeight);
                    iconRect = {QPoint(rect.left() + margin, rect.center().y() - height / 2),
                                QSize(width, height)};
                    painter->drawPixmap(iconRect, *pixmap);
                }
            }
        }

        textRect = rect.adjusted(iconRect.width() + margin * 2, 0, -margin, 0);
    }
    else
    {
        textRect = rect.adjusted(margin, 0, -margin, 0);
    }

    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      painter->fontMetrics().elidedText(this->text_, Qt::ElideRight, std::max(0, textRect.width())));
}

QSize InputCompletionItem::sizeHint(const QRect &rect) const
{
    return QSize(rect.width(), ICON_SIZE.height());
}

}  // namespace chatterino
