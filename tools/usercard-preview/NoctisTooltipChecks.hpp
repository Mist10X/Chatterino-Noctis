// SPDX-License-Identifier: MIT
#pragma once

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/NoctisTooltip.hpp"
#include "widgets/settingspages/SettingWidget.hpp"
#include "widgets/TooltipWidget.hpp"

#include <QApplication>
#include <QFile>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino::preview {

inline QWidget *visibleTextTooltip()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "NoctisTextTooltip" && widget->isVisible())
            return widget;
    return nullptr;
}

inline void requestTooltip(QWidget *owner, QPoint point)
{
    QHelpEvent event(QEvent::ToolTip, point, owner->mapToGlobal(point));
    QApplication::sendEvent(owner, &event);
    QCoreApplication::processEvents();
}

inline int checkNoctisTooltips(const QString &report, const QString &screenshot)
{
    noctistooltip::install();
    noctistooltip::install();
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](QString name, bool ok) {
        passed &= ok;
        checks.append(QJsonObject{{"name", name}, {"ok", ok}});
    };

    QWidget host;
    QVBoxLayout layout(&host);
    auto *button = new QPushButton(uiText("Save"), &host);
    const auto duplicate = button->text();
    button->setToolTip(duplicate);
    layout.addWidget(button);
    auto *hint = new QPushButton(uiText("Options"), &host);
    const auto help = QStringLiteral("Additional information about the selected option.");
    hint->setToolTip(help);
    layout.addWidget(hint);
    auto *list = new QListWidget(&host);
    auto *item = new QListWidgetItem(QStringLiteral("Item"), list);
    item->setToolTip(QStringLiteral("Additional item information"));
    auto *duplicateItem = new QListWidgetItem(QStringLiteral("Duplicate"), list);
    duplicateItem->setToolTip(duplicateItem->text());
    layout.addWidget(list);
    auto *setting = SettingWidget::checkbox(uiText("Show timestamps"),
                                             getSettings()->showTimestamps)
                        ->setTooltip(uiText("Show timestamps"));
    layout.addWidget(setting);
    host.resize(480, 320);
    host.show();
    QCoreApplication::processEvents();

    requestTooltip(button, button->rect().center());
    check("fully visible duplicate caption does not open a tip",
          !visibleTextTooltip());
    check("a duplicate settings tooltip has no visible hint icon or text",
          std::ranges::none_of(setting->findChildren<QWidget *>(),
                              [](QWidget *w) { return !w->toolTip().isEmpty(); }));

    requestTooltip(hint, hint->rect().center());
    auto *popup = visibleTextTooltip();
    check("useful QWidget tooltip uses common Noctis popup", popup != nullptr);
    if (popup)
    {
        auto *tipText = popup->findChild<QLabel *>("NoctisTooltipText");
        check("popup has chat font", tipText && tipText->font().family() ==
                getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, 1.F).family());
        check("popup fits on its screen",
              popup->screen()->availableGeometry().contains(popup->frameGeometry()));
        const auto image = popup->grab().toImage();
        check("popup has transparent rounded corners and opaque content",
              image.pixelColor(0, 0).alpha() < 64 &&
              image.pixelColor(image.width()/2, image.height()/2).alpha() > 150);
        if (!screenshot.isEmpty())
            popup->grab().save(screenshot);
        const auto original = popup->geometry();
        requestTooltip(hint, hint->rect().center() + QPoint(4, 1));
        check("unchanged hint does not jump when cursor moves",
              popup->geometry() == original);
    }
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(hint, &leave);
    check("tooltip hides when owner is left", !visibleTextTooltip());

    requestTooltip(list->viewport(), list->visualItemRect(item).center());
    check("item-model tooltip also uses common popup", visibleTextTooltip() != nullptr);
    requestTooltip(list->viewport(), list->visualItemRect(duplicateItem).center());
    check("fully visible duplicate item text is suppressed",
          !visibleTextTooltip());
    noctistooltip::hideText();
    const QString longUrl = "https://example.invalid/" + QString(200, u'x');
    noctistooltip::showText(hint->mapToGlobal(hint->rect().center()), longUrl, hint);
    popup = visibleTextTooltip();
    check("long unbroken value stays within screen", popup &&
          popup->screen()->availableGeometry().contains(popup->frameGeometry()) &&
          popup->width() < 700);
    host.hide();
    QCoreApplication::processEvents();
    check("hidden owner dismisses tooltip", !visibleTextTooltip());
    noctistooltip::hideText();

    BaseWidget previewOwner;
    TooltipWidget emoteTip(&previewOwner);
    emoteTip.setOverrideScale(1.3F);
    QPixmap wideSource(560, 280);
    wideSource.fill(QColor("#506bd1"));
    QPainter sourcePainter(&wideSource);
    sourcePainter.fillRect(QRect(0, 0, 280, 280), QColor("#b18de8"));
    sourcePainter.end();
    const auto source = Image::fromResourcePixmap(wideSource, 0.25F);
    emoteTip.setOne(TooltipEntry::scaled(source,
        QStringLiteral("scary<br>Alias of DogLookingSussyAndCold<br>Channel 7TV Emote<br>By: joosiie"), 1.F));
    emoteTip.show();
    QCoreApplication::processEvents();
    emoteTip.adjustSize();
    QLabel *media = nullptr;
    QLabel *caption = nullptr;
    for (auto *label : emoteTip.findChildren<QLabel *>())
    {
        if (!label->pixmap().isNull()) media = label;
        if (label->text().contains("scary")) caption = label;
    }
    check("wide emote retains its aspect ratio in a compact high-resolution preview",
          media && media->pixmap().deviceIndependentSize().width() <= 233 &&
          media->pixmap().deviceIndependentSize().width() >= 200 &&
          media->pixmap().width() == 2 * media->pixmap().height());
    check("emote caption wraps at compact width using a slightly smaller chat font",
          caption && caption->width() <= 264 && caption->wordWrap() &&
          qAbs(caption->font().pointSizeF() -
              std::max(7.5, emoteTip.font().pointSizeF() * 0.88)) < 0.2 &&
          caption->font().pointSizeF() >= 7.5);
    if (!screenshot.isEmpty())
        emoteTip.grab().save(screenshot + QStringLiteral("-emote.png"));
    emoteTip.setOne({.image = source, .text = {}, .customWidth = 450, .customHeight = 225});
    QCoreApplication::processEvents();
    check("explicit screenshot preview keeps its requested physical resolution",
          media && media->pixmap().size() == QSize(450, 225));
    emoteTip.setOne({.image = {}, .text = QStringLiteral("Ordinary tooltip")});
    QCoreApplication::processEvents();
    check("reused tooltip restores normal caption font for non-emote entries",
          caption && qAbs(caption->font().pointSizeF() -
                          emoteTip.font().pointSizeF()) < 0.2);
    emoteTip.hide();

    QFile output(report);
    if (!output.open(QIODevice::WriteOnly))
        return 2;
    output.write(QJsonDocument(QJsonObject{{"checks", checks}}).toJson());
    return passed ? 0 : 3;
}

}  // namespace chatterino::preview
