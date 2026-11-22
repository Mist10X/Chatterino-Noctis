// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SettingsDialogTab.hpp"

#include "singletons/Theme.hpp"
#include "common/UiLanguage.hpp"

#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QKeyEvent>
#include <QPainter>
#include <algorithm>

namespace chatterino {

SettingsDialogTab::SettingsDialogTab(SettingsDialog *_dialog,
                                     std::function<SettingsPage *()> _lazyPage,
                                     const QString &name, QString imageFileName,
                                     SettingsTabId id)
    : BaseWidget(_dialog)
    , dialog_(_dialog)
    , lazyPage_(std::move(_lazyPage))
    , id_(id)
    , name_(uiText(name))
{
    this->ui_.icon.addFile(imageFileName);

    this->setAccessibleName(this->name_);
    this->setFocusPolicy(Qt::StrongFocus);
    this->setCursor(QCursor(Qt::PointingHandCursor));
    this->setAttribute(Qt::WA_Hover);
}

void SettingsDialogTab::setSelected(bool _selected)
{
    if (this->selected_ == _selected)
    {
        return;
    }

    this->selected_ = _selected;
    this->update();
    this->selectedChanged(this->selected_);
}

SettingsPage *SettingsDialogTab::page()
{
    if (this->page_)
    {
        return this->page_;
    }

    this->page_ = this->lazyPage_();
    this->page_->setTab(this);
    return this->page_;
}

SettingsPage *SettingsDialogTab::createdPage() const
{
    return this->page_;
}

void SettingsDialogTab::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const auto colors = this->dialog_->palette();
    const QRectF pill = QRectF(this->rect()).adjusted(4, 1, -3, -1);
    const bool focus = this->hasFocus() && this->window()->testAttribute(Qt::WA_KeyboardFocusChange);
    painter.setPen(focus ? QPen(colors.color(QPalette::Link)) : QPen(Qt::NoPen));
    if (this->selected_ || this->underMouse() || focus)
    {
        painter.setBrush(colors.brush(this->selected_ ? QPalette::Highlight : QPalette::Button));
        painter.drawRoundedRect(pill, 7, 7);
    }
    if (this->selected_)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.brush(QPalette::Link));
        painter.drawRoundedRect(QRectF(pill.left() + 2, pill.center().y() - 7, 2, 14), 1, 1);
    }

    constexpr int iconSize = 17;
    auto pixmap = this->ui_.icon.pixmap(QSize(iconSize, iconSize), this->devicePixelRatioF());
    if (!pixmap.isNull())
    {
        QPainter tint(&pixmap);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(pixmap.rect(), colors.brush(this->selected_ ? QPalette::Link : QPalette::PlaceholderText));
        tint.end();
        painter.drawPixmap(QRect(13, (this->height() - iconSize) / 2, iconSize, iconSize), pixmap);
    }
    painter.setPen(colors.color(this->selected_ ? QPalette::WindowText : QPalette::PlaceholderText));
    painter.setFont(this->font());
    painter.drawText(QRect(38, 0, this->width() - textInset, this->height()),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, this->name_);
}

void SettingsDialogTab::keyPressEvent(QKeyEvent *event)
{
    switch (event->key())
    {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Space:
            this->dialog_->selectTab(this);
            return;
        case Qt::Key_Up:
        case Qt::Key_Down: {
            const auto &tabs = this->dialog_->tabs_;
            const auto current = std::ranges::find(tabs, this);
            if (current != tabs.end())
            {
                const int direction = event->key() == Qt::Key_Up ? -1 : 1;
                for (auto i = current - tabs.begin() + direction;
                     i >= 0 && i < static_cast<qsizetype>(tabs.size()); i += direction)
                {
                    if (!tabs[i]->isHidden())
                    {
                        this->dialog_->selectTab(tabs[i]);
                        tabs[i]->setFocus(Qt::TabFocusReason);
                        break;
                    }
                }
            }
            return;
        }
        default:
            BaseWidget::keyPressEvent(event);
    }
}

void SettingsDialogTab::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }

    this->dialog_->selectTab(this);

    this->setFocus();
}

const QString &SettingsDialogTab::name() const
{
    return this->name_;
}

SettingsTabId SettingsDialogTab::id() const
{
    return this->id_;
}

}  // namespace chatterino
