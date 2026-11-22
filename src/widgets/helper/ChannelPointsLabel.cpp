// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ChannelPointsLabel.hpp"

#include "common/UiLanguage.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"

#include <QHash>
#include <QEvent>
#include <QList>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace chatterino {

namespace {

/// Downloaded points icons by URL, shared by all inputs. A failed download
/// stays a null pixmap, so the default coin is drawn and nothing is retried.
QHash<QString, QPixmap> &icons()
{
    static QHash<QString, QPixmap> icons;
    return icons;
}

/// Labels waiting for an icon that is still downloading.
QHash<QString, QList<QPointer<ChannelPointsLabel>>> &waiting()
{
    static QHash<QString, QList<QPointer<ChannelPointsLabel>>> waiting;
    return waiting;
}

void download(const QString &url)
{
    NetworkRequest(QUrl(url))
        .onSuccess([url](const NetworkResult &result) {
            QPixmap pixmap;
            pixmap.loadFromData(result.getData());
            icons().insert(url, pixmap);
            for (const auto &label : waiting().take(url))
            {
                if (label)
                {
                    label->update();
                }
            }
        })
        .onError([url](const NetworkResult &result) {
            qCWarning(chatterinoWidget)
                << "Channel points icon failed:" << result.formatError();
            icons().insert(url, {});
            waiting().remove(url);
        })
        .execute();
}

}  // namespace

ChannelPointsLabel::ChannelPointsLabel()
    : balanceAnimation_(this)
{
    this->setObjectName("ChannelPointsBalancePill");
    this->balanceAnimation_.setDuration(260);
    this->balanceAnimation_.setStartValue(0.0);
    this->balanceAnimation_.setEndValue(1.0);
    this->balanceAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&this->balanceAnimation_, &QVariantAnimation::valueChanged,
                     this, [this](const QVariant &value) {
        this->balanceProgress_ = value.toReal();
        const auto start = this->animationStartBalance_;
        const auto target = this->balance_;
        if (start < 0 || target < 0)
        {
            return;
        }
        // Both balances are nonnegative; subtract within the signed range
        // and interpolate the unsigned distance to avoid overflowing qint64.
        const auto distance = quint64(std::max(start, target) -
                                      std::min(start, target));
        const auto advanced = std::min(distance, quint64(distance * this->balanceProgress_));
        this->shownBalance_ = start <= target
                                  ? start + qint64(advanced)
                                  : start - qint64(advanced);
        this->update();
    });
    QObject::connect(&this->balanceAnimation_, &QVariantAnimation::finished,
                     this, [this] {
        this->shownBalance_ = this->balance_;
        this->balanceProgress_ = 1.0;
        this->update();
    });
}

void ChannelPointsLabel::setBalance(qint64 balance, const QString &identity)
{
    if (balance < 0)
    {
        this->setStatusText(formatBalance(balance));
        return;
    }
    if (this->balanceIdentity_ == identity && this->balance_ == balance)
    {
        return;
    }
    const bool animate = this->balanceIdentity_ == identity &&
                         this->balance_ >= 0 && this->shownBalance_ >= 0 &&
                         this->isVisible();
    this->balanceAnimation_.stop();
    this->balanceIdentity_ = identity;
    this->balance_ = balance;
    this->QLabel::setText(formatBalance(balance));
    if (animate)
    {
        this->animationStartBalance_ = this->shownBalance_;
        this->balanceProgress_ = 0.0;
        this->balanceAnimation_.start();
    }
    else
    {
        this->shownBalance_ = balance;
        this->balanceProgress_ = 1.0;
        this->update();
    }
}

void ChannelPointsLabel::setStatusText(const QString &text)
{
    this->balanceAnimation_.stop();
    this->balanceIdentity_.clear();
    this->balance_ = -1;
    this->shownBalance_ = -1;
    this->balanceProgress_ = 1.0;
    this->QLabel::setText(text);
    this->update();
}

