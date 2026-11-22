// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QPainter>
#include <QPixmap>

#include <algorithm>

namespace chatterino {

// Rasterize at the target screen's physical resolution, not the logical UI
// size (or an integer-rounded DPR).
inline int avatarPixelSize(qreal logicalSize, qreal devicePixelRatio)
{
    return std::max(1, qRound(logicalSize * devicePixelRatio));
}

// A premultiplied alpha mask preserves fractional coverage at the circular
// edge. A painter clip path or QRegion alone has binary pixel coverage on
// the raster paint engine, even when Antialiasing is enabled. Keep the
// original aspect ratio and crop its centre, rather than stretching faces.
// The returned pixmap is in physical pixels, at DPR 1; callers displaying
// it in a QLabel may assign the destination widget's DPR afterwards.
inline QPixmap roundAvatarPixmap(const QPixmap &source, int physicalPixels)
{
    if (source.isNull() || physicalPixels <= 0)
    {
        return {};
    }

    auto image = source.toImage();
    image.setDevicePixelRatio(1.0);
    image = image.scaled(physicalPixels, physicalPixels,
                         Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
    if (image.isNull())
    {
        return {};
    }

    QImage result(physicalPixels, physicalPixels,
                  QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHints(QPainter::Antialiasing |
                           QPainter::SmoothPixmapTransform);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(0, 0, physicalPixels, physicalPixels));
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.drawImage(QPointF((physicalPixels - image.width()) / 2.0,
                              (physicalPixels - image.height()) / 2.0),
                      image);
    painter.end();
    return QPixmap::fromImage(result);
}

}  // namespace chatterino
