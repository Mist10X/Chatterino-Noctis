#pragma once

#include <QPalette>
#include <QWidget>

namespace chatterino {

inline void setBannerTextColor(QWidget *widget, const QColor &color)
{
    const auto role = widget->foregroundRole();
    auto palette = widget->palette();
    if (palette.color(role) != color)
    {
        palette.setColor(role, color);
        widget->setPalette(palette);
    }
}

}  // namespace chatterino
