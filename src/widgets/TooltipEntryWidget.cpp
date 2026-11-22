// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/TooltipEntryWidget.hpp"

#include <QEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

TooltipEntryWidget::TooltipEntryWidget(QWidget *parent)
    : TooltipEntryWidget(nullptr, "", 0, 0, parent)
{
}

TooltipEntryWidget::TooltipEntryWidget(ImagePtr image, const QString &text,
                                       int customWidth, int customHeight,
                                       QWidget *parent)
    : QWidget(parent)
    , image_(image)
    , customSize(customWidth, customHeight)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    this->setLayout(layout);

    this->displayImage_ = new QLabel();
    this->displayImage_->setAlignment(Qt::AlignHCenter);
    this->displayImage_->setStyleSheet("background: transparent");
    this->displayImage_->hide();
    this->displayText_ = new QLabel(text);
    this->displayText_->setAlignment(Qt::AlignHCenter);
    this->displayText_->setStyleSheet("background: transparent");

    layout->addWidget(this->displayImage_);
    layout->addWidget(this->displayText_);
}

void TooltipEntryWidget::setWordWrap(bool wrap)
{
    this->wordWrap_ = wrap;
    this->displayText_->setWordWrap(wrap || this->previewScale_ > 0.F);
}

void TooltipEntryWidget::setPreviewScale(float scale)
{
    if (this->previewScale_ == scale)
    {
        return;
    }
    this->previewScale_ = scale;
    this->displayText_->setMaximumWidth(scale > 0.F ? 264 : QWIDGETSIZE_MAX);
    this->setWordWrap(this->wordWrap_);
    this->refreshPixmap();
}

bool TooltipEntryWidget::event(QEvent *event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange && this->displayImage_)
    {
        this->refreshPixmap();
    }
    return handled;
}

void TooltipEntryWidget::setImageScale(int w, int h)
{
    if (this->customSize == QSize{w, h})
    {
        return;
    }
    this->customSize = QSize{w, h};
    this->refreshPixmap();
}

void TooltipEntryWidget::setText(const QString &text)
{
    this->displayText_->setText(text);
    this->displayText_->setVisible(!text.isEmpty());
}

void TooltipEntryWidget::setImage(ImagePtr image)
{
    if (this->image_ == image)
    {
        return;
    }

    this->clearImage();
    this->image_ = std::move(image);
    this->refreshPixmap();
}

void TooltipEntryWidget::clearImage()
{
    this->displayImage_->hide();
    this->image_ = nullptr;
    this->attemptRefresh_ = false;
    this->setImageScale(0, 0);
}

bool TooltipEntryWidget::refreshPixmap()
{
    if (!this->image_)
    {
        return false;
    }

    auto pixmap = this->image_->pixmapOrLoad();
    if (!pixmap)
    {
        this->attemptRefresh_ = true;
        return false;
    }
    // Do not round the monitor's fractional DPR (125%, 150%, etc.). The
    // physical custom size is already derived from the selected image's
    // resolution, exactly as it is for the chat's own previews.
    pixmap->setDevicePixelRatio(this->devicePixelRatioF());

    auto target = this->customSize;
    if (this->previewScale_ > 0.F)
    {
        // Size the decoded image, including images that were still loading
        // when the tooltip was opened. Keep the source resolution for smooth
        // downscaling and limit unusually wide or tall emotes on each monitor.
        target = pixmap->size() * (0.78F * this->previewScale_);
        const auto limitScale = std::min(this->previewScale_, 1.5F) * this->devicePixelRatioF();
        const QSize limit(qRound(232 * limitScale), qRound(160 * limitScale));
        if (target.width() > limit.width() || target.height() > limit.height())
        {
            target.scale(limit, Qt::KeepAspectRatio);
        }
        target = target.expandedTo(QSize(1, 1));
    }

    if (!target.isEmpty())
    {
        this->displayImage_->setPixmap(pixmap->size() == target
            ? *pixmap
            : pixmap->scaled(target, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation));

        if (this->displayImage_->pixmap().size() != target)
        {
            this->adjustSize();
        }
    }
    else
    {
        this->displayImage_->setPixmap(*pixmap);
    }
    this->displayImage_->show();
    // The image arrived; unrelated later layout notifications shouldn't
    // rescale it and call adjustSize repeatedly while the cursor is moving.
    this->attemptRefresh_ = false;

    return true;
}

bool TooltipEntryWidget::animated() const
{
    return this->image_ && this->image_->animated();
}

bool TooltipEntryWidget::hasImage() const
{
    return this->image_ != nullptr;
}

bool TooltipEntryWidget::attemptRefresh() const
{
    return this->attemptRefresh_;
}

}  // namespace chatterino
