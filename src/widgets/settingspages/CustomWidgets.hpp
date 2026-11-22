// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/UiComboBox.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStylePainter>

#include <algorithm>

#define SETTINGS_PAGE_WIDGET_BOILERPLATE(type, parent) \
    class type : public parent                         \
    {                                                  \
        using parent::parent;                          \
                                                       \
    public:                                            \
        bool greyedOut{};                              \
                                                       \
    protected:                                         \
        void paintEvent(QPaintEvent *e) override       \
        {                                              \
            parent::paintEvent(e);                     \
                                                       \
            if (this->greyedOut)                       \
            {                                          \
                QPainter painter(this);                \
                QColor color = QColor("#222222");      \
                color.setAlphaF(0.7F);                 \
                painter.fillRect(this->rect(), color); \
            }                                          \
        }                                              \
    };

namespace chatterino {

// QLabel's wrapped size hint assumes a narrow column even when room is
// available. Prefer one line and let heightForWidth wrap it when necessary.
class SettingCaption : public QLabel
{
public:
    explicit SettingCaption(const QString &text)
        : QLabel(text)
    {
        this->setWordWrap(true);
    }

    QSize sizeHint() const override
    {
        return this->fontMetrics().size(Qt::TextSingleLine, this->text()) + QSize(2, 0);
    }
};

// Keep the native checkbox and its accessible text. Long localized captions
// can use a second line instead of setting the minimum width of the whole page.
class SCheckBox : public QCheckBox
{
public:
    explicit SCheckBox(const QString &text, QWidget *parent = nullptr)
        : QCheckBox(text, parent)
    {
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        this->setSizePolicy(policy);
    }

    bool greyedOut{};

    QSize minimumSizeHint() const override
    {
        auto hint = QCheckBox::minimumSizeHint();
        hint.setWidth(std::min(hint.width(), 90));
        return hint;
    }

    int heightForWidth(int width) const override
    {
        auto opt = this->buttonOption(width);
        const auto text = this->captionRect(opt);
        const int textHeight = this->fontMetrics().boundingRect(
            QRect(0, 0, std::max(1, text.width()), 0), this->textFlags(), opt.text).height();
        return std::max(QCheckBox::sizeHint().height(), textHeight + 4);
    }

protected:
    bool hitButton(const QPoint &point) const override
    {
        return this->rect().contains(point);
    }

    void paintEvent(QPaintEvent *event) override
    {
        auto opt = this->buttonOption(this->width());
        const auto text = this->captionRect(opt);
        if (this->fontMetrics().horizontalAdvance(opt.text) <= text.width())
        {
            QCheckBox::paintEvent(event);
        }
        else
        {
            QStylePainter painter(this);
            opt.text.clear();
            painter.drawControl(QStyle::CE_CheckBox, opt);
            this->style()->drawItemText(&painter, text, this->textFlags(),
                                        opt.palette, this->isEnabled(),
                                        this->text(), QPalette::WindowText);
        }
        if (this->greyedOut)
        {
            QPainter painter(this);
            painter.fillRect(this->rect(), QColor(34, 34, 34, 178));
        }
    }

private:
    QStyleOptionButton buttonOption(int width) const
    {
        QStyleOptionButton opt;
        this->initStyleOption(&opt);
        opt.rect.setWidth(width);
        return opt;
    }

    QRect captionRect(const QStyleOptionButton &opt) const
    {
        return this->style()->subElementRect(QStyle::SE_CheckBoxContents, &opt, this);
    }

    int textFlags() const
    {
        return Qt::TextWordWrap | Qt::TextShowMnemonic | Qt::AlignVCenter |
               QStyle::visualAlignment(this->layoutDirection(), Qt::AlignLeft);
    }
};

SETTINGS_PAGE_WIDGET_BOILERPLATE(SLabel, QLabel)
SETTINGS_PAGE_WIDGET_BOILERPLATE(SComboBox, UiComboBox)
SETTINGS_PAGE_WIDGET_BOILERPLATE(SPushButton, QPushButton)

}  // namespace chatterino
