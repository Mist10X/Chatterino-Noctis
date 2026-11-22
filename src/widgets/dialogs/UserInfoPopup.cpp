// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/UiLanguage.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkProxy.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/builtin/Misc.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/IvrApi.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/pronouns/Pronouns.hpp"
#include "providers/seventv/paints/Paint.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/PublicChatLogs.hpp"
#include "providers/twitch/TwitchNameHistory.hpp"
#include "providers/twitch/UsercardBadges.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/LayoutCreator.hpp"
#include "util/PostToThread.hpp"
#include "widgets/buttons/FollowButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EditUserNotesDialog.hpp"
#include "widgets/helper/ChannelAvatarLabel.hpp"
#include "widgets/helper/RoundAvatar.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/MenuStyle.hpp"
#include "widgets/helper/NoctisTooltip.hpp"
#include "widgets/helper/UsercardActivity.hpp"
#include "widgets/helper/UsercardChrome.hpp"
#include "widgets/helper/UsercardRolesPage.hpp"
#include "widgets/helper/UsercardMotionViewport.hpp"
#include "widgets/helper/UsercardResizeGrip.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/helper/LiveIndicator.hpp"
#include "widgets/helper/ScalingSpacerItem.hpp"
#include "widgets/Label.hpp"
#include "widgets/MarkdownLabel.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/TooltipWidget.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <IrcMessage>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QDate>
#include <QDesktopServices>
#include <QFile>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImage>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QMovie>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegion>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringBuilder>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace {

/// A click on a label with the "openUrl" property opens it in the browser
/// (the followage row when Twitch keeps the date hidden).
class OpenUrlOnClick : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // on the press: the card takes a left button's release (dragging)
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        {
            const auto url = watched->property("openUrl").toString();
            if (!url.isEmpty())
            {
                QDesktopServices::openUrl(QUrl(url));
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};
constexpr QStringView TEXT_FOLLOWERS = u"Фолловеров %1";
constexpr QStringView TEXT_CREATED = u"Создан %1";
constexpr QStringView TEXT_TITLE = u"%1 — карточка в %2";
constexpr QStringView TEXT_USER_ID = u"ID: ";
constexpr QStringView TEXT_UNAVAILABLE = u"нет данных";
constexpr QStringView TEXT_PRONOUNS = u"Местоимения: %1";
constexpr QStringView TEXT_UNSPECIFIED = u"не указаны";
constexpr QStringView TEXT_LOADING = u"загрузка…";
constexpr QStringView TEXT_LAST_LIVE = u"Последний эфир: %1";
constexpr QStringView TEXT_COLOR = u"Цвет: %1";
constexpr QStringView TEXT_SEVENTV_PAINT = u"7TV Paint: ";
constexpr QStringView TEXT_CHATTERS = u"Зрителей в чате: %1";

constexpr QStringView SEVENTV_TWITCH_USER_API =
    u"https://7tv.io/v3/users/twitch/%1";
constexpr QStringView SEVENTV_KICK_USER_API =
    u"https://7tv.io/v3/users/kick/%1";
constexpr QStringView SEVENTV_USER_PAGE = u"https://7tv.app/users/";
constexpr QStringView SEVENTV_PAINT_PAGE = u"https://7database.com/paint/";

using namespace chatterino;

class UsercardAvatarButton final : public PixmapButton
{
public:
    void setRingColor(QColor color)
    {
        this->ringColor_ = color;
        this->update();
    }

protected:
    void paintContent(QPainter &painter) override
    {
        const auto image = this->pixmap();
        if (image.isNull())
        {
            return;
        }
        painter.save();
        painter.setRenderHints(QPainter::Antialiasing |
                               QPainter::SmoothPixmapTransform);
        painter.setOpacity(this->currentContentOpacity());
        const auto bounds = QRectF(this->rect()).adjusted(2, 2, -2, -2);
        const qreal ring = this->ringColor_.isValid() ? std::max(2.0, this->width() / 29.0) : 0;
        const auto inner = bounds.adjusted(ring, ring, -ring, -ring);
        if (this->ringColor_.isValid())
        {
            painter.setPen(QPen(this->ringColor_.lighter(145), 0.8));
            painter.setBrush(this->ringColor_);
            painter.drawEllipse(bounds.adjusted(.4, .4, -.4, -.4));
        }
        const auto pixels = avatarPixelSize(inner.width(), this->devicePixelRatioF());
        if (this->roundCache_.isNull() || this->cacheKey_ != image.cacheKey() ||
            this->roundCache_.width() != pixels)
        {
            this->roundCache_ = roundAvatarPixmap(image, pixels);
            this->cacheKey_ = image.cacheKey();
        }
        painter.drawPixmap(inner, this->roundCache_, QRectF(this->roundCache_.rect()));
        painter.restore();
    }

private:
    QColor ringColor_;
    QPixmap roundCache_;
    qint64 cacheKey_ = 0;
};

// The profile cover is painted as part of the same palette, with an AA upper
// contour even when a real wide cover image is supplied by the platform.
class UsercardCoverLabel final : public QLabel
{
public:
    explicit UsercardCoverLabel(QWidget *parent) : QLabel(parent) {}

    void setCover(const QPixmap &source, float scale, const usercardChrome::Palette &colors)
    {
        const auto key = source.cacheKey();
        if (this->sourceKey_ != key || this->scale_ != scale || this->colors_.cover != colors.cover ||
            this->colors_.profile != colors.profile)
        {
            this->source_ = source;
            this->sourceKey_ = key;
            this->scale_ = scale;
            this->colors_ = colors;
            this->cache_ = {};
            this->update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const auto ratio = this->devicePixelRatioF();
        const QSize pixels(qRound(this->width() * ratio), qRound(this->height() * ratio));
        if (pixels.isEmpty())
            return;
        if (this->cache_.size() != pixels)
        {
            QImage result(pixels, QImage::Format_ARGB32_Premultiplied);
            result.fill(Qt::transparent);
            QPainter p(&result);
            p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
            const QRectF area(0, 0, pixels.width(), pixels.height());
            if (this->source_.isNull())
            {
                QLinearGradient gradient(area.topLeft(), area.bottomLeft());
                gradient.setColorAt(0, QColor(this->colors_.profile));
                gradient.setColorAt(.24, QColor(this->colors_.cover));
                gradient.setColorAt(1, QColor(this->colors_.profile));
                p.fillRect(area, gradient);
            }
            else
            {
                auto image = this->source_.toImage();
                image.setDevicePixelRatio(1.0);
                image = image.scaled(pixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                p.drawImage(QPointF((pixels.width() - image.width()) / 2., (pixels.height() - image.height()) / 2.), image);
            }
            // Feather the inset cover into the actual profile surface. Both
            // dimensions fade to zero alpha at the outside edge, rather than
            // revealing a second, separately rounded rectangular panel.
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            const auto feather = usercardChrome::coverFeather(this->scale_) * ratio;
            auto fadeEdges = [&](QPointF end, qreal fraction) {
                QLinearGradient alpha(area.topLeft(), end);
                const auto edge = std::min(.33, fraction);
                alpha.setColorAt(0, QColor(0,0,0,0));
                alpha.setColorAt(edge * .35, QColor(0,0,0,72));
                alpha.setColorAt(edge, Qt::black);
                alpha.setColorAt(1 - edge, Qt::black);
                alpha.setColorAt(1 - edge * .35, QColor(0,0,0,72));
                alpha.setColorAt(1, QColor(0,0,0,0));
                p.fillRect(area, alpha);
            };
            fadeEdges(area.topRight(), feather / area.width());
            fadeEdges(area.bottomLeft(), feather / area.height());
            p.end();
            this->cache_ = QPixmap::fromImage(result);
        }
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(QRectF(this->rect()), this->cache_, QRectF(this->cache_.rect()));
    }

private:
    QPixmap source_, cache_;
    qint64 sourceKey_ = 0;
    float scale_ = 1.F;
    usercardChrome::Palette colors_ = usercardChrome::palette(false);
};

/// Retains Label's copy text, elision and sizing. Only the existing 7TV paint
/// renders differently; an absent paint uses the ordinary label renderer.
class UsercardNameLabel final : public Label
{
public:
    UsercardNameLabel()
    {
        this->connections_.managedConnect(
            getApp()->getWindows()->layoutRequested, [this](Channel *) {
                if (this->paint_ && this->paint_->animated())
                {
                    this->update();
                }
            });
    }

    void setPaint(std::shared_ptr<Paint> paint, QColor color)
    {
        this->paint_ = std::move(paint);
        this->color_ = color;
        auto palette = this->palette();
        palette.setColor(QPalette::WindowText, color);
        this->setPalette(palette);
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (!this->paint_)
        {
            Label::paintEvent(event);
            return;
        }
        const auto bounds = this->textRect();
        const auto metrics = this->getFontMetrics();
        const auto text =
            metrics.elidedText(this->getText(), Qt::ElideRight, bounds.width());
        if (text.isEmpty())
        {
            return;
        }
        const auto font =
            getApp()->getFonts()->getFont(this->getFontStyle(), this->scale());
        const auto size =
            QSizeF(std::min(bounds.width(), metrics.horizontalAdvance(text)),
                   bounds.height());
        qreal margin = 0;
        // Name labels are deliberately compact. Keep room for the paint's
        // shadow in its own bitmap and feather any part that would otherwise
        // be cut by the widget edge, leaving the painted glyphs intact.
        const auto shadowClip = QRectF(this->rect()).translated(-bounds.topLeft());
        const auto pixmap = this->paint_->getPixmap(
            text, font, this->color_, size, this->scale(),
            this->devicePixelRatioF(), true, &margin, &shadowClip);
        QPainter painter(this);
        painter.drawPixmap(bounds.topLeft() - QPointF(margin, margin), pixmap);
    }

private:
    std::shared_ptr<Paint> paint_;
    QColor color_;
};

class UsercardFactRow final : public QWidget
{
public:
    enum class Icon { Created, Followers };

    UsercardFactRow(Label *label, Icon icon, QWidget *parent)
        : QWidget(parent)
        , label_(label)
        , icon_(icon)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(22, 0, 0, 0);
        layout->setSpacing(0);
        label->setPadding({});
        label->setParent(this);
        this->setObjectName("UsercardFactRow");
        layout->addWidget(label, 1);
        label->installEventFilter(this);
        this->setVisible(!label->isHidden());
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->label_ && (event->type() == QEvent::ShowToParent ||
                                        event->type() == QEvent::HideToParent))
        {
            this->setVisible(!this->label_->isHidden());
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(142, 145, 157), 1.4));
        painter.translate(0, (this->height() - 14 * this->scale_) / 2.);
        painter.scale(this->scale_, this->scale_);
        if (this->icon_ == Icon::Created)
        {
            painter.drawRoundedRect(QRectF(1.5, 3, 12, 10), 1.5, 1.5);
            painter.drawLine(QPointF(1.5, 6), QPointF(13.5, 6));
            painter.drawLine(QPointF(4.5, 1), QPointF(4.5, 4.5));
            painter.drawLine(QPointF(10.5, 1), QPointF(10.5, 4.5));
        }
        else
        {
            painter.drawEllipse(QRectF(5, 1, 5, 5));
            painter.drawArc(QRectF(2, 6, 11, 13), 0, 180 * 16);
        }
    }

public:
    void setScale(qreal scale)
    {
        this->scale_ = scale;
        this->layout()->setContentsMargins(int(22 * scale), 0, 0, 0);
        this->update();
    }

private:
    Label *label_;
    Icon icon_;
    qreal scale_ = 1;
};

/// Uses the normal cached badge images from messages already in this channel.
/// This is deliberately not presented as a complete earned-badge collection.
class UsercardBadgeStrip final : public QWidget
{
public:
    explicit UsercardBadgeStrip(QWidget *parent)
        : QWidget(parent)
        , motionTimer_(this)
    {
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->setMouseTracking(true);
        // Use a precise local frame clock: the shared Qt animation timer may
        // produce ~33 ms gaps on Windows. Derive progress from elapsed time
        // so a delayed event cannot prolong the animation or jump endpoints.
        this->motionTimer_.setTimerType(Qt::PreciseTimer);
        // A 16 ms request can miss the 15.625 ms Windows timer tick and
        // regularly land on the next one. Wake slightly earlier, but only
        // produce a frame when enough monotonic time has passed.
        this->motionTimer_.setInterval(8);
        QObject::connect(&this->motionTimer_, &QTimer::timeout, this,
                         [this] { this->motionTick(); });
        this->connections_.managedConnect(
            getApp()->getWindows()->layoutRequested, [this](Channel *) {
                if (!this->motionTimer_.isActive() && this->isVisible())
                    this->update();
            });
    }

    ~UsercardBadgeStrip() override
    {
        // On Windows TooltipWidget deliberately has no QWidget parent.
        // Delete it with the strip, not only when the whole card closes.
        if (this->badgePreview_)
            delete this->badgePreview_.data();
    }

    void setBadges(std::vector<EmotePtr> badges, float scale)
    {
        if (this->frozenLayout_ && this->motionTimer_.isActive())
        {
            // Earned badges may arrive while the user is closing the list.
            // Keep the current layout until the transition has ended.
            this->pendingBadges_ = std::make_pair(std::move(badges), scale);
            return;
        }
        if (this->preserveOrder_ && this->badges_ != badges)
        {
            // A later fetch must not reshuffle the already visible prefix.
            // Match by the same stable identity used to deduplicate the list.
            auto key = [](const EmotePtr &badge) {
                const auto url = badge->images.getImage1()->url().string;
                return url.isEmpty() ? badge->id.string : url;
            };
            QHash<QString, qsizetype> byKey;
            for (qsizetype i = 0; i < qsizetype(badges.size()); ++i)
            {
                const auto id = key(badges[size_t(i)]);
                if (!id.isEmpty())
                    byKey.insert(id, i);
            }
            std::vector<bool> used(badges.size());
            std::vector<EmotePtr> ordered;
            ordered.reserve(badges.size());
            for (const auto &old : this->badges_)
            {
                const auto id = key(old);
                const auto it = byKey.constFind(id);
                if (it != byKey.constEnd() && !used[size_t(*it)])
                {
                    used[size_t(*it)] = true;
                    ordered.push_back(badges[size_t(*it)]);
                }
            }
            for (size_t i = 0; i < badges.size(); ++i)
                if (!used[i]) ordered.push_back(std::move(badges[i]));
            badges = std::move(ordered);
        }
        this->preserveOrder_ = true;
        if (this->badges_ == badges && this->scale_ == scale)
            return;
        this->hideBadgePreview();
        this->badges_ = std::move(badges);
        this->scale_ = scale;
        this->layoutCache_.reset();
        this->frameCache_.clear();
        this->frameCache_.resize(this->badges_.size());
        this->setVisible(!this->badges_.empty());
        this->setProperty("badgeCount", int(this->badges_.size()));
        this->updateHeight();
        this->update();
    }

    QSize sizeHint() const override
    {
        return {int(200 * this->scale_), this->currentHeight()};
    }

    QSize minimumSizeHint() const override
    {
        return {0, this->currentHeight()};
    }

    void setBadgeScale(float scale)
    {
        if (this->scale_ == scale)
            return;
        this->hideBadgePreview();
        this->scale_ = scale;
        this->layoutCache_.reset();
        this->frozenLayout_.reset();
        this->frameCache_.clear();
        this->frameCache_.resize(this->badges_.size());
        this->updateHeight();
        this->update();
    }

    void resetExpansion()
    {
        this->hideBadgePreview();
        this->motionTimer_.stop();
        this->frozenLayout_.reset();
        this->layoutCache_.reset();
        this->pendingBadges_.reset();
        this->preserveOrder_ = false;
        this->expanded_ = false;
        this->progress_ = 0;
        this->chipPressed_ = false;
        this->setProperty("expanded", false);
        this->updateHeight();
        this->update();
        if (this->onFinished)
            this->onFinished(false);
    }

    std::function<void(bool, int)> onStarted;
    std::function<void(qreal)> onHeightChanged;
    std::function<void(bool)> onFinished;

    bool hasOverflow() const
    {
        return this->firstRowCount() < this->badges_.size();
    }

    void toggleExpansion()
    {
        if (!this->hasOverflow())
            return;
        this->hideBadgePreview();
        this->expanded_ = !this->expanded_;
        this->setProperty("expanded", this->expanded_);
        this->updateAccessibility();
        const auto destination = this->expanded_ ? 1.0 : 0.0;
        const auto from = this->progress_;
        // Reverse from the visible progress, retaining the same layout.
        this->motionTimer_.stop();
        if (!this->frozenLayout_)
            this->frozenLayout_ = this->layoutPlan();
        const int duration = this->expanded_ ? this->timelineMs() : std::max(1300, this->timelineMs());
        this->motionDuration_ = std::max(60, int(duration * std::abs(destination - from)));
        this->motionStart_ = from;
        this->motionEnd_ = destination;
        if (this->onStarted)
            this->onStarted(this->expanded_, this->stride() * (1 + this->extraRows()));
        this->progress_ = from;
        this->lastMotionFrame_ = 0;
        this->motionElapsed_.start();
        this->motionTimer_.start();
    }

    QRectF chipRect() const
    {
        if (!this->hasOverflow())
            return {};
        return QRectF(int(this->firstRowCount()) * this->stride() + 0.5,
                      (this->stride() - this->iconSize()) / 2.0 + 0.5,
                      this->chipWidth() - 1, this->iconSize() - 1);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        this->hideBadgePreview();
        this->chipPressed_ = event->button() == Qt::LeftButton &&
                             this->chipRect().contains(event->position());
        event->setAccepted(this->chipPressed_);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->chipPressed_ &&
            this->chipRect().contains(event->position()))
        {
            this->toggleExpansion();
            event->accept();
        }
        this->chipPressed_ = false;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        this->setCursor(this->chipRect().contains(event->position())
                            ? Qt::PointingHandCursor : Qt::ArrowCursor);
        this->updateBadgePreview(event->position(), event->globalPosition().toPoint());
    }

    void leaveEvent(QEvent *event) override
    {
        this->hideBadgePreview();
        QWidget::leaveEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->badgePreviewOwner_.data() &&
            (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
             event->type() == QEvent::Hide || event->type() == QEvent::Close ||
             event->type() == QEvent::WindowDeactivate))
            this->hideBadgePreview();
        return QWidget::eventFilter(watched, event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return)
        {
            this->toggleExpansion();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (event->size() != event->oldSize())
            this->hideBadgePreview();
        if (event->size().width() != event->oldSize().width() && !this->frozenLayout_)
        {
            this->layoutCache_.reset();
            this->updateHeight();
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip)
        {
            auto *help = static_cast<QHelpEvent *>(event);
            // Handle Qt's delayed tooltip event as well as ordinary mouse
            // movement. Never allow the native text-only tooltip to appear.
            this->updateBadgePreview(help->pos(), help->globalPos());
            return true;
        }
        if (event->type() == QEvent::Hide || event->type() == QEvent::Move)
            this->hideBadgePreview();
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHints(QPainter::SmoothPixmapTransform |
                               QPainter::Antialiasing);
        const int size = this->iconSize();
        const int stride = this->stride();
        const auto total = this->badges_.size();
        const auto shown = this->firstRowCount();
        const auto columns = this->columns();
        auto font = this->chipFont();
        const auto countText = QString::number(total);
        auto drawBadge = [&](size_t i, int row, int col, qreal reveal = 1.0)
        {
            const auto &pixmap = this->badgePixmap(i);
            if (!pixmap.isNull())
            {
                const qreal eased = smoothStep(reveal);
                const qreal actualSize = size * (0.78 + 0.22 * eased);
                const qreal inset = (size - actualSize) / 2;
                painter.setOpacity(eased);
                painter.drawPixmap(QRectF(col * stride + inset,
                    row * stride + (stride - size) / 2.0 + inset +
                        (1 - eased) * 4 * this->scale_, actualSize, actualSize),
                    pixmap, QRectF(pixmap.rect()));
            }
        };
        for (size_t i = 0; i < shown; ++i)
        {
            if (event->region().intersects(QRect(int(i) * stride, 0, stride, stride)))
                drawBadge(i, 0, int(i));
        }
        // The first row never changes during the animation. The remaining
        // distinct badges reveal and fade into rows immediately below it.
        if (shown < total)
        {
            const auto chip = this->chipRect();
            const bool light = getTheme()->isLightTheme();
            painter.setPen(QColor(light ? "#bcb1ce" : "#5b516e"));
            painter.setBrush(QColor(light ? "#ece6f4" : "#2a2635"));
            painter.drawRoundedRect(chip, 5 * this->scale_, 5 * this->scale_);
            painter.setPen(QColor(light ? "#634c84" : "#c8b7eb"));
            painter.setFont(font);
            auto numberRect = chip.adjusted(4 * this->scale_, 0, -10 * this->scale_, 0);
            painter.drawText(numberRect, Qt::AlignCenter, countText);
            const auto center = QPointF(chip.right() - 6 * this->scale_, chip.center().y());
            const qreal h = (1 - 2 * smoothStep(this->progress_)) * 2 * this->scale_;
            painter.setPen(QPen(QColor(light ? "#634c84" : "#c8b7eb"), 1.25));
            painter.drawPolyline(QPolygonF{center + QPointF(-2.5 * this->scale_, -h / 2),
                                            center + QPointF(0, h / 2),
                                            center + QPointF(2.5 * this->scale_, -h / 2)});
            for (size_t i = shown; i < total && this->progress_ > 0; ++i)
            {
                const auto offset = i - shown;
                const auto row = int(offset / columns);
                const auto reveal = this->badgeProgress(offset, total - shown);
                if (reveal <= 0)
                    break;
                const auto col = int(offset % columns);
                if (event->region().intersects(QRect(col * stride, (row+1) * stride, stride, stride)))
                    drawBadge(i, row + 1, col, reveal);
            }
        }
    }

private:
    struct BadgeLayout {
        int width = 0;
        int chipWidth = 0;
        size_t columns = 1;
        size_t firstRow = 0;
        int extraRows = 0;
    };
    struct BadgeFrame {
        ImagePtr image;
        QPixmap pixmap;
        qint64 cacheKey = 0;
        int physicalSize = 0;
    };

    const QPixmap &badgePixmap(size_t index)
    {
        auto &frame = this->frameCache_[index];
        // Keep the selected source and scaled static raster throughout a
        // transition. Animated badges still receive their current frame.
        if (!frame.image || !this->frozenLayout_ || frame.pixmap.isNull())
        {
            const auto selected = this->badges_[index]->images.getImageOrLoaded(this->scale_);
            if (selected != frame.image)
            {
                frame = {};
                frame.image = selected;
            }
        }
        const auto pixels = avatarPixelSize(this->iconSize(), this->devicePixelRatioF());
        if (frame.image && (frame.pixmap.isNull() || frame.physicalSize != pixels || frame.image->animated()))
        {
            if (auto source = frame.image->pixmapOrLoad())
            {
                if (frame.cacheKey != source->cacheKey() || frame.physicalSize != pixels || frame.pixmap.isNull())
                {
                    frame.cacheKey = source->cacheKey();
                    frame.physicalSize = pixels;
                    frame.pixmap = source->scaled(pixels, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    frame.pixmap.setDevicePixelRatio(1.0);
                }
            }
        }
        return frame.pixmap;
    }

    void hideBadgePreview()
    {
        this->hoveredBadge_.reset();
        if (this->badgePreview_ && this->badgePreview_->isVisible())
            this->badgePreview_->hide();
    }

    void updateBadgePreview(const QPointF &position, const QPoint &globalPosition)
    {
        if (this->motionTimer_.isActive() ||
            (this->progress_ > 0 && this->progress_ < 1) ||
            !QRectF(this->rect()).contains(position) ||
            this->chipRect().contains(position))
        {
            this->hideBadgePreview();
            return;
        }

        const auto stride = this->stride();
        const auto size = this->iconSize();
        const int row = int(position.y() / stride);
        const int col = int(position.x() / stride);
        const auto prefix = this->firstRowCount();
        if ((row == 0 && size_t(col) >= prefix) ||
            (row > 0 && (this->progress_ < 1 || size_t(col) >= this->columns())))
        {
            this->hideBadgePreview();
            return;
        }
        const auto index = row == 0 ? size_t(col) :
            prefix + size_t(row - 1) * this->columns() + size_t(col);
        const QRectF iconRect(col * stride, row * stride + (stride - size) / 2., size, size);
        if (index >= this->badges_.size() || !iconRect.contains(position))
        {
            this->hideBadgePreview();
            return;
        }
        // The chat's preview explicitly selects the high-resolution source,
        // letting TooltipEntryWidget request it on demand. Do not magnify
        // the cached 1x badge that is used for painting the row.
        const auto &badge = this->badges_[index];
        const auto &image = badge->images.getImage(3.F);
        if (!image || image->isEmpty() || image->scale() <= 0)
        {
            this->hideBadgePreview();
            return;
        }
        if (!this->badgePreview_)
        {
            auto *owner = dynamic_cast<BaseWidget *>(this->window());
            if (!owner)
                return;
            this->badgePreviewOwner_ = owner;
            this->badgePreview_ = new TooltipWidget(owner);
            this->badgePreview_->setObjectName(QStringLiteral("UsercardBadgeHoverPreview"));
            // Preserve the isolated preview's invisible native-window mode.
            if (owner->testAttribute(Qt::WA_DontShowOnScreen))
                this->badgePreview_->setAttribute(Qt::WA_DontShowOnScreen);
            owner->installEventFilter(this);
        }

        if (this->hoveredBadge_ != index || !this->badgePreview_->isVisible())
        {
            float zoom = 1.F;
            switch (getSettings()->emoteTooltipScale.getEnum())
            {
                case EmoteTooltipScale::Small: zoom = .5F; break;
                case EmoteTooltipScale::Large: zoom = 1.5F; break;
                case EmoteTooltipScale::Huge: zoom = 2.F; break;
                default: break;
            }

            // Earned badges already provide a separate, concise title in
            // name; their tooltip adds the acquisition instructions after
            // <br>. Observed chat badges may have no name, so use only the
            // first line of their existing tooltip.
            auto caption = badge->name.string.trimmed();
            if (!caption.isEmpty())
            {
                const auto escaped = caption.toHtmlEscaped();
                if (escaped != caption)
                    caption = QStringLiteral("<span>") + escaped + QStringLiteral("</span>");
            }
            else
            {
                caption = badge->tooltip.string;
                const auto br = caption.indexOf(QStringLiteral("<br"), 0, Qt::CaseInsensitive);
                const auto nl = caption.indexOf(u'\n');
                const auto end = br < 0 ? nl : nl < 0 ? br : std::min(br, nl);
                if (end >= 0)
                    caption.truncate(end);
                caption = caption.trimmed();
            }
            this->badgePreview_->setOne(TooltipEntry::scaled(image, caption, zoom));
            this->badgePreview_->setWordWrap(false);
        }
        // Match ChannelView's placement. Movement within the same icon only
        // moves the window: its image, caption and layout are unchanged.
        this->badgePreview_->moveTo(globalPosition + QPoint(16, 16),
                                   widgets::BoundsChecking::CursorPosition);
        this->hoveredBadge_ = index;
        this->badgePreview_->show();
    }

    int stride() const { return std::max(1, int(24 * this->scale_)); }
    int iconSize() const { return std::max(1, int(20 * this->scale_)); }
    // Reserve the scrollbar's layout width, even when it is hidden. Its
    // appearance during a resize must never move badges into a different row.
    int stableWidth() const
    {
        int margins = 0;
        for (auto *p = this->parentWidget(); p; p = p->parentWidget())
        {
            if (auto *area = qobject_cast<QScrollArea *>(p))
            {
                const int available = area->contentsRect().width() - margins -
                    area->verticalScrollBar()->sizeHint().width();
                return std::max(1, available);
            }
            if (p->layout())
            {
                const auto m = p->layout()->contentsMargins();
                margins += m.left() + m.right();
            }
        }
        return this->width();
    }
    const BadgeLayout &layoutPlan() const
    {
        if (this->frozenLayout_)
            return *this->frozenLayout_;
        const int width = this->stableWidth();
        if (!this->layoutCache_ || this->layoutCache_->width != width)
        {
            BadgeLayout layout;
            layout.width = width;
            layout.chipWidth = QFontMetrics(this->chipFont()).horizontalAdvance(QString::number(this->badges_.size())) + int(20 * this->scale_);
            layout.columns = size_t(std::max(1, width / this->stride()));
            layout.firstRow = layout.columns >= this->badges_.size() ? this->badges_.size() :
                size_t(std::max(0, (width - layout.chipWidth) / this->stride()));
            const auto extra = this->badges_.size() - layout.firstRow;
            layout.extraRows = int((extra + layout.columns - 1) / layout.columns);
            this->layoutCache_ = layout;
        }
        return *this->layoutCache_;
    }
    size_t columns() const { return this->layoutPlan().columns; }
    QFont chipFont() const
    {
        auto font = this->font();
        font.setPixelSize(std::max(1, int(11 * this->scale_)));
        return font;
    }
    int chipWidth() const
    {
        return this->layoutPlan().chipWidth;
    }
    size_t firstRowCount() const
    {
        return this->layoutPlan().firstRow;
    }
    int extraRows() const
    {
        return this->layoutPlan().extraRows;
    }
    int currentHeight() const
    {
        if (this->badges_.empty())
            return 0;
        return this->stride() + qRound(this->heightFraction() * this->extraRows() * this->stride());
    }
    int timelineMs() const
    {
        return std::clamp(290 + 38 * int(this->badges_.size() - this->firstRowCount()), 460, 1850);
    }
    static qreal smoothStep(qreal value)
    {
        const auto t = std::clamp(value, 0.0, 1.0);
        return t * t * t * (t * (t * 6 - 15) + 10);
    }
    qreal badgeProgress(size_t offset) const
    {
        return this->badgeProgress(offset, this->badges_.size() - this->firstRowCount());
    }
    qreal badgeProgress(size_t offset, size_t extra) const
    {
        const auto duration = std::clamp(290 + 38 * int(extra), 460, 1850);
        // Make room for the rows before the first badge becomes visible;
        // when reversing the same timeline, fade badges before closing.
        const qreal lead = duration * 0.40;
        const qreal reveal = std::min(255.0, duration * 0.35);
        const qreal stagger = extra > 1 ? (duration - lead - reveal) / (extra - 1) : 0;
        return std::clamp((this->progress_ * duration - lead - stagger * offset) / reveal, 0.0, 1.0);
    }
    qreal heightFraction() const
    {
        return smoothStep(this->progress_ / 0.45);
    }
    void updateAccessibility()
    {
        this->setAccessibleName(uiText(this->expanded_ ? "Collapse badges (%1)" :
                                                         "Show all badges (%1)")
                                    .arg(this->badges_.size()));
        this->setFocusPolicy(this->hasOverflow() ? Qt::StrongFocus : Qt::NoFocus);
    }
    void motionTick()
    {
        const auto elapsed = this->motionElapsed_.nsecsElapsed();
        const auto fraction = std::min(1.0, elapsed / (this->motionDuration_ * 1e6));
        if (fraction < 1.0 && elapsed - this->lastMotionFrame_ < 11'000'000)
            return;
        this->lastMotionFrame_ = elapsed;
        this->progress_ = fraction >= 1.0 ? this->motionEnd_
            : this->motionStart_ + (this->motionEnd_ - this->motionStart_) * fraction;
        this->updateHeight();
        // Only repaint the moving rows and chip, not the stable first row.
        this->update(this->chipRect().toAlignedRect());
        if (this->height() > this->stride())
            this->update(QRect(0, this->stride(), this->width(), this->height() - this->stride()));
        if (fraction < 1.0 || !this->motionTimer_.isActive())
            return;
        this->motionTimer_.stop();
        if (this->onFinished)
            this->onFinished(this->expanded_);
        this->frozenLayout_.reset();
        this->updateHeight();
        if (auto pending = std::exchange(this->pendingBadges_, std::nullopt))
            this->setBadges(std::move(pending->first), pending->second);
        this->update();
    }

    void updateHeight()
    {
        const bool wasExpanded = this->expanded_ || this->progress_ > 0;
        if (!this->hasOverflow())
        {
            this->motionTimer_.stop();
            this->progress_ = 0;
            this->expanded_ = false;
            this->setProperty("expanded", false);
        }
        this->setProperty("firstRowBadgeCount", int(this->firstRowCount()));
        this->setProperty("badgeRevealProgress", this->progress_);
        const auto extra = this->badges_.size() - this->firstRowCount();
        this->setProperty("badgeFirstExtraProgress", extra ? this->badgeProgress(0) : 0.0);
        this->setProperty("badgeLastExtraProgress", extra ? this->badgeProgress(extra - 1) : 0.0);
        const auto height = this->currentHeight();
        if (this->minimumHeight() != height || this->maximumHeight() != height)
        {
            const bool shrinking = height < this->height();
            // During collapse release the inner height before the window
            // shrinks; during expansion do the opposite. Never require a
            // one-frame overflow just because the two layouts update apart.
            if (shrinking)
                this->setFixedHeight(height);
            if (this->onHeightChanged)
                this->onHeightChanged(this->heightFraction());
            if (!shrinking)
                this->setFixedHeight(height);
        }
        else if (this->motionTimer_.isActive() && this->onHeightChanged)
        {
            // The outer window may still have an integer-pixel step even
            // when the strip's rounded height hasn't changed. Complete it
            // now, not in a catch-up resize after the animation ends.
            this->onHeightChanged(this->heightFraction());
        }
        // Only the label/focus state changes when the badge count or toggle
        // changes; avoid generating accessibility events on animation frames.
        if (this->property("badgeRevealProgress").toDouble() == 0 ||
            this->progress_ >= 1)
            this->updateAccessibility();
        if (wasExpanded && !this->hasOverflow() && this->onFinished)
            this->onFinished(false);
    }

    std::vector<EmotePtr> badges_;
    std::vector<BadgeFrame> frameCache_;
    mutable std::optional<BadgeLayout> layoutCache_;
    std::optional<BadgeLayout> frozenLayout_;
    std::optional<std::pair<std::vector<EmotePtr>, float>> pendingBadges_;
    bool preserveOrder_ = false;
    float scale_ = 1.F;
    qreal progress_ = 0;
    bool expanded_ = false;
    bool chipPressed_ = false;
    std::optional<size_t> hoveredBadge_;
    QPointer<BaseWidget> badgePreviewOwner_;
    QPointer<TooltipWidget> badgePreview_;
    QTimer motionTimer_;
    QElapsedTimer motionElapsed_;
    qint64 lastMotionFrame_ = 0;
    qreal motionStart_ = 0;
    qreal motionEnd_ = 0;
    int motionDuration_ = 1;
    pajlada::Signals::SignalHolder connections_;
};

QString chatVaultTwitchChannelUrl(const QString &login)
{
    return QStringLiteral("https://chatvau.lt/channel/twitch/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(login.toLower())));
}

QString lurkologyTwitchProfileUrl(const QString &login)
{
    return QStringLiteral("https://lurkology.com/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(login.toLower())));
}

QString sevenTVUserCacheKey(const QString &userID, bool isKick)
{
    return (isKick ? QStringLiteral("kick:") : QStringLiteral("twitch:")) +
           userID;
}

QHash<QString, QString> &sevenTVUserIDCache()
{
    static QHash<QString, QString> cache;
    return cache;
}

class NameHistoryMenuRow final : public QWidget
{
public:
    NameHistoryMenuRow(QString login, QString leftText, QString rightText,
                       QWidget *parent)
        : QWidget(parent)
        , login_(std::move(login))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setMouseTracking(true);
        this->setToolTip(uiText("Click to copy ") + this->login_);
        if (parent)
        {
            this->setFont(parent->font());
        }
        auto palette = this->palette();
        palette.setColor(QPalette::WindowText,
                         QColor(getTheme()->isLightTheme() ? "#1b1a22"
                                                          : "#e8e6f0"));
        this->setPalette(palette);

        const auto metrics = this->fontMetrics();
        const auto loginWidth = std::max(
            metrics.horizontalAdvance("koplayzenthraquiluxmorive") + 8, 132);
        const auto dateWidth = metrics.horizontalAdvance("Sep 30, 2026") + 8;
        const auto dashWidth = metrics.horizontalAdvance("-") + 8;

        auto *layout = new QGridLayout(this);
        layout->setContentsMargins(8, 2, 8, 2);
        layout->setHorizontalSpacing(4);
        layout->setVerticalSpacing(0);

        auto *loginLabel = new QLabel(
            metrics.elidedText(this->login_, Qt::ElideRight, loginWidth), this);
        loginLabel->setFixedWidth(loginWidth);
        loginLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(loginLabel, 0, 0, Qt::AlignVCenter);

        auto *leftLabel = new QLabel(std::move(leftText), this);
        leftLabel->setFixedWidth(dateWidth);
        leftLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        leftLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(leftLabel, 0, 1, Qt::AlignVCenter);

        auto *dashLabel = new QLabel("-", this);
        dashLabel->setFixedWidth(dashWidth);
        dashLabel->setAlignment(Qt::AlignCenter);
        dashLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(dashLabel, 0, 2, Qt::AlignVCenter);

        auto *rightLabel = new QLabel(std::move(rightText), this);
        rightLabel->setFixedWidth(dateWidth);
        rightLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        rightLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(rightLabel, 0, 3, Qt::AlignVCenter);

        const auto height = std::max(metrics.height() + 6, 22);
        this->setFixedSize(loginWidth + dateWidth * 2 + dashWidth + 36, height);
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter)
        {
            this->hovered_ = true;
            this->update();
        }
        else if (event->type() == QEvent::Leave)
        {
            this->hovered_ = false;
            this->update();
        }

        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        if (this->hovered_)
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            auto highlight = getTheme()->accent;
            highlight.setAlphaF(getTheme()->isLightTheme() ? 0.18F : 0.28F);
            painter.setPen(Qt::NoPen);
            painter.setBrush(highlight);
            painter.drawRoundedRect(QRectF(this->rect()).adjusted(1, 0, -1, 0),
                                    6, 6);
        }

        QWidget::paintEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QWidget::mouseReleaseEvent(event);
            return;
        }

        crossPlatformCopy(this->login_);
        for (auto *widget = this->parentWidget(); widget != nullptr;
             widget = widget->parentWidget())
        {
            if (auto *menu = qobject_cast<QMenu *>(widget))
            {
                noctistooltip::showText(event->globalPosition().toPoint(),
                    uiText(QString("Copied %1")).arg(this->login_),
                    menu->parentWidget(), {}, 1600);
                menu->close();
                break;
            }
        }
    }

private:
    QString login_;
    bool hovered_ = false;
};

class ClickableColorRow final : public Button
{
public:
    ClickableColorRow()
        : Button(nullptr)
        , layout_(this)
    {
        this->layout_.setContentsMargins(8, 0, 8, 0);
        this->layout_.setSpacing(5);
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }

    QHBoxLayout *layout()
    {
        return &this->layout_;
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        // Keep Button's reliable click handling without its hover/click wash.
    }

