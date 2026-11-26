// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/MenuStyle.hpp"

#include "Application.hpp"
#include "common/UiScale.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/helper/UsercardChrome.hpp"

#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QPointer>
#include <QProxyStyle>

namespace chatterino {

namespace {

class MenuNoUnderlineStyle final : public QProxyStyle
{
public:
    MenuNoUnderlineStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    int styleHint(StyleHint hint, const QStyleOption *option,
                  const QWidget *widget, QStyleHintReturn *data) const override
    {
        if (hint == QStyle::SH_UnderlineShortcut)
        {
            return 0;
        }
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};

QStyle *menuBaseStyle()
{
    static QPointer<MenuNoUnderlineStyle> style;
    if (!style)
    {
        style = new MenuNoUnderlineStyle();
        style->setParent(qApp);
    }
    return style;
}

QString menuStyleSheet()
{
    const bool light = getTheme()->isLightTheme();
    const auto colors = usercardChrome::palette(light);
    const QString mode = light ? QStringLiteral("light") : QStringLiteral("dark");
    return QStringLiteral(
               "QMenu { background: %1; border: 1px solid %2; "
               "border-radius: 7px; padding: 3px; }"
               "QMenu::item { color: %3; background: transparent; "
               "padding: 3px 12px 3px 13px; border-radius: 4px; }"
               "QMenu::item:selected { background: %4; }"
               "QMenu::item:disabled { color: %5; }"
               "QMenu::separator { height: 1px; background: %2; "
               "margin: 2px 6px; }"
               "QMenu::indicator { width: 12px; height: 12px; left: 3px; }"
               "QMenu::indicator:checked { image: url(:/buttons/"
               "menu-check-%6.svg); }"
               "QMenu::right-arrow { image: url(:/buttons/menu-arrow-%6.svg); "
               "width: 11px; height: 11px; right: 3px; }")
        .arg(colors.panel, colors.border, colors.foreground, colors.cover,
             colors.disabled, mode);
}

void refreshMenu(QMenu *menu)
{
    auto scale = contentUiScale(getSettings()->getClampedUiScale());
    for (auto *owner = menu->parentWidget(); owner; owner = owner->parentWidget())
    {
        if (auto *base = dynamic_cast<BaseWidget *>(owner))
        {
            scale = base->scale();
            break;
        }
    }
    // Use the actual bundled semibold face, like the tabs. Keep the nominal
    // point size and avoid a second CSS font resolution at fractional DPI.
    auto font = getApp()->getFonts()->getFont(FontStyle::UiTabs, scale);
    font.setHintingPreference(QFont::PreferFullHinting);
    if (menu->font() != font)
    {
        menu->setFont(font);
    }
    const auto sheet = menuStyleSheet();
    if (menu->styleSheet() != sheet)
    {
        menu->setStyleSheet(sheet);
    }
}

class MenuFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *target, QEvent *event) override
    {
        if (event->type() == QEvent::Polish)
        {
            if (auto *menu = qobject_cast<QMenu *>(target))
            {
                styleMenu(menu);
            }
        }
        return false;
    }
};

}  // namespace

void installMenuStyle()
{
    static QPointer<MenuFilter> filter;
    if (!filter && qApp)
    {
        filter = new MenuFilter(qApp);
        qApp->installEventFilter(filter);
    }
}

void styleMenu(QMenu *menu)
{
    if (menu == nullptr || menu->property("mist1xStyled").toBool())
    {
        return;
    }
    menu->setProperty("mist1xStyled", true);
    menu->setStyle(menuBaseStyle());
    if (menu->isWindow())
    {
        menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint |
                             Qt::NoDropShadowWindowHint);
        menu->setAttribute(Qt::WA_TranslucentBackground);
    }
    refreshMenu(menu);
    // Persistent menus can be reopened after a theme, scale or chat font change.
    QObject::connect(menu, &QMenu::aboutToShow, menu,
                     [menu] { refreshMenu(menu); });
}

}  // namespace chatterino
