// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Image.hpp"

#include <QLabel>
#include <QWidget>

namespace chatterino {

class TooltipEntryWidget : public QWidget
{
    Q_OBJECT

public:
    TooltipEntryWidget(QWidget *parent = nullptr);
    TooltipEntryWidget(ImagePtr image, const QString &text, int customWidth,
                       int customHeight, QWidget *parent = nullptr);

    void setImageScale(int w, int h);
    void setPreviewScale(float scale);
    void setWordWrap(bool wrap);

    void setText(const QString &text);
    void setImage(ImagePtr image);
    void clearImage();
    bool refreshPixmap();

    bool animated() const;
    bool hasImage() const;
    bool attemptRefresh() const;

protected:
    bool event(QEvent *event) override;

private:
    QLabel *displayImage_ = nullptr;
    QLabel *displayText_ = nullptr;

    bool attemptRefresh_ = false;

    ImagePtr image_ = nullptr;
    QSize customSize;
    float previewScale_ = 0.F;
    bool wordWrap_ = false;
};

}  // namespace chatterino