    void paintContent(QPainter & /*painter*/) override
    {
    }

private:
    QHBoxLayout layout_;
};

class ClickablePaintName final : public Button
{
public:
    ClickablePaintName()
        : Button(nullptr)
        , layout_(this)
    {
        this->layout_.setContentsMargins(0, 0, 0, 0);
        this->layout_.setSpacing(0);
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        this->setCursor(Qt::PointingHandCursor);

        this->pixmapLabel_ = new QLabel(this);
        this->pixmapLabel_->setSizePolicy(QSizePolicy::Fixed,
                                          QSizePolicy::Fixed);
        this->pixmapLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        this->pixmapLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->layout_.addWidget(this->pixmapLabel_);
    }

    QLabel *pixmapLabel() const
    {
        return this->pixmapLabel_;
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        // Keep Button's reliable click handling without its hover/click wash.
    }

    void paintContent(QPainter & /*painter*/) override
    {
    }

private:
    QHBoxLayout layout_;
    QLabel *pixmapLabel_ = nullptr;
};

Label *addCopyableLabel(LayoutCreator<QHBoxLayout> box, const QString &tooltip,
                        PixmapButton **copyButton = nullptr,
                        Label *customLabel = nullptr)
{
    auto *label = customLabel ? customLabel : new Label;
    box->addWidget(label);
    auto button = box.emplace<PixmapButton>();
    if (copyButton != nullptr)
    {
        button.assign(copyButton);
    }
    button->setPixmap(getApp()->getThemes()->buttons.copy);
    button->setScaleIndependentSize(18, 18);
    button->setDim(DimButton::Dim::Lots);
    button->setToolTip(uiText(tooltip));
    QObject::connect(button.getElement(), &Button::leftClicked, [label] {
        auto copyText = label->property("copy-text").toString();

        crossPlatformCopy(copyText.isEmpty() ? label->getText() : copyText);
    });

    return label;
};

void createUsercardStatusRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                             QLabel **iconOut, Label **labelOut)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(4);

    auto *icon = new QLabel(row);
    icon->setVisible(false);
    layout->addWidget(icon, 0, Qt::AlignVCenter);

    auto *label = new Label("");
    label->setPadding({});
    layout->addWidget(label, 1, Qt::AlignVCenter);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *iconOut = icon;
    *labelOut = label;
}

void createUsercardPaintRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                            QLabel **pixmapOut, Button **paintNameButtonOut)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(0);

    auto *prefixLabel = new Label(uiText(TEXT_SEVENTV_PAINT));
    prefixLabel->setPadding({});
    layout->addWidget(prefixLabel, 0, Qt::AlignVCenter);

    auto *paintNameButton = new ClickablePaintName;
    layout->addWidget(paintNameButton, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *pixmapOut = paintNameButton->pixmapLabel();
    *paintNameButtonOut = paintNameButton;
}

void createUsercardColorRow(LayoutCreator<QVBoxLayout> &vbox, QWidget **rowOut,
                            QWidget **swatchOut, Label **labelOut)
{
    auto *row = new ClickableColorRow;
    auto *layout = row->layout();
    row->setCursor(Qt::PointingHandCursor);
    row->setToolTip(uiText("Click to copy color"));

    auto *swatch = new QFrame(row);
    swatch->setObjectName("UsercardColorSwatch");
    swatch->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(swatch, 0, Qt::AlignVCenter);

    auto *label = new Label("");
    label->setPadding({});
    label->setCursor(Qt::PointingHandCursor);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);

    row->setVisible(false);
    vbox->addWidget(row);

    *rowOut = row;
    *swatchOut = swatch;
    *labelOut = label;
}

QPixmap renderUsercardStatusIcon(const QString &path, int size, qreal scale)
{
    static QHash<QString, QPixmap> cache;
    const auto key = QStringLiteral("%1:%2:%3").arg(path).arg(size).arg(scale);
    if (auto it = cache.find(key); it != cache.end())
    {
        return *it;
    }

    QPixmap pixmap(QSize(size, size) * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);

    QSvgRenderer renderer(path);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(0, 0, size, size));

    cache.insert(key, pixmap);
    return pixmap;
}

QDateTime parseIvrTimestamp(const QString &isoTimestamp)
{
    auto timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    }
    if (!timestamp.isValid() && isoTimestamp.contains('.'))
    {
        auto trimmed = isoTimestamp;
        const auto dotIndex = trimmed.indexOf('.');
        const auto zoneIndex = trimmed.indexOf('Z', dotIndex);
        if (zoneIndex > dotIndex + 4)
        {
            trimmed = trimmed.left(dotIndex + 4) + trimmed.mid(zoneIndex);
            timestamp = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
        }
    }

    return timestamp;
}

QString formatIvrDate(const QString &isoTimestamp)
{
    const auto timestamp = parseIvrTimestamp(isoTimestamp);
    if (!timestamp.isValid())
    {
        return {};
    }

    return timestamp.toLocalTime().date().toString(Qt::ISODate);
}

int completeCalendarMonthsBetween(const QDate &from, const QDate &to)
{
    if (!from.isValid() || !to.isValid() || from > to)
    {
        return 0;
    }

    auto months = (to.year() - from.year()) * 12 + (to.month() - from.month());
    if (to.day() < from.day())
    {
        --months;
    }

    return std::max(months, 0);
}

/// "1 месяц", "3 месяца", "11 месяцев"
QString formatRussianMonths(int months)
{
    if (activeUiLanguage() != "ru")
    {
        return QStringLiteral("%1 %2").arg(months).arg(months == 1 ? "month" : "months");
    }
    const int mod10 = months % 10;
    const int mod100 = months % 100;
    const char *word = (mod10 == 1 && mod100 != 11) ? "месяц"
                       : (mod10 >= 2 && mod10 <= 4 &&
                          (mod100 < 10 || mod100 >= 20))
                           ? "месяца"
                           : "месяцев";
    return QString::number(months) + u' ' + QString::fromUtf8(word);
}

QString formatUsercardCount(int count, const QString &unit)
{
    if (activeUiLanguage() == QStringLiteral("ru"))
    {
        if (unit == QStringLiteral("month"))
        {
            return formatRussianMonths(count);
        }
        const auto mod10 = count % 10;
        const auto mod100 = count % 100;
        const bool singular = mod10 == 1 && mod100 != 11;
        const bool few = mod10 >= 2 && mod10 <= 4 &&
                         (mod100 < 10 || mod100 >= 20);
        if (unit == QStringLiteral("week"))
        {
            return QString::number(count) + u' ' +
                   (singular ? QStringLiteral("неделя")
                             : few ? QStringLiteral("недели")
                                   : QStringLiteral("недель"));
        }
        if (unit == QStringLiteral("day"))
        {
            return QString::number(count) + u' ' +
                   (singular ? QStringLiteral("день")
                             : few ? QStringLiteral("дня")
                                   : QStringLiteral("дней"));
        }
    }
    return QStringLiteral("%1 %2%3").arg(count).arg(unit).arg(
        count == 1 ? QString() : QStringLiteral("s"));
}

QString formatUsercardYearsMonths(int totalMonths)
{
    if (totalMonths < 12)
    {
        return {};
    }

    const auto years = totalMonths / 12;
    const auto months = totalMonths % 12;
    auto result = uiText(QStringLiteral("%1 г.")).arg(years);
    if (months > 0)
    {
        result += uiText(QStringLiteral(" %1 мес.")).arg(months);
    }

    return QStringLiteral(" (%1)").arg(result);
}

QString formatAccountCreated(const QDateTime &created)
{
    if (!created.isValid())
    {
        return uiText(TEXT_CREATED).arg(uiText(TEXT_UNAVAILABLE));
    }
    const auto date = created.toLocalTime().date();
    auto text = uiText(TEXT_CREATED).arg(date.toString(Qt::ISODate));
    const auto today = QDate::currentDate();
    if (date > today)
    {
        return text;
    }
    const auto months = completeCalendarMonthsBetween(date, today);
    QString age;
    if (months >= 12)
    {
        age = uiText(QStringLiteral("%1 г.")).arg(months / 12);
        if (months % 12)
        {
            age += uiText(QStringLiteral(" %1 мес.")).arg(months % 12);
        }
    }
    else if (months)
    {
        age = uiText(QStringLiteral("%1 мес.")).arg(months);
    }
    else
    {
        age = uiText(QStringLiteral("%1 дн.")).arg(date.daysTo(today));
    }
    return text + QStringLiteral(" · ") + age;
}

QString formatUsercardFollowRelativeTime(const QDate &followedDate)
{
    const auto today = QDateTime::currentDateTimeUtc().date();
    if (!followedDate.isValid() || followedDate > today)
    {
        return {};
    }

    const auto months = completeCalendarMonthsBetween(followedDate, today);
    if (months >= 12)
    {
        return formatUsercardYearsMonths(months);
    }
    if (months >= 1)
    {
        return QStringLiteral(" (%1)").arg(
            formatUsercardCount(months, QStringLiteral("month")));
    }

    const auto days = followedDate.daysTo(today);
    if (days >= 14)
    {
        return QStringLiteral(" (%1)").arg(
            formatUsercardCount(days / 7, QStringLiteral("week")));
    }
    if (days > 0)
    {
        return QStringLiteral(" (%1)").arg(
            formatUsercardCount(days, QStringLiteral("day")));
    }

    return uiText(QStringLiteral(" (today)"));
}

QString formatFollowButtonToolTip(const QString &displayName, bool following,
                                  const std::optional<QDateTime> &followedAt)
{
    if (!following)
    {
        return QString("Follow %1").arg(displayName);
    }

    QString tooltip = QString("Unfollow %1").arg(displayName);

    if (!followedAt || !followedAt->isValid())
    {
        return tooltip;
    }

    const auto followedDate = followedAt->date();
    const auto followingSince = followedDate.toString(Qt::ISODate);
    auto relativeTime = QString();
    if (getSettings()->showUsercardFollowageRelativeTime)
    {
        relativeTime = formatUsercardFollowRelativeTime(followedDate);
    }

    tooltip += u'\n' + uiText(QStringLiteral("Following since ")) + followingSince +
               relativeTime;

    return tooltip;
}

QString formatUsercardStatus(const IvrUserProfile &profile)
{
    if (profile.isStaff)
    {
        return uiText("Staff");
    }
    if (profile.isVerifiedBot)
    {
        return uiText("Verified Bot");
    }
    if (profile.isPartner)
    {
        return uiText("Partner");
    }
    if (profile.isAffiliate)
    {
        return uiText("Affiliate");
    }
    if (profile.isPreAffiliate)
    {
        return uiText("Pre Affiliate");
    }

    return uiText("Non Affiliate");
}

/// A ban / timeout / deleted-message notice about someone (not "chat cleared").
bool isModerationNoticeAbout(const Message &message)
{
    return !message.timeoutUser.isEmpty() &&
           !message.flags.has(MessageFlag::ClearChat) &&
           message.flags.hasAny(MessageFlag::Timeout, MessageFlag::Untimeout,
                                MessageFlag::ModerationAction,
                                MessageFlag::Deletion);
}

bool checkMessageUserName(const QString &userName, MessagePtr message,
                          const QString &userId = {},
                          const QString &channelName = {})
{
    if (message->flags.has(MessageFlag::Whisper))
    {
        return false;
    }

    // Punishments seen in this channel are shown even when the person wrote
    // nothing here. A shared-chat notice from another channel is not one.
    if (isModerationNoticeAbout(*message))
    {
        if (message->flags.has(MessageFlag::SharedMessage) &&
            !channelName.isEmpty() &&
            message->channelName.compare(channelName, Qt::CaseInsensitive) != 0)
        {
            return false;
        }
        if (!userId.isEmpty() && !message->timeoutUserID.isEmpty())
        {
            return message->timeoutUserID == userId;
        }
        // No target ID was sent (older notices): the login in this channel's
        // buffer is the only link, and a new owner of a reused login cannot
        // have been punished here before taking it within one session.
        if (message->timeoutUser.compare(userName, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
        if (!userId.isEmpty())
        {
            return false;
        }
    }

    // Once identity is known, a reused login or an ID-less legacy record must
    // never be attributed to this person.
    if (!userId.isEmpty())
    {
        return message->userID == userId;
    }

    bool isSubscription = message->flags.has(MessageFlag::Subscription) &&
                          message->loginName.isEmpty() &&
                          message->messageText.split(" ").at(0).compare(
                              userName, Qt::CaseInsensitive) == 0;

    bool isModAction =
        message->timeoutUser.compare(userName, Qt::CaseInsensitive) == 0;
    bool isSelectedUser =
        message->loginName.compare(userName, Qt::CaseInsensitive) == 0;

    return (isSubscription || isModAction || isSelectedUser);
}

bool messageHasTwitchBadge(const Message &message, QStringView badge)
{
    const auto badgeName = badge.toString();
    for (const auto &twitchBadge : message.twitchBadges)
    {
        if (twitchBadge.key_.compare(badgeName, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }

    return false;
}

ChannelPtr filterMessages(const QString &userName, const QString &userId,
                          ChannelPtr channel)
{
    std::vector<MessagePtr> snapshot = channel->getMessageSnapshot();

    ChannelPtr channelPtr;
    if (channel->isTwitchChannel())
    {
        channelPtr = std::make_shared<TwitchChannel>(channel->getName());
    }
    else
    {
        channelPtr =
            std::make_shared<Channel>(channel->getName(), Channel::Type::None);
    }

    for (const auto &message : snapshot)
    {
        if (checkMessageUserName(userName, message, userId, channel->getName()))
        {
            channelPtr->addMessage(message, MessageContext::Repost);
        }
    }

    return channelPtr;
};

QString escapeIrcTagValue(QString value)
{
    value.replace(QChar(u'\\'), QStringLiteral("\\\\"));
    value.replace(QChar(u';'), QStringLiteral("\\:"));
    value.replace(QChar(u' '), QStringLiteral("\\s"));
    value.replace(QChar(u'\r'), QStringLiteral("\\r"));
    value.replace(QChar(u'\n'), QStringLiteral("\\n"));
    return value;
}

QString cleanIrcMessageBody(QString value)
{
    value.replace(QChar(u'\r'), QChar(u' '));
    value.replace(QChar(u'\n'), QChar(u' '));
    return value;
}

MessagePtr makeUsercardModLogMessage(const GqlUsercardMessage &message,
                                     TwitchChannel *twitchChannel,
                                     const QString &channelName,
                                     const QString &fallbackUserId,
                                     const QString &profileDisplayName)
{
    auto sentAt = parseIvrTimestamp(message.sentAt);
    if (!sentAt.isValid())
    {
        sentAt = QDateTime::currentDateTime();
    }
    else
    {
        sentAt = sentAt.toLocalTime();
    }

    const auto userId =
        message.senderId.isEmpty() ? fallbackUserId : message.senderId;
    auto displayName = message.senderDisplayName.trimmed();
    if (displayName.isEmpty() ||
        (displayName == message.senderLogin &&
         displayName == displayName.toLower()))
    {
        // Keep historical logins after a rename. A profile can only restore
        // capitalization for the same login and Twitch identity.
        displayName = userId == fallbackUserId && !userId.isEmpty() &&
                              profileDisplayName.compare(message.senderLogin,
                                                         Qt::CaseInsensitive) == 0
                          ? profileDisplayName
                          : message.senderLogin;
    }
    const auto login =
        message.senderLogin.isEmpty() ? displayName : message.senderLogin;
    const auto body = cleanIrcMessageBody(message.text);

    if (twitchChannel != nullptr && !login.isEmpty())
    {
        QStringList tags;
        if (!message.id.isEmpty())
        {
            tags << QStringLiteral("id=") + escapeIrcTagValue(message.id);
        }
        if (!userId.isEmpty())
        {
            tags << QStringLiteral("user-id=") + escapeIrcTagValue(userId);
        }
        if (!message.senderColor.isEmpty())
        {
            tags << QStringLiteral("color=") +
                        escapeIrcTagValue(message.senderColor);
        }
        if (!message.senderBadges.isEmpty())
        {
            tags << QStringLiteral("badges=") +
                        escapeIrcTagValue(message.senderBadges);
        }
        if (!twitchChannel->roomId().isEmpty())
        {
            tags << QStringLiteral("room-id=") +
                        escapeIrcTagValue(twitchChannel->roomId());
        }
        if (!displayName.isEmpty())
        {
            tags << QStringLiteral("display-name=") +
                        escapeIrcTagValue(displayName);
        }
        tags << QStringLiteral("login=") + escapeIrcTagValue(login);
        if (message.isFirstMessage)
        {
            tags << QStringLiteral("first-msg=1");
        }
        if (sentAt.isValid())
        {
            tags << QStringLiteral("tmi-sent-ts=") +
                        QString::number(sentAt.toMSecsSinceEpoch());
        }

        const auto tagsText =
            tags.isEmpty() ? QString() : u"@" % tags.join(';') % u" ";
        const auto fakeIrcData =
            QStringLiteral("%1:%2!%2@%2.tmi.twitch.tv PRIVMSG #%3 :%4")
                .arg(tagsText, login, twitchChannel->getName(), body);

        auto *fakeMessage =
            Communi::IrcMessage::fromData(fakeIrcData.toUtf8(), nullptr);
        if (fakeMessage != nullptr && fakeMessage->command() == "PRIVMSG")
        {
            MessageParseArgs args;
            args.allowIgnore = false;
            auto result = MessageBuilder::makeIrcMessage(
                twitchChannel, fakeMessage, args, body, 0);
            auto builtMessage = std::move(result.first);
            fakeMessage->deleteLater();
            fakeMessage = nullptr;

            if (builtMessage)
            {
                builtMessage->flags.set(MessageFlag::DoNotLog,
                                        MessageFlag::DoNotTriggerNotification);
                if (message.isDeleted)
                {
                    builtMessage->flags.set(MessageFlag::Disabled,
                                            MessageFlag::InvalidReplyTarget);
                }
                return builtMessage;
            }
        }
        if (fakeMessage != nullptr)
        {
            fakeMessage->deleteLater();
        }
    }

    auto color = QColor(message.senderColor);
    const auto userColor = color.isValid() ? MessageColor(color)
                                           : MessageColor(MessageColor::Text);

    MessageBuilder builder;
    builder->id = message.id;
    builder->loginName = message.senderLogin;
    builder->displayName = displayName;
    builder->userID = userId;
    builder->messageText = body;
    builder->searchText = displayName + QStringLiteral(": ") + body;
    builder->channelName = channelName;
    builder->serverReceivedTime = sentAt;
    builder->usernameColor = color;
    builder->flags.set(MessageFlag::DoNotLog,
                       MessageFlag::DoNotTriggerNotification);
    if (message.isDeleted)
    {
        builder->flags.set(MessageFlag::Disabled,
                           MessageFlag::InvalidReplyTarget);
    }
    if (message.isFirstMessage)
    {
        builder->flags.set(MessageFlag::FirstMessage);
    }

    builder.emplace<TimestampElement>(sentAt.time());
    builder
        .emplace<TextElement>(displayName + QStringLiteral(":"),
                              MessageElementFlag::Username, userColor,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, message.senderLogin});
    builder.appendOrEmplaceText(body, MessageColor::Text);

    return builder.release();
}

QDateTime oldestUsercardMessageTime(const ChannelPtr &channel)
{
    QDateTime oldest;
    if (!channel)
    {
        return oldest;
    }

    for (const auto &message : channel->getMessageSnapshot())
    {
        if (message == nullptr || !message->serverReceivedTime.isValid())
        {
            continue;
        }

        if (!oldest.isValid() || message->serverReceivedTime < oldest)
        {
            oldest = message->serverReceivedTime;
        }
    }

    return oldest;
}

qreal usercardMessagePreloadDistance(const Scrollbar &scrollbar)
{
    return std::clamp<qreal>(scrollbar.getPageSize() * 0.75, 8.0, 24.0);
}

const auto borderColor = QColor(255, 255, 255, 80);

int calculateTimeoutDuration(TimeoutButton timeout)
{
    static const QMap<QString, int> durations{
        {"s", 1}, {"m", 60}, {"h", 3600}, {"d", 86400}, {"w", 604800},
    };
    return timeout.second * durations[timeout.first];
}

QString normalizeModerationReason(QString reason)
{
    reason.replace('\r', ' ');
    reason.replace('\n', ' ');
    return reason.trimmed();
}

QString appendModerationReason(QString command, const QString &reason)
{
    const auto cleanedReason = normalizeModerationReason(reason);
    if (cleanedReason.isEmpty())
    {
        return command;
    }

    return command + ' ' + cleanedReason;
}

QString timeoutButtonReason(int index)
{
    if (index < 0)
    {
        return {};
    }

    const auto reasons = getSettings()->timeoutButtonReasons.getValue();
    if (index >= static_cast<int>(reasons.size()))
    {
        return {};
    }

    return normalizeModerationReason(reasons[index]);
}

QString timeoutBanReason()
{
    return normalizeModerationReason(
        getSettings()->timeoutBanReason.getValue());
}

bool shouldPromptForModerationReason(Qt::MouseButton button)
{
    if (button == Qt::RightButton &&
        getSettings()->timeoutReasonPromptOnRightClick.getValue())
    {
        return true;
    }

    if (!getSettings()->timeoutReasonPromptOnModifier.getValue())
    {
        return false;
    }

    const auto configuredModifier =
        getSettings()->timeoutReasonPromptModifier.getValue();
    const auto modifiers = QGuiApplication::keyboardModifiers();
    if (configuredModifier == "Ctrl")
    {
        return modifiers.testFlag(Qt::ControlModifier);
    }
    if (configuredModifier == "Alt")
    {
        return modifiers.testFlag(Qt::AltModifier);
    }

    return modifiers.testFlag(Qt::ShiftModifier);
}

bool shouldHandleModerationButtonClick(Qt::MouseButton button)
{
    return button == Qt::LeftButton ||
           (button == Qt::RightButton &&
            getSettings()->timeoutReasonPromptOnRightClick.getValue());
}

class ModerationReasonPopup final : public DraggablePopup
{
public:
    ModerationReasonPopup(const QString &title, const QString &placeholder,
                          const QString &initialReason, bool showSendButton,
                          std::function<void(QString)> onSend,
                          QWidget *parent = nullptr)
        : DraggablePopup(true, parent)
        , showSendButton_(showSendButton)
        , onSend_(std::move(onSend))
    {
        this->setWindowTitle(title);
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setScaleIndependentSize(showSendButton ? QSize(360, 42)
                                                     : QSize(290, 42));

        auto layout = LayoutCreator<QWidget>(this->getLayoutContainer())
                          .setLayoutType<QHBoxLayout>();
        this->layout_ = layout.getElement();

        this->input_ = layout.emplace<QLineEdit>().getElement();
        this->input_->setPlaceholderText(placeholder);
        this->input_->setText(initialReason);
        this->input_->setMouseTracking(true);
        this->input_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        if (this->showSendButton_)
        {
            this->sendButton_ =
                layout.emplace<QPushButton>(uiText("Send")).getElement();
            this->sendButton_->setSizePolicy(QSizePolicy::Fixed,
                                             QSizePolicy::Fixed);
            this->sendButton_->setCursor(Qt::PointingHandCursor);
            this->sendButton_->setMouseTracking(true);

            QObject::connect(this->sendButton_, &QPushButton::clicked, this,
                             [this] {
                                 this->send();
                             });
        }
        QObject::connect(this->input_, &QLineEdit::returnPressed, this, [this] {
            this->send();
        });

        this->applyScaledLayout();
    }

    void showCenteredAt(const QPoint &center)
    {
        this->show();
        this->moveTo(center - QPoint(this->width() / 2, this->height() / 2),
                     widgets::BoundsChecking::DesiredPosition);
    }

protected:
    void scaleChangedEvent(float scale) override
    {
        DraggablePopup::scaleChangedEvent(scale);
        this->applyScaledLayout();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
        {
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }

        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            (!event->modifiers() ||
             event->modifiers().testFlag(Qt::KeypadModifier)))
        {
            this->send();
            return;
        }

        DraggablePopup::keyPressEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        DraggablePopup::showEvent(event);

        this->input_->setFocus(Qt::PopupFocusReason);
        this->input_->selectAll();
    }

private:
    void applyScaledLayout()
    {
        const auto effectiveScale = std::max(0.75F, this->scale());
        const int marginX = std::max(4, int(6 * effectiveScale));
        const int marginY = std::max(2, int(4 * effectiveScale));
        const int spacing = std::max(4, int(6 * effectiveScale));
        const int controlHeight = std::max(22, int(24 * effectiveScale));

        this->layout_->setContentsMargins(marginX, marginY, marginX, marginY);
        this->layout_->setSpacing(spacing);

        const auto uiFont =
            getApp()->getFonts()->getFont(FontStyle::UiMedium, effectiveScale);
        const auto buttonFont = getApp()->getFonts()->getFont(
            FontStyle::UiMediumBold, effectiveScale);
        this->input_->setFont(uiFont);
        this->input_->setFixedHeight(controlHeight);
        if (this->sendButton_ != nullptr)
        {
            this->sendButton_->setFont(buttonFont);
            const QFontMetrics buttonMetrics(buttonFont);
            this->sendButton_->setFixedHeight(controlHeight);
            this->sendButton_->setMinimumWidth(
                buttonMetrics.horizontalAdvance("Send") +
                std::max(22, int(24 * effectiveScale)));
        }
    }

    void send()
    {
        if (this->sent_)
        {
            return;
        }
        this->sent_ = true;

        if (this->onSend_)
        {
            this->onSend_(this->input_->text());
        }
        QTimer::singleShot(0, this, &QWidget::close);
    }

    QHBoxLayout *layout_{};
    QLineEdit *input_{};
    QPushButton *sendButton_{};
    bool showSendButton_ = false;
    std::function<void(QString)> onSend_;
    bool sent_ = false;
};

QString hashUrl(const QString &url)
{
    QByteArray bytes;

    bytes.append(url.toUtf8());
    QByteArray hashBytes(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));

    return hashBytes.toHex();
}

constexpr qint64 FOLLOWING_STATUS_RETRY_INTERVAL_MS = 30'000;

QMutex &activeUsercardsMutex()
{
    static QMutex mutex;
    return mutex;
}

QList<QPointer<UserInfoPopup>> &activeUsercards()
{
    static QList<QPointer<UserInfoPopup>> cards;
    return cards;
}

void registerActiveUsercard(UserInfoPopup *popup)
{
    QMutexLocker locker(&activeUsercardsMutex());
    activeUsercards().append(popup);
}

void unregisterActiveUsercard(UserInfoPopup *popup)
{
    QMutexLocker locker(&activeUsercardsMutex());
    activeUsercards().removeAll(popup);
}

}  // namespace

namespace chatterino {

using namespace literals;

UserInfoPopup::UserInfoPopup(bool closeAutomatically, Split *split,
                             bool offlinePreview)
    : DraggablePopup(closeAutomatically, split)
    , split_(split)
    , closeAutomatically_(closeAutomatically)
    , offlinePreview_(offlinePreview)
{
    if (this->closeAutomatically_)
    {
        // A translucent antialiased shell instead of a binary QRegion mask.
        // Set this before the first native show to keep Windows layered bounds
        // and the inner widgets consistent for the lifetime of the popup.
        this->setAttribute(Qt::WA_TranslucentBackground, true);
        this->setAutoFillBackground(false);
    }
    registerActiveUsercard(this);

    this->followingStatusChangedConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->followingStatusChanged_.connect([this] {
                this->updateUsercardFollowButton();
            }));

