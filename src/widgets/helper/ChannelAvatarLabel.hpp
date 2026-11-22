// SPDX-License-Identifier: MIT
#pragma once

#include "common/network/NetworkManager.hpp"
#include "widgets/helper/RoundAvatar.hpp"
#include "widgets/helper/TabAvatars.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace chatterino {

// A channel's cached circular profile picture, with a stable initial-letter
// placeholder while it loads. It uses the same cache as the channel tabs.
class ChannelAvatarLabel final : public QLabel
{
public:
    explicit ChannelAvatarLabel(QLabel *caption, QWidget *parent)
        : QLabel(parent), caption_(caption)
    {
        this->setObjectName("ChannelIdentityAvatar");
        this->setAlignment(Qt::AlignCenter);
        caption->installEventFilter(this);
        QObject::connect(&TabAvatars::instance(), &TabAvatars::loaded, this,
                         [this](const QString &login) {
            if (login == this->login_)
                this->refresh();
        });
    }

    void setChannel(QString name, bool lookup = true, bool kick = false)
    {
        this->name_ = std::move(name);
        this->login_ = (kick ? QStringLiteral("kick:") : QString()) + this->name_.toLower();
        this->lookup_ = lookup;
        this->refresh();
    }

protected:
    bool event(QEvent *event) override
    {
        const bool result = QLabel::event(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        if (event->type() == QEvent::DevicePixelRatioChange)
            this->refresh();
#endif
        return result;
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->caption_ && event->type() == QEvent::FontChange)
            this->refresh();
        return QLabel::eventFilter(watched, event);
    }

private:
    void refresh()
    {
        if (!this->caption_)
            return;
        if (this->name_.isEmpty())
        {
            this->clear();
            this->hide();
            return;
        }
        const int side = std::max(14, QFontMetrics(this->caption_->font()).height());
        const qreal ratio = this->devicePixelRatioF();
        const int pixels = avatarPixelSize(side, ratio);
        auto pixmap = this->lookup_ && NetworkManager::workerThread
            ? TabAvatars::instance().circle(this->login_, pixels) : QPixmap{};
        if (pixmap.isNull())
        {
            pixmap = QPixmap(pixels, pixels);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#373043"));
            painter.drawEllipse(QRectF(0, 0, pixels, pixels));
            auto font = this->caption_->font();
            font.setPixelSize(std::max(9, qRound(pixels * .65)));
            painter.setFont(font);
            painter.setPen(QColor("#d7c8f0"));
            painter.drawText(QRect(0, 0, pixels, pixels), Qt::AlignCenter, this->name_.left(1).toUpper());
        }
        pixmap.setDevicePixelRatio(ratio);
        this->setFixedSize(side, side);
        this->setPixmap(pixmap);
        this->show();
    }

    QPointer<QLabel> caption_;
    QString name_, login_;
    bool lookup_ = true;
};

inline QWidget *withChannelAvatar(QLabel *caption, const QString &login)
{
    auto *row = new QWidget(caption->parentWidget());
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *avatar = new ChannelAvatarLabel(caption, row);
    avatar->setChannel(login);
    layout->addWidget(avatar);
    layout->addWidget(caption, 1);
    return row;
}

}  // namespace chatterino
