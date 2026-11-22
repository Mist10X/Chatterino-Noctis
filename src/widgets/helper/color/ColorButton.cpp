// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/color/ColorButton.hpp"

#include "widgets/helper/color/Checkerboard.hpp"

#include <QPixmap>

namespace chatterino {

ColorButton::ColorButton(QColor color, QWidget *parent)
    : QAbstractButton(parent)
    , currentColor_(color)
{
    this->setSizePolicy({QSizePolicy::Expanding, QSizePolicy::Expanding});
    this->setMinimumSize({30, 30});
}

QSize ColorButton::sizeHint() const
{
    return {50, 30};
}

void ColorButton::setColor(const QColor &color)
{
    if (this->currentColor_ == color)
    {
        return;
    }

    this->currentColor_ = color;
    this->update();
}

QColor ColorButton::color() const
{
    return this->currentColor_;
}

void ColorButton::resizeEvent(QResizeEvent *)
{
    this->checkerboardCacheValid_ = false;
    this->repaint();
}

void ColorButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    auto rect = this->rect();

    if (this->currentColor_.alpha() != 255)
    {
        const qreal dpr = this->devicePixelRatioF();
        if (!this->checkerboardCacheValid_ ||
            !qFuzzyCompare(this->checkerboardCache_.devicePixelRatio(), dpr))
        {
            const auto pixels = this->size() * dpr;
            QPixmap cache(pixels);
            cache.setDevicePixelRatio(dpr);
            cache.fill(Qt::transparent);

            QPainter cachePainter(&cache);
            drawCheckerboard(cachePainter, this->size(),
                             std::min(this->height() / 2, 10));

            // Make fractional alpha at the corners rather than clipping the
            // checkerboard through a rasterized path at logical resolution.
            QPixmap mask(pixels);
            mask.setDevicePixelRatio(dpr);
            mask.fill(Qt::transparent);
            QPainter maskPainter(&mask);
            maskPainter.setRenderHint(QPainter::Antialiasing);
            maskPainter.setPen(Qt::NoPen);
            maskPainter.setBrush(Qt::white);
            maskPainter.drawRoundedRect(QRectF(1, 1, this->width() - 2,
                                               this->height() - 2), 5, 5);
            maskPainter.end();
            cachePainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            cachePainter.drawPixmap(QPointF(0, 0), mask);
            cachePainter.end();

            this->checkerboardCache_ = std::move(cache);
            this->checkerboardCacheValid_ = true;
        }
        painter.drawPixmap(rect.topLeft(), this->checkerboardCache_);
    }
    painter.setBrush(this->currentColor_);
    painter.setPen({QColor(255, 255, 255, 127), 1});
    painter.drawRoundedRect(rect.x() + 1, rect.y() + 1, rect.width() - 2,
                            rect.height() - 2, 5, 5);
}

}  // namespace chatterino