    assert(split != nullptr &&
           "split being nullptr causes lots of bugs down the road");
    this->setWindowTitle(uiText("Usercard"));

    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](std::vector<QString>) -> QString {
             this->deleteLater();
             return "";
         }},
        {"scrollPage",
         [this](std::vector<QString> arguments) -> QString {
             if (arguments.size() == 0)
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             auto direction = arguments.at(0);

             auto &scrollbar = this->ui_.latestMessages->getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"execModeratorAction",
         [this](std::vector<QString> arguments) -> QString {
             if (!this->shouldShowModerationActions())
             {
                 return "";
             }

             if (arguments.empty())
             {
                 return "execModeratorAction action needs an argument, which "
                        "moderation action to execute, see description in the "
                        "editor";
             }
             auto target = arguments.at(0);
             UsercardModerationRequest request;

             // these can't have /timeout/ buttons because they are not timeouts
             if (target == "ban")
             {
                 request.action = UsercardModerationAction::Ban;
                 request.reason = timeoutBanReason();
             }
             else if (target == "unban")
             {
                 request.action = UsercardModerationAction::Unban;
             }
             else
             {
                 // find and execute timeout button #TARGET

                 bool ok;
                 int buttonNum = target.toInt(&ok);
                 if (!ok)
                 {
                     return uiText(QString("Invalid argument for execModeratorAction: "
                                    "%1. Use "
                                    "\"ban\", \"unban\" or the number of the "
                                    "timeout "
                                    "button to execute"))
                         .arg(target);
                 }

                 const auto &timeoutButtons =
                     getSettings()->timeoutButtons.getValue();
                 if (static_cast<int>(timeoutButtons.size()) < buttonNum ||
                     0 >= buttonNum)
                 {
                     return uiText(QString("Invalid argument for execModeratorAction: "
                                    "%1. Integer out of usable range: [1, %2]"))
                         .arg(buttonNum,
                              static_cast<int>(timeoutButtons.size()));
                 }
                 const auto &button = timeoutButtons.at(buttonNum - 1);
                 request.action = UsercardModerationAction::Timeout;
                 request.durationSeconds = calculateTimeoutDuration(button);
                 request.reason = timeoutButtonReason(buttonNum - 1);
             }

             this->executeUsercardModerationAction(request);
             return "";
         }},
        {"pin",
         [this](std::vector<QString> /*arguments*/) -> QString {
             this->togglePinned();
             return "";
         }},
        {"openProfilePictureMenu",
         [this](std::vector<QString> /*arguments*/) -> QString {
             return this->showProfilePictureContextMenu();
         }},

        // these actions make no sense in the context of a usercard, so they aren't implemented
        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"search", nullptr},
    };

    if (!this->offlinePreview_)
    {
        this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
            HotkeyCategory::PopupWindow, actions, this);
    }

    auto layers = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QGridLayout>()
                      .withoutMargin();
    const int shadow = this->closeAutomatically_ ? usercardChrome::shadowInset(this->scale()) : 0;
    const int edge = this->closeAutomatically_
        ? usercardChrome::contentInset(this->scale(), UsercardResizeGrip::edgeBand(this->scale()))
        : UsercardResizeGrip::edgeBand(this->scale());
    layers->setContentsMargins(edge, edge, edge, edge);
    this->ui_.columns = new QSplitter(Qt::Horizontal, this);
    this->ui_.columns->setObjectName("UsercardColumns");
    this->ui_.columns->setChildrenCollapsible(false);
    this->ui_.columns->setOpaqueResize(false);
    layers->addWidget(this->ui_.columns, 0, 0);
    QObject::connect(this->ui_.columns, &QSplitter::splitterMoved, this,
                     [this] { this->update(); this->scheduleExpandedBadgeFit(); });

    this->ui_.profileScroll = new QScrollArea(this->ui_.columns);
    this->ui_.profileScroll->setObjectName("UsercardProfileScroll");
    // FramelessDraggable treats descendants of a mouse-tracking widget as
    // client controls rather than caption drag targets.
    this->ui_.profileScroll->setMouseTracking(true);
    this->ui_.profileScroll->setWidgetResizable(true);
    this->ui_.profileScroll->viewport()->setObjectName("UsercardProfileViewport");
    this->ui_.profileScroll->viewport()->setAutoFillBackground(false);
    this->ui_.profileScroll->viewport()->installEventFilter(this);
    this->ui_.profileScroll->setFrameShape(QFrame::NoFrame);
    this->ui_.profileScroll->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->ui_.profileScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->badgeFitTimer_ = new QTimer(this);
    this->badgeFitTimer_->setSingleShot(true);
    this->badgeFitTimer_->setInterval(30);
    QObject::connect(this->badgeFitTimer_, &QTimer::timeout, this,
                     &UserInfoPopup::ensureExpandedBadgeFit);
    QObject::connect(this->ui_.profileScroll->verticalScrollBar(), &QScrollBar::rangeChanged,
                     this, [this] { this->scheduleExpandedBadgeFit(); });
    this->ui_.profile = new QWidget;
    this->ui_.profile->setObjectName("UsercardProfile");
    this->ui_.profileScroll->setWidget(this->ui_.profile);
    this->ui_.profile->setAutoFillBackground(false);
    auto layout =
        LayoutCreator<QWidget>(this->ui_.profile).setLayoutType<QVBoxLayout>();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // Preserve technical metadata and its original updates/copy actions in the
    // compact menu instead of letting it dominate the profile column.
    this->ui_.profileDetails = new QWidget(this->ui_.profile);
    auto details = LayoutCreator<QWidget>(this->ui_.profileDetails)
                       .setLayoutType<QVBoxLayout>()
                       .withoutMargin();
    this->ui_.profileDetails->hide();

    // Right column: one header line (like Chatterino+) above the pages.
    this->ui_.rightColumn = new QWidget(this->ui_.columns);
    this->ui_.rightColumn->setObjectName("UsercardRight");
    this->ui_.rightColumn->setMouseTracking(true);
    auto *rightLayout = new QVBoxLayout(this->ui_.rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    this->ui_.rightMotionViewport = new UsercardMotionViewport(this->ui_.rightColumn);
    this->ui_.rightMotionViewport->setMouseTracking(true);
    this->ui_.contentTabs = new QTabWidget(this->ui_.rightMotionViewport);
    this->ui_.rightMotionViewport->setContent(this->ui_.contentTabs);
    rightLayout->addWidget(this->ui_.rightMotionViewport, 1);
    this->ui_.contentTabs->setObjectName("UsercardContent");
    this->ui_.contentTabs->setMouseTracking(true);
    this->ui_.contentTabs->setDocumentMode(true);
    this->ui_.contentTabs->setElideMode(Qt::ElideRight);
    this->ui_.contentTabs->setUsesScrollButtons(true);
    this->ui_.messagesPage = new QWidget(this->ui_.contentTabs);
    this->ui_.messagesPage->setObjectName("UsercardMessages");
    this->ui_.messagesPage->installEventFilter(this);
    this->ui_.contentTabs->addTab(this->ui_.messagesPage, tr("Сообщения"));
    this->ui_.rolesPage = new UsercardRolesPage(this->ui_.contentTabs);
    this->ui_.rolesPage->setNetworkEnabled(!this->offlinePreview_);
    this->ui_.contentTabs->addTab(this->ui_.rolesPage, uiText("Roles"));
    // Pages are switched from the header line, not by a tab bar.
    this->ui_.contentTabs->tabBar()->hide();
    QObject::connect(this->ui_.contentTabs, &QTabWidget::currentChanged, this,
                     [this] {
                         // Tabs are interactive even during badge animation;
                         // a deliberate page switch immediately resumes normal layout.
                         if (this->ui_.rightMotionViewport)
                             this->ui_.rightMotionViewport->endMotion();
                         this->updateMessagesHeader();
                         this->ui_.rolesPage->setActive(
                             this->ui_.contentTabs->currentWidget() ==
                                 this->ui_.rolesPage &&
                             !this->isKick_ && !this->isYouTube_);
                     });
    this->ui_.columns->setStretchFactor(0, 1);
    this->ui_.columns->setStretchFactor(1, 1);

    this->ui_.profileHeader = new QWidget(this->ui_.profile);
    this->ui_.profileHeader->setObjectName("UsercardProfileHeader");
    auto *hero = new QGridLayout(this->ui_.profileHeader);
    hero->setContentsMargins(0, 0, 0, 0);
    hero->setSpacing(0);
    layout->addWidget(this->ui_.profileHeader);
    // A neutral cover is styling, not a claimed profile image. A real Twitch
    // offline image, when supplied, is drawn over it.
    this->ui_.banner = new UsercardCoverLabel(this->ui_.profileHeader);
    this->ui_.banner->setObjectName("UsercardBanner");
    this->ui_.banner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    this->ui_.banner->setAttribute(Qt::WA_TransparentForMouseEvents);
    hero->addWidget(this->ui_.banner, 0, 0, Qt::AlignTop);

    // first line
    auto *identityHost = new QWidget(this->ui_.profileHeader);
    identityHost->setObjectName("UsercardIdentity");
    this->ui_.identityHeader = identityHost;
    auto head =
        LayoutCreator<QWidget>(identityHost).setLayoutType<QVBoxLayout>();
    head->setContentsMargins(12, 30, 12, 6);
    head->setSpacing(0);
    hero->addWidget(identityHost, 0, 0);
    auto identity = head.emplace<QHBoxLayout>().withoutMargin();
    identity->setSpacing(14);
    identity->setAlignment(Qt::AlignBottom);
    {
        auto avatarBox = identity.emplace<QVBoxLayout>().withoutMargin();
        avatarBox->setAlignment(Qt::AlignBottom);
        // avatar
        auto *avatarFrame = new QWidget(this);
        auto *avatarLayout = new QGridLayout(avatarFrame);
        avatarLayout->setContentsMargins(0, 0, 0, 0);
        avatarLayout->setSpacing(0);
        avatarBox->addWidget(avatarFrame);

        auto *avatar = new UsercardAvatarButton;
        this->ui_.avatarButton = avatar;
        avatar->setEnabled(!this->offlinePreview_);
        avatar->setScaleIndependentSize(64, 64);
        avatar->setMarginEnabled(false);
        avatar->setDim(DimButton::Dim::None);
        avatarLayout->addWidget(avatar, 0, 0);
        QObject::connect(
            avatar, &Button::clicked, [this](Qt::MouseButton button) {
                if (this->isKick_)
                {
                    this->onKickProfilePictureClick(button);
                    return;
                }

                if (this->isYouTube_)
                {
                    if (button == Qt::LeftButton &&
                        !this->youtubeChannelId_.isEmpty())
                    {
                        QDesktopServices::openUrl(
                            QUrl(QStringLiteral("https://www.youtube.com/"
                                                "channel/") +
                                 this->youtubeChannelId_));
                    }
                    return;
                }

                QUrl channelURL("https://www.twitch.tv/" +
                                this->userName_.toLower());

                switch (button)
                {
                    case Qt::LeftButton: {
                        QDesktopServices::openUrl(channelURL);
                    }
                    break;

                    case Qt::RightButton: {
                        if (this->avatarUrl_.isEmpty())
                        {
                            return;
                        }
                        this->showProfilePictureContextMenu();
                    }
                    break;

                    default:;
                }
            });
        auto *bannedLabel = new QLabel(uiText("BANNED"), avatarFrame);
        bannedLabel->setAlignment(Qt::AlignCenter);
        bannedLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        bannedLabel->setStyleSheet("QLabel { background: rgba(185, 28, 28, "
                                   "220); color: white; font-weight: 700; "
                                   "padding: 2px 6px; border-radius: 3px; }");
        bannedLabel->hide();
        avatarLayout->addWidget(bannedLabel, 0, 0,
                                Qt::AlignHCenter | Qt::AlignBottom);
        this->ui_.bannedAvatarLabel = bannedLabel;

        auto switchAv =
            details.emplace<LabelButton>(QString{}, nullptr, QSize{2, 2})
                .assign(&this->ui_.switchAvatars);
        switchAv->hide();
        QObject::connect(
            switchAv.getElement(), &LabelButton::leftClicked, [this] {
                if (!this->seventvAvatar_)
                {
                    this->ui_.switchAvatars->hide();
                    return;
                }
                this->isTwitchAvatarShown_ = !this->isTwitchAvatarShown_;
                if (this->isTwitchAvatarShown_)
                {
                    this->seventvAvatar_->stop();
                    this->ui_.avatarButton->setPixmap(this->avatarPixmap_);
                    this->ui_.switchAvatars->setText(uiText("Show 7TV"));
                }
                else
                {
                    this->ui_.avatarButton->setPixmap(
                        this->seventvAvatar_->currentPixmap());
                    this->seventvAvatar_->start();
                    this->ui_.switchAvatars->setText(uiText(u"Show ") %
                                                     this->platformName());
                }
                this->updateAvatarUrl();
            });

        auto *followButton =
            new SvgButton(followButtonSource(false), this, {4, 4});
        followButton->hide();
        this->ui_.followButton = followButton;
        QObject::connect(followButton, &Button::leftClicked, this, [this] {
            this->toggleUsercardFollow();
        });

        auto vbox = identity.emplace<QVBoxLayout>().withoutMargin();
        vbox->setSpacing(3);
        vbox->setAlignment(Qt::AlignBottom);
        {
            // items on the right
            {
                auto box = vbox.emplace<QHBoxLayout>()
                               .withoutMargin()
                               .withoutSpacing();

                PixmapButton *nameCopyButton = nullptr;
                this->ui_.nameLabel =
                    addCopyableLabel(box, uiText("Копировать имя"), &nameCopyButton,
                                     new UsercardNameLabel);
                nameCopyButton->hide();
                this->ui_.nameLabel->setFontStyle(FontStyle::UiMediumBold);
                this->ui_.nameLabel->setPadding(QMargins(0, 0, 1, 0));
                this->ui_.nameLabel->setShouldElide(true);
                box->setStretch(0, 1);
                this->ui_.liveIndicator = new LiveIndicator;
                this->ui_.liveIndicator->hide();
                // addCopyableLabel adds the copy button last -> add the indicator before that
                box->insertWidget(box->count() - 1, this->ui_.liveIndicator);
                box->insertItem(box->count() - 1,
                                ScalingSpacerItem::horizontal(7));
                auto identityActions = details.emplace<QHBoxLayout>()
                                           .withoutMargin()
                                           .withoutSpacing();
                auto nameHistory =
                    identityActions
                        .emplace<LabelButton>("aka", this, QSize{4, 0})
                        .assign(&this->ui_.nameHistoryButton);
                nameHistory->setToolTip(uiText("Show name history"));
                nameHistory->hide();
                QObject::connect(nameHistory.getElement(), &Button::leftClicked,
                                 [this] {
                                     this->showNameHistoryMenu();
                                 });
                auto badges =
                    identityActions
                        .emplace<LabelButton>("badges", this, QSize{4, 0})
                        .assign(&this->ui_.badgesLabel);
                badges->setAccessibleDescription(uiText("View earned Twitch badges"));
                badges->hide();
                QObject::connect(badges.getElement(), &Button::leftClicked,
                                 [this] {
                                     this->toggleInlineBadges();
                                 });
                identityActions->addStretch(1);

                auto localizedBox = vbox.emplace<QHBoxLayout>()
                                        .withoutMargin()
                                        .withoutSpacing();

                this->ui_.localizedNameLabel =
                    addCopyableLabel(localizedBox, "Copy localized name",
                                     &this->ui_.localizedNameCopyButton);
                this->ui_.localizedNameLabel->setFontStyle(
                    FontStyle::UiMediumBold);
                this->ui_.localizedNameLabel->setShouldElide(true);
                localizedBox->setStretch(0, 1);

                auto palette = QPalette();
                palette.setColor(QPalette::WindowText, QColor("#aaa"));
                auto idBox = details.emplace<QHBoxLayout>()
                                 .withoutMargin()
                                 .withoutSpacing();
                this->ui_.userIDLabel = addCopyableLabel(idBox, "Copy ID");
                this->ui_.userIDLabel->setShouldElide(true);
                idBox->setStretch(0, 1);
                this->ui_.userIDLabel->setPalette(palette);

                this->ui_.localizedNameLabel->setVisible(false);
                this->ui_.localizedNameCopyButton->setVisible(false);

                // button to pin the window (only if we close automatically)
                if (this->closeAutomatically_)
                {
                    auto *pinHost = new QWidget(this->ui_.profileHeader);
                    pinHost->setObjectName("UsercardPinHost");
                    auto *pinLayout = new QHBoxLayout(pinHost);
                    pinLayout->setContentsMargins(0, 10, 10, 0);
                    pinLayout->addWidget(this->createPinButton());
                    hero->addWidget(pinHost, 0, 0,
                                    Qt::AlignRight | Qt::AlignTop);
                }

                // kept for its state/tooltip logic; offered in the ⋮ menu
                details->addWidget(followButton);

                this->ui_.actions = new QToolButton(this->ui_.profile);
                this->ui_.actions->setText(QString::fromUtf8("⋮"));
                this->ui_.actions->setObjectName("UsercardActions");
                this->ui_.actions->setAccessibleName(
                    tr("Действия с пользователем"));
                this->ui_.actions->setAutoRaise(true);
                box->addWidget(this->ui_.actions);
                QObject::connect(this->ui_.actions, &QToolButton::clicked, this,
                                 &UserInfoPopup::showActionsMenu);

                QPointer<UserInfoPopup> self(this);
                if (!this->offlinePreview_)
                {
                    this->currentUserChangedConnection_ =
                        getApp()
                            ->getAccounts()
                            ->twitch.currentUserChanged.connect([self] {
                                runInGuiThread([self] {
                                    if (!self)
                                    {
                                        return;
                                    }

                                    if (!self->isKick_ &&
                                        self->underlyingChannel_)
                                    {
                                        if (auto *twitchChannel =
                                                dynamic_cast<TwitchChannel *>(
                                                    self->underlyingChannel_
                                                        .get()))
                                        {
                                            twitchChannel
                                                ->refreshLeadModStatus();
                                        }
                                    }

                                    if (!self->isKick_ &&
                                        (!self->userName_.isEmpty() ||
                                         !self->userId_.isEmpty()))
                                    {
                                        self->resetUsercardInfoRows();
                                        self->updateUserData();
                                    }

                                    if (!self->isKick_ &&
                                        !self->userId_.isEmpty() &&
                                        getSettings()
                                            ->showFollowButtonInUsercard)
                                    {
                                        self->refreshFollowingStatus(true);
                                    }
                                    self->updateUsercardFollowButton();
                                    self->userStateChanged_.invoke();
                                });
                            });
                }
            }

            // Metadata spans the profile column, independently of the avatar.
            auto metadata = details.emplace<QVBoxLayout>().withoutMargin();
            metadata->setSpacing(3);
            if (getSettings()->showPronouns)
            {
                metadata.emplace<Label>(uiText(TEXT_PRONOUNS).arg(uiText(TEXT_LOADING)))
                    .assign(&this->ui_.pronounsLabel);
            }
            metadata.emplace<Label>(uiText(TEXT_FOLLOWERS).arg(""))
                .assign(&this->ui_.followerCountLabel);
            metadata.emplace<Label>(uiText(TEXT_CREATED).arg(""))
                .assign(&this->ui_.createdDateLabel);
            metadata.emplace<Label>("").assign(&this->ui_.lastLiveLabel);
            createUsercardColorRow(metadata, &this->ui_.userColorRow,
                                   &this->ui_.userColorSwatch,
                                   &this->ui_.userColorLabel);
            if (auto *colorRow =
                    dynamic_cast<ClickableColorRow *>(this->ui_.userColorRow))
            {
                QObject::connect(colorRow, &Button::leftClicked, this, [this] {
                    const auto color =
                        this->ui_.userColorRow->property("copy-color")
                            .toString();
                    if (color.isEmpty())
                    {
                        return;
                    }

                    crossPlatformCopy(color);
                    const auto message =
                        uiText(QString("Copied user color %1")).arg(color);
                    noctistooltip::showText(QCursor::pos(), message, this, {}, 1600);
                    if (this->channel_)
                    {
                        this->channel_->addSystemMessage(message);
                    }
                });
            }
            Button *seventvPaintNameButton = nullptr;
            createUsercardPaintRow(metadata, &this->ui_.seventvPaintRow,
                                   &this->ui_.seventvPaintPixmapLabel,
                                   &seventvPaintNameButton);
            if (seventvPaintNameButton)
            {
                QObject::connect(
                    seventvPaintNameButton, &Button::leftClicked, this, [this] {
                        if (!this->seventvPaint_ ||
                            this->seventvPaint_->id.isEmpty())
                        {
                            return;
                        }

                        const auto encodedID = QString::fromLatin1(
                            QUrl::toPercentEncoding(this->seventvPaint_->id));
                        QDesktopServices::openUrl(
                            QUrl(SEVENTV_PAINT_PAGE.toString() + encodedID));
                    });
            }
            metadata.emplace<Label>("").assign(&this->ui_.statusLabel);
            metadata.emplace<Label>("").assign(&this->ui_.chatterCountLabel);
            createUsercardStatusRow(metadata, &this->ui_.followageRow,
                                    &this->ui_.followageIcon,
                                    &this->ui_.followageLabel);
            this->ui_.followageLabel->installEventFilter(
                new OpenUrlOnClick(this->ui_.followageLabel));
            createUsercardStatusRow(metadata, &this->ui_.subageRow,
                                    &this->ui_.subageIcon,
                                    &this->ui_.subageLabel);
            createUsercardStatusRow(metadata, &this->ui_.subGiftRow,
                                    &this->ui_.subGiftIcon,
                                    &this->ui_.subGiftLabel);

            auto applyPopupVisibility = [this] {
                auto *settings = getSettings();
                if (this->ui_.chatterCountLabel)
                {
                    this->ui_.chatterCountLabel->setVisible(
                        settings->showUsercardChatterCount &&
                        settings->showUserinfoPopupChatters.getValue());
                }
                if (this->ui_.lastLiveLabel)
                {
                    this->ui_.lastLiveLabel->setVisible(
                        settings->showUsercardLastLive &&
                        settings->showUserinfoPopupLastLive.getValue());
                }
                if (this->ui_.userColorRow)
                {
                    this->ui_.userColorRow->setVisible(
                        settings->showUsercardColor &&
                        settings->showUserinfoPopupColor.getValue());
                }
            };

            applyPopupVisibility();
            getSettings()->showUserinfoPopupChatters.connect(
                [applyPopupVisibility](auto) {
                    applyPopupVisibility();
                },
                this->signalHolder_, false);
            getSettings()->showUserinfoPopupLastLive.connect(
                [applyPopupVisibility](auto) {
                    applyPopupVisibility();
                },
                this->signalHolder_, false);
            getSettings()->showUserinfoPopupColor.connect(
                [applyPopupVisibility](auto) {
                    applyPopupVisibility();
                },
                this->signalHolder_, false);
        }
    }

    this->ui_.badgeStrip = new UsercardBadgeStrip(this->ui_.profile);
    auto *badgeStrip = static_cast<UsercardBadgeStrip *>(this->ui_.badgeStrip);
    badgeStrip->onStarted = [this](bool expanded, int height) { this->beginBadgeFit(expanded, height); };
    badgeStrip->onHeightChanged = [this](qreal fraction) { this->applyBadgeFit(fraction); };
    badgeStrip->onFinished = [this](bool expanded) { this->finishBadgeFit(expanded); };
    this->ui_.badgeStrip->setObjectName("UsercardObservedBadges");
    this->ui_.badgeStrip->hide();
    auto *badgesHost = new QWidget(this->ui_.profile);
    // The strip shrinks before the host receives its layout event. Leave any
    // temporary spare height below the badges so the first row never shifts.
    auto *badgesLayout = new QHBoxLayout(badgesHost);
    badgesLayout->setContentsMargins(14, 4, 14, 8);
    badgesLayout->addWidget(badgeStrip, 1, Qt::AlignTop);
    layout->addWidget(badgesHost);
    auto *facts = new QWidget(this->ui_.profile);
    this->ui_.profileFacts = facts;
    facts->setObjectName("UsercardFacts");
    auto *factsLayout = new QVBoxLayout(facts);
    factsLayout->setContentsMargins(14, 9, 14, 9);
    factsLayout->setSpacing(5);
    this->ui_.createdDateLabel->setVisible(
        getSettings()->showUsercardCreatedDate);
    this->ui_.followerCountLabel->setVisible(
        getSettings()->showUsercardFollowerCount);
    factsLayout->addWidget(new UsercardFactRow(
        this->ui_.createdDateLabel, UsercardFactRow::Icon::Created, facts));
    for (auto *row :
         {this->ui_.followageRow, this->ui_.subageRow, this->ui_.subGiftRow})
    {
        row->layout()->setContentsMargins(0, 0, 0, 0);
        factsLayout->addWidget(row);
    }
    factsLayout->addWidget(new UsercardFactRow(
        this->ui_.followerCountLabel, UsercardFactRow::Icon::Followers, facts));
    // The facts sit in a rounded panel instead of between two lines.
    auto *factsHost = new QWidget(this->ui_.profile);
    auto *factsHostLayout = new QVBoxLayout(factsHost);
    factsHostLayout->setContentsMargins(10, 0, 10, 8);
    factsHostLayout->addWidget(facts);
    facts->setAttribute(Qt::WA_StyledBackground, true);
    layout->addWidget(factsHost);

    auto *quick = new QWidget(this->ui_.profile);
    this->ui_.quickActions = quick;
    quick->setObjectName("UsercardQuickActions");
    auto *quickLayout = new QHBoxLayout(quick);
    quickLayout->setContentsMargins(10, 0, 10, 8);
    quickLayout->setSpacing(6);
    auto addQuick = [&](const QString &objectName, const QString &text,
                        const QString &tooltip) {
        auto *button = new QToolButton(quick);
        button->setObjectName(objectName);
        button->setProperty("usercardQuickAction", true);
        button->setText(text);
        button->setAccessibleName(tooltip);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setMinimumWidth(0);
        button->setMouseTracking(true);
        button->setCursor(Qt::PointingHandCursor);
        quickLayout->addWidget(button, 1);
        return button;
    };
    this->ui_.quickWhisper = addQuick("UsercardQuickWhisper", uiText("DM"),
                                     uiText("Write a private message"));
    this->ui_.quickProfile = addQuick("UsercardQuickProfile", uiText("Profile"),
                                     uiText("Open profile in browser"));
    this->ui_.quickCopyId = addQuick("UsercardQuickCopyId", QStringLiteral("ID"),
                                    uiText("Copy user ID"));
    QObject::connect(this->ui_.quickWhisper, &QToolButton::clicked, this,
                     [this] { this->openWhisperConversation(); });
    QObject::connect(this->ui_.quickProfile, &QToolButton::clicked, this, [this] {
        if (this->offlinePreview_)
            return;
        QString url;
        if (this->isYouTube_ && !this->youtubeChannelId_.isEmpty())
            url = "https://www.youtube.com/channel/" + this->youtubeChannelId_;
        else if (!this->userId_.isEmpty() && !this->userName_.isEmpty())
            url = this->isKick_ ? QStringLiteral("https://kick.com/") +
                                     KickApi::slugify(this->userName_)
                                : QStringLiteral("https://www.twitch.tv/") +
                                     this->userName_.toLower();
        if (url.isEmpty())
            return;
        if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
            openLinkIncognito(url);
        else
            QDesktopServices::openUrl(QUrl(url));
    });
    QObject::connect(this->ui_.quickCopyId, &QToolButton::clicked, this, [this] {
        const auto id = this->isYouTube_ ? this->youtubeChannelId_ : this->userId_;
        if (this->offlinePreview_ || id.isEmpty())
            return;
        crossPlatformCopy(id.startsWith("kick:") ? id.mid(5) : id);
        noctistooltip::showText(this->ui_.quickCopyId->mapToGlobal(
                               QPoint(0, this->ui_.quickCopyId->height())),
                           uiText("ID copied"), this->ui_.quickCopyId, {}, 1600);
    });
    layout->addWidget(quick);
    auto *activityHost = new QWidget(this->ui_.profile);
    auto *activityHostLayout = new QVBoxLayout(activityHost);
    activityHostLayout->setContentsMargins(10, 0, 10, 8);
    this->ui_.activity = new QWidget(activityHost);
    this->ui_.activity->setObjectName("UsercardActivity");
    this->ui_.activity->setAttribute(Qt::WA_StyledBackground);
    auto *activityLayout = new QGridLayout(this->ui_.activity);
    activityLayout->setContentsMargins(14, 8, 14, 8);
    activityLayout->setHorizontalSpacing(8);
    activityLayout->setVerticalSpacing(5);
    auto *activityTitle = new QLabel(uiText("Channel activity"), this->ui_.activity);
    activityTitle->setObjectName("UsercardActivityTitle");
    activityLayout->addWidget(activityTitle, 0, 0, 1, 2);
    auto addActivityRow = [&](int row, const QString &title, const QString &name) {
        auto *label = new QLabel(title, this->ui_.activity);
        label->setObjectName("UsercardActivityLabel");
        label->setWordWrap(true);
        auto *value = new QLabel(QStringLiteral("—"), this->ui_.activity);
        value->setObjectName(name);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        activityLayout->addWidget(label, row, 0);
        activityLayout->addWidget(value, row, 1);
        return value;
    };
    this->ui_.activityCount = addActivityRow(1, uiText("Messages in loaded history"), "UsercardActivityCount");
    this->ui_.activityLatest = addActivityRow(2, uiText("Last message"), "UsercardActivityLatest");
    activityLayout->setColumnStretch(0, 1);
    activityHostLayout->addWidget(this->ui_.activity);
    layout->addWidget(activityHost);
    this->activityRefreshTimer_ = new QTimer(this);
    this->activityRefreshTimer_->setSingleShot(true);
    this->activityRefreshTimer_->setInterval(120);
    QObject::connect(this->activityRefreshTimer_, &QTimer::timeout, this, &UserInfoPopup::refreshUsercardActivity);
    QObject::connect(this->ui_.columns, &QSplitter::splitterMoved, this,
                     [this] { this->updateQuickActionLayout(); });
    std::ignore = this->userStateChanged_.connect([this] {
        this->updateQuickActions();
    });

    // second line
    // Keep the original action widgets and all their permissions/handlers.
    // The compact menu proxies them, rather than reimplementing the actions.
    this->ui_.actionControls = new QWidget(this->ui_.profile);
    auto user = LayoutCreator<QWidget>(this->ui_.actionControls)
                    .setLayoutType<QHBoxLayout>()
                    .withoutMargin();
    {
        user->addStretch(1);

        user.emplace<QCheckBox>(uiText("Block")).assign(&this->ui_.block);
        user.emplace<QCheckBox>(uiText("Ignore highlights"))
            .assign(&this->ui_.ignoreHighlights);
        // visibility of this is updated in setData

        user.emplace<LabelButton>("&Notes", this).assign(&this->ui_.notesAdd);
        auto usercard = user.emplace<LabelButton>("&Usercard", this)
                            .assign(&this->ui_.usercardLabel);
        auto userlogs = user.emplace<LabelButton>("&Logs", this)
                            .assign(&this->ui_.userlogsLabel);
        userlogs->hide();
        auto sevenTVUser = user.emplace<LabelButton>("7TV", this)
                               .assign(&this->ui_.sevenTVUserLabel);
        sevenTVUser->setToolTip(uiText("Checking 7TV profile..."));
        sevenTVUser->setEnabled(false);
        sevenTVUser->hide();
        auto roles = user.emplace<LabelButton>("Roles", this)
                         .assign(&this->ui_.rolesLabel);
        roles->setToolTip(uiText("Manage editor and lead mod roles"));
        roles->hide();
        auto mod = user.emplace<PixmapButton>(this);
        mod->setPixmap(getResources().buttons.mod);
        mod->setScaleIndependentSize(30, 30);
        auto unmod = user.emplace<PixmapButton>(this);
        unmod->setPixmap(getResources().buttons.unmod);
        unmod->setScaleIndependentSize(30, 30);
        auto vip = user.emplace<PixmapButton>(this);
        vip->setPixmap(getResources().buttons.vip);
        vip->setScaleIndependentSize(30, 30);
        auto unvip = user.emplace<PixmapButton>(this);
        unvip->setPixmap(getResources().buttons.unvip);
        unvip->setScaleIndependentSize(30, 30);

        this->actionMenuButtons_ = {
            {{}, this->ui_.usercardLabel},
            {{}, this->ui_.userlogsLabel},
            {{}, this->ui_.notesAdd},
            {{}, this->ui_.sevenTVUserLabel},
            {{}, this->ui_.rolesLabel},
            {tr("Make moderator"), mod.getElement()},
            {tr("Remove moderator"), unmod.getElement()},
            {tr("Make VIP"), vip.getElement()},
            {tr("Remove VIP"), unvip.getElement()},
        };

        user->addStretch(1);

        auto openUsercard = [this] {
            MessagePlatform platform =
                this->isYouTube_ ? MessagePlatform::YouTube
                : this->isKick_  ? MessagePlatform::Kick
                                 : MessagePlatform::AnyOrTwitch;
            QString channelName = this->underlyingChannel_
                                      ? this->underlyingChannel_->getName()
                                      : QString();
            UserInfoPopup::openUserChannelAction(this->userName_, platform,
                                                 channelName,
                                                 this->youtubeChannelId_);
        };
        QObject::connect(usercard.getElement(), &Button::leftClicked,
                         openUsercard);
        this->registerMnemonicButton(this->ui_.usercardLabel, Qt::Key_U,
                                     openUsercard);

        auto openLogs = [this] {
            if (!this->underlyingChannel_)
            {
                return;
            }

            QUrl url("https://lurkology.com/logs");
            QUrlQuery query;
            query.addQueryItem("c", this->underlyingChannel_->getName());
            query.addQueryItem("u", this->userName_);
            url.setQuery(query);
            QDesktopServices::openUrl(url);
        };
        QObject::connect(userlogs.getElement(), &Button::leftClicked, openLogs);
        this->registerMnemonicButton(this->ui_.userlogsLabel, Qt::Key_L,
                                     openLogs);

        auto openSevenTVUser = [this] {
            if (this->seventvUserID_.isEmpty())
            {
                this->refreshSevenTVUserButtonVisibility();
                return;
            }

            QDesktopServices::openUrl(
                QUrl(SEVENTV_USER_PAGE % this->seventvUserID_));
        };
        QObject::connect(sevenTVUser.getElement(), &Button::leftClicked,
                         openSevenTVUser);
        this->registerMnemonicButton(this->ui_.sevenTVUserLabel, Qt::Key_V,
                                     openSevenTVUser);

        auto openRoleMenu = [this] {
            this->showRoleManagementMenu();
        };
        QObject::connect(roles.getElement(), &Button::leftClicked,
                         openRoleMenu);
        this->registerMnemonicButton(this->ui_.rolesLabel, Qt::Key_R,
                                     openRoleMenu);

        QObject::connect(mod.getElement(), &Button::leftClicked, [this] {
            QString value = "/mod " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(unmod.getElement(), &Button::leftClicked, [this] {
            QString value = "/unmod " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(vip.getElement(), &Button::leftClicked, [this] {
            QString value = "/vip " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });
        QObject::connect(unvip.getElement(), &Button::leftClicked, [this] {
            QString value = "/unvip " + this->userName_;
            value = getApp()->getCommands()->execCommand(
                value, this->underlyingChannel_, false);
            this->underlyingChannel_->sendMessage(value);
        });

        // userstate
        // We can safely ignore this signal connection since this is a private signal, and
        // we only connect once
        std::ignore = this->userStateChanged_.connect([this, mod, unmod, vip,
                                                       unvip, roles]() mutable {
            TwitchChannel *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());

            bool visibilityModButtons = false;

            if (twitchChannel)
            {
                bool isMyself =
                    QString::compare(getApp()
                                         ->getAccounts()
                                         ->twitch.getCurrent()
                                         ->getUserName(),
                                     this->userName_, Qt::CaseInsensitive) == 0;

                const bool canManageRoles =
                    twitchChannel->isBroadcaster() ||
                    (getSettings()->showLeadModRoleButtons &&
                     twitchChannel->isLeadMod());

                visibilityModButtons =
                    canManageRoles && !isMyself && !this->isBroadcaster_;
            }
            mod->setVisible(visibilityModButtons);
            unmod->setVisible(visibilityModButtons);
            vip->setVisible(visibilityModButtons);
            unvip->setVisible(visibilityModButtons);
            roles->setVisible(this->canShowRoleManagementMenu());
        });
    }
    this->ui_.actionControls->hide();

    auto notesPreview = layout.emplace<MarkdownLabel>(this, QString())
                            .assign(&this->ui_.notesPreview);
    notesPreview->setVisible(false);
    notesPreview->setShouldElide(true);
    notesPreview->setWordWrap(true);
    notesPreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // Moderation: one strip across the whole card, under both columns, so
    // every button is visible without scrolling.
    {
        auto *strip = new QWidget(this);
        strip->setObjectName("UsercardModerationStrip");
        strip->setAttribute(Qt::WA_StyledBackground, true);
        strip->setMouseTracking(true);
        auto *stripLayout = new QHBoxLayout(strip);
        stripLayout->setContentsMargins(10, 5, 10, 5);
        stripLayout->addStretch(1);
        auto *timeout = new TimeoutWidget();
        timeout->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        stripLayout->addWidget(timeout, 0, Qt::AlignVCenter);
        stripLayout->addStretch(1);
        this->ui_.timeoutWidget = timeout;
        this->ui_.moderationStrip = strip;
        strip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        layers->addWidget(strip, 1, 0);
        layers->setRowStretch(0, 1);
        strip->hide();

        // We can safely ignore this signal connection since this is a private signal, and
        // we only connect once
        std::ignore =
            this->userStateChanged_.connect([this, timeout]() mutable {
                const bool moderation = this->shouldShowModerationActions();
                const bool cross = this->crossActionAvailable();
                timeout->setModerationVisible(moderation);
                timeout->setCrossVisible(cross);
                timeout->setVisible(moderation || cross);
                this->setModerationStripVisible(moderation || cross);
            });
        getSettings()->showCrossActionsInUnmoderatedChannels.connect(
            [this](bool, auto) {
                this->userStateChanged_.invoke();
            },
            this->signalHolder_, false);

        // We can safely ignore this signal connection since we own the button, and
        // the button will always be destroyed before the UserInfoPopup
        std::ignore = timeout->buttonClicked.connect(
            [this](const UsercardModerationRequest &request) {
                if (request.promptForReason)
                {
                    this->showUsercardModerationReasonPopup(request);
                    return;
                }

                this->executeUsercardModerationAction(request);
            });
        std::ignore = timeout->crossActionClicked.connect([this](bool ban) {
            this->runCrossAction(ban ? QStringLiteral("/crossban")
                                     : QStringLiteral("/crossunban"));
        });
    }

    layout->addStretch(1);

    // fourth line (last messages)
    auto logs = LayoutCreator<QWidget>(this->ui_.messagesPage)
                    .setLayoutType<QVBoxLayout>()
                    .withoutMargin();
    logs->setSpacing(0);
    {
        auto *messagesHeader = new QWidget(this->ui_.rightColumn);
        this->ui_.messagesHeader = messagesHeader;
        messagesHeader->setObjectName("UsercardMessagesHeader");
        messagesHeader->setMouseTracking(true);
        messagesHeader->installEventFilter(this);
        auto *headerLayout = new QHBoxLayout(messagesHeader);
        headerLayout->setContentsMargins(10, 6, 10, 6);
        headerLayout->setSpacing(8);
        this->ui_.messagesHeading =
            new QLabel(tr("Сообщения в текущем канале"));
        this->ui_.messagesHeading->setObjectName("UsercardMessagesHeading");
        this->ui_.messagesHeading->setTextFormat(Qt::PlainText);
        this->ui_.messagesHeading->setWordWrap(false);
        this->ui_.messagesHeading->setSizePolicy(QSizePolicy::Ignored,
                                                 QSizePolicy::Preferred);
        auto *headingGroup = new QWidget(messagesHeader);
        this->ui_.headingGroup = headingGroup;
        headingGroup->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *headingLayout = new QHBoxLayout(headingGroup);
        headingLayout->setContentsMargins(0, 0, 0, 0);
        headingLayout->setSpacing(5);
        this->ui_.messagesHeading->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        headingLayout->addWidget(this->ui_.messagesHeading);
        this->ui_.headingChannelName = new QLabel(headingGroup);
        this->ui_.headingChannelName->setObjectName("UsercardHeadingChannel");
        this->ui_.headingChannelName->setTextFormat(Qt::PlainText);
        this->ui_.headingChannelName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        this->ui_.headingAvatar = new ChannelAvatarLabel(this->ui_.headingChannelName, headingGroup);
        headingLayout->addWidget(this->ui_.headingAvatar);
        headingLayout->addWidget(this->ui_.headingChannelName, 1);
        headerLayout->addWidget(headingGroup, 1);
        this->ui_.headingActions = new QWidget(messagesHeader);
        auto *actionsLayout = new QHBoxLayout(this->ui_.headingActions);
        actionsLayout->setContentsMargins(0, 0, 0, 0);
        actionsLayout->setSpacing(8);
        actionsLayout->addStretch(1);
        this->ui_.headingActions->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        headerLayout->addWidget(this->ui_.headingActions);
        this->ui_.rolesToggle =
            new LabelButton(uiText("Roles"), this, QSize{2, 0});
        this->ui_.rolesToggle->setObjectName("UsercardRolesToggle");
        actionsLayout->addWidget(this->ui_.rolesToggle);
        QObject::connect(this->ui_.rolesToggle, &Button::leftClicked, this,
                         [this] {
                             this->ui_.contentTabs->setCurrentWidget(
                                 this->ui_.rolesPage);
                         });
        this->ui_.messagesToggle =
            new LabelButton(tr("Сообщения"), this, QSize{2, 0});
        this->ui_.messagesToggle->setObjectName("UsercardMessagesToggle");
        this->ui_.messagesToggle->setAccessibleDescription(tr("Вернуться к сообщениям"));
        this->ui_.messagesToggle->hide();
        actionsLayout->addWidget(this->ui_.messagesToggle);
        QObject::connect(this->ui_.messagesToggle, &Button::leftClicked, this,
                         [this] {
                             this->ui_.contentTabs->setCurrentWidget(
                                 this->ui_.messagesPage);
                         });
        this->ui_.historyButton =
            new LabelButton(tr("История"), this, QSize{2, 0});
        this->ui_.historyButton->setObjectName("UsercardHistory");
        this->ui_.historyButton->setAccessibleDescription(
            tr("Открыть доступную историю сообщений"));
        actionsLayout->addWidget(this->ui_.historyButton);
        QObject::connect(this->ui_.historyButton, &Button::leftClicked, this,
                         [this] {
                             if (this->canUsePublicHistory())
                             {
                                 this->requestPublicHistory(HistoryRequest::Manual);
                             }
                             else if (!this->offlinePreview_ &&
                                      this->canLoadMoreUsercardMessages())
                             {
                                 this->requestMoreUsercardMessages(true);
                             }
                         });
        rightLayout->insertWidget(0, messagesHeader);
        this->ui_.noMessagesLabel = new Label(tr("В буфере сообщений нет"));
        this->ui_.noMessagesLabel->setVisible(false);
        this->ui_.noMessagesLabel->setSizePolicy(QSizePolicy::Expanding,
                                                 QSizePolicy::Expanding);

        this->ui_.latestMessages =
            new ChannelView(this, this->split_, ChannelView::Context::UserCard,
                            getSettings()->scrollbackUsercardLimit);
        this->ui_.latestMessages->setMinimumSize(0, 110);
        this->ui_.latestMessages->setSizePolicy(QSizePolicy::Expanding,
                                                QSizePolicy::Expanding);
        this->ui_.latestMessages->installEventFilter(this);
        this->ui_.latestMessages->getScrollBar().installEventFilter(this);

        auto loadMore =
            new LabelButton("Load more messages", this, QSize{8, 2});
        loadMore->setVisible(false);
        loadMore->setToolTip(uiText("Load older messages from Twitch mod logs"));
        this->ui_.loadMoreMessages = loadMore;

        QObject::connect(loadMore, &Button::leftClicked, this, [this] {
            this->requestMoreUsercardMessages(true);
        });
        this->usercardScrollConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                this->ui_.latestMessages->getScrollBar()
                    .getDesiredValueChanged()
                    .connect([this] {
                        this->maybeLoadMoreUsercardMessagesFromScroll();
                    }));

        logs->addWidget(this->ui_.loadMoreMessages);
        logs->addWidget(this->ui_.noMessagesLabel);
        logs->addWidget(this->ui_.latestMessages);
        logs->setAlignment(this->ui_.noMessagesLabel, Qt::AlignHCenter);
        logs->setAlignment(this->ui_.loadMoreMessages, Qt::AlignHCenter);
    }

    // The masked ring does not intercept any central controls or scrolling.
    this->ui_.resizeGrip = new UsercardResizeGrip(this);
    this->ui_.resizeGrip->setObjectName("UsercardResizeGrip");
    this->ui_.resizeGrip->syncGeometry(this->scale(), shadow);

    if (!this->offlinePreview_)
    {
        this->installEvents();
    }
    this->updateUsercardStatusIcons();
    std::ignore = this->userStateChanged_.connect([this] {
        this->updateLoadMoreMessagesButton();
    });
    this->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Policy::Ignored);
    // Ellipsize metadata instead of imposing the widest name/status on both
    // columns. The full stored text remains available to the existing copy actions.
    for (auto *label : {this->ui_.nameLabel, this->ui_.localizedNameLabel,
                        this->ui_.userIDLabel, this->ui_.pronounsLabel,
                        this->ui_.followerCountLabel,
                        this->ui_.createdDateLabel, this->ui_.lastLiveLabel,
                        this->ui_.userColorLabel, this->ui_.statusLabel,
                        this->ui_.chatterCountLabel, this->ui_.followageLabel,
                        this->ui_.subageLabel, this->ui_.subGiftLabel})
    {
        if (label)
        {
            label->setShouldElide(true);
        }
    }
    if (this->offlinePreview_)
    {
        this->ui_.followButton->hide();
        this->ui_.timeoutWidget->hide();
        this->ui_.latestMessages->setContextMenuPolicy(Qt::NoContextMenu);
    }
    // This is an initial size, not a fixed size. BaseWidget's
    // setScaleIndependentSize() applies fixed min/max constraints on every DPI
    // change, preventing both interactive resizing and expanding badge rows.
    this->setMinimumSize(qRound(300 * this->scale()), qRound(240 * this->scale()));
    this->resize(qRound(670 * this->scale()), qRound(370 * this->scale()));
    this->ui_.nameLabel->setOverrideScale(this->scale() * 1.6F);
    this->themeChangedEvent();
    this->updateResponsiveLayout();
    this->updateObservedBadges();
    this->updateQuickActions();
    this->updateUsercardCorners();
}

void UserInfoPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    if (this->ui_.moderationStrip)
    {
        this->ui_.moderationStrip->setStyleSheet(
            this->theme->isLightTheme()
                ? "QWidget#UsercardModerationStrip { border-top: 1px solid "
                  "rgba(0,0,0,0.12); background: rgba(0,0,0,0.03); }"
                : "QWidget#UsercardModerationStrip { border-top: 1px solid "
                  "rgba(255,255,255,0.08); background: rgba(0,0,0,0.18); }");
    }

    if (this->ui_.profileScroll && this->ui_.contentTabs)
    {
        const bool light = this->theme->isLightTheme();
        const auto colors = usercardChrome::palette(light);
        const auto &profile = colors.profile;
        const auto &messages = colors.messages;
        const auto &foreground = colors.foreground;
        const auto &border = colors.border;
        const auto &accent = colors.accent;
        this->ui_.profileScroll->setStyleSheet(
            QString("QScrollArea#UsercardProfileScroll, QWidget#UsercardProfileViewport { background: transparent; "
                    "border: 0; }"
                    "QScrollArea#UsercardProfileScroll QScrollBar:vertical { "
                    "background: transparent; width: 7px; margin: 2px 0; border: 0; }"
                    "QScrollArea#UsercardProfileScroll QScrollBar::handle:vertical { "
                    "background: %8; min-height: 24px; border-radius: 3px; }"
                    "QScrollArea#UsercardProfileScroll QScrollBar::handle:vertical:hover { "
                    "background: %6; }"
                    "QScrollArea#UsercardProfileScroll QScrollBar::add-line:vertical, "
                    "QScrollArea#UsercardProfileScroll QScrollBar::sub-line:vertical { height: 0; }"
                    "QScrollArea#UsercardProfileScroll QScrollBar::add-page:vertical, "
                    "QScrollArea#UsercardProfileScroll QScrollBar::sub-page:vertical { background: none; }"
                    "QWidget#UsercardProfile { background: %1; color: %2; }"
                    "QWidget#UsercardIdentity, QWidget#UsercardProfileHeader, "
                    "QWidget#UsercardPinHost { background: transparent; }"
                    "QLabel#UsercardBanner { background: transparent; }"
                    "QWidget#UsercardFacts { background: %5; "
                    "border: 1px solid %3; border-radius: 8px; }"
                    "QWidget#UsercardFacts * { color: %2; }"
                    "QWidget#UsercardActivity { background: %5; "
                    "border: 1px solid %3; border-radius: 8px; }"
                    "QWidget#UsercardActivity QLabel { color: %2; background: transparent; }"
                    "QWidget#UsercardActivity QLabel#UsercardActivityTitle { color: %6; }"
                    "QWidget#UsercardActivity QLabel#UsercardActivityLabel { color: %9; }"
                    "QToolButton#UsercardActions { color: %2; border: 0; "
                    "padding: 2px 6px; }"
                    "QToolButton#UsercardActions:hover { background: %3; "
                    "border-radius: 3px; }"
                    "QWidget#UsercardQuickActions { background: transparent; }"
                    "QToolButton[usercardQuickAction=\"true\"] { color: %2; "
                    "background: %5; border: 1px solid %3; border-radius: 7px; "
                    "padding: 3px 4px; }"
                    "QToolButton[usercardQuickAction=\"true\"]:hover { "
                    "background: %4; border-color: %6; }"
                    "QToolButton[usercardQuickAction=\"true\"]:pressed { "
                    "background: %3; }"
                    "QToolButton[usercardQuickAction=\"true\"]:disabled { "
                    "color: %7; background: transparent; border-color: %3; }")
                .arg(profile, foreground, border, colors.cover,
                     colors.panel, accent, colors.disabled, colors.scroll, colors.muted));
        // These are custom-painted Labels rather than QLabels; give them an
        // explicit foreground palette so the card does not depend on the
        // operating system's (possibly light) default palette.
        for (auto *label : {this->ui_.createdDateLabel, this->ui_.followerCountLabel,
                            this->ui_.followageLabel, this->ui_.subageLabel,
                            this->ui_.subGiftLabel})
        {
            if (!label)
                continue;
            auto palette = label->palette();
            palette.setColor(QPalette::WindowText, QColor(foreground));
            palette.setColor(QPalette::Text, QColor(foreground));
            label->setPalette(palette);
        }
        if (auto *avatar =
                dynamic_cast<UsercardAvatarButton *>(this->ui_.avatarButton))
        {
            avatar->setRingColor(QColor(profile));
        }
        this->ui_.rightColumn->setStyleSheet(
            QString(
                "QWidget#UsercardRight { background: %1; }"
                "QTabWidget#UsercardContent::pane { border: 0; background: %1; "
                "}"
                "QWidget#UsercardMessages { background: %1; }"
                "QTabBar::tab { color: %2; background: %1; padding: 4px 10px;"
                " border-bottom: 1px solid %3; }"
                "QTabBar::tab:selected { color: %4; border-bottom-color: %4; }"
                "QWidget#UsercardMessagesHeader { border-bottom: 1px solid %3; "
                "}"
                "QWidget#UsercardMessagesHeader QLabel { color: %4; }"
                "QLabel#UsercardMessagesHeading { color: %2; }")
                .arg(messages, foreground, border, accent));
        const auto font =
            getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
        this->ui_.contentTabs->setFont(font);
        if (this->ui_.activity)
        {
            for (auto *label : this->ui_.activity->findChildren<QLabel *>())
                label->setFont(getApp()->getFonts()->getFont(
                    label->objectName() == "UsercardActivityTitle" ? FontStyle::ChatMediumBold : FontStyle::ChatMedium,
                    this->scale()));
        }
        this->ui_.messagesHeading->setFont(font);
        this->ui_.headingChannelName->setFont(font);
        this->ui_.actions->setFont(font);
        this->ui_.rolesPage->setScale(this->scale());
        this->updateMessagesHeader();
        this->updateProfileBanner();
        this->updateQuickActionLayout();
    }

    for (auto &&child : this->findChildren<QCheckBox *>())
    {
        child->setFont(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale()));
    }

    this->updateUsercardStatusIcons();
    this->updateNamePresentation();

    if (this->seventvPaint_)
    {
        this->updateSeventvPaintPixmap();
    }
}

