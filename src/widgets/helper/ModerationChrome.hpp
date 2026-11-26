#pragma once

#include "common/UiLanguage.hpp"
#include "singletons/Fonts.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetricsF>
#include <algorithm>

namespace chatterino::moderationchrome {

enum class Kind { Timeout, Ban, Unban, CrossBan, CrossUnban, Pin, Delete, Custom };

inline QColor tone(Kind kind, bool light)
{
    switch (kind)
    {
        case Kind::Ban:
        case Kind::CrossBan:
        case Kind::Delete:
            return light ? QColor("#a83f51") : QColor("#e6a0a9");
        case Kind::Unban:
        case Kind::CrossUnban:
            return light ? QColor("#277c61") : QColor("#8ac5ae");
        case Kind::Timeout:
            return light ? QColor("#805c2d") : QColor("#d8b98b");
        default:
            return light ? QColor("#65519a") : QColor("#b9a9ea");
    }
}

inline QString durationLabel(QString number, QString unit)
{
    if (activeUiLanguage() == u"ru")
    {
        if (unit == "s") unit = QStringLiteral("с");
        else if (unit == "m") unit = QStringLiteral("м");
        else if (unit == "h") unit = QStringLiteral("ч");
        else if (unit == "d") unit = QStringLiteral("д");
        else if (unit == "w") unit = QStringLiteral("н");
    }
    return number + unit;
}

inline bool hasGlyph(Kind kind)
{
    return kind != Kind::Timeout && kind != Kind::Custom;
}

inline void paintGlyph(QPainter &p, Kind kind, const QRectF &rect, QColor color)
{
    const auto unit = std::min(rect.width(), rect.height()) / 16.;
    p.save();
    p.translate(rect.center() - QPointF(8 * unit, 8 * unit));
    p.scale(unit, unit);
    p.setPen(QPen(color, 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (kind)
    {
        case Kind::Ban:
            p.drawEllipse(QPointF(8, 8), 5.6, 5.6);
            p.drawLine(QPointF(4, 4), QPointF(12, 12));
            break;
        case Kind::Unban: {
            p.drawRoundedRect(QRectF(3, 7, 10, 7), 2, 2);
            QPainterPath shackle;
            shackle.moveTo(5, 7);
            shackle.lineTo(5, 4.8);
            shackle.cubicTo(5, .9, 11, .9, 11, 4.8);
            p.drawPath(shackle);
            p.drawLine(QPointF(8, 10), QPointF(8, 11.5));
            break;
        }
        case Kind::CrossBan:
        case Kind::CrossUnban:
            p.drawRoundedRect(QRectF(2, 2, 8, 7), 1.5, 1.5);
            p.drawRoundedRect(QRectF(6, 6, 8, 8), 1.5, 1.5);
            if (kind == Kind::CrossBan)
                p.drawLine(QPointF(8.5, 8.5), QPointF(11.5, 11.5));
            else
                p.drawPolyline(QPolygonF{{8, 10}, {9.5, 11.5}, {12, 8.5}});
            break;
        case Kind::Pin:
            p.drawPath([] {
                QPainterPath path;
                path.moveTo(7, 2);
                path.lineTo(14, 9);
                path.lineTo(10.5, 9.8);
                path.lineTo(8, 12.4);
                path.lineTo(3.6, 8);
                path.lineTo(6.2, 5.5);
                path.closeSubpath();
                return path;
            }());
            p.drawLine(QPointF(6, 10), QPointF(2.5, 13.5));
            break;
        case Kind::Delete:
            p.drawLine(QPointF(3, 4.5), QPointF(13, 4.5));
            p.drawLine(QPointF(6, 2.8), QPointF(10, 2.8));
            p.drawRoundedRect(QRectF(4.5, 4.5, 7, 9), 1.25, 1.25);
            p.drawLine(QPointF(7, 7.2), QPointF(7, 11));
            p.drawLine(QPointF(9, 7.2), QPointF(9, 11));
            break;
        default:
            break;
    }
    p.restore();
}

// Render at the actual device scale, not through a low-resolution pixmap.
inline void paintPill(QPainter &p, QRectF rect, Kind kind, const QString &label,
                      const QFont &font, float scale, bool light, bool hovered = false,
                      bool pressed = false)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const auto color = tone(kind, light);
    auto fill = color;
    fill.setAlpha(pressed ? 41 : hovered ? 32 : 16);
    auto edge = color;
    edge.setAlpha(hovered ? 145 : 77);
    const auto box = rect.adjusted(.5, .5, -.5, -.5);
    p.setPen(QPen(edge, 1));
    p.setBrush(fill);
    p.drawRoundedRect(box, 4.5 * scale, 4.5 * scale);
    p.setFont(font);
    p.setPen(color);
    const auto iconSide = std::min(16.0 * scale, box.height() - 3.0 * scale);
    if (hasGlyph(kind))
    {
        if (label.isEmpty())
        {
            paintGlyph(p, kind, QRectF(box.center() - QPointF(iconSide/2, iconSide/2),
                                        QSizeF(iconSide, iconSide)), color);
        }
        else
        {
            const auto textWidth = QFontMetricsF(font).horizontalAdvance(label);
            const auto start = box.center().x() - (iconSide + 3 * scale + textWidth) / 2;
            paintGlyph(p, kind, QRectF(start, box.center().y() - iconSide/2, iconSide, iconSide), color);
            p.drawText(QRectF(start + iconSide + 3 * scale, box.top(), textWidth + 1, box.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, label);
        }
    }
    else
    {
        p.drawText(box, Qt::AlignCenter, label);
    }
    p.restore();
}

}  // namespace chatterino::moderationchrome