QString ChannelPointsLabel::formatBalance(qint64 balance)
{
    if (balance < 0)
    {
        return QStringLiteral("...");
    }
    const bool russian = activeUiLanguage().startsWith(u"ru");
    const QLocale locale(russian ? QLocale::Russian : QLocale::English);
    const auto compact = [&](qint64 divisor, QString suffix) {
        // Truncate rather than round up: "150,6к" for 150 694 points.
        const auto whole = balance / divisor;
        const auto tenth = (balance % divisor) / (divisor / 10);
        auto value = locale.toString(whole);
        if (tenth != 0)
        {
            value += locale.decimalPoint() + QString::number(tenth);
        }
        return value + suffix;
    };
    if (balance >= 1'000'000'000'000LL)
    {
        return compact(1'000'000'000'000LL, russian ? QStringLiteral("трлн") : QStringLiteral("t"));
    }
    if (balance >= 1'000'000'000LL)
    {
        return compact(1'000'000'000LL, russian ? QStringLiteral("млрд") : QStringLiteral("b"));
    }
    if (balance >= 1'000'000LL)
    {
        return compact(1'000'000LL, russian ? QStringLiteral("м") : QStringLiteral("m"));
    }
    if (balance >= 1000)
    {
        return compact(1000, russian ? QStringLiteral("к") : QStringLiteral("k"));
    }
    return locale.toString(balance);
}

void ChannelPointsLabel::setIconUrl(const QString &url)
{
    if (this->iconUrl_ == url)
    {
        return;
    }
    this->iconUrl_ = url;

    if (!url.isEmpty() && !icons().contains(url))
    {
        auto &labels = waiting()[url];
        if (labels.isEmpty())
        {
            download(url);
        }
        labels.append(this);
    }
    this->update();
}

int ChannelPointsLabel::iconSpace() const
{
    if (this->text().isEmpty())
    {
        return 0;
    }
    auto line = this->fontMetrics().height();
    return qRound(line * 0.78) + std::max(3, qRound(line * 0.18));
}

int ChannelPointsLabel::horizontalPadding() const
{
    return std::max(4, qRound(this->fontMetrics().height() * 0.25));
}

QSize ChannelPointsLabel::sizeHint() const
{
    if (this->text().isEmpty())
    {
        return {};
    }
    const auto metrics = this->fontMetrics();
    const auto margins = this->contentsMargins();
    return {metrics.horizontalAdvance(this->text()) + this->iconSpace() +
                2 * this->horizontalPadding() + margins.left() + margins.right(),
            metrics.height() + 4 + margins.top() + margins.bottom()};
}

QSize ChannelPointsLabel::minimumSizeHint() const
{
    return this->sizeHint();
}

bool ChannelPointsLabel::event(QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
    {
        this->update();
    }
    return QLabel::event(event);
}

void ChannelPointsLabel::paintEvent(QPaintEvent *)
{
    if (this->text().isEmpty())
    {
        return;
    }

    const auto metrics = this->fontMetrics();
    const auto line = metrics.height();
    const auto size = qRound(line * 0.78);
    const auto area = this->contentsRect();
    const auto padding = this->horizontalPadding();
    const QRectF rect(area.left() + padding,
                      area.center().y() - size / 2.0, size, size);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto colors = this->palette();
    const auto base = colors.color(this->underMouse() ? QPalette::Highlight
                                                      : QPalette::Window);
    const auto accent = colors.color(QPalette::WindowText);
    const auto blend = [&](QColor from, qreal weight) {
        return QColor::fromRgbF(from.redF() + (accent.redF() - from.redF()) * weight,
                                from.greenF() + (accent.greenF() - from.greenF()) * weight,
                                from.blueF() + (accent.blueF() - from.blueF()) * weight);
    };
    // A quiet accent pulse is visible even if the compact number rounded down
    // stays the same after a small award.
    const auto pulse = 4.0 * this->balanceProgress_ * (1.0 - this->balanceProgress_);
    painter.setPen(QPen(blend(colors.color(QPalette::Mid), pulse * .24), 1));
    painter.setBrush(blend(base, pulse * .10));
    const auto radius = std::max(4.0, line * 0.30);
    painter.drawRoundedRect(QRectF(area).adjusted(.5, .5, -.5, -.5),
                            radius, radius);

    const auto icon = icons().value(this->iconUrl_);
    if (!icon.isNull())
    {
        painter.drawPixmap(rect, icon, QRectF(icon.rect()));
    }
    else
    {
        this->paintDefaultIcon(painter, rect);
    }

    painter.setFont(this->font());
    painter.setPen(colors.color(QPalette::WindowText));
    const auto textLeft = area.left() + padding + this->iconSpace();
    const QRect textRect(textLeft, area.top(), area.right() - textLeft - padding + 1,
                         area.height());
    auto displayed = this->shownBalance_ >= 0 ? formatBalance(this->shownBalance_)
                                               : this->text();
    // Layout depends on the final value only. On magnitude boundaries an
    // intermediate value can be wider; display the full target instead of
    // resizing or clipping the pill on each frame.
    if (metrics.horizontalAdvance(displayed) > textRect.width())
    {
        displayed = this->text();
    }
    painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, displayed);
}

void ChannelPointsLabel::paintDefaultIcon(QPainter &painter, QRectF rect) const
{
    // Twitch's coin: a ring with a curved stroke inside.
    const auto color = this->palette().color(QPalette::WindowText);
    const auto width = std::max(1.0, rect.width() / 9.0);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);

    const auto ring = rect.adjusted(width / 2, width / 2, -width / 2,
                                    -width / 2);
    painter.drawEllipse(ring);

    const auto inner = ring.adjusted(ring.width() * 0.27, ring.height() * 0.27,
                                     -ring.width() * 0.27,
                                     -ring.height() * 0.27);
    // from the top, round the left side, to the bottom-left
    painter.drawArc(inner, 90 * 16, 150 * 16);
}

}  // namespace chatterino