void UserInfoPopup::scaleChangedEvent(float scale)
{
    this->themeChangedEvent();

    if (this->ui_.nameLabel)
    {
        this->ui_.nameLabel->setOverrideScale(scale * 1.6F);
    }

    if (this->ui_.followButton != nullptr)
    {
        const int buttonSize = std::max(1, int(22 * scale));
        this->ui_.followButton->setFixedSize(buttonSize, buttonSize);
    }

    this->setMinimumSize(int(300 * scale), int(240 * scale));
    if (this->ui_.resizeGrip && !this->isVisible())
        this->resize(qRound(670 * scale), qRound(370 * scale));
    if (this->ui_.resizeGrip)
    {
        const auto shadow = this->closeAutomatically_ ? usercardChrome::shadowInset(scale) : 0;
        const auto edge = this->closeAutomatically_
            ? usercardChrome::contentInset(scale, UsercardResizeGrip::edgeBand(scale))
            : UsercardResizeGrip::edgeBand(scale);
        this->getLayoutContainer()->layout()->setContentsMargins(edge, edge, edge, edge);
        this->ui_.resizeGrip->syncGeometry(scale, shadow);
    }
    if (this->ui_.latestMessages)
    {
        this->ui_.latestMessages->setMinimumHeight(int(110 * scale));
    }
    this->updateResponsiveLayout();
    if (this->ui_.badgeStrip)
        static_cast<UsercardBadgeStrip *>(this->ui_.badgeStrip)->setBadgeScale(scale);
    this->updateUsercardCorners();
}

#ifdef Q_OS_WIN
bool UserInfoPopup::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    const bool handled = DraggablePopup::nativeEvent(eventType, message, result);
    if (handled && static_cast<MSG *>(message)->message == WM_NCHITTEST &&
        *result == HTCAPTION && this->badgeFitInProgress_)
        *result = HTCLIENT;
    return handled;
}
#endif

void UserInfoPopup::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && this->badgeFitInProgress_)
    {
        this->badgeDragOffset_ = event->globalPosition().toPoint() - this->pos();
        this->grabMouse();
        event->accept();
        return;
    }
    DraggablePopup::mousePressEvent(event);
}

void UserInfoPopup::mouseMoveEvent(QMouseEvent *event)
{
    if (this->badgeDragOffset_ && event->buttons().testFlag(Qt::LeftButton))
    {
        this->badgeFitMoved_ = true;
        this->move(event->globalPosition().toPoint() - *this->badgeDragOffset_);
        event->accept();
        return;
    }
    DraggablePopup::mouseMoveEvent(event);
}

void UserInfoPopup::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && this->badgeDragOffset_)
    {
        this->badgeDragOffset_.reset();
        if (QWidget::mouseGrabber() == this)
            this->releaseMouse();
        event->accept();
        return;
    }
    DraggablePopup::mouseReleaseEvent(event);
}

void UserInfoPopup::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
    if (!this->badgeFitApplying_)
    {
        if (this->ui_.resizeGrip && this->ui_.resizeGrip->isResizing())
            this->manualProfileSize_ = true;
        if (this->badgeFitBase_ && event->oldSize().isValid() &&
            ((this->ui_.resizeGrip && this->ui_.resizeGrip->isResizing()) ||
             ((!this->badgeFitExpectedSize_ || event->size() != *this->badgeFitExpectedSize_) &&
              (!this->badgeFitInProgress_ || event->size().width() != event->oldSize().width()))))
        {
            // A deliberate manual resize supersedes automatic badge sizing.
            this->badgeFitBase_.reset();
            this->badgeFitInProgress_ = false;
            this->badgeFitExpectedSize_.reset();
            this->setBadgeScrollbarSuppressed(false);
            if (this->ui_.rightMotionViewport)
                this->ui_.rightMotionViewport->endMotion();
        }
        this->updateResponsiveLayout();
        this->scheduleExpandedBadgeFit();
    }
    this->updateUsercardCorners();
    if (this->ui_.resizeGrip)
        this->ui_.resizeGrip->syncGeometry(this->scale(),
            this->closeAutomatically_ ? usercardChrome::shadowInset(this->scale()) : 0);
}

void UserInfoPopup::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    // Fit the real profile on the first show, then repeat after Qt has
    // delivered deferred layout requests or remote profile facts.
    if (this->getLayoutContainer()->layout())
        this->getLayoutContainer()->layout()->activate();
    this->ensureExpandedBadgeFit();
    this->scheduleExpandedBadgeFit();
}

void UserInfoPopup::beginBadgeFit(bool expanded, int expandedBadgeHeight)
{
    auto *strip = this->ui_.badgeStrip;
    auto *scroll = this->ui_.profileScroll;
    if (!strip || !scroll || !this->ui_.profile || this->badgeFitApplying_)
        return;
    if (!expanded && !this->badgeFitBase_)
        return;
    if (!this->badgeFitBase_)
    {
        this->badgeFitBase_ = this->geometry();
        this->badgeFitBaseProfileHeight_ = scroll->height();
    }
    this->badgeFitMoved_ = this->badgeDragOffset_.has_value();
    this->badgeFitInProgress_ = true;
    this->ui_.profile->layout()->activate();
    const auto *profileLayout = this->ui_.profile->layout();
    const int natural = std::max({profileLayout->minimumSize().height(),
        profileLayout->totalHeightForWidth(scroll->viewport()->width()),
        scroll->verticalScrollBar()->maximum() > 0
            ? scroll->viewport()->height() + scroll->verticalScrollBar()->maximum() : 0});
    bool predictedFits = false;
    if (expanded)
    {
        // A wrapped activity label adds height at the actual viewport width;
        // the width-independent minimum size alone would underestimate this.
        const int required = natural -
                             strip->height() + expandedBadgeHeight + 2;
        this->badgeFitTargetProfileHeight_ = std::max(required, this->badgeFitBaseProfileHeight_);
        int overhead = this->height() - scroll->viewport()->height();
        if (this->ui_.columns->orientation() == Qt::Vertical)
        {
            // Keep room for the message/punishment pane, but allow the profile
            // to take all space needed for its contents when the monitor fits it.
            const int right = std::max(this->ui_.rightColumn->minimumSizeHint().height(), int(110 * this->scale()));
            overhead = this->height() - this->ui_.columns->height() +
                       this->ui_.columns->handleWidth() + right;
        }
        const auto available = this->screen()->availableGeometry();
        this->badgeFitTargetHeight_ = std::clamp(required + overhead,
            this->badgeFitBase_->height(), std::max(this->badgeFitBase_->height(), available.height()));
        predictedFits = required + overhead <= available.height();
    }
    else
    {
        const int collapsed = std::max(1, int(24 * this->scale()));
        predictedFits = natural - strip->height() + collapsed <= this->badgeFitBaseProfileHeight_ + 2;
    }
    // AsNeeded can see the transient mismatch between geometry and content
    // even when both endpoints fit. Keep the policy stable until the final
    // deferred layout has settled. Do not conceal genuinely clipped cards.
    this->setBadgeScrollbarSuppressed(predictedFits &&
        (this->badgeScrollbarSuppressed_ || !scroll->verticalScrollBar()->isVisible()));
    if (this->ui_.rightMotionViewport && this->ui_.columns->orientation() == Qt::Horizontal)
    {
        // Lay out the messages/roles pane once at the largest height, then
        // clip that real, already laid-out page while the native card grows
        // or shrinks. Never walk and reflow thousands of messages per frame.
        const int delta = expanded ? std::max(0, this->badgeFitTargetHeight_ - this->height()) : 0;
        const bool atBottom = this->ui_.latestMessages && this->ui_.latestMessages->isVisible() &&
                              this->ui_.latestMessages->getScrollBar().isAtBottom();
        this->ui_.rightMotionViewport->beginMotion(this->ui_.rightMotionViewport->height() + delta, atBottom);
    }
}

void UserInfoPopup::setBadgeScrollbarSuppressed(bool suppressed)
{
    if (!this->ui_.profileScroll || this->badgeScrollbarSuppressed_ == suppressed)
        return;
    this->badgeScrollbarSuppressed_ = suppressed;
    this->ui_.profileScroll->setVerticalScrollBarPolicy(suppressed ? Qt::ScrollBarAlwaysOff
                                                                  : Qt::ScrollBarAsNeeded);
}

void UserInfoPopup::applyBadgeFit(qreal fraction)
{
    if (!this->badgeFitInProgress_ || !this->badgeFitBase_ || this->badgeFitApplying_)
        return;
    this->badgeFitApplying_ = true;
    const auto base = *this->badgeFitBase_;
    const auto available = this->screen()->availableGeometry();
    const int interpolated = qRound(base.height() + (this->badgeFitTargetHeight_ - base.height()) * fraction);
    const bool opening = this->ui_.badgeStrip->property("expanded").toBool();
    const int h = opening ? std::max(this->height(), interpolated) : interpolated;
    const int y = this->badgeFitMoved_ || this->badgeDragOffset_ ? this->y() : std::clamp(this->y(), available.top(), std::max(available.top(), available.bottom() - h + 1));
    const QRect desired(this->x(), y, this->width(), h);
    if (desired != this->geometry())
    {
        this->badgeFitExpectedSize_ = desired.size();
        this->setGeometry(desired);
    }
    if (this->ui_.columns->orientation() == Qt::Vertical)
    {
        const int profileHeight = qRound(this->badgeFitBaseProfileHeight_ +
            (this->badgeFitTargetProfileHeight_ - this->badgeFitBaseProfileHeight_) * fraction);
        const int total = this->ui_.columns->height() - this->ui_.columns->handleWidth();
        if (this->ui_.profileScroll->height() != profileHeight)
            this->ui_.columns->setSizes({profileHeight, std::max(1, total - profileHeight)});
    }
    this->badgeFitApplying_ = false;
}

void UserInfoPopup::finishBadgeFit(bool expanded)
{
    if (this->badgeFitBase_)
    {
        // On an identity reset, we may not have started a closing animation.
        if (!expanded)
        {
            this->badgeFitInProgress_ = true;
            this->applyBadgeFit(0);
        }
        else
            this->applyBadgeFit(1);
    }
    this->badgeFitInProgress_ = false;
    if (this->ui_.rightMotionViewport)
        this->ui_.rightMotionViewport->endMotion();
    if (!expanded)
    {
        this->badgeFitBase_.reset();
        this->badgeFitExpectedSize_.reset();
    }
    if (!this->isVisible())
        this->setBadgeScrollbarSuppressed(false);
    this->fitVerticalProfile();
    this->scheduleExpandedBadgeFit();
}

void UserInfoPopup::scheduleExpandedBadgeFit()
{
    const bool expanded = this->badgeFitBase_ && this->ui_.badgeStrip && this->ui_.badgeStrip->property("expanded").toBool();
    if (this->badgeFitTimer_ && !this->badgeFitInProgress_ && !this->badgeFitApplying_ &&
        this->isVisible() && (expanded || !this->manualProfileSize_ || this->badgeScrollbarSuppressed_) &&
        !this->badgeFitTimer_->isActive())
        this->badgeFitTimer_->start();
}

void UserInfoPopup::ensureExpandedBadgeFit()
{
    const bool expanded = this->badgeFitBase_ && this->ui_.badgeStrip && this->ui_.badgeStrip->property("expanded").toBool();
    if (this->badgeFitInProgress_ || this->badgeFitApplying_ || !this->ui_.profileScroll || !this->ui_.profile)
        return;
    if (!this->isVisible() || (!expanded && this->manualProfileSize_))
    {
        this->setBadgeScrollbarSuppressed(false);
        return;
    }
    this->fitVerticalProfile();
    auto *scroll = this->ui_.profileScroll;
    auto *profileLayout = this->ui_.profile->layout();
    profileLayout->activate();
    const int desired = std::max(profileLayout->minimumSize().height(),
        profileLayout->totalHeightForWidth(scroll->viewport()->width()));
    const int extra = std::max(scroll->verticalScrollBar()->maximum(), desired - scroll->viewport()->height());
    const auto available = this->screen()->availableGeometry();
    const int cap = available.height();
    if (extra <= 0 || this->height() >= cap)
    {
        this->setBadgeScrollbarSuppressed(false);
        return;
    }
    const int newHeight = std::min(cap, this->height() + extra + 2);
    const int newProfileHeight = scroll->height() + extra + 2;
    if (expanded)
    {
        this->badgeFitTargetHeight_ = newHeight;
        this->badgeFitTargetProfileHeight_ = newProfileHeight;
        this->badgeFitInProgress_ = true;
        this->applyBadgeFit(1);
        this->badgeFitInProgress_ = false;
    }
    else
    {
        this->badgeFitApplying_ = true;
        const int y = std::clamp(this->y(), available.top(), std::max(available.top(), available.bottom() - newHeight + 1));
        this->setGeometry(this->x(), y, this->width(), newHeight);
        if (this->ui_.columns->orientation() == Qt::Vertical)
        {
            const int total = this->ui_.columns->height() - this->ui_.columns->handleWidth();
            this->ui_.columns->setSizes({newProfileHeight, std::max(1, total - newProfileHeight)});
        }
        this->badgeFitApplying_ = false;
    }
    // Layout may have been recomputed while the fit guard suppressed the
    // rangeChanged callback. Recheck only when a resize really happened.
    this->scheduleExpandedBadgeFit();
}

void UserInfoPopup::fitVerticalProfile()
{
    if (!this->ui_.columns || this->ui_.columns->orientation() != Qt::Vertical ||
        !this->ui_.profile || this->badgeFitInProgress_)
        return;
    auto *scroll = this->ui_.profileScroll;
    auto *columns = this->ui_.columns;
    this->ui_.profile->layout()->activate();
    const auto *profileLayout = this->ui_.profile->layout();
    const int desired = std::max(profileLayout->minimumSize().height(),
        profileLayout->totalHeightForWidth(scroll->viewport()->width())) + 2;
    const int total = columns->height() - columns->handleWidth();
    const int right = std::max(this->ui_.rightColumn->minimumSizeHint().height(), int(110 * this->scale()));
    if (scroll->height() < desired && total >= desired + right)
        columns->setSizes({desired, total - desired});
}

void UserInfoPopup::updateUsercardCorners()
{
    if (!this->closeAutomatically_ || this->width() < 2 || this->height() < 2)
        return;
    if (!this->mask().isEmpty())
        this->clearMask();
    this->update();
}

void UserInfoPopup::drawOutline(QPainter &painter)
{
    if (!this->closeAutomatically_)
    {
        BaseWindow::drawOutline(painter);
        return;
    }
    const auto orientation = this->ui_.columns ? this->ui_.columns->orientation() : Qt::Horizontal;
    qreal partition = orientation == Qt::Horizontal ? this->width() / 2. : this->height() / 2.;
    if (this->ui_.profileScroll && this->ui_.columns)
    {
        const auto origin = this->ui_.profileScroll->mapTo(this, QPoint(0, 0));
        partition = orientation == Qt::Horizontal ? origin.x() + this->ui_.profileScroll->width()
                                                     : origin.y() + this->ui_.profileScroll->height();
        partition += this->ui_.columns->handleWidth() / 2.;
    }
    const bool light = this->theme->isLightTheme();
    usercardChrome::paint(painter, this->rect(), this->scale(), usercardChrome::palette(light), light, orientation, partition);
}

void UserInfoPopup::updateQuickActionLayout()
{
    if (!this->ui_.quickActions || !this->ui_.profileScroll)
        return;
    const auto scale = std::max(0.75F, this->scale());
    const auto iconOnly = this->ui_.profileScroll->viewport()->width() < int(250 * scale);
    const auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, scale);
    const bool light = this->theme->isLightTheme();
    const QColor color(light ? "#67557e" : "#b4a4d5");
    const auto iconSize = std::max(12, int(15 * scale));
    const auto dpr = this->devicePixelRatioF();
    const auto key = color.name() + QString::number(iconSize) + QString::number(dpr);
    const auto buttons = std::array{this->ui_.quickWhisper, this->ui_.quickProfile,
                                    this->ui_.quickCopyId};
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        auto *button = buttons[i];
        button->setFont(font);
        button->setFixedHeight(int(30 * scale));
        button->setToolButtonStyle(iconOnly ? Qt::ToolButtonIconOnly :
                                             Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(iconSize, iconSize));
        if (button->property("quickIconKey").toString() == key)
            continue;
        QPixmap pixmap(QSize(qRound(iconSize * dpr), qRound(iconSize * dpr)));
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(iconSize / 16.0, iconSize / 16.0);
        QPen pen(color, 1.5);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        if (i == 0)
        {
            p.drawRoundedRect(QRectF(2, 3.5, 12, 9), 1.6, 1.6);
            p.drawPolyline(QPolygonF{{2.8, 4.5}, {8, 8.6}, {13.2, 4.5}});
        }
        else if (i == 1)
        {
            p.drawEllipse(QPointF(6.3, 4.6), 2.6, 2.6);
            p.drawArc(QRectF(1.5, 8, 9.6, 9), 0, 180 * 16);
            p.drawPolyline(QPolygonF{{10.0, 2.0}, {14.2, 2.0}, {14.2, 6.2}});
            p.drawLine(QPointF(14.2, 2.0), QPointF(10.2, 6.0));
        }
        else
        {
            p.drawRoundedRect(QRectF(5, 5, 8.5, 9.5), 1.4, 1.4);
            p.drawPolyline(QPolygonF{{3, 11}, {2, 11}, {2, 2.5},
                                     {10.5, 2.5}, {10.5, 3.5}});
        }
        p.end();
        button->setIcon(QIcon(pixmap));
        button->setProperty("quickIconKey", key);
    }
}

void UserInfoPopup::updateQuickActions()
{
    if (!this->ui_.quickActions)
        return;
    const bool twitch = !this->isYouTube_ && !this->isKick_;
    const bool verified = !this->userId_.isEmpty() && !this->userName_.isEmpty();
    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    this->ui_.quickWhisper->setVisible(twitch);
    this->ui_.quickWhisper->setEnabled(this->offlinePreview_ ||
        (verified && !current->isAnon() && current->getUserId() != this->userId_));
    this->ui_.quickProfile->setEnabled(this->offlinePreview_ ||
        (this->isYouTube_ ? !this->youtubeChannelId_.isEmpty() : verified));
    this->ui_.quickCopyId->setEnabled(this->offlinePreview_ ||
        (this->isYouTube_ ? !this->youtubeChannelId_.isEmpty() : !this->userId_.isEmpty()));
    this->updateQuickActionLayout();
}

void UserInfoPopup::openWhisperConversation()
{
    if (this->offlinePreview_ || this->isKick_ || this->isYouTube_ ||
        this->userId_.isEmpty() || this->userName_.isEmpty())
        return;
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon() || account->getUserId() == this->userId_)
        return;
    auto &window = getApp()->getWindows()->getMainWindow();
    auto &notebook = window.getNotebook();
    notebook.ensureWhispersTab();
    for (int index = 0; index < notebook.getPageCount(); ++index)
    {
        auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(index));
        if (!page || !page->isPermanentWhispersTab())
            continue;
        for (auto *split : page->getSplits())
        {
            if (split->getChannel()->getType() != Channel::Type::TwitchWhispers)
                continue;
            const auto login = this->userName_;
            // Select the existing local DM conversation; nothing is sent.
            const bool selected = split->selectWhisperPeer(login);
            notebook.select(page);
            window.raise();
            window.activateWindow();
            split->setFocus(Qt::OtherFocusReason);
            if (!selected)
                noctistooltip::showText(split->mapToGlobal(QPoint(12, split->height() - 30)),
                    uiText("Finish the existing whisper draft before changing conversations."), split, {}, 2400);
            return;
        }
    }
}

bool UserInfoPopup::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == this->ui_.messagesPage || watched == this->ui_.latestMessages) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        this->scheduleHistoryViewportFill();
    if (this->ui_.rightMotionViewport && this->ui_.rightMotionViewport->motionActive() && this->ui_.latestMessages &&
        (watched == this->ui_.latestMessages || watched == &this->ui_.latestMessages->getScrollBar()) &&
        (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress))
        this->ui_.rightMotionViewport->endMotion();
    if (watched == this->ui_.messagesHeader && event->type() == QEvent::Resize &&
        static_cast<QResizeEvent *>(event)->size().width() != static_cast<QResizeEvent *>(event)->oldSize().width())
        this->updateMessagesHeadingLayout();
    if (this->ui_.profileScroll &&
        watched == this->ui_.profileScroll->viewport() &&
        event->type() == QEvent::Resize &&
        static_cast<QResizeEvent *>(event)->size().width() != static_cast<QResizeEvent *>(event)->oldSize().width())
        this->updateQuickActionLayout();
    if (this->ui_.latestMessages &&
        (watched == this->ui_.latestMessages ||
         watched == &this->ui_.latestMessages->getScrollBar()) &&
        event->type() == QEvent::Wheel &&
        static_cast<QWheelEvent *>(event)->angleDelta().y() > 0)
    {
        // A deliberate up-scroll may request more even when the viewport is full.
        const auto generation = this->usercardMessagesRequestGeneration_;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != this->usercardMessagesRequestGeneration_)
            {
                return;
            }
            auto &scrollbar = this->ui_.latestMessages->getScrollBar();
            if (scrollbar.getDesiredValue() <=
                scrollbar.getMinimum() +
                    usercardMessagePreloadDistance(scrollbar))
            {
                this->requestPublicHistory(HistoryRequest::Manual);
            }
        });
    }
    return DraggablePopup::eventFilter(watched, event);
}

void UserInfoPopup::updateResponsiveLayout()
{
    if (!this->ui_.columns || !this->ui_.profileScroll)
    {
        return;
    }
    const auto scale = std::max(0.75F, this->scale());
    if (this->ui_.identityHeader)
    {
        this->ui_.identityHeader->layout()->setContentsMargins(
            int(12 * scale), int(30 * scale), int(12 * scale), int(6 * scale));
    }
    if (this->ui_.profileFacts)
    {
        this->ui_.profileFacts->layout()->setContentsMargins(
            int(14 * scale), int(9 * scale), int(14 * scale), int(9 * scale));
        this->ui_.profileFacts->layout()->setSpacing(int(5 * scale));
        for (auto *child : this->ui_.profileFacts->children())
        {
            if (auto *row = dynamic_cast<UsercardFactRow *>(child))
            {
                row->setScale(scale);
            }
        }
        // status icons are 15 px boxes: indent their text like the fact rows
        for (auto *row : {this->ui_.followageRow, this->ui_.subageRow,
                          this->ui_.subGiftRow})
        {
            if (row)
            {
                row->layout()->setSpacing(int(7 * scale));
            }
        }
    }
    const bool narrow = this->getLayoutContainer()->width() < int(520 * scale);
    const auto orientation = narrow ? Qt::Vertical : Qt::Horizontal;
    const bool orientationChanged =
        this->ui_.columns->orientation() != orientation;
    this->ui_.columns->setOrientation(orientation);
    this->ui_.columns->setHandleWidth(std::max(1, int(2 * scale)));
    this->ui_.profileScroll->setMinimumWidth(narrow ? 0 : int(195 * scale));
    this->ui_.profileScroll->setMinimumHeight(int(125 * scale));
    this->ui_.profileScroll->setMaximumHeight(QWIDGETSIZE_MAX);
    this->ui_.rightColumn->setMinimumWidth(narrow ? 0 : int(195 * scale));
    // Two equal columns, like Chatterino+ (until the user drags the handle).
    if (orientationChanged || (!narrow && !this->columnsBalanced_))
    {
        this->ui_.columns->setSizes(narrow ? QList<int>{2, 3}
                                           : QList<int>{1, 1});
        this->columnsBalanced_ = !narrow && this->ui_.columns->width() > 0;
    }
    this->updateQuickActionLayout();
    this->updateProfileBanner();
    this->fitVerticalProfile();
}

void UserInfoPopup::setMessagesHeading(const QString &channel)
{
    this->headingChannel_ = channel;
    if (this->headingChannel_.startsWith(u'#'))
        this->headingChannel_.remove(0, 1);
    this->updateMessagesHeader();
}

void UserInfoPopup::updateMessagesHeader()
{
    if (!this->ui_.messagesHeading || !this->ui_.contentTabs)
    {
        return;
    }
    const bool onRoles =
        this->ui_.contentTabs->currentWidget() == this->ui_.rolesPage;
    const bool twitchRolesAvailable = !this->isKick_ && !this->isYouTube_;
    const bool light = this->theme && this->theme->isLightTheme();
    const QString muted = light ? "#6b6577" : "#a19dad";
    const QString channel = light ? "#3d2a6b" : "#ddd3f7";
    if (onRoles)
    {
        this->ui_.messagesHeading->setText(
            uiText("Roles on Twitch"));
        this->ui_.headingAvatar->setChannel({});
        this->ui_.headingChannelName->hide();
    }
    else
    {
        this->ui_.messagesHeading->setText(tr("Сообщения в чате"));
        this->ui_.headingChannelName->setText(this->headingChannel_);
        this->ui_.headingChannelName->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(channel));
        this->ui_.headingChannelName->setVisible(!this->headingChannel_.isEmpty());
        this->ui_.headingAvatar->setChannel(this->headingChannel_, !this->offlinePreview_ && !this->isYouTube_, this->isKick_);
    }
    if (this->ui_.messagesToggle)
    {
        this->ui_.messagesToggle->setVisible(onRoles);
    }
    if (this->ui_.rolesToggle)
    {
        this->ui_.rolesToggle->setVisible(twitchRolesAvailable && !onRoles);
    }
    if (this->ui_.historyButton)
    {
        this->ui_.historyButton->setVisible(!onRoles);
    }
    this->ui_.messagesHeading->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(muted));
    this->updateMessagesHeadingLayout();
}

void UserInfoPopup::updateMessagesHeadingLayout()
{
    if (!this->ui_.messagesHeader || !this->ui_.headingGroup || !this->ui_.headingActions ||
        !this->ui_.messagesHeading || !this->ui_.headingChannelName || !this->ui_.headingAvatar)
        return;
    auto *outer = static_cast<QBoxLayout *>(this->ui_.messagesHeader->layout());
    auto *heading = this->ui_.messagesHeading;
    auto *channel = this->ui_.headingChannelName;
    const bool showChannel = !channel->isHidden() && !this->headingChannel_.isEmpty();
    const auto margins = outer->contentsMargins();
    const int width = this->ui_.messagesHeader->width() - margins.left() - margins.right();
    if (width <= 0)
        return;
    const int actions = this->ui_.headingActions->layout()->sizeHint().width();
    const int gap = this->ui_.headingGroup->layout()->spacing();
    const int iconWidth = showChannel ? this->ui_.headingAvatar->width() + gap : 0;
    const auto channelMetrics = QFontMetrics(channel->font());
    const int identity = showChannel ? channelMetrics.horizontalAdvance(this->headingChannel_) + iconWidth + 3 : 0;
    const auto headingMetrics = QFontMetrics(heading->font());
    const bool roles = this->ui_.contentTabs->currentWidget() == this->ui_.rolesPage;
    const auto full = roles ? uiText("Roles on Twitch")
                            : tr("Сообщения в чате");
    const auto compact = roles ? uiText("Roles") : tr("Чат");
    const int minimumTitle = showChannel ? identity : headingMetrics.horizontalAdvance(compact) + 3;
    const bool stack = actions > 0 && width < minimumTitle + actions + outer->spacing();
    const auto direction = stack ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (outer->direction() != direction)
        outer->setDirection(direction);
    const int available = std::max(0, width - (stack || actions == 0 ? 0 : actions + outer->spacing()));
    const int prefixRoom = available - identity - (showChannel ? gap : 0);
    QString prefix = full;
    if (headingMetrics.horizontalAdvance(full) + 3 > prefixRoom)
        prefix = compact;
    if (showChannel && headingMetrics.horizontalAdvance(prefix) + 3 > prefixRoom)
        prefix.clear();
    if (heading->text() != prefix)
        heading->setText(prefix);
    heading->setVisible(!prefix.isEmpty());
    if (showChannel)
    {
        const int nameAvailable = std::max(0, available - iconWidth -
            (prefix.isEmpty() ? 0 : headingMetrics.horizontalAdvance(prefix) + gap + 4));
        const auto shown = channelMetrics.elidedText(this->headingChannel_, Qt::ElideRight, nameAvailable);
        if (channel->text() != shown)
            channel->setText(shown);
        channel->setAccessibleName(this->headingChannel_);
    }
}

void UserInfoPopup::showActionsMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName("UsercardActionsMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto menuFont = getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                        this->scale());
    menu->setFont(menuFont);
    const auto addButton = [this](QMenu *target, Button *button,
                                  const QString &caption) {
        // Only the hidden controls' own visibility reflects permissions.
        // The synthetic preview uses inert placeholders for visual QA.
        if (this->offlinePreview_)
        {
            target->addAction(caption)->setEnabled(false);
            return;
        }
        if (!button || button->isHidden())
        {
            return;
        }
        auto *action = target->addAction(caption);
        action->setEnabled(button->enabled());
        QObject::connect(action, &QAction::triggered, this,
                         [button = QPointer<Button>(button)] {
            if (button && button->enabled() && !button->isHidden())
            {
                Q_EMIT button->leftClicked();
                Q_EMIT button->clicked(Qt::LeftButton);
            }
        });
    };

    // The card already has the profile link, copy-ID button, copyable name,
    // badges and roles tab. No second copy of those controls in this menu.
    addButton(menu, this->ui_.nameHistoryButton, tr("История имён"));
    addButton(menu, this->ui_.userlogsLabel, uiText("Logs"));
    addButton(menu, this->ui_.notesAdd, uiText("Notes"));

    auto *management = new QMenu(tr("Действия с пользователем"), menu);
    management->setObjectName("UsercardManagementMenu");
    management->setFont(menuFont);
    if (this->offlinePreview_)
    {
        for (const auto &caption : {uiText("Block"), uiText("Ignore highlights")})
        {
            management->addAction(caption)->setEnabled(false);
        }
    }
    else
    {
        for (auto *checkbox : {this->ui_.block, this->ui_.ignoreHighlights})
        {
            if (checkbox->isHidden())
            {
                continue;
            }
            auto *action = management->addAction(checkbox->text());
            action->setCheckable(true);
            action->setChecked(checkbox->isChecked());
            action->setEnabled(checkbox->isEnabled());
            QObject::connect(action, &QAction::triggered, checkbox,
                             [checkbox](bool checked) {
                                 checkbox->setChecked(checked);
                             });
        }
        if (this->ui_.followButton && !this->ui_.followButton->isHidden())
        {
            const bool following =
                this->isFollowingStatusKnown() && this->isFollowing();
            auto *follow = management->addAction(
                following ? tr("Перестать отслеживать") : tr("Отслеживать"));
            follow->setEnabled(this->ui_.followButton->enabled());
            QObject::connect(follow, &QAction::triggered, this, [this] {
                this->toggleUsercardFollow();
            });
        }
        // Only role management entries from the old action list; do not
        // repeat ordinary profile, log, note or service actions here.
        for (const auto &[caption, button] : this->actionMenuButtons_)
        {
            if (button == this->ui_.rolesLabel)
            {
                addButton(management, button, uiText("Roles"));
            }
            else if (!caption.isEmpty())
            {
                addButton(management, button, caption);
            }
        }
    }
    if (!management->isEmpty())
    {
        if (!menu->isEmpty())
        {
            menu->addSeparator();
        }
        menu->addMenu(management);
    }
    else
        management->deleteLater();

    // Rarely used profile metadata and external services sit behind one
    // compact menu instead of competing with the usual actions.
    auto *extras = new QMenu(uiText("Advanced"), menu);
    extras->setObjectName("UsercardExtrasMenu");
    extras->setFont(menuFont);
    this->showProfileDetails(extras);
    if (this->offlinePreview_ ||
        (!this->isKick_ && !this->isYouTube_ && !this->userName_.isEmpty()))
    {
        if (!extras->isEmpty())
        {
            extras->addSeparator();
        }
        auto addLink = [this, extras](const QString &caption, const QUrl &url) {
            auto *action = extras->addAction(caption);
            action->setEnabled(!this->offlinePreview_);
            if (!this->offlinePreview_)
            {
                QObject::connect(action, &QAction::triggered, extras,
                                 [url] { QDesktopServices::openUrl(url); });
            }
        };
        addLink(QStringLiteral("ChatVault"),
                QUrl(chatVaultTwitchChannelUrl(this->userName_)));
        addLink(QStringLiteral("Lurkology"),
                QUrl(lurkologyTwitchProfileUrl(this->userName_)));
        addButton(extras, this->ui_.sevenTVUserLabel, uiText("7TV"));
    }
    else if (!this->isYouTube_)
    {
        addButton(extras, this->ui_.sevenTVUserLabel, uiText("7TV"));
    }
    if (!this->offlinePreview_ && this->ui_.switchAvatars &&
        !this->ui_.switchAvatars->isHidden())
    {
        addButton(extras, this->ui_.switchAvatars,
                  this->ui_.switchAvatars->text());
    }
    if (!extras->isEmpty())
    {
        menu->addMenu(extras);
    }
    else
        extras->deleteLater();

    styleMenu(menu);
    menu->popup(
        this->ui_.actions->mapToGlobal(QPoint(0, this->ui_.actions->height())));
}

void UserInfoPopup::showProfileDetails(QMenu *menu)
{
    auto *details = new QMenu(tr("Сведения о профиле"), menu);
    details->setFont(menu->font());
    for (auto *label : {this->ui_.pronounsLabel,
                        this->ui_.lastLiveLabel, this->ui_.statusLabel,
                        this->ui_.chatterCountLabel, this->ui_.userColorLabel})
    {
        if (!label || label->getText().isEmpty())
        {
            continue;
        }
        auto *action = details->addAction(label->getText());
        action->setEnabled(!this->offlinePreview_);
        action->setToolTip(tr("Нажмите, чтобы скопировать"));
        const auto copy = label->property("copy-text").toString();
        QObject::connect(action, &QAction::triggered, this,
                         [text = copy.isEmpty() ? label->getText() : copy] {
                             crossPlatformCopy(text);
                         });
    }
    if (this->seventvPaint_)
    {
        auto *action = details->addAction(
            tr("7TV-пейнт: %1").arg(this->seventvPaint_->name));
        action->setEnabled(!this->offlinePreview_);
        QObject::connect(action, &QAction::triggered, this,
                         [id = this->seventvPaint_->id] {
                             crossPlatformCopy(id);
                         });
    }
    if (!details->isEmpty())
    {
        menu->addMenu(details);
    }
    else
        details->deleteLater();
}

void UserInfoPopup::updateNamePresentation()
{
    auto *name = dynamic_cast<UsercardNameLabel *>(this->ui_.nameLabel);
    if (!name)
    {
        return;
    }
    auto color = this->palette().color(QPalette::WindowText);
    if (this->ui_.userColorRow)
    {
        const QColor actual(
            this->ui_.userColorRow->property("swatch-color").toString());
        if (actual.isValid())
        {
            color = actual;
        }
    }
    name->setPaint(this->seventvPaint_, color);
}

