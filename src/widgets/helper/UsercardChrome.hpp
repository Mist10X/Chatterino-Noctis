// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QImage>
#include <QPaintDevice>
#include <QPainter>
#include <QLinearGradient>
#include <QPen>
#include <QRect>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace chatterino::usercardChrome {

inline int shadowInset(float scale)
{
    return std::max(8, qRound(12.F * scale));
}

inline qreal bodyRadius(float scale)
{
    return 20.0 * std::max(0.75F, scale);
}

// All opaque child surfaces must lie inside the rounded body, including their
// four rectangular corners. At the corner of a square inset d, the distance
// from the circle center is sqrt(2)*(radius-d). Reserve another 1.5 logical
// px for the antialiased rim. Changing the radius automatically changes this
// constraint rather than letting a banner or viewport protrude again.
inline int contentInset(float scale, int minimumEdge)
{
    const auto corner = int(std::ceil(bodyRadius(scale) * (1 - 1 / std::sqrt(2.0)) + 1.5 * scale));
    return shadowInset(scale) + std::max(minimumEdge, corner);
}

struct Palette {
    QString profile, messages, foreground, border, accent, cover, panel, muted,
            disabled, scroll;
};

inline Palette palette(bool light)
{
    if (light)
        return {"#f0f0f4", "#ffffff", "#292631", "#d7d3de", "#684aa0", "#e1d9ef",
                "#ffffff", "#6b6577", "#99939f", "#c3b8d1"};
    // Cold graphite, subtly violet; keep the profile and chat surfaces close.
    return {"#1a1b23", "#14151b", "#ded9ee", "#32323f", "#b59be5", "#29243a",
            "#20212a", "#a7a3b1", "#74717f", "#51495e"};
}

// The inset cover has no separate frame: fade its pigment/image into the
// common surface instead of adding another rounded-rectangle silhouette.
inline qreal coverFeather(float scale)
{
    return std::max(4.0, 10.0 * double(scale));
}

namespace detail {

// Signed distance to a rounded rectangle, zero on the true curving boundary.
// In the straight parts only one axis contributes; in the corners the same
// function gives a radial falloff without concentric rings or square joins.
inline qreal roundedDistance(qreal x, qreal y, const QRectF &body, qreal radius)
{
    const auto center = body.center();
    const qreal dx = std::abs(x - center.x()) - (body.width() / 2 - radius);
    const qreal dy = std::abs(y - center.y()) - (body.height() / 2 - radius);
    return std::hypot(std::max(0.0, dx), std::max(0.0, dy)) +
           std::min(std::max(dx, dy), 0.0) - radius;
}

inline QImage rasterShadow(QSize pixels, qreal inset, qreal radius,
                           qreal offset, bool light)
{
    QImage result(pixels, QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        return result;

    const auto body = QRectF(0, 0, pixels.width(), pixels.height())
                          .adjusted(inset, inset, -inset, -inset)
                          .translated(0, offset);
    // Less than the remaining space below the shifted card. At *every* image
    // edge, even the bottom one, the shadow has already reached alpha zero.
    // The underlying alpha-one hit region is the only thing left there.
    const qreal reach = std::max(0.5, inset - offset - 1.25);
    const qreal peak = light ? 80.0 : 96.0;
    for (int y = 0; y < pixels.height(); ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < pixels.width(); ++x)
        {
            const qreal distance = roundedDistance(x + 0.5, y + 0.5, body, radius);
            const qreal t = 1 - std::clamp(distance / reach, 0.0, 1.0);
            // Smoothstep: no derivative discontinuity where the shadow
            // begins or ends. One pigment value per pixel, no stacked rings.
            const int alpha = qRound(peak * t * t * (3 - 2 * t));
            line[x] = qRgba(0, 0, 0, alpha);
        }
    }
    return result;
}

inline void paintShadow(QPainter &painter, QRect widgetRect, qreal inset,
                        qreal radius, float scale, bool light)
{
    const qreal ratio = std::max(1.0, painter.device()->devicePixelRatioF());
    const qreal offset = std::max(1.5, 2.5 * double(scale));
    const qreal physicalInset = inset * ratio;
    const qreal physicalRadius = radius * ratio;
    const qreal physicalOffset = offset * ratio;
    // The tile includes both the complete rounded corner and the translated
    // shadow. Beyond it the distance field is constant along the edge, so a
    // single pixel can be stretched between corners during resize/animation.
    const int cornerPixels = int(std::ceil(physicalInset + physicalRadius +
                                           physicalOffset + 2));
    const qreal corner = cornerPixels / ratio;
    const QRectF target(widgetRect);
    if (target.width() <= 2 * corner || target.height() <= 2 * corner)
    {
        // Not normally reached (the card's minimum size exceeds the tile),
        // but retain exact geometry for very small test/preview surfaces.
        const auto image = rasterShadow(
            QSize(std::max(1, qRound(target.width() * ratio)),
                  std::max(1, qRound(target.height() * ratio))),
            physicalInset, physicalRadius, physicalOffset, light);
        painter.drawImage(target, image);
        return;
    }

    struct ShadowTile {
        QImage image;
        qreal inset = 0, radius = 0, offset = 0;
        bool light = false;
    };
    // The only expensive operation is the small tile when scale/DPR changes.
    // All other paints, including badge-height animation, reuse its pixels.
    static thread_local ShadowTile tile;
    const int tileSize = 2 * cornerPixels + 1;
    if (tile.image.size() != QSize(tileSize, tileSize) ||
        tile.inset != physicalInset || tile.radius != physicalRadius ||
        tile.offset != physicalOffset || tile.light != light)
    {
        tile.image = rasterShadow(QSize(tileSize, tileSize), physicalInset,
                                  physicalRadius, physicalOffset, light);
        tile.inset = physicalInset;
        tile.radius = physicalRadius;
        tile.offset = physicalOffset;
        tile.light = light;
    }
    if (tile.image.isNull())
        return;

    const qreal xs[] = {target.left(), target.left() + corner,
                        target.right() - corner, target.right()};
    const qreal ys[] = {target.top(), target.top() + corner,
                        target.bottom() - corner, target.bottom()};
    const qreal src[] = {0.0, qreal(cornerPixels), qreal(cornerPixels + 1),
                         qreal(tileSize)};
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
        {
            // The centre is wholly beneath the opaque card. No need to draw
            // it, or to allocate a full-size shadow image on each resize.
            if (row == 1 && column == 1)
                continue;
            painter.drawImage(QRectF(xs[column], ys[row],
                                     xs[column + 1] - xs[column],
                                     ys[row + 1] - ys[row]),
                              tile.image,
                              QRectF(src[column], src[row],
                                     src[column + 1] - src[column],
                                     src[row + 1] - src[row]));
        }
}

}  // namespace detail

