#include "providers/seventv/paints/Paint.hpp"

#include <cmath>

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <private/qpixmapfilter_p.h>
#include <QFontMetricsF>
#include <QImage>
#include <QLabel>
#include <QPainter>

#include <algorithm>

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

namespace {

qreal textBaseline(const QFont &font, const QRectF &rect)
{
    const QFontMetricsF metrics(font);

    return rect.bottom() - metrics.descent();
}

/// The drop-shadow filter returns a flattened foreground-over-shadow image.
/// Interpolate it with the original glyph image in premultiplied alpha space
/// wherever the widget would cut the glow. Unlike fading the whole pixmap,
/// this cannot make the actual lettering transparent at the top/bottom edge.
void featherClippedShadows(QPixmap &composite, const QPixmap &foreground,
                           const QRectF &clip, qreal pad, qreal glyphHeight)
{
    const auto ratio = composite.devicePixelRatio();
    auto image = composite.toImage().convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    const auto original = foreground.toImage().convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    const auto region = clip.translated(pad, pad).intersected(
        QRectF(0, 0, image.width() / ratio, image.height() / ratio));
    const auto feather = std::max<qreal>(2, glyphHeight * 0.28);

    auto blend = [](int original, int composite, int amount) {
        return (original * (256 - amount) + composite * amount + 128) / 256;
    };
    for (int y = 0; y < image.height(); ++y)
    {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        const auto *glyphRow =
            reinterpret_cast<const QRgb *>(original.constScanLine(y));
        const qreal ly = (y + 0.5) / ratio;
        const qreal dy = std::min(ly - region.top(), region.bottom() - ly);
        for (int x = 0; x < image.width(); ++x)
        {
            const qreal lx = (x + 0.5) / ratio;
            const qreal distance = std::min(
                dy, std::min(lx - region.left(), region.right() - lx));
            const qreal t = std::clamp(
                (distance - 0.5 / ratio) / feather, qreal(0), qreal(1));
            if (t == 1)
            {
                continue;
            }
            if (t == 0)
            {
                row[x] = glyphRow[x];
                continue;
            }

            const int amount = qRound(t * t * (3 - 2 * t) * 256);
            const QRgb glyph = glyphRow[x];
            const QRgb shadowed = row[x];
            row[x] = qRgba(blend(qRed(glyph), qRed(shadowed), amount),
                           blend(qGreen(glyph), qGreen(shadowed), amount),
                           blend(qBlue(glyph), qBlue(shadowed), amount),
                           blend(qAlpha(glyph), qAlpha(shadowed), amount));
        }
    }
    composite = QPixmap::fromImage(std::move(image));
    composite.setDevicePixelRatio(ratio);
}

}  // namespace