void UserInfoPopup::setOfflinePreviewData(const UserInfoPreviewData &data)
{
    // Deliberately separate from setData(): no Helix, IVR, 7TV, account lookup,
    // follow-state query or public-history request is started by this method.
    if (!this->offlinePreview_)
    {
        return;
    }
    ++this->userDataRequestGeneration_;
    ++this->usercardMessagesRequestGeneration_;
    this->earnedBadges_.clear();
    static_cast<UsercardBadgeStrip *>(this->ui_.badgeStrip)->resetExpansion();
    this->earnedBadgesReady_ = false;
    this->earnedBadgesError_.clear();
    this->userId_ = data.userId;
    this->userName_ = data.login;
    if (this->ui_.contentTabs->currentWidget() == this->ui_.rolesPage)
    {
        this->ui_.contentTabs->setCurrentWidget(this->ui_.messagesPage);
    }
    this->ui_.rolesPage->setTarget(data.userId, data.login);
    this->historyProfileDisplayName_ = data.displayName;
    this->loginHint_ = data.login;
    if (this->ui_.userColorRow)
    {
        this->ui_.userColorRow->setProperty(
            "swatch-color",
            data.chatColor.isEmpty() ? QVariant() : QVariant(data.chatColor));
    }
    this->offlinePublicHistoryAllowed_ = data.allowPublicHistory;
    this->offlineHistoryAutoFill_ = data.allowPublicHistory && data.autoFillHistory;
    this->resetPublicHistory();
    this->underlyingChannel_ = data.messages;
    this->channel_ = data.messages;
    this->ui_.nameLabel->setText(data.displayName.isEmpty() ? data.login
                                                            : data.displayName);
    this->ui_.nameLabel->setProperty("copy-text", data.login);
    this->ui_.userIDLabel->setText(uiText(TEXT_USER_ID) % data.userId);
    this->ui_.userIDLabel->setProperty("copy-text", data.userId);
    this->ui_.createdDateLabel->setText(data.createdText);
    this->ui_.followerCountLabel->setText(data.followersText);
    if (this->ui_.pronounsLabel)
    {
        this->ui_.pronounsLabel->setText(data.pronouns);
    }
    this->ui_.lastLiveLabel->hide();
    this->ui_.statusLabel->hide();
    this->ui_.chatterCountLabel->hide();
    this->setUsercardFollowage(std::nullopt);
    this->ui_.subageLabel->setText(tr("Подписка была — 1 месяц · тестовые данные"));
    this->ui_.subageRow->setVisible(true);
    this->ui_.subageIcon->setVisible(true);
    this->updateUsercardStatusIcons();
    this->ui_.subGiftRow->hide();
    this->ui_.localizedNameLabel->hide();
    this->ui_.localizedNameCopyButton->hide();
    this->ui_.userColorRow->hide();
    this->ui_.seventvPaintRow->hide();
    this->ui_.nameHistoryButton->hide();
    this->ui_.notesPreview->hide();
    this->ui_.badgesLabel->setVisible(true);
    this->ui_.badgesLabel->setEnabled(false);
    this->ui_.badgesLabel->setAccessibleDescription(
        tr("Badge lookup is disabled in offline preview"));
    this->ui_.followButton->hide();
    this->ui_.timeoutWidget->hide();
    this->ui_.badgeStrip->hide();
    this->ui_.avatarButton->setPixmap(data.avatar);
    this->earnedBadges_ = data.earnedBadges;
    this->earnedBadgesReady_ = !data.earnedBadges.empty();
    this->updateObservedBadges();
    this->updateQuickActions();
    this->bannerPixmap_ = data.banner;
    this->updateProfileBanner();
    this->usercardMessagesChannel_ =
        data.sourceMessages
            ? filterMessages(data.login, data.userId, data.sourceMessages)
            : data.messages;
    const auto visibleMessages = this->usercardMessagesChannel_;
    this->ui_.latestMessages->setChannel(visibleMessages ? visibleMessages
                                                         : Channel::getEmpty());
    this->ui_.latestMessages->setSourceChannel(
        data.messages ? data.messages : Channel::getEmpty());
    const bool hasMessages = visibleMessages && visibleMessages->hasMessages();
    this->ui_.latestMessages->setVisible(hasMessages);
    this->ui_.noMessagesLabel->setText(
        tr("В загруженном буфере канала сообщений нет"));
    this->ui_.noMessagesLabel->setVisible(!hasMessages);
    this->ui_.loadMoreMessages->hide();
    const auto channelName =
        data.messages ? data.messages->getName() : QString();
    this->setMessagesHeading(channelName);
    this->setWindowTitle(tr("%1 — offline usercard preview").arg(data.login));
    this->scheduleHistoryViewportFill();
    this->updatePublicHistoryButton();
    this->refreshUsercardActivity();
}

void UserInfoPopup::loadProfileBanner(const QString &url)
{
    if (this->offlinePreview_ || url == this->bannerUrl_)
    {
        return;
    }
    this->bannerUrl_ = url;
    this->bannerPixmap_ = {};
    this->updateProfileBanner();
    if (getApp()->getStreamerMode()->isEnabled() &&
        getSettings()->streamerModeHideUsercardAvatars)
    {
        return;
    }
    const QUrl imageUrl(url);
    if (url.isEmpty() || imageUrl.scheme() != u"https")
    {
        return;
    }
    const auto generation = this->userDataRequestGeneration_;
    NetworkRequest(imageUrl)
        .caller(this)
        .timeout(8000)
        .cache()
        .onSuccess([this, generation, url](const NetworkResult &result) {
            if (generation != this->userDataRequestGeneration_ ||
                url != this->bannerUrl_ ||
                result.getData().size() > 4 * 1024 * 1024)
            {
                return;
            }
            QBuffer buffer;
            buffer.setData(result.getData());
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const auto dimensions = reader.size();
            if (!dimensions.isValid() || dimensions.width() > 8192 ||
                dimensions.height() > 4096 ||
                qint64(dimensions.width()) * dimensions.height() > 16'000'000)
            {
                return;
            }
            const auto image = reader.read();
            if (!image.isNull())
            {
                this->bannerPixmap_ = QPixmap::fromImage(image);
                this->updateProfileBanner();
            }
        })
        .execute();
}

void UserInfoPopup::updateProfileBanner()
{
    if (!this->ui_.banner)
    {
        return;
    }
    this->ui_.banner->setFixedHeight(std::max(1, int(62 * this->scale())));
    this->ui_.banner->show();
    static_cast<UsercardCoverLabel *>(this->ui_.banner)->setCover(this->bannerPixmap_, this->scale(),
        usercardChrome::palette(this->theme->isLightTheme()));
}

void UserInfoPopup::updateObservedBadges()
{
    auto *strip = static_cast<UsercardBadgeStrip *>(this->ui_.badgeStrip);
    if (!strip)
    {
        return;
    }
    std::vector<EmotePtr> badges;
    QSet<QString> seen;
    auto append = [&](const EmotePtr &emote) {
        if (!emote)
        {
            return;
        }
        const auto imageKey = emote->images.getImage1()->url().string;
        const auto key = imageKey.isEmpty() ? emote->id.string : imageKey;
        if (!key.isEmpty() && seen.contains(key))
        {
            return;
        }
        seen.insert(key);
        badges.push_back(emote);
    };
    for (const auto &emote : this->earnedBadges_)
    {
        append(emote);
    }
    if (!this->offlinePreview_ && this->underlyingChannel_ && !this->isKick_ &&
        !this->isYouTube_ && !this->userId_.isEmpty())
    {
        MessagePtr latest;
        for (const auto &message :
             this->underlyingChannel_->getMessageSnapshot())
        {
            if (message->userID == this->userId_ &&
                !message->twitchBadges.empty())
            {
                latest = message;
            }
        }
        if (latest)
        {
            auto *channel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
            for (const auto &badge : latest->twitchBadges)
            {
                auto emote =
                    channel ? channel->twitchBadge(badge.key_, badge.value_)
                            : std::optional<EmotePtr>{};
                if (!emote)
                {
                    emote = getApp()->getTwitchBadges()->badge(badge.key_,
                                                               badge.value_);
                }
                if (emote && *emote)
                {
                    append(*emote);
                }
            }
        }
    }
    strip->setBadges(std::move(badges), this->scale());
    // The strip handles hover itself with image previews; keep collection
    // availability accessible without a competing native text tooltip.
    strip->setAccessibleDescription(
        this->earnedBadgesReady_
            ? tr("Полученные значки Twitch и значки из загруженных сообщений")
        : this->earnedBadgesError_.isEmpty()
            ? tr("Значки из сообщений; коллекция Twitch загружается")
            : tr("Показаны только значки из сообщений. %1")
                  .arg(this->earnedBadgesError_));
}

void UserInfoPopup::requestEarnedBadges()
{
    if (this->offlinePreview_ || this->isKick_ || this->isYouTube_ ||
        this->userId_.isEmpty() || !this->underlyingChannel_)
    {
        return;
    }
    const auto uid = this->userId_;
    const auto channel = this->underlyingChannel_->getName();
    const auto generation = this->userDataRequestGeneration_;
    const QPointer<UserInfoPopup> self(this);
    auto current = [self, uid, channel, generation] {
        return self && self->userId_ == uid && self->underlyingChannel_ &&
               self->underlyingChannel_->getName() == channel &&
               self->userDataRequestGeneration_ == generation;
    };
    requestUsercardBadges(
        uid, this->userName_, channel, this,
        [self, current](QVector<GqlBadge> badges) {
            if (!current())
            {
                return;
            }
            self->earnedBadges_ = usercardBadgeEmotes(badges);
            self->earnedBadgesReady_ = true;
            self->earnedBadgesError_.clear();
            self->updateObservedBadges();
        },
        [self, current](const QString &error) {
            if (!current())
            {
                return;
            }
            self->earnedBadgesError_ = error;
            self->updateObservedBadges();
        });
}

void UserInfoPopup::windowDeactivationEvent()
{
    // Opening this card's compact menu must not destroy the card (and its
    // open menus) before an action can be selected.
    for (auto *popup = QApplication::activePopupWidget(); popup;
         popup = popup->parentWidget())
    {
        if (popup == this)
        {
            return;
        }
    }
    if (this->editUserNotesDialog_.isNull() ||
        !this->editUserNotesDialog_->isVisible())
    {
        BaseWindow::windowDeactivationEvent();
    }
}

void UserInfoPopup::registerMnemonicButton(LabelButton *button, int key,
                                           std::function<void()> action)
{
    if (button == nullptr)
    {
        return;
    }

    this->mnemonicActions_[key] = {
        std::move(action),
        [button] {
            return !button->isHidden() && button->enabled();
        },
    };
}

void UserInfoPopup::keyPressEvent(QKeyEvent *event)
{
    if (this->offlinePreview_)
    {
        DraggablePopup::keyPressEvent(event);
        return;
    }
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers == Qt::NoModifier || modifiers == Qt::AltModifier)
    {
        auto it = this->mnemonicActions_.find(event->key());
        if (it != this->mnemonicActions_.end())
        {
            const auto &[action, canRun] = it->second;
            if (!canRun || canRun())
            {
                action();
                event->accept();
                return;
            }
        }
    }

    DraggablePopup::keyPressEvent(event);
}

void UserInfoPopup::installEvents()
{
    std::shared_ptr<bool> ignoreNext = std::make_shared<bool>(false);

    // block
    QObject::connect(
        this->ui_.block, &QCheckBox::stateChanged,
        [this](int newState) mutable {
            if (this->isKick_ || this->isYouTube_)
            {
                return;
            }

            auto currentUser = getApp()->getAccounts()->twitch.getCurrent();

            const auto reenableBlockCheckbox = [this] {
                this->ui_.block->setEnabled(true);
            };

            if (!this->ui_.block->isEnabled())
            {
                reenableBlockCheckbox();
                return;
            }

            if (newState == Qt::Unchecked)
            {
                this->ui_.block->setEnabled(false);

                getApp()->getAccounts()->twitch.getCurrent()->unblockUser(
                    this->userId_, this->userName_, this,
                    [this, reenableBlockCheckbox, currentUser] {
                        this->channel_->addSystemMessage(
                            uiText(QString("You successfully unblocked user %1"))
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    },
                    [this, reenableBlockCheckbox] {
                        this->channel_->addSystemMessage(
                            uiText(QString("User %1 couldn't be unblocked, an unknown "
                                    "error occurred!"))
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    });
                return;
            }

            if (newState == Qt::Checked)
            {
                this->ui_.block->setEnabled(false);

                bool wasPinned = this->ensurePinned();
                auto btn = QMessageBox::warning(
                    this, uiText(u"Blocking ") % this->userName_,
                    uiText(u"Blocking %1 can cause unintended side-effects like unfollowing.\n\n"_s
                    "Are you sure you want to block %1?").arg(this->userName_),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (wasPinned)
                {
                    this->togglePinned();
                }
                if (btn != QMessageBox::Yes)
                {
                    reenableBlockCheckbox();
                    QSignalBlocker blocker(this->ui_.block);
                    this->ui_.block->setCheckState(Qt::Unchecked);
                    return;
                }

                getApp()->getAccounts()->twitch.getCurrent()->blockUser(
                    this->userId_, this->userName_, this,
                    [this, reenableBlockCheckbox, currentUser] {
                        this->channel_->addSystemMessage(
                            uiText(QString("You successfully blocked user %1"))
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    },
                    [this, reenableBlockCheckbox] {
                        this->channel_->addSystemMessage(
                            uiText(QString("User %1 couldn't be blocked, an "
                                    "unknown error occurred!"))
                                .arg(this->userName_));
                        reenableBlockCheckbox();
                    });
                return;
            }

            qCWarning(chatterinoWidget)
                << "Unexpected check-state when blocking" << this->userName_
                << QMetaEnum::fromType<Qt::CheckState>().valueToKey(newState);
        });

    // ignore highlights
    QObject::connect(
        this->ui_.ignoreHighlights, &QCheckBox::clicked,
        [this](bool checked) mutable {
            this->ui_.ignoreHighlights->setEnabled(false);

            if (checked)
            {
                getSettings()->blacklistedUsers.insert(
                    HighlightBlacklistUser{this->userName_, false});
                this->ui_.ignoreHighlights->setEnabled(true);
            }
            else
            {
                const auto &vector = getSettings()->blacklistedUsers.raw();

                for (int i = 0; i < static_cast<int>(vector.size()); i++)
                {
                    if (this->userName_ ==
                        vector[static_cast<size_t>(i)].getPattern())
                    {
                        getSettings()->blacklistedUsers.removeAt(i);
                        i--;
                    }
                }
                if (getSettings()->isBlacklistedUser(this->userName_))
                {
                    this->ui_.ignoreHighlights->setToolTip(
                        uiText("Name matched by regex"));
                }
                else
                {
                    this->ui_.ignoreHighlights->setEnabled(true);
                }
            }
        });

    // user notes
    auto openNotes = [this]() mutable {
        if (this->editUserNotesDialog_.isNull())
        {
            this->editUserNotesDialog_ = new EditUserNotesDialog(this);
            // ignoring since it the dialog is only used in this instance
            std::ignore = this->editUserNotesDialog_->onOk.connect(
                [userId = this->userId_](const QString &newNotes) {
                    getApp()->getUserData()->setUserNotes(userId, newNotes);
                });
        }

        auto userData = getApp()->getUserData()->getUser(this->userId_);
        auto initialNotes = userData.has_value() ? userData->notes : QString();

        this->editUserNotesDialog_->setNotes(initialNotes);
        this->editUserNotesDialog_->updateWindowTitle(this->userName_);
        this->editUserNotesDialog_->show();
    };
    QObject::connect(this->ui_.notesAdd, &LabelButton::clicked,
                     [openNotes](Qt::MouseButton) mutable {
                         openNotes();
                     });
    this->registerMnemonicButton(this->ui_.notesAdd, Qt::Key_N, openNotes);

    this->userDataUpdatedConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            getApp()->getUserData()->userDataUpdated().connect([this]() {
                this->updateNotes();
            }));

    getSettings()->hideModActionsOnModUsercards.connect(
        [this](bool enabled) {
            if (enabled && getSettings()->showModActionsOnModUsercardsAsLeadMod)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus();
                }
            }
            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showModActionsOnModUsercardsAsLeadMod.connect(
        [this](bool enabled) {
            if (enabled && getSettings()->hideModActionsOnModUsercards)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus();
                }
            }

            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showLeadModRoleButtons.connect(
        [this](bool enabled) {
            if (enabled)
            {
                if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                        this->underlyingChannel_.get()))
                {
                    twitchChannel->refreshLeadModStatus(true);
                }
            }

            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showUsercardRoleManagementMenu.connect(
        [this](bool) {
            this->userStateChanged_.invoke();
        },
        this->signalHolder_);
    getSettings()->showUsercardSevenTVPaint.connect(
        [this](bool) {
            this->refreshSeventvPaint();
        },
        this->signalHolder_);
    getSettings()->showSevenTVUsercardButton.connect(
        [this](bool enabled) {
            if (enabled && this->seventvUserID_.isEmpty() &&
                !this->seventvUserLookupInFlight_ &&
                !this->seventvUserLookupFinished_ && !this->userId_.isEmpty())
            {
                auto userID = this->userId_;
                const QStringView kickPrefix = u"kick:";
                if (this->isKick_ && userID.startsWith(kickPrefix))
                {
                    userID = userID.mid(kickPrefix.size());
                }
                this->loadSevenTVAvatar(userID, this->isKick_, false);
                return;
            }

            this->refreshSevenTVUserButtonVisibility();
        },
        this->signalHolder_);
    getSettings()->showUsercardNameHistoryButton.connect(
        [this](bool) {
            this->updateNameHistoryButton();
        },
        this->signalHolder_);
    getSettings()->showFollowButtonInUsercard.connect(
        [this](bool enabled) {
            if (enabled && !this->isKick_ && !this->userId_.isEmpty())
            {
                this->refreshFollowingStatus(false);
            }
            this->updateUsercardFollowButton();
        },
        this->signalHolder_);
    getSettings()->showUsercardLiveViewerCount.connect(
        [this](bool) {
            this->updateLiveIndicatorDisplay();
        },
        this->signalHolder_);
    getSettings()->showUsercardLoadMoreMessagesButton.connect(
        [this](bool) {
            this->updateLoadMoreMessagesButton();
        },
        this->signalHolder_);
    getSettings()->alwaysLoadMoreUsercardMessages.connect(
        [this](bool enabled) {
            if (!enabled)
            {
                this->usercardMessagesLazyLoadEnabled_ = false;
                this->updateLoadMoreMessagesButton();
                return;
            }

            this->maybeStartUsercardMessageAutoLoad();
        },
        this->signalHolder_);
    getSettings()->moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            if (!this->isKick_ && !this->userId_.isEmpty() &&
                getSettings()->showFollowButtonInUsercard)
            {
                this->refreshFollowingStatus(true);
            }
            this->updateUsercardFollowButton();
            this->userStateChanged_.invoke();
            this->updateLoadMoreMessagesButton();
            this->maybeStartUsercardMessageAutoLoad();
        },
        this->signalHolder_);
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [this] {
            if (!this->isVisible() ||
                !getSettings()->showUsercardSevenTVPaint ||
                !this->seventvPaint_ || !this->seventvPaint_->animated())
            {
                return;
            }

            this->updateSeventvPaintPixmap();
        });
}

void UserInfoPopup::refreshTargetModerationStatus()
{
    if (this->userName_.isEmpty() || !this->underlyingChannel_ ||
        !this->underlyingChannel_->isTwitchChannel())
    {
        return;
    }

    this->isBroadcaster_ =
        this->userName_.compare(this->underlyingChannel_->getName(),
                                Qt::CaseInsensitive) == 0;

    for (const auto &message : this->underlyingChannel_->getMessageSnapshot())
    {
        this->updateTargetModerationStatusFromMessage(message);

        if (this->isMod_ && this->isBroadcaster_)
        {
            break;
        }
    }
}

bool UserInfoPopup::updateTargetModerationStatusFromMessage(
    const MessagePtr &message)
{
    if (message == nullptr || this->userName_.isEmpty())
    {
        return false;
    }

    if (message->loginName.compare(this->userName_, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    bool changed = false;
    if (!this->isMod_ && (messageHasTwitchBadge(*message, u"moderator") ||
                          messageHasTwitchBadge(*message, u"lead_moderator")))
    {
        this->isMod_ = true;
        changed = true;
    }
    if (!this->isBroadcaster_ &&
        (messageHasTwitchBadge(*message, u"broadcaster") ||
         (this->underlyingChannel_ &&
          this->userName_.compare(this->underlyingChannel_->getName(),
                                  Qt::CaseInsensitive) == 0)))
    {
        this->isBroadcaster_ = true;
        changed = true;
    }

    return changed;
}

bool UserInfoPopup::shouldShowModerationActions() const
{
    if (this->isYouTube_)
    {
        return false;
    }
    if (this->userName_.isEmpty() || !this->underlyingChannel_)
    {
        return false;
    }

    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
    {
        const bool isMyself =
            getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
                this->userName_, Qt::CaseInsensitive) == 0;
        if (isMyself || !twitchChannel->hasModRights())
        {
            return false;
        }
        if (twitchChannel->isBroadcaster())
        {
            return true;
        }

        if (!getSettings()->hideModActionsOnModUsercards)
        {
            return true;
        }

        if (!this->isMod_ && !this->isBroadcaster_)
        {
            return true;
        }

        return getSettings()->showModActionsOnModUsercardsAsLeadMod &&
               twitchChannel->isLeadMod() && this->isMod_ &&
               !this->isBroadcaster_;
    }

    if (auto *kickChannel =
            dynamic_cast<KickChannel *>(this->underlyingChannel_.get()))
    {
        const bool isMyself =
            getApp()->getAccounts()->kick.current()->username().compare(
                this->userName_, Qt::CaseInsensitive) == 0;
        return kickChannel->hasModRights() && !isMyself;
    }

    return false;
}

bool UserInfoPopup::crossActionAvailable() const
{
    const auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (this->isKick_ || this->isYouTube_ || this->userName_.isEmpty() ||
        twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    const bool isMyself =
        getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
            this->userName_, Qt::CaseInsensitive) == 0;
    if (isMyself)
    {
        return false;
    }

    auto auth = MoltorinoAuth::resolveModerationToken(twitchChannel->roomId(),
                                                      twitchChannel->getName());
    if (!auth.hasToken() &&
        getSettings()->showCrossActionsInUnmoderatedChannels)
    {
        auth = MoltorinoAuth::resolveCurrentUserToken();
    }
    return auth.hasToken() && !auth.legacy;
}

void UserInfoPopup::runCrossAction(const QString &command)
{
    if (!this->crossActionAvailable() || !this->underlyingChannel_)
    {
        return;
    }

    auto value = getApp()->getCommands()->execCommand(
        command + ' ' + this->userName_, this->underlyingChannel_, false);
    if (!value.isEmpty())
    {
        this->underlyingChannel_->sendMessage(value);
    }
}

void UserInfoPopup::setData(const QString &name, const ChannelPtr &channel)
{
    this->setData(name, channel, channel);
}

void UserInfoPopup::setData(const QString &name,
                            const ChannelPtr &contextChannel,
                            const ChannelPtr &openingChannel,
                            const QString &expectedUserId)
{
    if (this->offlinePreview_)
    {
        return;
    }
    const QStringView idPrefix = u"id:";
    bool isId = name.startsWith(idPrefix);
    this->historyProfileDisplayName_.clear();
    this->loginHint_ = isId ? QString{} : name;
    if (isId)
    {
        this->userId_ = name.mid(idPrefix.size());
        this->updateNotes();
        this->userName_ = "";
    }
    else if (!expectedUserId.isEmpty())
    {
        // Resolve by this exact ID, never by the current owner of a saved nick.
        // Keep Twitch actions disabled until the canonical login is resolved;
        // the displayed login can use the saved login immediately.
        this->userId_ = expectedUserId;
        this->userName_.clear();
    }
    else
    {
        this->userId_.clear();
        this->userName_ = name;
        this->kickUserSlug_ = KickApi::slugify(name);
    }

    this->channel_ = openingChannel;

    if (!contextChannel->isEmpty())
    {
        this->underlyingChannel_ = contextChannel;
    }
    else
    {
        this->underlyingChannel_ = openingChannel;
    }
    this->twitchUserStateConnection_.reset();
    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
    {
        QPointer<UserInfoPopup> self(this);
        this->twitchUserStateConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                twitchChannel->userStateChanged.connect([self] {
                    QTimer::singleShot(0, self, [self] {
                        if (!self)
                        {
                            return;
                        }

                        self->userStateChanged_.invoke();
                    });
                }));
    }

    this->setWindowTitle(
        uiText(TEXT_TITLE).arg(name, this->underlyingChannel_->getName()));
    this->isKick_ = this->underlyingChannel_->getType() == Channel::Type::Kick;
    this->isYouTube_ =
        this->forceYouTube_ ||
        this->underlyingChannel_->getType() == Channel::Type::YouTube;
    this->forceYouTube_ = false;
    if (this->ui_.contentTabs->currentWidget() == this->ui_.rolesPage)
    {
        this->ui_.contentTabs->setCurrentWidget(this->ui_.messagesPage);
    }
    if (!this->isKick_ && !this->isYouTube_)
    {
        this->ui_.rolesPage->setTarget(this->userId_,
                                      this->loginHint_);
    }
    else
    {
        this->ui_.rolesPage->setTarget({}, {});
    }
    this->ui_.contentTabs->setTabEnabled(
        this->ui_.contentTabs->indexOf(this->ui_.rolesPage),
        !this->isKick_ && !this->isYouTube_);
    if (this->seventvAvatar_)
    {
        this->seventvAvatar_->stop();
        this->seventvAvatar_->deleteLater();
        this->seventvAvatar_ = nullptr;
    }
    this->avatarPixmap_ = {};
    this->ui_.avatarButton->setPixmap({});
    this->ui_.switchAvatars->hide();
    this->avatarUrl_.clear();
    this->helixAvatarUrl_.clear();
    this->seventvAvatarUrl_.clear();
    this->isTwitchAvatarShown_ = true;
    this->bannerUrl_.clear();
    this->bannerPixmap_ = {};
    this->earnedBadges_.clear();
    static_cast<UsercardBadgeStrip *>(this->ui_.badgeStrip)->resetExpansion();
    this->earnedBadgesReady_ = false;
    this->earnedBadgesError_.clear();
    this->updateProfileBanner();
    this->setMessagesHeading(this->underlyingChannel_->getName());
    if (this->isKick_)
    {
        this->ui_.timeoutWidget->setMinTimeout(60);
    }

    this->resetNameHistory();
    this->resetUsercardMessageLoader();
    this->isMod_ = false;
    this->isBroadcaster_ = false;
    this->userDataRequestGeneration_++;
    this->seventvUserRequestGeneration_++;
    this->seventvUserID_.clear();
    this->seventvUserLookupInFlight_ = false;
    this->seventvUserLookupFinished_ = false;
    this->seventvPaintAsked_ = false;
    this->refreshSevenTVUserButtonVisibility();
    this->refreshSeventvPaint();
    this->refreshTargetModerationStatus();
    if (!this->isKick_)
    {
        if (auto *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()))
        {
            twitchChannel->refreshLeadModStatus();
        }
    }

    this->ui_.nameLabel->setText(name);
    this->ui_.nameLabel->setProperty("copy-text", name);
    this->ui_.localizedNameLabel->hide();
    this->ui_.localizedNameCopyButton->hide();
    this->ui_.userIDLabel->setText(uiText(TEXT_USER_ID) % (this->userId_.isEmpty()
                                                       ? uiText(TEXT_LOADING)
                                                       : this->userId_));
    this->ui_.userIDLabel->setProperty("copy-text", this->userId_);
    this->ui_.notesPreview->setText({});
    this->ui_.notesPreview->hide();
    this->resetUsercardInfoRows();
    this->resetFollowingStatus();
    this->updateUsercardFollowButton();

    if (this->isKick_)
    {
        this->updateKickUserData();
        if (this->ui_.pronounsLabel)
        {
            this->ui_.pronounsLabel->hide();
        }
    }
    else if (this->isYouTube_)
    {
        this->updateYouTubeUserData();
    }
    else
    {
        this->updateUserData();
        if (!this->userId_.isEmpty())
        {
            this->refreshFollowingStatus(false);
        }
    }

    this->userStateChanged_.invoke();

    if (this->isYouTube_)
    {
        this->updateYouTubeLatestMessages();
    }
    else if (!isId || !this->userId_.isEmpty())
    {
        this->updateLatestMessages();
    }
    // If we're opening by ID, this will be called as soon as we get the information from twitch

    auto type = this->channel_->getType();
    if (this->isYouTube_)
    {
        this->ui_.usercardLabel->setText(uiText("Open channel on &YouTube"));
        this->ui_.usercardLabel->setVisible(!this->youtubeChannelId_.isEmpty());
        this->ui_.userlogsLabel->hide();
    }
    else if (type == Channel::Type::TwitchLive ||
             type == Channel::Type::TwitchWhispers ||
             type == Channel::Type::Misc || type == Channel::Type::Kick)
    {
        // not a normal twitch channel, the url opened by the button will be invalid, so hide the button
        this->ui_.usercardLabel->hide();
        this->ui_.userlogsLabel->hide();
    }
    else
    {
        this->ui_.usercardLabel->setText(uiText("&Usercard"));
        this->ui_.usercardLabel->show();
        this->ui_.userlogsLabel->show();
    }

    this->updateBadgesButton();
    this->updateObservedBadges();
}

void UserInfoPopup::setYouTubeContext()
{
    this->forceYouTube_ = true;
}

void UserInfoPopup::openUserChannelAction(const QString &userName,
                                          MessagePlatform platform,
                                          const QString &channelName,
                                          const QString &channelId)
{
    QString url;
    switch (platform)
    {
        case MessagePlatform::YouTube:
            if (channelId.isEmpty())
            {
                return;
            }
            url = u"https://www.youtube.com/channel/" % channelId;
            break;
        case MessagePlatform::Kick:
            url = u"https://kick.com/" % KickApi::slugify(userName);
            break;
        case MessagePlatform::AnyOrTwitch:
            if (channelName.isEmpty())
            {
                return;
            }
            url = u"https://www.twitch.tv/popout/" % channelName %
                  u"/viewercard/" % userName;
            break;
    }

    if (url.isEmpty())
    {
        return;
    }

    if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
    {
        openLinkIncognito(url);
    }
    else
    {
        QDesktopServices::openUrl(QUrl(url));
    }
}

void UserInfoPopup::updateYouTubeUserData()
{
    this->youtubeChannelId_ = YouTubeChannel::channelIdForDisplayName(
        this->underlyingChannel_, this->userName_);

    QString labelName = this->userName_;
    if (this->userName_.startsWith(u"UC") &&
        !this->youtubeChannelId_.isEmpty() && this->underlyingChannel_)
    {
        for (const auto &message :
             this->underlyingChannel_->getMessageSnapshot())
        {
            if (message->loginName == this->youtubeChannelId_ &&
                !message->displayName.isEmpty())
            {
                labelName = message->displayName;
                break;
            }
        }
    }
    this->ui_.nameLabel->setText(labelName);

    if (this->youtubeChannelId_.isEmpty())
    {
        this->ui_.userIDLabel->setText(uiText(u"ID ") % uiText(TEXT_UNAVAILABLE));
        this->ui_.userIDLabel->setProperty("copy-text",
                                           uiText(TEXT_UNAVAILABLE));
    }
    else
    {
        this->ui_.userIDLabel->setText(uiText(TEXT_USER_ID) % this->youtubeChannelId_);
        this->ui_.userIDLabel->setProperty("copy-text",
                                           this->youtubeChannelId_);
    }

    if (this->ui_.pronounsLabel)
    {
        this->ui_.pronounsLabel->hide();
    }
    this->ui_.followerCountLabel->hide();
    this->ui_.createdDateLabel->hide();
    this->ui_.followageRow->hide();
    this->ui_.subageRow->hide();
    this->hideUsercardSubGiftRow();
    this->ui_.chatterCountLabel->hide();
    this->ui_.lastLiveLabel->hide();
    this->ui_.sevenTVUserLabel->hide();
    this->ui_.rolesLabel->hide();
    this->ui_.notesAdd->hide();
    this->ui_.notesPreview->setVisible(false);
    this->ui_.block->hide();
    if (this->ui_.followButton)
    {
        this->ui_.followButton->hide();
    }

    this->loadYouTubeAvatar(
        YouTubeChannel::authorPhotoFor(this->youtubeChannelId_));
}

void UserInfoPopup::loadYouTubeAvatar(const QString &url)
{
    const auto generation = this->userDataRequestGeneration_;
    if (url.isEmpty())
    {
        this->ui_.avatarButton->setPixmap(QPixmap());
        return;
    }

    this->avatarUrl_ = url;

    auto filename = getApp()->getPaths().cacheDirectory() + "/" + hashUrl(url);
    QFile cacheFile(filename);
    if (cacheFile.exists() && cacheFile.open(QIODevice::ReadOnly))
    {
        QPixmap avatar;
        if (avatar.loadFromData(cacheFile.readAll()))
        {
            this->ui_.avatarButton->setPixmap(avatar);
            this->avatarPixmap_ = std::move(avatar);
            return;
        }
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, "Chatterino");
    static auto *manager = createNetworkAccessManager();
    auto *reply = manager->get(req);
    setNetworkReplyTimeout(reply, 15000);
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     &QObject::deleteLater);
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, filename, generation] {
                         if (generation != this->userDataRequestGeneration_)
                         {
                             return;
                         }
                         if (reply->error() == QNetworkReply::NoError)
                         {
                             const auto data = reply->readAll();
                             QPixmap avatar;
                             if (!avatar.loadFromData(data))
                             {
                                 return;
                             }
                             this->ui_.avatarButton->setPixmap(avatar);
                             this->saveCacheAvatar(data, filename);
                             this->avatarPixmap_ = std::move(avatar);
                         }
                         else
                         {
                             this->ui_.avatarButton->setPixmap(QPixmap());
                         }
                     });
}

void UserInfoPopup::updateYouTubeLatestMessages()
{
    const QString needle =
        YouTubeChannel::normalizeDisplayName(this->userName_);
    auto matches = [this, needle](const MessagePtr &message) {
        if (!this->youtubeChannelId_.isEmpty() &&
            message->loginName.compare(this->youtubeChannelId_,
                                       Qt::CaseInsensitive) == 0)
        {
            return true;
        }
        if (needle.isEmpty())
        {
            return false;
        }
        const auto name =
            YouTubeChannel::normalizeDisplayName(message->displayName);
        return name == needle || name.startsWith(needle);
    };

    auto channelPtr = std::make_shared<Channel>(
        this->underlyingChannel_ ? this->underlyingChannel_->getName()
                                 : QString(),
        Channel::Type::None);
    if (this->underlyingChannel_)
    {
        for (const auto &message :
             this->underlyingChannel_->getMessageSnapshot())
        {
            if (matches(message))
            {
                channelPtr->addMessage(message, MessageContext::Repost);
            }
        }
    }

    this->usercardMessagesChannel_ = channelPtr;
    this->ui_.latestMessages->setChannel(channelPtr);
    this->ui_.latestMessages->setSourceChannel(this->underlyingChannel_);

    this->updateUsercardMessagesVisibility();

    if (this->underlyingChannel_)
    {
        this->refreshConnection_ =
            std::make_unique<pajlada::Signals::ScopedConnection>(
                this->underlyingChannel_->messageAppended.connect(
                    [this, matches](auto message, auto) {
                        if (!matches(message))
                        {
                            return;
                        }
                        if (this->usercardMessagesChannel_)
                        {
                            this->usercardMessagesChannel_->addMessage(
                                message, MessageContext::Repost);
                            this->updateUsercardMessagesVisibility();
                        }
                    }));
    }
}

void UserInfoPopup::updateLatestMessages()
{
    this->usercardMessagesChannel_ = filterMessages(
        this->userName_, this->isKick_ ? QString{} : this->userId_,
        this->underlyingChannel_);
    this->ui_.latestMessages->setChannel(this->usercardMessagesChannel_);
    this->ui_.latestMessages->setSourceChannel(this->underlyingChannel_);
    if (!this->publicHistoryMessages_.empty())
    {
        this->rebuildPublicHistoryView(true);
    }

    this->updateUsercardMessagesVisibility();
    this->maybeStartUsercardMessageAutoLoad();
    this->scheduleHistoryViewportFill();

    this->refreshConnection_ =
        std::make_unique<pajlada::Signals::ScopedConnection>(
            this->underlyingChannel_->messageAppended.connect(
                [this](auto message, auto) {
                    if (this->updateTargetModerationStatusFromMessage(message))
                    {
                        this->userStateChanged_.invoke();
                    }

                    if (!checkMessageUserName(
                            this->userName_, message,
                            this->isKick_ ? QString{} : this->userId_,
                            this->underlyingChannel_
                                ? this->underlyingChannel_->getName()
                                : QString{}))
                    {
                        return;
                    }

                    if (this->usercardMessagesChannel_ &&
                        this->usercardMessagesChannel_->hasMessages())
                    {
                        this->usercardMessagesChannel_->addMessage(
                            message, MessageContext::Repost);
                        this->updateUsercardMessagesVisibility();
                    }
                    else
                    {
                        // The ChannelView is currently hidden, so manually refresh
                        // and display the latest messages
                        this->updateLatestMessages();
                    }
                }));
}

void UserInfoPopup::updateUsercardMessagesVisibility()
{
    if (this->activityRefreshTimer_ && !this->activityRefreshTimer_->isActive())
        this->activityRefreshTimer_->start();
    const bool hasMessages = this->usercardMessagesChannel_ &&
                             this->usercardMessagesChannel_->hasMessages();
    const bool hadMessages = this->ui_.latestMessages->isVisible();
    const bool hadNoMessagesLabel = this->ui_.noMessagesLabel->isVisible();
    const bool hadLoadMoreButton = this->ui_.loadMoreMessages != nullptr &&
                                   this->ui_.loadMoreMessages->isVisible();
    const auto previousNoMessagesText = this->ui_.noMessagesLabel->getText();
    const auto noMessagesText =
        this->usercardMessagesLoading_ || this->publicHistoryLoading_
            ? tr("Загрузка сообщений…")
        : this->publicHistoryStatus_.isEmpty()
            ? tr("В загруженном буфере сообщений нет")
            : this->publicHistoryStatus_;

    this->ui_.latestMessages->setVisible(hasMessages);
    this->ui_.noMessagesLabel->setText(noMessagesText);
    this->ui_.noMessagesLabel->setVisible(!hasMessages);
    this->updateLoadMoreMessagesButton();
    this->updatePublicHistoryButton();

    const bool hasLoadMoreButton = this->ui_.loadMoreMessages != nullptr &&
                                   this->ui_.loadMoreMessages->isVisible();
    if (hadMessages != hasMessages || hadNoMessagesLabel != !hasMessages ||
        hadLoadMoreButton != hasLoadMoreButton ||
        previousNoMessagesText != noMessagesText)
    {
        // Preserve the user's chosen responsive window size during loading and
        // empty/non-empty transitions; only its child layout needs updating.
        this->ui_.messagesPage->updateGeometry();
    }
}

void UserInfoPopup::refreshUsercardActivity()
{
    if (!this->ui_.activityCount || !this->ui_.activityLatest)
        return;
    const auto channelName = this->underlyingChannel_ ? this->underlyingChannel_->getName() : QString();
    const auto activity = this->usercardMessagesChannel_
        ? summarizeUsercardActivity(this->usercardMessagesChannel_->getMessageSnapshot(), this->userId_, channelName)
        : UsercardActivity{};
    const auto count = this->userId_.isEmpty() ? QStringLiteral("—") :
        QLocale().toString(qulonglong(activity.messageCount));
    if (this->ui_.activityCount->text() != count)
        this->ui_.activityCount->setText(count);
    QString latest = QStringLiteral("—");
    if (activity.latestMessage.isValid())
    {
        // Use a stable local timestamp, with no timer or periodic history scan.
        const auto time = activity.latestMessage.toLocalTime();
        latest = time.date() == QDate::currentDate() ? time.toString("HH:mm") : time.toString("dd.MM.yy HH:mm");
    }
    if (this->ui_.activityLatest->text() != latest)
        this->ui_.activityLatest->setText(latest);
}