// Draw the complete frameless surface behind its (ordinary QWidget) children.
// No widget mask is needed: the translucent backing surface provides
// antialiased fractional-pixel coverage instead of a binary window region.
inline void paint(QPainter &painter, QRect widgetRect, float scale,
                  const Palette &colors, bool light, Qt::Orientation orientation,
                  qreal partition)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    // Source also clears stale pixels when the window moves/resizes. The
    // imperceptible one-alpha fill keeps the outer resize band hit-testable on
    // Windows, where fully transparent layered-window pixels pass clicks
    // through to underlying windows.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(widgetRect, QColor(0, 0, 0, 1));
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const int inset = shadowInset(scale);
    const QRectF body = QRectF(widgetRect).adjusted(inset, inset, -inset,
                                                   -inset);
    if (body.isEmpty())
    {
        painter.restore();
        return;
    }

    const qreal radius = std::min({bodyRadius(scale), body.width() / 2,
                                   body.height() / 2});
    detail::paintShadow(painter, widgetRect, inset, radius, scale, light);

    // The shell owns the whole background. Paint both sides of the card as a
    // single rounded surface: no rectangular strips outside the rim and no
    // unrelated OS-gray background when margins or radius change.
    const qreal start = orientation == Qt::Horizontal ? body.left() : body.top();
    const qreal length = orientation == Qt::Horizontal ? body.width() : body.height();
    const qreal split = std::clamp((partition - start) / length, 0.0, 1.0);
    const qreal blend = 0.8 / length;
    QLinearGradient surface(body.topLeft(), orientation == Qt::Horizontal ? body.topRight() : body.bottomLeft());
    surface.setColorAt(0, QColor(colors.profile));
    surface.setColorAt(std::max(0.0, split - blend), QColor(colors.profile));
    surface.setColorAt(std::min(1.0, split + blend), QColor(colors.messages));
    surface.setColorAt(1, QColor(colors.messages));
    painter.setPen(Qt::NoPen);
    painter.setBrush(surface);
    painter.drawRoundedRect(body, radius, radius);
    painter.setBrush(Qt::NoBrush);
    // A restrained lighter upper edge and darker lower edge create depth
    // without tinting the contents or changing the chat's palette.
    QLinearGradient rim(body.topLeft(), body.bottomRight());
    rim.setColorAt(0, QColor(light ? "#d9d5e2" : "#494453"));
    rim.setColorAt(0.42, QColor(light ? "#ccc7d5" : "#3a3645"));
    rim.setColorAt(1, QColor(light ? "#bfb9ca" : "#2c2936"));
    painter.setPen(QPen(QBrush(rim), 1.0));
    const qreal strokeRadius = std::max(0.0, radius - 0.5);
    painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), strokeRadius,
                            strokeRadius);
    painter.restore();
}

}  // namespace chatterino::usercardChrome
