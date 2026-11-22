// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QLabel>
#include <QPixmap>
#include <QString>
#include <QVariantAnimation>

namespace chatterino {

/// The channel points balance under the input, with the channel's own points
/// icon to the left of the number (Twitch's coin when the channel has none).
class ChannelPointsLabel : public QLabel
{
public:
    ChannelPointsLabel();

    // Compact display only. The channel still stores the exact balance.
    static QString formatBalance(qint64 balance);

    /// Updates the real balance immediately, animating only changes within
    /// the same account and channel.
    void setBalance(qint64 balance, const QString &identity);
    void setStatusText(const QString &text);

    /// The channel's points icon; empty for Twitch's default coin.
    void setIconUrl(const QString &url);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    int iconSpace() const;
    int horizontalPadding() const;
    void paintDefaultIcon(QPainter &painter, QRectF rect) const;

    QString iconUrl_;
    QString balanceIdentity_;
    qint64 balance_ = -1;
    qint64 shownBalance_ = -1;
    qint64 animationStartBalance_ = -1;
    qreal balanceProgress_ = 1.0;
    QVariantAnimation balanceAnimation_;
};

}  // namespace chatterino