bool UserInfoPopup::canUsePublicHistory() const
{
    return (this->offlinePreview_ ? this->offlinePublicHistoryAllowed_
                                 : getSettings()->usercardPublicLogs.getValue()) &&
           !this->isKick_ && !this->isYouTube_ && !this->userId_.isEmpty() &&
           this->underlyingChannel_ &&
           !this->underlyingChannel_->getName().isEmpty();
}

void UserInfoPopup::resetPublicHistory()
{
    this->publicHistoryCursor_ = {};
    this->publicHistoryMessages_.clear();
    this->publicHistoryIncomplete_.clear();
    this->publicHistoryColor_.clear();
    this->publicHistoryDisplayName_.clear();
    this->publicHistoryTimestamps_.clear();
    this->publicHistoryStatus_.clear();
    this->publicHistoryStarted_ = false;
    this->publicHistoryLoading_ = false;
    this->publicHistoryComplete_ = false;
    this->publicHistoryPartialRetried_ = false;
    this->publicHistoryPreserveAnchor_ = false;
    this->historyViewportFillBlocked_ = false;
    this->historyViewportFillQueued_ = false;
    this->publicHistoryWindowLimited_ = false;
    this->updatePublicHistoryButton();
}

bool UserInfoPopup::hasCachedOlderHistory() const
{
    if (!this->usercardMessagesChannel_)
    {
        return false;
    }
    MessagePtr oldest;
    for (const auto &message :
         this->usercardMessagesChannel_->getMessageSnapshot())
    {
        if (message->userID == this->userId_)
        {
            oldest = message;
            break;
        }
    }
    return oldest && std::any_of(this->publicHistoryMessages_.begin(),
                                 this->publicHistoryMessages_.end(),
                                 [&](const auto &message) {
                                     return message->serverReceivedTime <
                                                oldest->serverReceivedTime ||
                                            (message->serverReceivedTime ==
                                                 oldest->serverReceivedTime &&
                                             message->id < oldest->id);
                                 });
}

void UserInfoPopup::scheduleHistoryViewportFill()
{
    if (this->historyViewportFillQueued_ || this->historyViewportFillBlocked_ ||
        this->publicHistoryLoading_ || !this->ui_.latestMessages ||
        (!this->publicHistoryStarted_ && !this->offlineHistoryAutoFill_) ||
        (this->offlinePreview_ && !this->offlineHistoryAutoFill_) ||
        !this->canUsePublicHistory())
        return;
    const auto generation = this->usercardMessagesRequestGeneration_;
    this->historyViewportFillQueued_ = true;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != this->usercardMessagesRequestGeneration_)
            return;
        this->historyViewportFillQueued_ = false;
        if (this->publicHistoryLoading_ || this->historyViewportFillBlocked_ ||
            !this->ui_.messagesPage->isVisible() || this->publicHistoryWindowLimited_)
            return;
        // A freshly replaced ChannelView can still carry the previous scrollbar
        // state until Qt delivers its layout event. Use the actual row heights.
        auto *view = this->ui_.latestMessages;
        if (this->usercardMessagesChannel_ && this->usercardMessagesChannel_->hasMessages())
        {
            view->performLayout(false, true);
            if (!view->getScrollBar().isHidden())
                return;
        }
        this->requestPublicHistory(HistoryRequest::FillViewport);
    });
}

void UserInfoPopup::requestPublicHistory(HistoryRequest reason)
{
    const bool userInitiated = reason == HistoryRequest::Manual;
    const auto login = this->userName_.isEmpty() ? this->loginHint_ : this->userName_;
    if (!this->canUsePublicHistory() || this->publicHistoryLoading_ ||
        login.isEmpty() || (!userInitiated && !this->publicHistoryStarted_ &&
                            !this->offlineHistoryAutoFill_))
    {
        return;
    }
    if (userInitiated && this->hasCachedOlderHistory())
    {
        this->rebuildPublicHistoryView(true);
        this->updateUsercardMessagesVisibility();
        return;
    }
    if (this->publicHistoryComplete_)
    {
        return;
    }

    this->publicHistoryStarted_ = true;
    if (userInitiated)
    {
        this->historyViewportFillBlocked_ = false;
    }
    this->publicHistoryLoading_ = true;
    this->publicHistoryPreserveAnchor_ = userInitiated;
    this->publicHistoryStatus_ = tr("Поиск в публичных логах…");
    this->updatePublicHistoryButton();
    this->updateUsercardMessagesVisibility();

    const auto generation = this->usercardMessagesRequestGeneration_;
    publiclogs::requestPage(
        this->userId_, login, this->underlyingChannel_->getName(),
        this->publicHistoryCursor_, this,
        [this, generation](const QJsonObject &page) {
            if (generation == this->usercardMessagesRequestGeneration_ &&
                this->publicHistoryLoading_)
            {
                this->applyPublicHistory(page);
            }
        });
}

void UserInfoPopup::applyPublicHistory(const QJsonObject &data)
{
    if (!this->underlyingChannel_ ||
        data.value("identity").toObject().value("user_id").toString() !=
            this->userId_ ||
        data.value("channel").toString().compare(
            this->underlyingChannel_->getName(), Qt::CaseInsensitive) != 0)
    {
        return;
    }
    this->publicHistoryLoading_ = false;
    const auto status = data.value("status").toString();
    const bool successful =
        status == u"found" || status == u"empty" || status == u"end";
    if (successful)
    {
        auto *renderChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
        QSet<QString> ids;
        for (const auto &message :
             this->underlyingChannel_->getMessageSnapshot())
        {
            if (message->userID == this->userId_ && !message->id.isEmpty())
            {
                ids.insert(message->id);
            }
        }
        for (const auto &message : this->publicHistoryMessages_)
        {
            ids.insert(message->id);
        }
        int added = 0;
        for (const auto &value : data.value("rows").toArray())
        {
            const auto row = value.toObject();
            const auto id = row.value("id").toString();
            if (row.value("user_id").toString() != this->userId_ ||
                row.value("channel").toString().compare(
                    this->underlyingChannel_->getName(), Qt::CaseInsensitive) !=
                    0)
            {
                continue;
            }
            this->publicHistoryTimestamps_.insert(id, row.value("time_utc").toString());
            if (!id.isEmpty() && ids.contains(id))
            {
                continue;
            }
            GqlUsercardMessage message;
            message.id = id;
            message.senderId = this->userId_;
            message.senderLogin = row.value("user").toString();
            message.senderDisplayName = row.value("display_name").toString();
            message.sentAt = row.value("time_utc").toString();
            message.text = row.value("text").toString();
            message.isFirstMessage = row.value("first_msg").toBool();
            message.senderColor = row.value("color").toString();
            message.senderBadges = row.value("badges").toString();
            if (message.senderLogin.isEmpty() ||
                !parseIvrTimestamp(message.sentAt).isValid())
            {
                continue;
            }
            if (message.senderColor.isEmpty() ||
                message.senderDisplayName.trimmed().isEmpty() ||
                (message.senderDisplayName == message.senderLogin &&
                 message.senderDisplayName == message.senderDisplayName.toLower()))
            {
                this->publicHistoryIncomplete_.insert(id, message);
            }
            if (message.senderColor.isEmpty())
            {
                this->publicHistoryColor_ = this->knownChatColor();
                message.senderColor = this->publicHistoryColor_;
            }
            this->publicHistoryMessages_.push_back(makeUsercardModLogMessage(
                message, renderChannel, this->underlyingChannel_->getName(),
                this->userId_, this->historyProfileDisplayName_));
            ids.insert(id);
            ++added;
        }
        auto &cursor = this->publicHistoryCursor_;
        const auto oldCursor = cursor;
        const bool partial =
            data.value("error").toString() == u"partial_source";
        const bool retryPartial = partial && !this->publicHistoryPartialRetried_;
        if (retryPartial)
        {
            // keep the cursor: the next request covers this range again and
            // picks up what the late mirror holds (rows already shown are
            // skipped)
            this->publicHistoryPartialRetried_ = true;
        }
        else
        {
            if (!partial || data.value("more").toBool())
                cursor = data.value("next_cursor").toObject();
            this->publicHistoryPartialRetried_ = false;
        }
        // A duplicate-only page still advances its own cursor. It is never
        // mistaken for the end of the other source's history.
        const bool complete =
            !data.value("more").toBool() &&
            data.value("error").toString() != u"partial_source";
        // A partial page with a next cursor can still fill the viewport. Give
        // the late mirror its one retry. Without an older cursor leave the
        // displayed rows in place for a later manual retry.
        const auto beforeTime = parseIvrTimestamp(oldCursor.value("time_utc").toString());
        const auto nextTime = parseIvrTimestamp(cursor.value("time_utc").toString());
        const bool advanced = nextTime.isValid() &&
            (oldCursor.isEmpty() || nextTime < beforeTime ||
             (nextTime == beforeTime && cursor.value("id").toString() < oldCursor.value("id").toString()));
        this->historyViewportFillBlocked_ = !complete && !advanced &&
            !(retryPartial && data.value("more").toBool());
        this->publicHistoryComplete_ = complete;
        const auto rows = data.value("rows").toArray().size();
        this->publicHistoryStatus_ =
            data.value("error").toString() == u"partial_source"
                ? tr("История частичная: один из источников недоступен")
            : tr("Публичные логи: получено %1 сообщений").arg(rows);
        if (added)
        {
            this->rebuildPublicHistoryView(
                this->publicHistoryPreserveAnchor_);
        }
    }
    else
    {
        this->publicHistoryStatus_ =
            status == u"refused" ? tr("Публичные логи закрыты для пользователя")
            : status == u"disabled"
                ? tr("Публичные логи сейчас отключены")
                : tr("История недоступна; повторите запрос кнопкой «История»");
        // Unknown coverage and a failed source do not prove a global end.
        this->historyViewportFillBlocked_ = true;
    }
    this->updatePublicHistoryButton();
    this->updateUsercardMessagesVisibility();
    this->scheduleHistoryViewportFill();
}

QString UserInfoPopup::knownChatColor() const
{
    if (this->ui_.userColorRow)
    {
        const QColor own(
            this->ui_.userColorRow->property("swatch-color").toString());
        if (own.isValid())
        {
            return own.name(QColor::HexRgb);
        }
    }
    if (!this->underlyingChannel_)
    {
        return {};
    }
    const auto snapshot = this->underlyingChannel_->getMessageSnapshot();
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        if ((*it)->userID == this->userId_ && (*it)->usernameColor.isValid() &&
            !(*it)->flags.has(MessageFlag::System))
        {
            return (*it)->usernameColor.name(QColor::HexRgb);
        }
    }
    if (auto *chatters =
            dynamic_cast<ChannelChatters *>(this->underlyingChannel_.get()))
    {
        const auto color = chatters->getUserColor(this->userName_);
        if (color.isValid())
        {
            return color.name(QColor::HexRgb);
        }
    }
    return {};
}

void UserInfoPopup::refreshPublicHistoryColors()
{
    if (this->publicHistoryIncomplete_.isEmpty() || !this->underlyingChannel_)
    {
        return;
    }
    const auto color = this->knownChatColor();
    if (color == this->publicHistoryColor_ &&
        this->historyProfileDisplayName_ == this->publicHistoryDisplayName_)
    {
        return;
    }
    this->publicHistoryColor_ = color;
    this->publicHistoryDisplayName_ = this->historyProfileDisplayName_;
    auto *renderChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    for (auto &message : this->publicHistoryMessages_)
    {
        auto source = this->publicHistoryIncomplete_.constFind(message->id);
        if (source == this->publicHistoryIncomplete_.cend())
        {
            continue;
        }
        auto colored = source.value();
        if (colored.senderColor.isEmpty())
        {
            colored.senderColor = color;
        }
        message = makeUsercardModLogMessage(colored, renderChannel,
                                            this->underlyingChannel_->getName(),
                                            this->userId_,
                                            this->historyProfileDisplayName_);
    }
    this->rebuildPublicHistoryView(true);
}

void UserInfoPopup::rebuildPublicHistoryView(bool preserveAnchor)
{
    if (!this->underlyingChannel_ || this->userId_.isEmpty())
    {
        return;
    }
    QString anchor;
    qreal anchorOffset = 0;
    if (preserveAnchor && this->usercardMessagesChannel_)
    {
        const auto before =
            this->usercardMessagesChannel_->getMessageSnapshot();
        const auto desired = std::max(
            0., this->ui_.latestMessages->getScrollBar().getDesiredValue());
        const auto index = size_t(desired);
        for (auto i = std::min(index, before.size()); i < before.size(); ++i)
        {
            if (!before[i]->id.isEmpty())
            {
                anchor = before[i]->id;
                anchorOffset = qreal(i) - desired;
                break;
            }
        }
    }
    QHash<QString, MessagePtr> byId;
    const auto channelName = this->underlyingChannel_->getName();
    auto add = [&](const MessagePtr &message, bool live) {
        // Live rows also include bans/timeouts/deletions about this person
        // (same rule as the plain list); Public log pages are their own messages.
        if (live ? !checkMessageUserName(this->userName_, message,
                                         this->userId_, channelName)
                 : message->userID != this->userId_)
        {
            return;
        }
        const auto key =
            message->id.isEmpty()
                ? message->serverReceivedTime.toString(Qt::ISODateWithMs) +
                      QChar('|') + message->messageText
                : message->id;
        byId.insert(key, message);
    };
    for (const auto &message : this->publicHistoryMessages_)
    {
        add(message, false);
    }
    // Keep the full original message (emotes/replies/badges) whenever it is
    // already present in the live buffer; normalized log text cannot
    // replace that richer object.
    for (const auto &message : this->underlyingChannel_->getMessageSnapshot())
    {
        add(message, true);
    }
    std::vector<MessagePtr> messages;
    messages.reserve(size_t(byId.size()));
    for (const auto &message : byId)
    {
        messages.push_back(message);
    }
    std::sort(messages.begin(), messages.end(),
              [](const auto &a, const auto &b) {
                  return a->serverReceivedTime == b->serverReceivedTime
                             ? a->id < b->id
                             : a->serverReceivedTime < b->serverReceivedTime;
              });
    const auto capacity = size_t(std::max(
        2, std::min(getSettings()->scrollbackSplitLimit.getValue(),
                    getSettings()->scrollbackUsercardLimit.getValue())));
    std::vector<MessagePtr> rendered;
    QDate previousDate;
    for (const auto &message : messages)
    {
        const auto date = message->serverReceivedTime.toLocalTime().date();
        if (date.isValid() && date != previousDate)
        {
            MessageBuilder separator;
            separator->flags.set(MessageFlag::System, MessageFlag::Centered,
                                 MessageFlag::DoNotLog,
                                 MessageFlag::DoNotTriggerNotification);
            const auto text =
                date == QDate::currentDate()
                    ? tr("сегодня")
                    : QLocale(QLocale::Russian).toString(date, "d MMMM");
            separator->messageText = text;
            separator->serverReceivedTime = message->serverReceivedTime;
            separator.emplace<TextElement>(text, MessageElementFlag::Text,
                                           MessageColor::System);
            rendered.push_back(separator.release());
            previousDate = date;
        }
        rendered.push_back(message);
    }
    this->publicHistoryWindowLimited_ = rendered.size() > capacity;
    if (this->publicHistoryWindowLimited_)
    {
        size_t start = rendered.size() - capacity;
        if (preserveAnchor && !anchor.isEmpty())
        {
            const auto found = std::find_if(rendered.begin(), rendered.end(),
                                            [&](const auto &message) {
                                                return message->id == anchor;
                                            });
            if (found != rendered.end())
            {
                const auto index = size_t(found - rendered.begin());
                start = std::min(index > 50 ? index - 50 : 0,
                                 rendered.size() - capacity);
            }
        }
        rendered = std::vector<MessagePtr>(rendered.begin() + start,
                                           rendered.begin() + start + capacity);
    }
    auto channel = std::make_shared<Channel>(
        this->underlyingChannel_->getName(), Channel::Type::None);
    for (const auto &message : rendered)
    {
        channel->addMessage(message, MessageContext::Repost);
    }
    this->usercardMessagesChannel_ = channel;
    this->ui_.latestMessages->setChannel(channel);
    this->ui_.latestMessages->setSourceChannel(this->underlyingChannel_);
    if (!anchor.isEmpty())
    {
        const auto generation = this->usercardMessagesRequestGeneration_;
        QTimer::singleShot(0, this, [this, anchor, anchorOffset, generation] {
            if (generation == this->usercardMessagesRequestGeneration_)
            {
                const auto messages =
                    this->usercardMessagesChannel_->getMessageSnapshot();
                const auto found = std::find_if(
                    messages.begin(), messages.end(), [&](const auto &message) {
                        return message->id == anchor;
                    });
                if (found != messages.end())
                {
                    auto &scrollbar = this->ui_.latestMessages->getScrollBar();
                    scrollbar.setDesiredValue(scrollbar.getMinimum() +
                                              qreal(found - messages.begin()) -
                                              anchorOffset);
                }
            }
        });
    }
    // Keep memory bounded independently of remote history length. Cursors
    // remain per-source and are never derived from this visible window.
    if (this->publicHistoryMessages_.size() > capacity * 4)
    {
        std::sort(this->publicHistoryMessages_.begin(),
                  this->publicHistoryMessages_.end(),
                  [](const auto &a, const auto &b) {
                      return a->serverReceivedTime < b->serverReceivedTime;
                  });
        auto visibleTime = rendered.empty()
                               ? QDateTime{}
                               : rendered.front()->serverReceivedTime;
        const auto firstVisible =
            std::lower_bound(this->publicHistoryMessages_.begin(),
                             this->publicHistoryMessages_.end(), visibleTime,
                             [](const auto &message, const auto &time) {
                                 return message->serverReceivedTime < time;
                             });
        const auto visibleIndex =
            size_t(firstVisible - this->publicHistoryMessages_.begin());
        const auto start =
            std::min(visibleIndex > capacity ? visibleIndex - capacity : 0,
                     this->publicHistoryMessages_.size() - capacity * 4);
        this->publicHistoryMessages_ = std::vector<MessagePtr>(
            this->publicHistoryMessages_.begin() + start,
            this->publicHistoryMessages_.begin() + start + capacity * 4);
        if (start != 0)
        {
            const auto &oldest = this->publicHistoryMessages_.front();
            this->publicHistoryCursor_ = {
                {"time_utc", this->publicHistoryTimestamps_.value(oldest->id)},
                {"id", oldest->id},
            };
            this->publicHistoryComplete_ = false;
        }
        QSet<QString> retained;
        for (const auto &message : this->publicHistoryMessages_)
            retained.insert(message->id);
        for (const auto &message :
             this->underlyingChannel_->getMessageSnapshot())
            retained.insert(message->id);
        for (auto it = this->publicHistoryTimestamps_.begin();
             it != this->publicHistoryTimestamps_.end();)
        {
            if (!retained.contains(it.key()))
            {
                this->publicHistoryIncomplete_.remove(it.key());
                it = this->publicHistoryTimestamps_.erase(it);
            }
            else
                ++it;
        }
    }
}

void UserInfoPopup::updatePublicHistoryButton()
{
    if (!this->ui_.historyButton)
    {
        return;
    }
    const bool available = this->canUsePublicHistory();
    this->ui_.historyButton->setEnabled(
        !this->publicHistoryLoading_ &&
        (available ||
         (!this->offlinePreview_ && this->canLoadMoreUsercardMessages())));
    this->ui_.historyButton->setText(
        this->publicHistoryLoading_ ? tr("Загрузка…") : tr("История"));
    this->ui_.historyButton->setToolTip(
        (!this->publicHistoryStatus_.isEmpty() ? this->publicHistoryStatus_
         : available ? tr("Публичные логи чата. Прокрутите вверх, чтобы "
                          "загрузить ещё")
                     : tr("Включите публичные логи в настройках")) +
        (this->publicHistoryWindowLimited_
             ? tr(" · Буфер карточки ограничен; старые строки доступны "
                  "прокруткой")
             : QString{}));
}

void UserInfoPopup::resetUsercardMessageLoader()
{
    this->resetPublicHistory();
    ++this->usercardMessagesRequestGeneration_;
    this->usercardMessagesCursor_.clear();
    this->usercardMessagesError_.clear();
    this->usercardMessagesLoading_ = false;
    this->usercardMessagesHasNextPage_ = true;
    this->usercardMessagesLazyLoadEnabled_ =
        getSettings()->alwaysLoadMoreUsercardMessages;
    this->usercardMessagesChannel_.reset();
    this->refreshUsercardActivity();
    this->updateLoadMoreMessagesButton();
}

bool UserInfoPopup::canLoadMoreUsercardMessages() const
{
    if (this->isKick_ || this->userName_.isEmpty() || this->userId_.isEmpty() ||
        !this->underlyingChannel_)
    {
        return false;
    }

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    const auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName());
    if (!auth.hasToken())
    {
        return false;
    }

    return !auth.legacy || twitchChannel->hasModRights();
}

void UserInfoPopup::updateLoadMoreMessagesButton()
{
    auto *button = this->ui_.loadMoreMessages;
    if (button == nullptr)
    {
        return;
    }

    const bool canLoad = getSettings()->showUsercardLoadMoreMessagesButton &&
                         this->canLoadMoreUsercardMessages() &&
                         this->usercardMessagesHasNextPage_ &&
                         !this->usercardMessagesLazyLoadEnabled_;
    button->setVisible(canLoad);
    button->setEnabled(canLoad && !this->usercardMessagesLoading_);

    if (this->usercardMessagesLoading_)
    {
        button->setText(uiText("Loading messages..."));
        button->setToolTip(uiText("Loading older messages from Twitch mod logs"));
    }
    else
    {
        button->setText(uiText("Load more messages"));
        button->setToolTip(this->usercardMessagesError_.isEmpty()
                               ? uiText("Load older messages from Twitch mod logs")
                               : uiText("Couldn't load messages. Try again."));
    }
}

void UserInfoPopup::maybeStartUsercardMessageAutoLoad()
{
    if (!getSettings()->alwaysLoadMoreUsercardMessages ||
        !this->canLoadMoreUsercardMessages() ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    this->usercardMessagesLazyLoadEnabled_ = true;
    this->updateLoadMoreMessagesButton();

    const bool hasMessages = this->usercardMessagesChannel_ &&
                             this->usercardMessagesChannel_->hasMessages();
    if (!hasMessages)
    {
        this->requestMoreUsercardMessages(false);
        return;
    }

    this->maybeLoadMoreUsercardMessagesFromScroll();
}

void UserInfoPopup::requestMoreUsercardMessages(bool enableLazyLoadOnSuccess)
{
    if (this->usercardMessagesLoading_ ||
        !this->canLoadMoreUsercardMessages() ||
        !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    this->usercardMessagesError_.clear();
    this->usercardMessagesLoading_ = true;
    this->updateUsercardMessagesVisibility();
    this->fetchMoreUsercardMessages(2, enableLazyLoadOnSuccess);
}

void UserInfoPopup::maybeLoadMoreUsercardMessagesFromScroll()
{
    if (!this->usercardMessagesLazyLoadEnabled_ ||
        this->usercardMessagesLoading_ || !this->usercardMessagesHasNextPage_)
    {
        return;
    }

    auto &scrollbar = this->ui_.latestMessages->getScrollBar();
    if (scrollbar.getDesiredValue() >
        scrollbar.getMinimum() + usercardMessagePreloadDistance(scrollbar))
    {
        return;
    }

    this->requestMoreUsercardMessages(false);
}

void UserInfoPopup::fetchMoreUsercardMessages(int emptyPageSkipsLeft,
                                              bool enableLazyLoadOnSuccess)
{
    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr)
    {
        this->usercardMessagesLazyLoadEnabled_ = false;
        this->usercardMessagesLoading_ = false;
        this->updateUsercardMessagesVisibility();
        return;
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveModerationToken(
        twitchChannel->roomId(), twitchChannel->getName(), &authError);
    if (!auth.hasToken() || (auth.legacy && !twitchChannel->hasModRights()))
    {
        this->usercardMessagesError_ =
            authError.isEmpty()
                ? uiText(QStringLiteral("No saved Chatterino Noctis moderator login found."))
                : authError;
        this->usercardMessagesLazyLoadEnabled_ = false;
        this->usercardMessagesLoading_ = false;
        this->updateUsercardMessagesVisibility();
        return;
    }

    const auto generation = this->usercardMessagesRequestGeneration_;
    const auto channelId = twitchChannel->roomId();
    const auto channelName = twitchChannel->getName();
    const auto targetUserId = this->userId_;
    const auto cursor = this->usercardMessagesCursor_;
    const auto oldestLoadedMessage =
        oldestUsercardMessageTime(this->usercardMessagesChannel_);
    const QPointer<UserInfoPopup> self(this);

    TwitchGql::getUsercardMessagesBySender(
        channelId, targetUserId, cursor, auth.token,
        [self, generation, targetUserId, channelName, emptyPageSkipsLeft,
         oldestLoadedMessage,
         enableLazyLoadOnSuccess](GqlUsercardMessagePage page) mutable {
            if (!self ||
                generation != self->usercardMessagesRequestGeneration_ ||
                self->userId_ != targetUserId)
            {
                return;
            }

            self->usercardMessagesCursor_ = page.nextCursor;
            self->usercardMessagesHasNextPage_ =
                page.hasNextPage && !page.nextCursor.isEmpty();

            if (!self->usercardMessagesChannel_)
            {
                self->usercardMessagesChannel_ =
                    std::make_shared<TwitchChannel>(channelName);
                self->ui_.latestMessages->setChannel(
                    self->usercardMessagesChannel_);
                self->ui_.latestMessages->setSourceChannel(
                    self->underlyingChannel_);
            }

            std::vector<MessagePtr> messages;
            messages.reserve(static_cast<size_t>(page.messages.size()));
            auto *renderChannel =
                dynamic_cast<TwitchChannel *>(self->underlyingChannel_.get());
            for (auto it = page.messages.crbegin(); it != page.messages.crend();
                 ++it)
            {
                const auto sentAt = parseIvrTimestamp(it->sentAt);
                if (oldestLoadedMessage.isValid() && sentAt.isValid() &&
                    sentAt >= oldestLoadedMessage)
                {
                    continue;
                }

                if (self->usercardMessagesChannel_->findMessageByID(it->id))
                {
                    continue;
                }

                messages.push_back(makeUsercardModLogMessage(
                    *it, renderChannel, channelName, targetUserId,
                    self->historyProfileDisplayName_));
            }

            if (!messages.empty())
            {
                self->usercardMessagesChannel_->addMessagesAtStart(messages);
                if (enableLazyLoadOnSuccess)
                {
                    self->usercardMessagesLazyLoadEnabled_ = true;
                }
                self->usercardMessagesLoading_ = false;
                self->usercardMessagesError_.clear();
                self->updateUsercardMessagesVisibility();
                QTimer::singleShot(0, self.data(), [self] {
                    if (self)
                    {
                        self->maybeLoadMoreUsercardMessagesFromScroll();
                    }
                });
                return;
            }

            if (self->usercardMessagesHasNextPage_ && emptyPageSkipsLeft > 0)
            {
                self->fetchMoreUsercardMessages(emptyPageSkipsLeft - 1,
                                                enableLazyLoadOnSuccess);
                return;
            }

            self->usercardMessagesLoading_ = false;
            self->updateUsercardMessagesVisibility();
        },
        [self, generation](const QString &error) {
            if (!self || generation != self->usercardMessagesRequestGeneration_)
            {
                return;
            }

            qCWarning(chatterinoWidget)
                << "Failed to load usercard messages:" << error;
            self->usercardMessagesError_ = error;
            self->usercardMessagesLazyLoadEnabled_ = false;
            self->usercardMessagesLoading_ = false;
            self->updateUsercardMessagesVisibility();
        });
}

void UserInfoPopup::updateUserData()
{
    this->ui_.userlogsLabel->setVisible(true);

    std::weak_ptr<bool> hack = this->lifetimeHack_;
    const auto requestGeneration = ++this->userDataRequestGeneration_;
    const auto expectedUserId = this->userId_;
    const auto isCurrentRequest = [this, hack, requestGeneration] {
        return hack.lock() &&
               requestGeneration == this->userDataRequestGeneration_;
    };
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();

    const auto onUserFetchFailed = [this, isCurrentRequest] {
        if (!isCurrentRequest())
        {
            return;
        }

        // this can occur when the account doesn't exist.
        if (getSettings()->showUsercardFollowerCount)
        {
            this->ui_.followerCountLabel->setText(
                uiText(TEXT_FOLLOWERS).arg(uiText(TEXT_UNAVAILABLE)));
            this->ui_.followerCountLabel->setVisible(true);
        }
        if (getSettings()->showUsercardCreatedDate)
        {
            this->ui_.createdDateLabel->setText(
                uiText(TEXT_CREATED).arg(uiText(TEXT_UNAVAILABLE)));
            this->ui_.createdDateLabel->setVisible(true);
        }

        this->ui_.nameLabel->setText(this->userName_.isEmpty()
                                         ? this->loginHint_
                                         : this->userName_);

        this->ui_.userIDLabel->setText(uiText(TEXT_USER_ID) %
                                       (this->userId_.isEmpty()
                                            ? uiText(TEXT_UNAVAILABLE)
                                            : this->userId_));
        this->ui_.userIDLabel->setProperty("copy-text", this->userId_);

        if (getSettings()->showUsercardFollowage)
        {
            this->ui_.followageLabel->setText({});
        }
        if (getSettings()->showUsercardSubage)
        {
            this->ui_.subageLabel->setText({});
        }
        this->hideUsercardSubGiftRow();
        if (getSettings()->showUsercardChatterCount)
        {
            this->ui_.chatterCountLabel->setText(uiText("Chatters: ") %
                                                 uiText(TEXT_UNAVAILABLE));
        }
        if (getSettings()->showUsercardLastLive)
        {
            this->ui_.lastLiveLabel->setText(uiText("Last live: ") % uiText(TEXT_UNAVAILABLE));
        }
        if (getSettings()->showUsercardColor)
        {
            this->ui_.userColorRow->setProperty("copy-color", {});
            this->ui_.userColorRow->setProperty("swatch-color", {});
            this->ui_.userColorLabel->setText(uiText("Color: ") % uiText(TEXT_UNAVAILABLE));
            this->updateUsercardStatusIcons();
        }
        if (getSettings()->showUsercardStatus)
        {
            this->ui_.statusLabel->setText(uiText("Status: ") % uiText(TEXT_UNAVAILABLE));
        }
        if (getSettings()->showPronouns && this->ui_.pronounsLabel)
        {
            this->ui_.pronounsLabel->setText(
                uiText(TEXT_PRONOUNS).arg(uiText(TEXT_UNSPECIFIED)));
        }

        this->seventvUserRequestGeneration_++;
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();
    };
    const auto onUserFetched = [this, isCurrentRequest, expectedUserId,
                                onUserFetchFailed,
                                currentUser](const HelixUser &user) {
        if (!isCurrentRequest())
        {
            return;
        }
        if (user.id.isEmpty() ||
            (!expectedUserId.isEmpty() && user.id != expectedUserId))
        {
            onUserFetchFailed();
            return;
        }

        this->userId_ = user.id;
        this->ui_.rolesPage->setTarget(user.id, user.login);
        this->historyProfileDisplayName_ = user.displayName;
        this->refreshFollowingStatus(false);
        this->updateUsercardFollowButton();

        // Resolve the current canonical login of this ID. It can differ from
        // the saved message's login; never switch to that login's new owner.
        if (this->userName_ != user.login)
        {
            this->userName_ = user.login;
            this->ui_.nameLabel->setText(user.login);

            this->refreshTargetModerationStatus();
            this->userStateChanged_.invoke();
            // opened by ID: the paint is looked up by login, known only now
            this->refreshSeventvPaint();
        }
        // Upgrade a provisional name-only buffer to a strict UID-filtered one.
        this->updateLatestMessages();
        this->refreshPublicHistoryColors();
        this->updateQuickActions();

        // The public logs' newest page starts loading now, so the history
        // button shows it at once.
        if (getSettings()->usercardPublicLogs && !this->offlinePreview_ &&
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()) !=
                nullptr)
        {
            publiclogs::prefetch(this->userId_, this->userName_,
                                 this->underlyingChannel_->getName());
        }

        this->resetNameHistory();
        this->updateLoadMoreMessagesButton();
        this->maybeStartUsercardMessageAutoLoad();
        this->helixAvatarUrl_ = user.profileImageUrl;
        this->updateAvatarUrl();
        this->updateNotes();

        // copyable button for login name of users with a localized username
        if (user.displayName.toLower() != user.login)
        {
            this->ui_.localizedNameLabel->setText(user.displayName);
            this->ui_.localizedNameLabel->setProperty("copy-text",
                                                      user.displayName);
            this->ui_.localizedNameLabel->setVisible(true);
            this->ui_.localizedNameCopyButton->setVisible(true);
        }
        else
        {
            this->ui_.nameLabel->setText(user.displayName);
            this->ui_.nameLabel->setProperty("copy-text", user.displayName);
        }

        this->setWindowTitle(uiText(TEXT_TITLE).arg(
            user.displayName, this->underlyingChannel_->getName()));
        if (getSettings()->showUsercardCreatedDate)
        {
            auto createdAt =
                QDateTime::fromString(user.createdAt, Qt::ISODateWithMs);
            this->ui_.createdDateLabel->setText(
                formatAccountCreated(createdAt));
            this->ui_.createdDateLabel->setToolTip(
                formatLongFriendlyDuration(createdAt,
                                           QDateTime::currentDateTimeUtc()) +
                uiText(u" ago"_s));
            this->ui_.createdDateLabel->setMouseTracking(true);
            this->ui_.createdDateLabel->setVisible(true);
        }
        this->ui_.userIDLabel->setText(uiText(TEXT_USER_ID) % user.id);
        this->ui_.userIDLabel->setProperty("copy-text", user.id);

        this->loadCurrentAvatar();
        this->loadProfileBanner(user.offlineImageUrl);
        this->updateObservedBadges();
        this->requestEarnedBadges();

        if (getSettings()->showUsercardFollowerCount)
        {
            getHelix()->getChannelFollowers(
                user.id, {},
                [this, isCurrentRequest](const auto &followers) {
                    if (!isCurrentRequest() ||
                        !getSettings()->showUsercardFollowerCount)
                    {
                        return;
                    }
                    this->ui_.followerCountLabel->setText(
                        uiText(TEXT_FOLLOWERS).arg(localizeNumbers(followers.total)));
                    this->ui_.followerCountLabel->setVisible(true);
                },
                [this, isCurrentRequest](const auto &errorMessage) {
                    qCWarning(chatterinoTwitch)
                        << "Error getting followers:" << errorMessage;
                    if (!isCurrentRequest() ||
                        !getSettings()->showUsercardFollowerCount)
                    {
                        return;
                    }
                    this->ui_.followerCountLabel->setText(
                        uiText(TEXT_FOLLOWERS).arg(uiText(TEXT_UNAVAILABLE)));
                    this->ui_.followerCountLabel->setVisible(true);
                });
        }
        getHelix()->getStreamById(
            user.id,
            [this, isCurrentRequest](bool isLive, const auto &stream) {
                if (!isCurrentRequest())
                {
                    return;
                }

                if (isLive)
                {
                    this->isUserLive_ = true;
                    this->liveViewerCount_ = stream.viewerCount;
                }
                else
                {
                    this->isUserLive_ = false;
                    this->liveViewerCount_ = 0;
                }
                this->updateLiveIndicatorDisplay();
            },
            [id{user.id}]() {
                qCWarning(chatterinoWidget)
                    << "Failed to get stream for user ID" << id;
            },
            []() {});

        // get ignore state
        bool isIgnoring = currentUser->blockedUserIds().contains(user.id);

        // get ignoreHighlights state
        bool isIgnoringHighlights = false;
        const auto &vector = getSettings()->blacklistedUsers.raw();
        for (const auto &blockedUser : vector)
        {
            if (this->userName_ == blockedUser.getPattern())
            {
                isIgnoringHighlights = true;
                break;
            }
        }
        if (getSettings()->isBlacklistedUser(this->userName_) &&
            !isIgnoringHighlights)
        {
            this->ui_.ignoreHighlights->setToolTip(uiText("Name matched by regex"));
        }
        else
        {
            this->ui_.ignoreHighlights->setEnabled(true);
        }
        this->ui_.block->setChecked(isIgnoring);
        this->ui_.block->setEnabled(true);
        this->ui_.ignoreHighlights->setChecked(isIgnoringHighlights);
        this->ui_.notesAdd->setEnabled(true);

        auto type = this->underlyingChannel_->getType();

        if (type == Channel::Type::Twitch)
        {
            auto *followChannel =
                dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
            const auto account = getApp()->getAccounts()->twitch.getCurrent();
            const bool ownCard = account && !account->isAnon() &&
                                 user.id == account->getUserId();
            const bool followageViaHelix =
                followChannel &&
                (followChannel->isBroadcaster() || followChannel->isMod()) &&
                !followChannel->roomId().isEmpty();
            const bool followageViaIvr =
                !ownCard && !followageViaHelix &&
                getSettings()->showUsercardFollowage;

            // get subage
            if (getSettings()->showUsercardSubage ||
                getSettings()->showUsercardSubGiftGifter || followageViaIvr)
            {
                getIvr()->getSubage(
                    this->userName_, this->underlyingChannel_->getName(),
                    [this, isCurrentRequest,
                     followageViaIvr](const IvrSubage &subageInfo) {
                        if (!isCurrentRequest())
                        {
                            return;
                        }

                        if (followageViaIvr)
                        {
                            auto followed = QDateTime::fromString(
                                subageInfo.followedAt, Qt::ISODateWithMs);
                            if (!followed.isValid())
                            {
                                followed = QDateTime::fromString(
                                    subageInfo.followedAt, Qt::ISODate);
                            }
                            this->setUsercardFollowage(
                                followed.isValid()
                                    ? std::optional<QDateTime>(followed)
                                    : std::nullopt);
                        }

                        if (!getSettings()->showUsercardSubage)
                        {
                            this->ui_.subageLabel->setText({});
                            this->ui_.subageRow->setVisible(false);
                            this->ui_.subageIcon->setVisible(false);
                            this->updateUsercardSubGiftRow(subageInfo);
                            return;
                        }

                        if (subageInfo.isSubHidden)
                        {
                            this->ui_.subageLabel->setText(
                                tr("Сведения о подписке скрыты"));
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }
                        else if (subageInfo.isSubbed)
                        {
                            auto subageText =
                                tr("Подписка T%1 — %2")
                                    .arg(subageInfo.subTier.left(1),
                                         formatRussianMonths(
                                             subageInfo.totalSubMonths));
                            if (getSettings()->showUsercardSubageRelativeTime)
                            {
                                subageText += formatUsercardYearsMonths(
                                    subageInfo.totalSubMonths);
                            }
                            this->ui_.subageLabel->setText(subageText);
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(true);
                        }
                        else if (subageInfo.totalSubMonths)
                        {
                            auto subageText =
                                tr("Подписка была — %1")
                                    .arg(formatRussianMonths(
                                        subageInfo.totalSubMonths));
                            if (getSettings()->showUsercardSubageRelativeTime)
                            {
                                subageText += formatUsercardYearsMonths(
                                    subageInfo.totalSubMonths);
                            }
                            this->ui_.subageLabel->setText(subageText);
                            this->updateUsercardStatusIcons();
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(true);
                        }
                        else
                        {
                            this->ui_.subageLabel->setText(
                                tr("Подписка не найдена"));
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }

                        this->updateUsercardSubGiftRow(subageInfo);
                    },
                    [this, isCurrentRequest, followageViaIvr] {
                        if (!isCurrentRequest())
                        {
                            return;
                        }

                        if (followageViaIvr)
                        {
                            this->setUsercardFollowage(std::nullopt);
                        }
                        if (getSettings()->showUsercardSubage)
                        {
                            this->ui_.subageLabel->setText(
                                tr("Нет информации"));
                            this->ui_.subageRow->setVisible(true);
                            this->ui_.subageIcon->setVisible(false);
                        }
                        if (getSettings()->showUsercardSubGiftGifter)
                        {
                            this->hideUsercardSubGiftRow();
                        }
                    });
            }

            // get followage
            if (getSettings()->showUsercardFollowage)
            {
                if (ownCard)
                {
                    const auto isOwnRequest = [isCurrentRequest, id = user.id] {
                        if (!isCurrentRequest())
                        {
                            return false;
                        }
                        const auto current =
                            getApp()->getAccounts()->twitch.getCurrent();
                        return current && !current->isAnon() &&
                               current->getUserId() == id;
                    };
                    const auto failed = [this, isOwnRequest] {
                        if (isOwnRequest())
                        {
                            this->setUsercardFollowage(
                                std::nullopt, uiText("Could not load your follow date"));
                        }
                    };
                    const auto lookup = [this, isOwnRequest, failed, id = user.id](
                                            const QString &channelId) {
                        if (!isOwnRequest())
                        {
                            return;
                        }
                        getHelix()->getFollowedChannel(
                            id, channelId, this,
                            [this, isOwnRequest](const auto &follow) {
                                if (!isOwnRequest())
                                {
                                    return;
                                }
                                if (!follow)
                                {
                                    this->setUsercardFollowage(
                                        std::nullopt, uiText("You do not follow this channel"));
                                }
                                else
                                {
                                    this->setUsercardFollowage(
                                        follow->followedAt, uiText("Could not load your follow date"));
                                }
                            },
                            [failed](const QString &) { failed(); });
                    };
                    if (followChannel && !followChannel->roomId().isEmpty())
                    {
                        lookup(followChannel->roomId());
                    }
                    else
                    {
                        getHelix()->getUserByName(
                            this->underlyingChannel_->getName(),
                            [lookup](const HelixUser &channel) { lookup(channel.id); },
                            failed);
                    }
                }
                else if (followageViaHelix)
                {
                    getHelix()->getChannelFollowers(
                        followChannel->roomId(), user.id,
                        [this, isCurrentRequest](const auto &response) {
                            if (!isCurrentRequest() ||
                                !getSettings()->showUsercardFollowage)
                            {
                                return;
                            }

                            if (response.specifiedFollower)
                            {
                                this->setUsercardFollowage(
                                    response.specifiedFollower->followedAt);
                            }
                            else
                            {
                                this->setUsercardFollowage(std::nullopt);
                            }
                        },
                        [this, isCurrentRequest](const auto &errorMessage) {
                            qCWarning(chatterinoTwitch)
                                << "Error getting follow age:" << errorMessage;
                            if (!isCurrentRequest() ||
                                !getSettings()->showUsercardFollowage)
                            {
                                return;
                            }

                            this->setUsercardFollowage(std::nullopt);
                        });
                }
            }

            getIvr()->getUser(
                user.login,
                [this, isCurrentRequest](const IvrUserProfile &profile) {
                    if (!isCurrentRequest())
                    {
                        return;
                    }

                    this->applyIvrUserProfile(profile);
                },
                [this, isCurrentRequest] {
                    if (!isCurrentRequest())
                    {
                        return;
                    }

                    if (getSettings()->showUsercardChatterCount)
                    {
                        this->ui_.chatterCountLabel->setText(uiText("Chatters: ") %
                                                             uiText(TEXT_UNAVAILABLE));
                    }
                    if (getSettings()->showUsercardLastLive)
                    {
                        this->ui_.lastLiveLabel->setText(uiText("Last live: ") %
                                                         uiText(TEXT_UNAVAILABLE));
                    }
                    if (getSettings()->showUsercardColor)
                    {
                        this->ui_.userColorRow->setProperty("copy-color", {});
                        this->ui_.userColorRow->setProperty("swatch-color", {});
                        this->ui_.userColorLabel->setText(uiText("Color: ") %
                                                          uiText(TEXT_UNAVAILABLE));
                        this->updateUsercardStatusIcons();
                    }
                    if (getSettings()->showUsercardStatus)
                    {
                        this->ui_.statusLabel->setText(uiText("Status: ") %
                                                       uiText(TEXT_UNAVAILABLE));
                    }
                });
        }

        // get roles
        getIvr()->getUserRoles(
            this->userName_,
            [this, isCurrentRequest](const IvrResolve &userInfo) {
                if (!isCurrentRequest())
                {
                    return;
                }

                QString rolesString = "";

                if (userInfo.isBot)
                {
                    rolesString += "Bot ";
                }
                if (userInfo.isPartner)
                {
                    rolesString += "Partner ";
                }
                if (userInfo.isAffiliate)
                {
                    rolesString += "Affiliate ";
                }
                if (userInfo.isStaff)
                {
                    rolesString += "Staff ";
                }
                if (userInfo.isExStaff)
                {
                    rolesString += "Ex-Staff ";
                }

                this->ui_.rolesLabel->setText((rolesString));

                if (userInfo.chatterCount >= 0)
                {
                    this->ui_.chatterCountLabel->setText(uiText(TEXT_CHATTERS).arg(
                        localizeNumbers(userInfo.chatterCount)));
                }
                else
                {
                    this->ui_.chatterCountLabel->setText(
                        uiText(TEXT_CHATTERS).arg(uiText(TEXT_UNAVAILABLE)));
                }

                if (!userInfo.lastBroadcastStartedAt.isEmpty())
                {
                    this->ui_.lastLiveLabel->setText(uiText(TEXT_LAST_LIVE).arg(
                        userInfo.lastBroadcastStartedAt.section("T", 0, 0)));
                    this->ui_.lastLiveLabel->setToolTip(
                        formatLongFriendlyDuration(
                            QDateTime::fromString(
                                userInfo.lastBroadcastStartedAt,
                                Qt::ISODateWithMs),
                            QDateTime::currentDateTimeUtc()) +
                        uiText(u" ago"_s));
                    this->ui_.lastLiveLabel->setMouseTracking(true);
                }
                else
                {
                    this->ui_.lastLiveLabel->setText(
                        uiText(TEXT_LAST_LIVE).arg(uiText(TEXT_UNAVAILABLE)));
                }

                if (!userInfo.chatColor.isEmpty())
                {
                    this->ui_.userColorRow->setProperty("copy-color",
                                                        userInfo.chatColor);
                    this->ui_.userColorRow->setProperty("swatch-color",
                                                        userInfo.chatColor);
                    this->ui_.userColorLabel->setText(
                        uiText(TEXT_COLOR).arg(userInfo.chatColor));
                    this->updateUsercardStatusIcons();
                    this->refreshPublicHistoryColors();
                }
                else
                {
                    this->ui_.userColorRow->setProperty("copy-color", {});
                    this->ui_.userColorRow->setProperty("swatch-color", {});
                    this->ui_.userColorLabel->setText(
                        uiText(TEXT_COLOR).arg(uiText(TEXT_UNAVAILABLE)));
                    this->updateUsercardStatusIcons();
                }
            },
            [] {});

        // get pronouns
        if (getSettings()->showPronouns)
        {
            getApp()->getPronouns()->getUserPronoun(
                user.login,
                [this, isCurrentRequest](const auto userPronoun) {
                    runInGuiThread([this, isCurrentRequest,
                                    userPronoun = std::move(userPronoun)]() {
                        if (!isCurrentRequest() ||
                            this->ui_.pronounsLabel == nullptr)
                        {
                            return;
                        }
                        if (!userPronoun.isUnspecified())
                        {
                            this->ui_.pronounsLabel->setText(
                                uiText(TEXT_PRONOUNS).arg(userPronoun.format()));
                        }
                        else
                        {
                            this->ui_.pronounsLabel->setText(
                                uiText(TEXT_PRONOUNS).arg(uiText(TEXT_UNSPECIFIED)));
                        }
                    });
                },
                [this, isCurrentRequest]() {
                    runInGuiThread([this, isCurrentRequest]() {
                        qCWarning(chatterinoTwitch) << "Error getting pronouns";
                        if (!isCurrentRequest())
                        {
                            return;
                        }
                        this->ui_.pronounsLabel->setText(
                            uiText(TEXT_PRONOUNS).arg(uiText(TEXT_UNSPECIFIED)));
                    });
                });
        }
    };

    if (!this->userId_.isEmpty())
    {
        getHelix()->getUserById(this->userId_, onUserFetched,
                                onUserFetchFailed);
    }
    else
    {
        getHelix()->getUserByName(this->userName_, onUserFetched,
                                  onUserFetchFailed);
    }

    this->ui_.block->setEnabled(false);
    this->ui_.ignoreHighlights->setEnabled(false);
    this->ui_.notesAdd->setEnabled(false);

    bool isMyself =
        getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
            this->userName_, Qt::CaseInsensitive) == 0;
    this->ui_.block->setVisible(!isMyself);
    this->ui_.ignoreHighlights->setVisible(!isMyself);
}