QPixmap Paint::getPixmap(const QString &text, const QFont &font,
                         QColor userColor, QSizeF size, float scale, float dpr,
                         bool centerVertically, qreal *margin,
                         const QRectF *shadowClip) const
{
    QSizeF drawSize = size;
    qreal pad = 0;
    if (margin != nullptr && getSettings()->displaySevenTVPaintShadows)
    {
        for (const auto &shadow : this->getDropShadows())
        {
            if (shadow.isValid())
            {
                pad = std::max<qreal>(
                    pad, shadow.scaled(scale / dpr).extentAround());
            }
        }
        pad = std::ceil(pad);
        *margin = pad;
    }
    if (centerVertically && margin == nullptr &&
        getSettings()->displaySevenTVPaintShadows)
    {
        float shadowExtent = 0;
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }

            shadowExtent = std::max(shadowExtent,
                                    shadow.scaled(scale / dpr).extentBelow());
        }

        drawSize.setHeight(size.height() + shadowExtent);
    }

    const QSizeF canvasSize(drawSize.width() + 2 * pad,
                            drawSize.height() + 2 * pad);
    QPixmap pixmap((canvasSize * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter pixmapPainter(&pixmap);
    pixmapPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    pixmapPainter.setFont(font);
    pixmapPainter.translate(pad, pad);

    const QRectF pixmapRect(QPointF{}, drawSize);
    const QRectF textRect(QPointF{}, size);

    // NOTE: draw colon separately from the nametag
    // otherwise the paint would extend onto the colon
    bool drawColon = false;
    QRectF nametagBoundingRect = centerVertically ? textRect : pixmapRect;
    QString nametagText = text;
    if (nametagText.endsWith(':'))
    {
        drawColon = true;
        nametagText = nametagText.chopped(1);
        const auto textBounds = pixmapPainter.boundingRect(
            QRectF(0, 0, 10000, 10000), nametagText,
            QTextOption(Qt::AlignLeft | Qt::AlignTop));
        nametagBoundingRect =
            QRectF(0, 0, textBounds.width(),
                   centerVertically ? textRect.height() : pixmapRect.height());
    }

    QPen pen;
    const QBrush brush = this->asBrush(userColor, nametagBoundingRect);
    pen.setBrush(brush);
    pixmapPainter.setPen(pen);

    if (centerVertically)
    {
        pixmapPainter.drawText(nametagBoundingRect, nametagText,
                               QTextOption(Qt::AlignLeft | Qt::AlignVCenter));
    }
    else
    {
        const auto baseline = textBaseline(font, nametagBoundingRect);
        pixmapPainter.drawText(QPointF(nametagBoundingRect.left(), baseline),
                               nametagText);
    }
    pixmapPainter.end();

    if (!this->getDropShadows().empty() &&
        getSettings()->displaySevenTVPaintShadows)
    {
        const QPixmap foreground = shadowClip ? pixmap : QPixmap{};
        QPixmap outMap((canvasSize * dpr).toSize());
        outMap.setDevicePixelRatio(dpr);
        bool shadowDrawn = false;
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }
            outMap.fill(Qt::transparent);

            {
                QPainter outPainter(&outMap);
                auto scaled = shadow.scaled(
                    scale / static_cast<float>(outMap.devicePixelRatio()));

                QPixmapDropShadowFilter filter;
                scaled.apply(filter);
                filter.draw(&outPainter, {0, 0}, pixmap);
            }
            outMap.swap(pixmap);
            shadowDrawn = true;
        }
        if (shadowClip != nullptr && shadowDrawn)
        {
            featherClippedShadows(pixmap, foreground, *shadowClip, pad,
                                  size.height());
        }
    }

    if (drawColon)
    {
        auto colonColor = getApp()->getThemes()->messages.textColors.regular;
        const auto baseline = textBaseline(font, nametagBoundingRect);

        pixmapPainter.begin(&pixmap);
        pixmapPainter.translate(pad, pad);

        pixmapPainter.setPen(QPen(colonColor));
        pixmapPainter.setFont(font);

        pixmapPainter.drawText(QPointF(nametagBoundingRect.right(), baseline),
                               u":"_s);
        pixmapPainter.end();
    }

    return pixmap;
}

QColor Paint::overlayColors(QColor background, QColor foreground)
{
    auto alpha = foreground.alphaF();

    auto r = ((1 - alpha) * static_cast<float>(background.red())) +
             (alpha * static_cast<float>(foreground.red()));
    auto g = ((1 - alpha) * static_cast<float>(background.green())) +
             (alpha * static_cast<float>(foreground.green()));
    auto b = ((1 - alpha) * static_cast<float>(background.blue())) +
             (alpha * static_cast<float>(foreground.blue()));

    return {static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
}

qreal Paint::offsetRepeatingStopPosition(const qreal position,
                                         const QGradientStops &stops)
{
    const qreal gradientStart = stops.first().first;
    const qreal gradientEnd = stops.last().first;
    const qreal gradientLength = gradientEnd - gradientStart;
    const qreal offsetPosition = (position - gradientStart) / gradientLength;

    return offsetPosition;
}

}  // namespace chatterino