void UserInfoPopup::loadAvatar(const QString &userID, const QString &pictureURL,
                               bool isKick)
{
    const auto generation = this->userDataRequestGeneration_;
    auto filename =
        getApp()->getPaths().cacheDirectory() + "/" + hashUrl(pictureURL);
    QFile cacheFile(filename);
    bool hasCachedAvatar = false;
    if (cacheFile.exists() && cacheFile.open(QIODevice::ReadOnly))
    {
        QPixmap avatar{};
        if (avatar.loadFromData(cacheFile.readAll()))
        {
            this->ui_.avatarButton->setPixmap(avatar);
            this->avatarPixmap_ = std::move(avatar);
            hasCachedAvatar = true;
        }
    }
    if (!hasCachedAvatar)
    {
        QNetworkRequest req(pictureURL);
        req.setHeader(QNetworkRequest::UserAgentHeader, "Chatterino");
        static auto *manager = createNetworkAccessManager();
        auto *reply = manager->get(req);
        setNetworkReplyTimeout(reply, 15000);
        QObject::connect(reply, &QNetworkReply::finished, reply,
                         &QObject::deleteLater);

        QObject::connect(reply, &QNetworkReply::finished, this,
                         [this, reply, filename, generation] {
                             if (generation != this->userDataRequestGeneration_)
                             {
                                 return;
                             }
                             if (reply->error() == QNetworkReply::NoError)
                             {
                                 const auto data = reply->readAll();

                                 QPixmap avatar;
                                 if (!avatar.loadFromData(data))
                                 {
                                     return;
                                 }
                                 this->ui_.avatarButton->setPixmap(avatar);
                                 this->saveCacheAvatar(data, filename);
                                 this->avatarPixmap_ = std::move(avatar);
                             }
                             else
                             {
                                 this->ui_.avatarButton->setPixmap(QPixmap());
                             }
                         });
    }

    this->helixAvatarUrl_ = pictureURL;
    this->updateAvatarUrl();

    if (getSettings()->displaySevenTVAnimatedProfile ||
        getSettings()->showSevenTVUsercardButton)
    {
        this->loadSevenTVAvatar(userID, isKick);
    }
}

void UserInfoPopup::loadSevenTVAvatar(const QString &userID, bool isKick,
                                      bool allowAvatarDownload)
{
    const auto generation = ++this->seventvUserRequestGeneration_;
    if (userID.isEmpty())
    {
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();
        return;
    }

    auto fmt = isKick ? SEVENTV_KICK_USER_API : SEVENTV_TWITCH_USER_API;
    const auto cacheKey = sevenTVUserCacheKey(userID, isKick);
    const auto cachedIt = sevenTVUserIDCache().constFind(cacheKey);
    const bool hadCachedUserID =
        cachedIt != sevenTVUserIDCache().constEnd() && !cachedIt->isEmpty();
    const bool needsAvatar =
        allowAvatarDownload && getSettings()->displaySevenTVAnimatedProfile;

    if (cachedIt != sevenTVUserIDCache().constEnd())
    {
        this->seventvUserID_ = *cachedIt;
        this->seventvUserLookupInFlight_ = false;
        this->seventvUserLookupFinished_ = true;
        this->refreshSevenTVUserButtonVisibility();

        if (!needsAvatar || this->seventvUserID_.isEmpty())
        {
            return;
        }
    }
    else
    {
        this->seventvUserID_.clear();
        this->seventvUserLookupInFlight_ = true;
        this->seventvUserLookupFinished_ = false;
        this->refreshSevenTVUserButtonVisibility();
    }

    NetworkRequest(fmt.arg(userID))
        .timeout(20000)
        .onSuccess([this, hack = std::weak_ptr<bool>(this->lifetimeHack_),
                    generation, cacheKey,
                    allowAvatarDownload](const NetworkResult &result) {
            if (!hack.lock() ||
                generation != this->seventvUserRequestGeneration_)
            {
                return;
            }

            const auto root = result.parseJson();
            const auto userObj = root["user"].toObject();
            this->seventvUserID_ = userObj["id"].toString();
            if (!this->seventvUserID_.isEmpty())
            {
                sevenTVUserIDCache().insert(cacheKey, this->seventvUserID_);
            }
            this->seventvUserLookupInFlight_ = false;
            this->seventvUserLookupFinished_ = true;
            this->refreshSevenTVUserButtonVisibility();

            if (!allowAvatarDownload ||
                !getSettings()->displaySevenTVAnimatedProfile)
            {
                return;
            }

            auto url = userObj["avatar_url"].toString();

            if (url.isEmpty())
            {
                return;
            }
            if (!url.startsWith(u"https:"))
            {
                url.prepend(u"https:");
            }
            this->seventvAvatarUrl_ = url;
            if (this->helixAvatarUrl_ == this->seventvAvatarUrl_)
            {
                return;
            }

            auto dotIdx = url.lastIndexOf('.') + 1;
            QByteArray format;
            if (dotIdx > 0)
            {
                auto end = url.size();
                auto queryIdx = url.lastIndexOf('?');
                if (queryIdx > dotIdx)
                {
                    end = queryIdx;
                }
                format = QStringView(url).sliced(dotIdx, end - dotIdx).toUtf8();
            }

            // We're implementing custom caching here,
            // because we need the cached file path.
            auto hash = hashUrl(url);
            auto filename = getApp()->getPaths().cacheDirectory() + "/" + hash;

            QFile cacheFile(filename);
            if (cacheFile.exists() && this->setSevenTVAvatar(filename, format))
            {
                return;
            }

            QNetworkRequest req(url);

            // We're using this manager instead of the one provided
            // in NetworkManager, because we're on a different thread.
            static auto *manager = createNetworkAccessManager();
            auto *reply = manager->get(req);
            setNetworkReplyTimeout(reply, 15000);
            QObject::connect(reply, &QNetworkReply::finished, reply,
                             &QObject::deleteLater);

            QObject::connect(
                reply, &QNetworkReply::finished, this,
                [this, reply, url, filename, format, generation] {
                    if (generation != this->seventvUserRequestGeneration_)
                    {
                        return;
                    }
                    if (reply->error() == QNetworkReply::NoError)
                    {
                        QBuffer buffer;
                        buffer.setData(reply->readAll());
                        buffer.open(QIODevice::ReadOnly);
                        QImageReader reader(&buffer, format);
                        if (!reader.read().isNull() &&
                            this->saveCacheAvatar(buffer.data(), filename))
                        {
                            this->setSevenTVAvatar(filename, format);
                        }
                    }
                    else
                    {
                        qCWarning(chatterinoSeventv)
                            << "Error fetching Profile Picture:"
                            << reply->error();
                    }
                });

            return;
        })
        .onError([this, hack = std::weak_ptr<bool>(this->lifetimeHack_),
                  generation, cacheKey,
                  hadCachedUserID](const NetworkResult &result) {
            if (!hack.lock() ||
                generation != this->seventvUserRequestGeneration_)
            {
                return;
            }

            const auto status = result.status();
            if (status && *status == 404)
            {
                sevenTVUserIDCache().insert(cacheKey, {});
            }
            else if (hadCachedUserID)
            {
                return;
            }

            this->seventvUserID_.clear();
            this->seventvUserLookupInFlight_ = false;
            this->seventvUserLookupFinished_ = true;
            this->refreshSevenTVUserButtonVisibility();
        })
        .execute();
}

bool UserInfoPopup::setSevenTVAvatar(const QString &filename,
                                     const QByteArray &format)
{
    QImageReader reader(filename, format);
    if (reader.read().isNull())
    {
        qCWarning(chatterinoSeventv)
            << "Error reading Profile Picture, " << reader.errorString();
        return false;
    }

    auto *movie = new QMovie(filename, format, this);
    if (!movie->isValid())
    {
        qCWarning(chatterinoSeventv)
            << "Error reading Profile Picture, " << movie->lastErrorString();
        movie->deleteLater();
        return false;
    }

    if (this->seventvAvatar_)
    {
        this->seventvAvatar_->stop();
        this->seventvAvatar_->deleteLater();
    }
    QObject::connect(movie, &QMovie::frameChanged, this, [this, movie] {
        if (movie != this->seventvAvatar_)
        {
            return;
        }
        this->ui_.avatarButton->setPixmap(movie->currentPixmap());
    });

    this->seventvAvatar_ = movie;
    movie->start();
    this->ui_.switchAvatars->show();
    this->ui_.switchAvatars->setText(uiText(u"Show ") % this->platformName());
    this->isTwitchAvatarShown_ = false;
    this->updateAvatarUrl();
    return true;
}

bool UserInfoPopup::saveCacheAvatar(const QByteArray &avatar,
                                    const QString &filename) const
{
    QSaveFile outfile(filename);
    if (outfile.open(QIODevice::WriteOnly) &&
        outfile.write(avatar) == avatar.size() && outfile.commit())
    {
        return true;
    }
    qCWarning(chatterinoImage) << "Error writing to cache" << filename;
    return false;
}

void UserInfoPopup::updateNotes()
{
    static QRegularExpression onlySpaceRegex{"^\\s*$"};

    auto userData = getApp()->getUserData()->getUser(this->userId_);
    if (!userData.has_value() ||
        onlySpaceRegex.match(userData->notes).hasMatch())
    {
        this->ui_.notesPreview->setText("");
        this->ui_.notesPreview->setVisible(false);
        return;
    }

    this->ui_.notesPreview->setText(userData->notes);
    this->ui_.notesPreview->setVisible(true);
}

void UserInfoPopup::updateKickUserData()
{
    assert(this->isKick_);

    auto onChannelFetchFailed = [](UserInfoPopup *self) {
        // this can occur when the account doesn't exist.
        if (getSettings()->showUsercardFollowerCount)
        {
            self->ui_.followerCountLabel->setText(
                uiText(TEXT_FOLLOWERS).arg(uiText(TEXT_UNAVAILABLE)));
            self->ui_.followerCountLabel->setVisible(true);
        }
        if (getSettings()->showUsercardCreatedDate)
        {
            self->ui_.createdDateLabel->setText(
                uiText(TEXT_CREATED).arg(uiText(TEXT_UNAVAILABLE)));
            self->ui_.createdDateLabel->setVisible(true);
        }

        self->ui_.nameLabel->setText(self->userName_);

        self->ui_.userIDLabel->setText(uiText(u"ID ") % uiText(TEXT_UNAVAILABLE));
        self->ui_.userIDLabel->setProperty("copy-text",
                                           uiText(TEXT_UNAVAILABLE));

        self->seventvUserRequestGeneration_++;
        self->seventvUserID_.clear();
        self->seventvUserLookupInFlight_ = false;
        self->seventvUserLookupFinished_ = true;
        self->refreshSevenTVUserButtonVisibility();
    };
    auto fetchDefaultAvatar = [](UserInfoPopup *self) {
        // The "full" channel info doesn't include the profile picture.
        KickApi::privateChannelInfoSmall(
            self->userName_, [self = QPointer(self)](const auto &res) {
                if (!self || !res)
                {
                    return;
                }
                const auto &url = res->user.profilePictureURL;
                if (!url || *url == self->helixAvatarUrl_ ||
                    !self->helixAvatarUrl_.startsWith(u"https://kick.com"))
                {
                    return;
                }
                self->helixAvatarUrl_ = *url;
                self->updateAvatarUrl();
                self->loadCurrentAvatar();
            });
    };
    auto onChannelFetched = [fetchDefaultAvatar](
                                UserInfoPopup *self,
                                const KickPrivateChannelInfo &channel) {
        // Correct for when being opened with ID or slug/username mismatch.
        self->kickUserSlug_ = channel.slug;
        self->userName_ = channel.user.username;
        self->ui_.nameLabel->setText(channel.user.username);
        if (self->userName_.compare(self->kickUserSlug_, Qt::CaseInsensitive) !=
            0)
        {
            self->ui_.localizedNameLabel->setText(self->kickUserSlug_);
            self->ui_.localizedNameLabel->setProperty("copy-text",
                                                      self->kickUserSlug_);
            self->ui_.localizedNameLabel->setVisible(true);
            self->ui_.localizedNameCopyButton->setVisible(true);
        }

        // Ensure recent messages are shown
        self->updateLatestMessages();

        self->kickUserID_ = channel.user.userID;
        auto userIDStr = QString::number(self->kickUserID_);
        self->userId_ = u"kick:" % userIDStr;
        self->helixAvatarUrl_ = channel.user.profilePictureURL.value_or(
            u"https://kick.com/img/default-profile-pictures/default-avatar-2.webp"_s);
        self->updateAvatarUrl();
        self->loadCurrentAvatar();
        self->updateNotes();

        if (!channel.user.profilePictureURL.has_value())
        {
            fetchDefaultAvatar(self);
        }

        self->ui_.nameLabel->setText(channel.user.username);
        self->ui_.nameLabel->setProperty("copy-text", channel.user.username);

        self->setWindowTitle(uiText(TEXT_TITLE).arg(
            channel.user.username, self->underlyingChannel_->getName()));
        if (getSettings()->showUsercardCreatedDate)
        {
            self->ui_.createdDateLabel->setText(uiText(TEXT_CREATED).arg(
                channel.chatroom.createdAt.date().toString(Qt::ISODate)));
            self->ui_.createdDateLabel->setToolTip(
                formatLongFriendlyDuration(channel.chatroom.createdAt,
                                           QDateTime::currentDateTimeUtc()) +
                uiText(u" ago"_s));
            self->ui_.createdDateLabel->setMouseTracking(true);
            self->ui_.createdDateLabel->setVisible(true);
        }
        self->ui_.userIDLabel->setText(uiText(TEXT_USER_ID) % userIDStr);
        self->ui_.userIDLabel->setProperty("copy-text", userIDStr);

        if (getSettings()->showUsercardFollowerCount)
        {
            self->ui_.followerCountLabel->setText(
                uiText(TEXT_FOLLOWERS).arg(localizeNumbers(channel.followersCount)));
            self->ui_.followerCountLabel->setVisible(true);
        }

        // get ignoreHighlights state
        bool isIgnoringHighlights = false;
        const auto &vector = getSettings()->blacklistedUsers.raw();
        for (const auto &blockedUser : vector)
        {
            if (self->userName_ == blockedUser.getPattern())
            {
                isIgnoringHighlights = true;
                break;
            }
        }
        if (getSettings()->isBlacklistedUser(self->userName_) &&
            !isIgnoringHighlights)
        {
            self->ui_.ignoreHighlights->setToolTip(uiText("Name matched by regex"));
        }
        else
        {
            self->ui_.ignoreHighlights->setEnabled(true);
        }
        self->ui_.block->setChecked(/*is_ignoring=*/false);
        self->ui_.block->setEnabled(true);
        self->ui_.ignoreHighlights->setChecked(isIgnoringHighlights);
        self->ui_.notesAdd->setEnabled(true);
    };

    auto fetchChannelInfo = [self = QPointer(this), onChannelFetched,
                             onChannelFetchFailed](const QString &userName) {
        KickApi::privateChannelInfo(
            userName,
            [self, onChannelFetched, onChannelFetchFailed](const auto &res) {
                if (!self)
                {
                    return;
                }
                if (res)
                {
                    onChannelFetched(self.get(), *res);
                }
                else
                {
                    qCDebug(chatterinoKick)
                        << "Channel fetch failed" << res.error();
                    onChannelFetchFailed(self.get());
                }
            });
    };
    auto fetchUserInChannelInfo = [self = QPointer(this),
                                   channelName =
                                       this->underlyingChannel_->getName()](
                                      const QString &userName) {
        KickApi::privateUserInChannelInfo(
            userName, channelName, [self](const auto &res) {
                if (!self || !res)
                {
                    return;
                }

                self->setUsercardFollowage(res->followingSince);

                if (getSettings()->showUsercardSubage &&
                    res->subscriptionMonths)
                {
                    auto subageText = uiText(QString("Subscribed for %1 months"))
                                          .arg(*res->subscriptionMonths);
                    if (getSettings()->showUsercardSubageRelativeTime)
                    {
                        subageText +=
                            formatUsercardYearsMonths(*res->subscriptionMonths);
                    }
                    self->ui_.subageLabel->setText(subageText);
                    self->updateUsercardStatusIcons();
                    self->ui_.subageRow->setVisible(true);
                    self->ui_.subageIcon->setVisible(true);
                }
                else if (getSettings()->showUsercardSubage)
                {
                    self->ui_.subageLabel->setText({});
                    self->ui_.subageRow->setVisible(true);
                    self->ui_.subageIcon->setVisible(false);
                }

                self->hideUsercardSubGiftRow();
            });
    };

    if (!this->userId_.isEmpty() && this->userName_.isEmpty())
    {
        std::array ids{static_cast<uint64_t>(this->userId_.toULongLong())};
        getKickApi()->getChannels(
            ids, [self = QPointer(this), onChannelFetchFailed, fetchChannelInfo,
                  fetchUserInChannelInfo](const auto &res) {
                if (!self)
                {
                    return;
                }
                if (!res || res->size() != 1)
                {
                    onChannelFetchFailed(self);
                    return;
                }
                fetchChannelInfo((*res)[0].slug);
                fetchUserInChannelInfo((*res)[0].slug);
            });
    }
    else
    {
        fetchChannelInfo(this->userName_);
        fetchUserInChannelInfo(this->userName_);
    }

    this->ui_.block->setEnabled(false);
    this->ui_.ignoreHighlights->setEnabled(false);
    this->ui_.notesAdd->setEnabled(false);

    bool isMyself = false;  // FIXME: kick account
    this->ui_.block->setVisible(!isMyself);
    this->ui_.ignoreHighlights->setVisible(!isMyself);
}

void UserInfoPopup::onKickProfilePictureClick(Qt::MouseButton button)
{
    assert(this->isKick_);
    auto channelURL = QUrl("https://kick.com/" + this->kickUserSlug_);

    switch (button)
    {
        case Qt::LeftButton: {
            QDesktopServices::openUrl(channelURL);
        }
        break;

        // largely the same as on Twitch
        case Qt::RightButton: {
            if (this->avatarUrl_.isEmpty())
            {
                return;
            }

            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);

            auto avatarUrl = this->avatarUrl_;

            // add context menu actions
            menu->addAction(uiText("Open &avatar in browser"), this, [avatarUrl] {
                QDesktopServices::openUrl(QUrl(avatarUrl));
            });

            menu->addAction(uiText("Copy a&vatar link"), this, [avatarUrl] {
                crossPlatformCopy(avatarUrl);
            });

            // we need to assign login name for msvc compilation
            auto username = this->userName_.toLower();
            menu->addAction(
                uiText("Open channel in a new &popup window"), this, [username] {
                    auto *app = getApp();
                    auto *split =
                        app->getWindows()
                            ->createWindow(WindowType::Popup, {.show = true})
                            .getNotebook()
                            .getOrAddSelectedPage()
                            ->appendNewSplit(false);
                    split->setChannel(
                        app->getKickChatServer()->getOrCreate(username));
                });

            menu->addAction(uiText("Open channel in a new &tab"), this, [username] {
                SplitContainer *container = getApp()
                                                ->getWindows()
                                                ->getMainWindow()
                                                .getNotebook()
                                                .addPage(true);
                auto *split = new Split(container);
                split->setChannel(
                    getApp()->getKickChatServer()->getOrCreate(username));
                container->insertSplit(split);
            });

            menu->addAction(uiText("Open channel in &browser"), this, [channelURL] {
                QDesktopServices::openUrl(channelURL);
            });

            this->appendCommonProfileActions(menu);

            menu->popup(QCursor::pos());
            menu->raise();
        }
        break;

        default:
            break;
    }
}

QString UserInfoPopup::showProfilePictureContextMenu()
{
    if (this->avatarUrl_.isEmpty())
    {
        return "No avatar is available for this user.";
    }

    if (this->isKick_)
    {
        this->onKickProfilePictureClick(Qt::RightButton);
        return {};
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto avatarUrl = this->avatarUrl_;
    auto channelURL =
        QUrl("https://www.twitch.tv/" + this->userName_.toLower());

    menu->addAction(uiText("Open &avatar in browser"), this, [avatarUrl] {
        QDesktopServices::openUrl(QUrl(avatarUrl));
    });

    menu->addAction(uiText("Copy a&vatar link"), this, [avatarUrl] {
        crossPlatformCopy(avatarUrl);
    });

    auto loginName = this->userName_.toLower();
    menu->addAction(uiText("Open channel in a new &popup window"), this, [loginName] {
        auto *app = getApp();
        auto &window =
            app->getWindows()->createWindow(WindowType::Popup, {.show = true});
        auto *split =
            window.getNotebook().getOrAddSelectedPage()->appendNewSplit(false);
        split->setChannel(app->getTwitch()->getOrAddChannel(loginName));
    });

    menu->addAction(uiText("Open channel in a new &tab"), this, [loginName] {
        ChannelPtr channel = getApp()->getTwitch()->getOrAddChannel(loginName);
        auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
        SplitContainer *container = notebook.addPage(true);
        Split *split = new Split(container);
        split->setChannel(channel);
        container->insertSplit(split);
    });

    menu->addAction(uiText("Open channel in &browser"), this, [channelURL] {
        QDesktopServices::openUrl(channelURL);
    });

    this->appendCommonProfileActions(menu);

    menu->popup(QCursor::pos());
    menu->raise();
    return {};
}

bool UserInfoPopup::canShowRoleManagementMenu() const
{
    if (!getSettings()->showUsercardRoleManagementMenu || this->isKick_ ||
        this->isYouTube_ || this->userName_.isEmpty() ||
        !this->underlyingChannel_)
    {
        return false;
    }

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get());
    if (twitchChannel == nullptr || twitchChannel->roomId().isEmpty())
    {
        return false;
    }

    const bool isMyself =
        getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
            this->userName_, Qt::CaseInsensitive) == 0;
    const bool isChannelOwner =
        this->userName_.compare(twitchChannel->getName(),
                                Qt::CaseInsensitive) == 0;
    if (isMyself || isChannelOwner || this->isBroadcaster_)
    {
        return false;
    }

    const auto auth = MoltorinoAuth::resolveSavedBroadcasterToken(
        twitchChannel->roomId(), twitchChannel->getName());
    return auth.hasToken();
}

void UserInfoPopup::showRoleManagementMenu()
{
    if (this->ui_.rolesLabel == nullptr || !this->canShowRoleManagementMenu())
    {
        return;
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const auto addAction = [this, menu](const QString &title,
                                        const QString &command,
                                        const QString &actionText) {
        menu->addAction(title, this, [this, command, actionText] {
            this->runRoleManagementCommand(command, actionText);
        });
    };

    addAction(uiText("Add lead moderator"), "/leadmod", "add lead moderator to");
    addAction(uiText("Remove lead moderator"), "/unleadmod",
              "remove lead moderator from");
    menu->addSeparator();
    addAction(uiText("Add editor"), "/editor", "add editor to");
    addAction(uiText("Remove editor"), "/uneditor", "remove editor from");
    menu->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                               this->scale()));
    styleMenu(menu);

    // The original role button now lives in the hidden action-controls host.
    // Anchor to the visible compact menu, or the profile viewport if scrolled.
    QWidget *anchor = this->ui_.actions;
    if (!anchor || anchor->visibleRegion().isEmpty())
    {
        anchor = this->ui_.profileScroll->viewport();
    }
    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height())));
    menu->raise();
}

void UserInfoPopup::runRoleManagementCommand(const QString &command,
                                             const QString &actionText)
{
    if (!this->underlyingChannel_ || this->userName_.isEmpty())
    {
        return;
    }

    const bool wasPinned = this->ensurePinned();
    auto reply = QMessageBox::warning(
        this, uiText("Confirm role change"),
        uiText(QString("Are you sure you want to %1 %2 in %3?"))
            .arg(actionText, this->userName_,
                 this->underlyingChannel_->getName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (wasPinned)
    {
        this->togglePinned();
    }
    if (reply != QMessageBox::Yes)
    {
        return;
    }

    auto value = command + ' ' + this->userName_;
    value = getApp()->getCommands()->execCommand(
        value, this->underlyingChannel_, false);
    if (!value.isEmpty())
    {
        this->underlyingChannel_->sendMessage(value);
    }
}

void UserInfoPopup::updateUsercardStatusIcons()
{
    const auto boxSize = std::max(1, qRound(15 * this->scale()));
    const auto iconSize = std::max(1, qRound(14 * this->scale()));
    const bool isLight = getApp()->getThemes()->isLightTheme();
    const auto iconScale = this->devicePixelRatioF();

    auto updateIcon = [boxSize, iconScale](QLabel *label, const QString &path,
                                           int iconSize) {
        if (label == nullptr)
        {
            return;
        }

        label->setFixedSize(boxSize, boxSize);
        label->setAlignment(Qt::AlignCenter);
        auto pixmap = renderUsercardStatusIcon(path, iconSize, iconScale);
        if (!pixmap.isNull())
        {
            QPainter tint(&pixmap);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(pixmap.rect(), QColor(142, 145, 157));
        }
        label->setPixmap(pixmap);
    };

    auto updateColorSwatch = [this] {
        if (this->ui_.userColorSwatch == nullptr ||
            this->ui_.userColorRow == nullptr)
        {
            return;
        }

        auto colorText =
            this->ui_.userColorRow->property("copy-color").toString();
        if (colorText.isEmpty())
        {
            colorText =
                this->ui_.userColorRow->property("swatch-color").toString();
        }

        const QColor color(colorText);
        const auto swatchSize = std::max(1, qRound(8 * this->scale()));
        this->ui_.userColorSwatch->setFixedSize(swatchSize, swatchSize);
        if (color.isValid())
        {
            this->ui_.userColorSwatch->setStyleSheet(
                QString("QFrame#UsercardColorSwatch { background: %1; "
                        "border-radius: %2px; }")
                    .arg(color.name(QColor::HexRgb))
                    .arg(std::max(1, qRound(2 * this->scale()))));
        }
        else
        {
            this->ui_.userColorSwatch->setStyleSheet({});
        }
    };

    updateIcon(this->ui_.followageIcon,
               isLight ? ":/buttons/usercardFollow-lightMode.svg"
                       : ":/buttons/usercardFollow-darkMode.svg",
               iconSize);
    updateIcon(this->ui_.subageIcon,
               isLight ? ":/buttons/usercardSub-lightMode.svg"
                       : ":/buttons/usercardSub-darkMode.svg",
               iconSize);
    updateIcon(this->ui_.subGiftIcon,
               isLight ? ":/buttons/usercardGift-lightMode.svg"
                       : ":/buttons/usercardGift-darkMode.svg",
               iconSize);
    updateColorSwatch();
    this->updateNamePresentation();
}

void UserInfoPopup::setUsercardFollowage(
    const std::optional<QDateTime> &followedAt, const QString &emptyText)
{
    this->ui_.followageLabel->setProperty("openUrl", QVariant());
    this->ui_.followageLabel->unsetCursor();
    if (!getSettings()->showUsercardFollowage)
    {
        this->ui_.followageLabel->setText({});
        this->ui_.followageLabel->setToolTip({});
        this->ui_.followageRow->setVisible(false);
        this->ui_.followageIcon->setVisible(false);
        return;
    }

    if (!followedAt || !followedAt->isValid())
    {
        if (!emptyText.isEmpty())
        {
            this->ui_.followageLabel->setText(emptyText);
            this->ui_.followageLabel->setToolTip({});
            this->updateUsercardStatusIcons();
            this->ui_.followageRow->show();
            this->ui_.followageIcon->show();
            return;
        }
        // Twitch no longer tells anyone but moderators: the followage site
        // shows it (opened in the browser, on this person).
        const auto url =
            u"https://followage.showmasterokda.com/?user="_s +
            QString::fromUtf8(QUrl::toPercentEncoding(this->userName_));
        this->ui_.followageLabel->setText(
            uiText("Twitch hides the date · open on followage"));
        this->ui_.followageLabel->setToolTip(
            uiText("Opens followage.showmasterokda.com on %1: the channels "
                   "they follow, with the dates")
                .arg(this->userName_));
        this->ui_.followageLabel->setProperty("openUrl", url);
        this->ui_.followageLabel->setCursor(Qt::PointingHandCursor);
        this->ui_.followageRow->setVisible(true);
        this->ui_.followageIcon->setVisible(true);
        return;
    }
    const auto followedDate = followedAt->toLocalTime().date();
    auto relativeTime = QString();
    if (getSettings()->showUsercardFollowageRelativeTime)
    {
        relativeTime = formatUsercardFollowRelativeTime(followedDate);
    }
    this->ui_.followageLabel->setText(
        tr("Фоллов с ") + followedDate.toString(Qt::ISODate) + relativeTime);
    this->ui_.followageLabel->setToolTip(
        formatLongFriendlyDuration(*followedAt,
                                   QDateTime::currentDateTimeUtc()) +
        uiText(u" ago"_s));
    this->ui_.followageLabel->setMouseTracking(true);
    this->updateUsercardStatusIcons();
    this->ui_.followageRow->setVisible(true);
    this->ui_.followageIcon->setVisible(true);
}

void UserInfoPopup::updateUsercardSubGiftRow(const IvrSubage &subageInfo)
{
    if (!getSettings()->showUsercardSubGiftGifter || !subageInfo.isSubbed ||
        !subageInfo.isGiftSub())
    {
        this->hideUsercardSubGiftRow();
        return;
    }

    const auto gifterName = subageInfo.giftGifterName();
    this->ui_.subGiftLabel->setText(
        gifterName.isEmpty() ? uiText(QString("Gifted by an anonymous user"))
                             : uiText(QString("Gifted by %1")).arg(gifterName));
    this->updateUsercardStatusIcons();
    this->ui_.subGiftRow->setVisible(true);
    this->ui_.subGiftIcon->setVisible(true);
}

void UserInfoPopup::hideUsercardSubGiftRow()
{
    this->ui_.subGiftLabel->setText({});
    this->ui_.subGiftLabel->setToolTip({});
    this->ui_.subGiftRow->setVisible(false);
    this->ui_.subGiftIcon->setVisible(false);
}

void UserInfoPopup::resetUsercardInfoRows()
{
    auto *settings = getSettings();
    const bool showTwitchProfileRows = !this->isKick_ && !this->isYouTube_;

    this->ui_.followerCountLabel->setText(uiText(TEXT_FOLLOWERS).arg(""));
    this->ui_.followerCountLabel->setVisible(
        settings->showUsercardFollowerCount);

    this->ui_.createdDateLabel->setText(uiText(TEXT_CREATED).arg(""));
    this->ui_.createdDateLabel->setToolTip({});
    this->ui_.createdDateLabel->setVisible(settings->showUsercardCreatedDate);

    this->ui_.followageLabel->setText(tr("Фоллов: загрузка…"));
    this->ui_.followageLabel->setProperty("openUrl", QVariant());
    this->ui_.followageLabel->unsetCursor();
    this->ui_.followageLabel->setToolTip({});
    this->ui_.followageRow->setVisible(settings->showUsercardFollowage);
    this->ui_.followageIcon->setVisible(false);

    this->ui_.subageLabel->setText(tr("Подписка: загрузка…"));
    this->ui_.subageRow->setVisible(settings->showUsercardSubage);
    this->ui_.subageIcon->setVisible(false);

    this->hideUsercardSubGiftRow();

    this->ui_.chatterCountLabel->setText(uiText("Chatters: ..."));
    this->ui_.chatterCountLabel->setVisible(
        showTwitchProfileRows && settings->showUsercardChatterCount &&
        settings->showUserinfoPopupChatters.getValue());
    this->ui_.lastLiveLabel->setText(uiText("Last live: 0000-00-00"));
    this->ui_.lastLiveLabel->setToolTip({});
    this->ui_.lastLiveLabel->setVisible(
        showTwitchProfileRows && settings->showUsercardLastLive &&
        settings->showUserinfoPopupLastLive.getValue());
    this->ui_.userColorLabel->setText(uiText("Color: #FFFFFF"));
    this->ui_.userColorRow->setProperty("copy-color", {});
    this->ui_.userColorRow->setProperty("swatch-color", "#FFFFFF");
    this->ui_.userColorSwatch->setStyleSheet(
        "QFrame#UsercardColorSwatch { background: #FFFFFF; "
        "border-radius: 2px; }");
    this->ui_.userColorRow->setVisible(
        showTwitchProfileRows && settings->showUsercardColor &&
        settings->showUserinfoPopupColor.getValue());
    this->ui_.statusLabel->setText(uiText("Status: ..."));
    this->ui_.statusLabel->setVisible(showTwitchProfileRows &&
                                      settings->showUsercardStatus);
    this->ui_.bannedAvatarLabel->hide();

    this->isUserLive_ = false;
    this->liveViewerCount_ = 0;
    if (this->ui_.liveIndicator)
    {
        this->ui_.liveIndicator->hide();
    }

    this->updateUsercardStatusIcons();
    this->refreshSeventvPaint();
}

void UserInfoPopup::applyIvrUserProfile(const IvrUserProfile &profile)
{
    auto *settings = getSettings();

    this->ui_.bannedAvatarLabel->setVisible(profile.banned);

    if (settings->showUsercardChatterCount &&
        settings->showUserinfoPopupChatters.getValue())
    {
        this->ui_.chatterCountLabel->setText(
            profile.chatterCount
                ? uiText("Chatters: ") + localizeNumbers(*profile.chatterCount)
                : uiText("Chatters: ") % uiText(TEXT_UNAVAILABLE));
        this->ui_.chatterCountLabel->setVisible(true);
    }

    if (settings->showUsercardLastLive &&
        settings->showUserinfoPopupLastLive.getValue())
    {
        const auto lastLive = formatIvrDate(profile.lastBroadcastStartedAt);
        if (lastLive.isEmpty())
        {
            this->ui_.lastLiveLabel->setText(uiText("Last live: ") % uiText(TEXT_UNAVAILABLE));
            this->ui_.lastLiveLabel->setToolTip({});
        }
        else
        {
            this->ui_.lastLiveLabel->setText(uiText("Last live: ") + lastLive);
            if (!profile.lastBroadcastTitle.isEmpty())
            {
                this->ui_.lastLiveLabel->setToolTip(profile.lastBroadcastTitle);
                this->ui_.lastLiveLabel->setMouseTracking(true);
            }
        }
        this->ui_.lastLiveLabel->setVisible(true);
    }

    if (settings->showUsercardColor &&
        settings->showUserinfoPopupColor.getValue())
    {
        const QColor color(profile.chatColor);
        if (color.isValid())
        {
            const auto colorHex = color.name(QColor::HexRgb).toUpper();
            this->ui_.userColorRow->setProperty("copy-color", colorHex);
            this->ui_.userColorRow->setProperty("swatch-color", colorHex);
            this->ui_.userColorLabel->setText(uiText("Color: ") + colorHex);
            this->updateUsercardStatusIcons();
            this->refreshPublicHistoryColors();
            this->ui_.userColorRow->setVisible(true);
            this->updateSeventvPaintPixmap();
        }
        else
        {
            this->ui_.userColorRow->setProperty("copy-color", {});
            this->ui_.userColorRow->setProperty("swatch-color", {});
            this->ui_.userColorLabel->setText(uiText("Color: ") % uiText(TEXT_UNAVAILABLE));
            this->ui_.userColorRow->setVisible(true);
            this->updateSeventvPaintPixmap();
        }
    }

    if (settings->showUsercardStatus)
    {
        this->ui_.statusLabel->setText(uiText("Status: ") +
                                       formatUsercardStatus(profile));
        this->ui_.statusLabel->setVisible(true);
    }
}

void UserInfoPopup::refreshSevenTVUserButtonVisibility()
{
    if (this->ui_.sevenTVUserLabel == nullptr)
    {
        return;
    }

    const bool settingEnabled = getSettings()->showSevenTVUsercardButton;
    const bool hasSevenTVUser = !this->seventvUserID_.isEmpty();
    const bool lookupPending =
        !this->seventvUserLookupFinished_ && !hasSevenTVUser;
    const bool shouldShow = settingEnabled && (lookupPending || hasSevenTVUser);

    this->ui_.sevenTVUserLabel->setVisible(shouldShow);
    this->ui_.sevenTVUserLabel->setEnabled(settingEnabled && hasSevenTVUser);

    if (hasSevenTVUser)
    {
        this->ui_.sevenTVUserLabel->setToolTip(uiText("Open 7TV profile"));
    }
    else if (this->seventvUserLookupInFlight_)
    {
        this->ui_.sevenTVUserLabel->setToolTip(uiText("Checking 7TV profile..."));
    }
    else if (this->seventvUserLookupFinished_)
    {
        this->ui_.sevenTVUserLabel->setToolTip(uiText("No 7TV profile found"));
    }
    else
    {
        this->ui_.sevenTVUserLabel->setToolTip(uiText("7TV profile not loaded yet"));
    }
}

void UserInfoPopup::resetNameHistory()
{
    ++this->nameHistoryRequestGeneration_;
    if (this->nameHistoryMenu_ != nullptr)
    {
        this->nameHistoryMenu_->close();
        this->nameHistoryMenu_ = nullptr;
    }
    this->nameHistoryLogin_.clear();
    this->nameHistoryEntries_.clear();
    this->nameHistoryLoading_ = false;
    this->nameHistoryLoaded_ = false;
    this->applyCachedNameHistory();
    this->updateNameHistoryButton();
}

bool UserInfoPopup::applyCachedNameHistory()
{
    if (this->userName_.isEmpty() || this->userId_.isEmpty() || this->isKick_ ||
        this->isYouTube_)
    {
        return false;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (login.isEmpty())
    {
        return false;
    }

    auto cached = getCachedTwitchNameHistory(this->userId_, login);
    if (!cached)
    {
        return false;
    }

    this->nameHistoryLogin_ = login;
    this->nameHistoryEntries_ = cached->entries;
    this->nameHistoryLoading_ = false;
    this->nameHistoryLoaded_ = true;
    return true;
}

void UserInfoPopup::updateNameHistoryButton()
{
    if (this->ui_.nameHistoryButton == nullptr)
    {
        return;
    }

    const bool canShow = getSettings()->showUsercardNameHistoryButton &&
                         !this->isKick_ && !this->isYouTube_ &&
                         !this->userName_.isEmpty();
    this->ui_.nameHistoryButton->setVisible(canShow);
    this->ui_.nameHistoryButton->setEnabled(canShow &&
                                            !this->userId_.isEmpty());
    this->ui_.nameHistoryButton->setText(this->nameHistoryLoading_ ? "..."
                                                                   : uiText("aka"));

    if (!canShow)
    {
        if (this->nameHistoryMenu_ != nullptr)
        {
            this->nameHistoryMenu_->close();
            this->nameHistoryMenu_ = nullptr;
        }
        this->ui_.nameHistoryButton->setToolTip({});
        return;
    }
    if (this->userId_.isEmpty())
    {
        this->ui_.nameHistoryButton->setToolTip(
            uiText("Name history loads after Twitch profile data."));
        return;
    }
    if (this->nameHistoryLoading_)
    {
        this->ui_.nameHistoryButton->setToolTip(uiText("Fetching name history..."));
        return;
    }
    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (this->nameHistoryLoaded_ && this->nameHistoryLogin_ == login &&
        this->nameHistoryEntries_.empty())
    {
        this->ui_.nameHistoryButton->setToolTip(uiText("No name history found"));
        return;
    }

    this->ui_.nameHistoryButton->setToolTip(uiText("Show name history"));
}

void UserInfoPopup::updateBadgesButton()
{
    if (this->ui_.badgesLabel == nullptr)
    {
        return;
    }

    bool canShow = !this->isKick_ && !this->isYouTube_ &&
                   !this->userName_.isEmpty() && this->underlyingChannel_ &&
                   !this->underlyingChannel_->getName().isEmpty();

    if (canShow && this->channel_)
    {
        const auto type = this->channel_->getType();
        if (type == Channel::Type::TwitchLive ||
            type == Channel::Type::TwitchWhispers ||
            type == Channel::Type::Misc || type == Channel::Type::Kick)
        {
            canShow = false;
        }
    }

    this->ui_.badgesLabel->setVisible(canShow);
    this->ui_.badgesLabel->setEnabled(canShow);
    if (!canShow)
    {
        this->ui_.badgesLabel->setAccessibleDescription({});
        return;
    }

    this->ui_.badgesLabel->setAccessibleDescription(uiText("Show all badges (%1)")
        .arg(this->ui_.badgeStrip ? this->ui_.badgeStrip->property("badgeCount").toInt() : 0));
}

void UserInfoPopup::toggleInlineBadges()
{
    if (auto *strip = static_cast<UsercardBadgeStrip *>(this->ui_.badgeStrip))
        strip->toggleExpansion();
}

void UserInfoPopup::showNameHistoryMenu()
{
    if (this->ui_.nameHistoryButton == nullptr || this->userName_.isEmpty() ||
        this->userId_.isEmpty() || this->isKick_ || this->isYouTube_)
    {
        return;
    }
    if (this->nameHistoryLoading_)
    {
        this->openNameHistoryMenu("Fetching name history...");
        return;
    }

    if (this->applyCachedNameHistory())
    {
        this->updateNameHistoryButton();
        this->openNameHistoryMenu();
        return;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (this->nameHistoryLoaded_ && this->nameHistoryLogin_ == login)
    {
        this->openNameHistoryMenu();
        return;
    }

    this->requestNameHistory();
}

void UserInfoPopup::openNameHistoryMenu(const QString &statusText)
{
    auto *button = this->ui_.nameHistoryButton;
    // This control lives in the hidden details container. The visible ⋮
    // menu now owns the action; its own visibility still reflects settings.
    if (button == nullptr || button->isHidden())
    {
        return;
    }

    if (this->nameHistoryMenu_ != nullptr)
    {
        this->nameHistoryMenu_->close();
    }

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    this->nameHistoryMenu_ = menu;
    menu->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                               this->scale()));
    styleMenu(menu);

    if (!statusText.isEmpty())
    {
        auto *status = menu->addAction(statusText);
        status->setEnabled(false);
    }
    else if (this->nameHistoryEntries_.empty())
    {
        auto *empty = menu->addAction(uiText("No name history found"));
        empty->setEnabled(false);
    }
    else
    {
        for (const auto &entry : this->nameHistoryEntries_)
        {
            auto *action = new QWidgetAction(menu);
            action->setDefaultWidget(new NameHistoryMenuRow(
                entry.login, entry.leftText, entry.rightText, menu));
            menu->addAction(action);
        }

        if (static_cast<int>(this->nameHistoryEntries_.size()) >=
            TWITCH_NAME_HISTORY_LIMIT)
        {
            menu->addSeparator();
            auto *limited =
                menu->addAction(uiText(QString("Showing latest %1 names"))
                                    .arg(TWITCH_NAME_HISTORY_LIMIT));
            limited->setEnabled(false);
        }
    }

    QWidget *anchor = this->ui_.actions;
    if (!anchor || anchor->visibleRegion().isEmpty())
    {
        anchor = this->ui_.profileScroll->viewport();
    }
    menu->popup(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void UserInfoPopup::requestNameHistory()
{
    if (this->userName_.isEmpty() || this->userId_.isEmpty() || this->isKick_ ||
        this->isYouTube_)
    {
        return;
    }

    const auto login = normalizeTwitchNameHistoryLogin(this->userName_);
    if (login.isEmpty())
    {
        return;
    }

    const auto generation = ++this->nameHistoryRequestGeneration_;
    const auto userId = this->userId_;
    this->nameHistoryLogin_ = login;
    this->nameHistoryEntries_.clear();
    this->nameHistoryLoading_ = true;
    this->nameHistoryLoaded_ = false;
    this->updateNameHistoryButton();
    this->openNameHistoryMenu("Fetching name history...");

    const QPointer<UserInfoPopup> self(this);

    fetchTwitchNameHistoryByUserId(
        userId, login,
        [self, generation, userId, login](TwitchNameHistory history) mutable {
            if (!self || generation != self->nameHistoryRequestGeneration_ ||
                self->userId_ != userId ||
                normalizeTwitchNameHistoryLogin(self->userName_) != login)
            {
                return;
            }

            self->nameHistoryLogin_ = login;
            self->nameHistoryEntries_ = std::move(history.entries);
            self->nameHistoryLoading_ = false;
            self->nameHistoryLoaded_ = true;
            self->updateNameHistoryButton();
            self->openNameHistoryMenu();
        },
        [self, generation, userId, login](const QString &error) {
            if (!self || generation != self->nameHistoryRequestGeneration_ ||
                self->userId_ != userId ||
                normalizeTwitchNameHistoryLogin(self->userName_) != login)
            {
                return;
            }

            qCWarning(chatterinoWidget)
                << "Failed to fetch name history:" << error;
            self->nameHistoryEntries_.clear();
            self->nameHistoryLoading_ = false;
            self->nameHistoryLoaded_ = false;
            self->updateNameHistoryButton();
            self->openNameHistoryMenu("Name history unavailable");
        });
}

QStringView UserInfoPopup::platformName() const
{
    if (this->isKick_)
    {
        return u"Kick";
    }
    return u"Twitch";
}

void UserInfoPopup::appendCommonProfileActions(QMenu *menu)
{
    if (!this->isKick_ && !this->userName_.isEmpty())
    {
        menu->addAction(uiText("Open profile in &ChatVault"), this,
                        [url = chatVaultTwitchChannelUrl(this->userName_)] {
                            QDesktopServices::openUrl(QUrl(url));
                        });

        menu->addAction(uiText("Open profile in Lur&kology"), this,
                        [url = lurkologyTwitchProfileUrl(this->userName_)] {
                            QDesktopServices::openUrl(QUrl(url));
                        });

        menu->addAction(uiText("Open channel &logs in browser"), this,
                        [username = this->userName_] {
                            QDesktopServices::openUrl(QUrl(
                                "https://lurkology.com/logs?c=" + username));
                        });
    }

    if (!this->seventvUserID_.isEmpty())
    {
        menu->addAction(
            uiText("Open &7TV user in browser"), this, [id = this->seventvUserID_] {
                QDesktopServices::openUrl(QUrl(SEVENTV_USER_PAGE % id));
            });
    }
}

void UserInfoPopup::executeUsercardModerationAction(
    const UsercardModerationRequest &request)
{
    if (!this->underlyingChannel_)
    {
        return;
    }

    QString value;
    switch (request.action)
    {
        case UsercardModerationAction::Ban: {
            value = appendModerationReason("/ban " + this->userName_,
                                           request.reason);
        }
        break;

        case UsercardModerationAction::Unban: {
            value = "/unban " + this->userName_;
        }
        break;

        case UsercardModerationAction::Timeout: {
            if (request.durationSeconds <= 0)
            {
                return;
            }

            value = appendModerationReason(
                "/timeout " + this->userName_ + " " +
                    QString::number(request.durationSeconds) + 's',
                request.reason);
        }
        break;
    }

    value = getApp()->getCommands()->execCommand(
        value, this->underlyingChannel_, false);
    this->underlyingChannel_->sendMessage(value);
}

void UserInfoPopup::showUsercardModerationReasonPopup(
    const UsercardModerationRequest &request)
{
    if (request.action == UsercardModerationAction::Unban)
    {
        this->executeUsercardModerationAction(request);
        return;
    }

    if (this->moderationReasonPopup_)
    {
        this->moderationReasonPopup_->close();
    }

    const bool wasPinned = this->ensurePinned();

    const auto isBan = request.action == UsercardModerationAction::Ban;
    const auto initialReason =
        getSettings()->timeoutReasonPromptPrefillSavedReason.getValue()
            ? request.reason
            : QString();
    auto *popup = new ModerationReasonPopup(
        isBan ? "Ban reason" : "Timeout reason", "optional reason",
        initialReason,
        getSettings()->timeoutReasonPromptShowSendButton.getValue(),
        [self = QPointer<UserInfoPopup>(this),
         request](QString reason) mutable {
            if (!self)
            {
                return;
            }

            auto updatedRequest = request;
            updatedRequest.reason = reason;
            updatedRequest.promptForReason = false;
            self->executeUsercardModerationAction(updatedRequest);
        },
        this);

    popup->setAttribute(Qt::WA_DeleteOnClose);
    this->moderationReasonPopup_ = popup;
    if (wasPinned)
    {
        auto didRestorePin = std::make_shared<bool>(false);
        auto restorePin = [self = QPointer<UserInfoPopup>(this),
                           didRestorePin] {
            if (*didRestorePin)
            {
                return;
            }
            *didRestorePin = true;

            if (self)
            {
                self->togglePinned();
            }
        };
        std::ignore = popup->closing.connect(restorePin);
        QObject::connect(popup, &QObject::destroyed, this,
                         std::move(restorePin));
    }
    popup->showCenteredAt(QCursor::pos());
    popup->activateWindow();
    popup->raise();
}

//
// TimeoutWidget
//
UserInfoPopup::TimeoutWidget::TimeoutWidget()
    : BaseWidget(nullptr)
{
    // One compact row: unban, the timeouts, ban. What each button does is in
    // its tooltip; the cross-channel actions sit next to unban / ban.
    auto layout = LayoutCreator<TimeoutWidget>(this)
                      .setLayoutType<QHBoxLayout>()
                      .withoutMargin();
    layout->setSpacing(6);

    const int buttonHeight = 26;
    const int timeoutWidth = 34;
    const QString reasonHint = uiText(
        "Use the configured reason prompt shortcut to edit the reason "
        "before sending.");

    const auto addButton = [&](UsercardModerationAction action,
                               const QString &title, const QPixmap &pixmap,
                               QWidget **buttonOut, QWidget **crossOut,
                               const QString &crossTooltip, bool crossIsBan) {
        auto *button = new PixmapButton(nullptr);
        layout->addWidget(button);
        button->setPixmap(pixmap);
        button->setScaleIndependentSize(buttonHeight, buttonHeight);
        button->setBorderColor(QColor(255, 255, 255, 127));
        button->setToolTip(action == UsercardModerationAction::Unban
                               ? uiText(title)
                               : uiText(title) + QStringLiteral("\n") +
                                     reasonHint);
        *buttonOut = button;

        QObject::connect(
            button, &Button::clicked,
            [this, action](Qt::MouseButton mouseButton) {
                if (!shouldHandleModerationButtonClick(mouseButton))
                {
                    return;
                }

                UsercardModerationRequest request;
                request.action = action;
                if (action == UsercardModerationAction::Ban)
                {
                    request.reason = timeoutBanReason();
                }
                request.promptForReason =
                    action != UsercardModerationAction::Unban &&
                    shouldPromptForModerationReason(mouseButton);

                this->buttonClicked.invoke(request);
            });

        auto *cross = new LabelButton("Cross", this, QSize{2, 0});
        layout->addWidget(cross);
        cross->setToolTip(crossTooltip);
        cross->setVisible(false);
        *crossOut = cross;
        QObject::connect(cross, &Button::leftClicked, this, [this, crossIsBan] {
            this->crossActionClicked.invoke(crossIsBan);
        });
    };

    const auto addTimeouts = [&] {
        auto *row = new QWidget;
        row->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        this->timeoutsColumn_ = row;
        layout->addWidget(row);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);

        int index = 0;
        for (const auto &item : getSettings()->timeoutButtons.getValue())
        {
            auto *button = new LabelButton;
            rowLayout->addWidget(button);
            const auto text = QString::number(item.second) + item.first;
            button->setPadding({0, 0});
            button->setText(text);
            button->setScaleIndependentSize(timeoutWidth, buttonHeight);
            button->setBorderColor(borderColor);

            const auto duration = calculateTimeoutDuration(item);
            const auto reason = timeoutButtonReason(index);
            this->timeoutButtons.emplace_back(button, duration);

            QObject::connect(button, &LabelButton::clicked,
                             [this, duration, reason](Qt::MouseButton mouse) {
                                 if (!shouldHandleModerationButtonClick(mouse))
                                 {
                                     return;
                                 }

                                 UsercardModerationRequest request;
                                 request.action =
                                     UsercardModerationAction::Timeout;
                                 request.durationSeconds = duration;
                                 request.reason = reason;
                                 request.promptForReason =
                                     shouldPromptForModerationReason(mouse);

                                 this->buttonClicked.invoke(request);
                             });
            button->setToolTip(uiText("Timeout for %1").arg(text) +
                               QStringLiteral("\n") + reasonHint);
            ++index;
        }
    };

    addButton(UsercardModerationAction::Unban, "Unban",
              getResources().buttons.unban, &this->unbanButton_,
              &this->crossUnbanButton_,
              "Unban this user in other channels you moderate", false);
    addTimeouts();
    addButton(UsercardModerationAction::Ban, "Ban", getResources().buttons.ban,
              &this->banButton_, &this->crossBanButton_,
              "Ban this user in other channels you moderate", true);
}

void UserInfoPopup::TimeoutWidget::paintEvent(QPaintEvent *)
{
    //    QPainter painter(this);

    //    painter.setPen(QColor(255, 255, 255, 63));

    //    painter.drawLine(0, this->height() / 2, this->width(), this->height()
    //    / 2);
}

void UserInfoPopup::TimeoutWidget::setMinTimeout(int minSecs)
{
    this->minTimeoutSecs_ = minSecs;
    this->applyModerationVisibility();
}

void UserInfoPopup::TimeoutWidget::setModerationVisible(bool visible)
{
    this->moderationVisible_ = visible;
    this->applyModerationVisibility();
}

void UserInfoPopup::TimeoutWidget::setCrossVisible(bool visible)
{
    if (this->crossUnbanButton_ != nullptr)
    {
        this->crossUnbanButton_->setVisible(visible);
    }
    if (this->crossBanButton_ != nullptr)
    {
        this->crossBanButton_->setVisible(visible);
    }
}

void UserInfoPopup::TimeoutWidget::applyModerationVisibility()
{
    if (this->unbanButton_ != nullptr)
    {
        this->unbanButton_->setVisible(this->moderationVisible_);
    }
    if (this->banButton_ != nullptr)
    {
        this->banButton_->setVisible(this->moderationVisible_);
    }
    if (this->timeoutsColumn_ != nullptr)
    {
        this->timeoutsColumn_->setVisible(this->moderationVisible_);
    }
    for (auto &[widget, dur] : this->timeoutButtons)
    {
        widget->setVisible(this->moderationVisible_ &&
                           dur >= this->minTimeoutSecs_);
    }
}

void UserInfoPopup::updateAvatarUrl()
{
    if (this->isTwitchAvatarShown_)
    {
        this->avatarUrl_ = this->helixAvatarUrl_;
    }
    else
    {
        this->avatarUrl_ = this->seventvAvatarUrl_;
    }
}

void UserInfoPopup::refreshSeventvPaint()
{
    if (!getSettings()->showUsercardSevenTVPaint)
    {
        this->seventvPaint_ = nullptr;
        if (this->ui_.seventvPaintRow)
        {
            this->ui_.seventvPaintRow->hide();
        }
        this->updateNamePresentation();
        return;
    }

    const auto paint = getApp()->getSeventvPaints()->getPaint(
        this->userName_.toLower(), this->isKick_);
    this->seventvPaint_ = paint;
    this->updateNamePresentation();

    if (!paint)
    {
        this->ui_.seventvPaintRow->hide();
        // Not seen in chat yet: ask 7TV (once per run) and look again.
        if (!this->isKick_ && !this->offlinePreview_ &&
            !this->userName_.isEmpty() && !this->userId_.isEmpty() &&
            !this->seventvPaintAsked_)
        {
            this->seventvPaintAsked_ = true;
            getApp()->getSeventvPaints()->noteTwitchChatter(
                this->userName_.toLower(), this->userId_);
            QTimer::singleShot(2500, this, [this] {
                if (!this->seventvPaint_)
                {
                    this->refreshSeventvPaint();
                }
            });
        }
        return;
    }

    this->ui_.seventvPaintRow->show();
    this->updateSeventvPaintPixmap();
}

void UserInfoPopup::previewModerationStrip()
{
    if (!this->offlinePreview_ || !this->ui_.timeoutWidget)
    {
        return;
    }
    this->ui_.timeoutWidget->setModerationVisible(true);
    this->ui_.timeoutWidget->setCrossVisible(false);
    this->ui_.timeoutWidget->setVisible(true);
    this->setModerationStripVisible(true);
}

void UserInfoPopup::setModerationStripVisible(bool visible)
{
    auto *strip = this->ui_.moderationStrip;
    if (strip == nullptr || strip->isVisibleTo(this) == visible)
    {
        return;
    }
    // The card grows by the strip instead of squeezing the profile column.
    const int stripHeight = std::max(strip->sizeHint().height(), 1);
    strip->setVisible(visible);
    auto geometry = this->geometry();
    geometry.setHeight(geometry.height() +
                       (visible ? stripHeight : -stripHeight));
    if (auto *screen = this->screen(); screen != nullptr && visible)
    {
        const auto available = screen->availableGeometry();
        if (geometry.height() > available.height())
        {
            geometry.setHeight(available.height());
        }
        if (geometry.bottom() > available.bottom())
        {
            geometry.moveBottom(available.bottom());
        }
    }
    if (this->badgeFitBase_)
    {
        // Asynchronously obtained moderator permissions can add/remove this
        // strip during a badge expansion. Retain the session, including the
        // collapsed height the user will return to.
        const int delta = visible ? stripHeight : -stripHeight;
        const int cap = std::max(this->minimumHeight(), this->screen()->availableGeometry().height());
        this->badgeFitBase_->setHeight(std::clamp(this->badgeFitBase_->height() + delta,
                                               this->minimumHeight(), cap));
        this->badgeFitTargetHeight_ = std::clamp(this->badgeFitTargetHeight_ + delta,
                                               this->badgeFitBase_->height(), cap);
        this->badgeFitExpectedSize_ = geometry.size();
    }
    this->setGeometry(geometry);
}

void UserInfoPopup::updateSeventvPaintPixmap()
{
    this->updateNamePresentation();
    const auto paint = this->seventvPaint_;
    if (!paint || !this->ui_.seventvPaintPixmapLabel)
    {
        return;
    }

    const auto font =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
    const auto dpr = this->devicePixelRatioF();
    const QString paintName = paint->name.isEmpty() ? paint->id : paint->name;

    const QFontMetricsF metrics(font);
    const int lineHeight = std::max(1, qRound(metrics.height()));
    const int maxPaintWidth =
        this->ui_.profileScroll
            ? std::max(1, this->ui_.profileScroll->viewport()->width() -
                              int(130 * this->scale()))
            : int(220 * this->scale());
    const auto visiblePaintName =
        metrics.elidedText(paintName, Qt::ElideRight, maxPaintWidth);
    const int paintWidth =
        std::max(1, qRound(metrics.horizontalAdvance(visiblePaintName)));
    const QSizeF size(paintWidth, lineHeight);

    this->ui_.seventvPaintPixmapLabel->setFixedSize(paintWidth, lineHeight);

    QColor userColor = Qt::white;
    if (this->ui_.userColorRow)
    {
        const auto colorText =
            this->ui_.userColorRow->property("swatch-color").toString();
        if (!colorText.isEmpty())
        {
            userColor = QColor(colorText);
        }
    }

    const auto pixmap = paint->getPixmap(visiblePaintName, font, userColor,
                                         size, this->scale(), dpr, true);

    this->ui_.seventvPaintPixmapLabel->setPixmap(pixmap);
    this->ui_.seventvPaintPixmapLabel->setToolTip(paintName);
}

UserInfoPopup::~UserInfoPopup()
{
    unregisterActiveUsercard(this);
}

void UserInfoPopup::notifyFollowMutation(const QString &targetId,
                                         const QString &requestUserId,
                                         const QString &requestLogin,
                                         bool unfollow)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    bool accountMatches = false;
    if (current && !current->isAnon())
    {
        const auto currentUserId = current->getUserId().trimmed();
        const auto normalizedUserId = requestUserId.trimmed();
        if (!currentUserId.isEmpty() && !normalizedUserId.isEmpty())
        {
            accountMatches = normalizedUserId == currentUserId;
        }
        else
        {
            const auto currentLogin =
                current->getUserName().trimmed().toLower();
            const auto normalizedLogin = requestLogin.trimmed().toLower();
            accountMatches = !currentLogin.isEmpty() &&
                             !normalizedLogin.isEmpty() &&
                             normalizedLogin == currentLogin;
        }
    }

    if (!accountMatches)
    {
        return;
    }

    QMutexLocker locker(&activeUsercardsMutex());
    for (const auto &popup : activeUsercards())
    {
        if (!popup || popup->userId_ != targetId)
        {
            continue;
        }

        std::optional<QDateTime> followedAt;
        if (!unfollow)
        {
            followedAt = QDateTime::currentDateTimeUtc();
        }
        popup->setFollowingStatus(!unfollow, followedAt);
    }
}

bool UserInfoPopup::isFollowing() const
{
    return this->following_;
}

bool UserInfoPopup::isFollowingStatusKnown() const
{
    return this->followingStatusKnown_;
}

void UserInfoPopup::setFollowingStatus(bool following,
                                       std::optional<QDateTime> followedAt)
{
    const bool changed = this->followingStatusKnown_ != true ||
                         this->following_ != following ||
                         this->followedAt_ != followedAt;

    this->following_ = following;
    this->followingStatusKnown_ = true;
    this->followedAt_ = std::move(followedAt);
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account && !account->isAnon())
    {
        this->followingStatusUserId_ = account->getUserId();
    }

    if (changed)
    {
        this->followingStatusChanged_.invoke();
    }
}

void UserInfoPopup::resetFollowingStatus()
{
    const bool changed = this->followingStatusKnown_ || this->following_;
    this->following_ = false;
    this->followingStatusKnown_ = false;
    this->followedAt_.reset();
    this->followingStatusUserId_.clear();
    this->followingStatusFetchInFlight_.store(false);

    if (changed)
    {
        this->followingStatusChanged_.invoke();
    }
}

void UserInfoPopup::refreshFollowingStatus(bool force)
{
    if (getApp()->isTest())
    {
        return;
    }

    const auto targetUserId = this->userId_;
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    const auto userId =
        account && !account->isAnon() ? account->getUserId() : QString();

    auto clearUnknown = [this] {
        const bool changed = this->followingStatusKnown_ || this->following_;
        this->following_ = false;
        this->followingStatusKnown_ = false;
        this->followedAt_.reset();
        this->followingStatusUserId_.clear();
        if (changed)
        {
            this->followingStatusChanged_.invoke();
        }
    };

    if (targetUserId.isEmpty() || userId.isEmpty())
    {
        clearUnknown();
        return;
    }

    if (userId == targetUserId)
    {
        this->followingStatusUserId_ = userId;
        this->setFollowingStatus(false);
        return;
    }

    if (!force && this->followingStatusKnown_ &&
        this->followingStatusUserId_ == userId)
    {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (!force && this->lastFollowingStatusRefreshAt_.isValid() &&
        this->lastFollowingStatusRefreshAt_.msecsTo(now) <
            FOLLOWING_STATUS_RETRY_INTERVAL_MS)
    {
        return;
    }

    if (this->followingStatusFetchInFlight_.exchange(true))
    {
        return;
    }

    this->lastFollowingStatusRefreshAt_ = now;
    const auto requestUserId = userId;
    const auto requestTargetUserId = targetUserId;
    QPointer<UserInfoPopup> self(this);

    getHelix()->getFollowedChannel(
        requestUserId, requestTargetUserId, this,
        [self, requestUserId, requestTargetUserId](const auto &chan) {
            if (!self)
            {
                return;
            }

            self->followingStatusFetchInFlight_.store(false);

            auto current = getApp()->getAccounts()->twitch.getCurrent();
            const auto currentUserId = current && !current->isAnon()
                                           ? current->getUserId()
                                           : QString();
            if (currentUserId != requestUserId ||
                self->userId_ != requestTargetUserId)
            {
                self->refreshFollowingStatus(true);
                return;
            }

            self->followingStatusUserId_ = requestUserId;
            self->setFollowingStatus(
                chan.has_value(),
                chan ? std::optional<QDateTime>(chan->followedAt)
                     : std::nullopt);
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }

            self->followingStatusFetchInFlight_.store(false);
            qCDebug(chatterinoTwitch)
                << "Failed to refresh usercard following status for"
                << self->userName_ << ':' << error;
        });
}

void UserInfoPopup::updateLiveIndicatorDisplay()
{
    if (this->ui_.liveIndicator == nullptr)
    {
        return;
    }

    this->ui_.liveIndicator->setTextMode(
        getSettings()->showUsercardLiveViewerCount);

    if (this->isUserLive_)
    {
        this->ui_.liveIndicator->setViewers(this->liveViewerCount_);
        this->ui_.liveIndicator->show();
    }
    else
    {
        this->ui_.liveIndicator->hide();
    }
}

void UserInfoPopup::updateUsercardFollowButton()
{
    if (this->ui_.followButton == nullptr)
    {
        return;
    }

    if (!getSettings()->showFollowButtonInUsercard || this->isKick_ ||
        this->userId_.isEmpty() ||
        !canUseFollowButtonForUser(this->userId_, this->userName_))
    {
        this->ui_.followButton->hide();
        return;
    }

    const auto following =
        this->isFollowingStatusKnown() && this->isFollowing();
    const auto displayName =
        this->ui_.localizedNameLabel->isVisible() &&
                !this->ui_.localizedNameLabel->getText().isEmpty()
            ? this->ui_.localizedNameLabel->getText()
            : (this->ui_.nameLabel->getText().isEmpty()
                   ? this->userName_
                   : this->ui_.nameLabel->getText());

    this->ui_.followButton->setSource(followButtonSource(following));
    this->ui_.followButton->setToolTip(
        formatFollowButtonToolTip(displayName, following, this->followedAt_));
    const int buttonSize = std::max(1, int(28 * this->scale()));
    this->ui_.followButton->setFixedSize(buttonSize, buttonSize);
    this->ui_.followButton->show();
}

void UserInfoPopup::toggleUsercardFollow()
{
    if (this->isKick_ || this->userId_.isEmpty() || this->userName_.isEmpty())
    {
        return;
    }

    const auto command = this->isFollowingStatusKnown() && this->isFollowing()
                             ? QStringLiteral("/unfollow")
                             : QStringLiteral("/follow");

    if (command == QStringLiteral("/unfollow") &&
        getSettings()->confirmUnfollowFromUsercard)
    {
        const auto displayName =
            this->ui_.localizedNameLabel->isVisible() &&
                    !this->ui_.localizedNameLabel->getText().isEmpty()
                ? this->ui_.localizedNameLabel->getText()
                : (this->ui_.nameLabel->getText().isEmpty()
                       ? this->userName_
                       : this->ui_.nameLabel->getText());

        const bool wasPinned = this->ensurePinned();

        QMessageBox box(this);
        box.setWindowTitle(uiText("Unfollow channel?"));
        box.setIcon(QMessageBox::Question);
        box.setText(
            uiText(QString("Are you sure you want to unfollow %1?")).arg(displayName));

        auto *confirmButton =
            box.addButton(uiText("Confirm"), QMessageBox::DestructiveRole);
        auto *cancelButton = box.addButton(uiText("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(cancelButton);
        box.setEscapeButton(cancelButton);
        box.exec();

        if (wasPinned)
        {
            this->togglePinned();
        }

        if (box.clickedButton() != confirmButton)
        {
            return;
        }
    }

    CommandContext ctx{
        .words = {command, this->userName_},
        .rawText = command % u' ' % this->userName_,
        .channel = this->channel_,
        .twitchChannel =
            dynamic_cast<TwitchChannel *>(this->underlyingChannel_.get()),
        .kickChannel = nullptr,
    };
    const auto text = command == QStringLiteral("/unfollow")
                          ? commands::unfollow(ctx)
                          : commands::follow(ctx);
    if (!text.isEmpty())
    {
        this->channel_->sendMessage(text);
    }
}

void UserInfoPopup::loadCurrentAvatar()
{
    auto uid = this->userId_;
    if (uid.startsWith(u"kick:"))
    {
        uid = uid.mid(5);
    }

    if (getApp()->getStreamerMode()->isEnabled() &&
        getSettings()->streamerModeHideUsercardAvatars)
    {
        this->ui_.avatarButton->setPixmap(getResources().streamerMode);
        if (getSettings()->showSevenTVUsercardButton)
        {
            this->loadSevenTVAvatar(uid, this->isKick_, false);
        }
    }
    else
    {
        this->loadAvatar(uid, this->helixAvatarUrl_, this->isKick_);
    }
}

}  // namespace chatterino
