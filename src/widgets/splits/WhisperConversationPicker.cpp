#include "widgets/splits/WhisperConversationPicker.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/Split.hpp"

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace chatterino {

WhisperConversationPicker::WhisperConversationPicker(
    Split *split, std::function<void(const QString &)> selected)
    : QPushButton(split)
    , split_(split)
    , panel_(new QFrame(split))
    , list_(new QListWidget(this->panel_))
    , selected_(std::move(selected))
{
    this->setObjectName("WhisperConversations");
    this->setCursor(Qt::PointingHandCursor);
    this->setFocusPolicy(Qt::StrongFocus);
    this->panel_->setObjectName("WhisperConversationPanel");
    this->panel_->hide();
    this->list_->setObjectName("WhisperConversationList");
    this->list_->setAccessibleName(uiText("Conversations"));
    this->list_->setUniformItemSizes(true);
    this->list_->setMouseTracking(true);
    this->list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->list_->viewport()->setAutoFillBackground(false);
    this->list_->setCursor(Qt::PointingHandCursor);
    this->reveal_.setDuration(145);
    this->reveal_.setEasingCurve(QEasingCurve::InOutCubic);
    this->reveal_.setStartValue(0.0);
    this->reveal_.setEndValue(1.0);
    this->refreshStyle();
    this->reset();
    connect(this, &QPushButton::clicked, this, [this] {
        this->setOpen(!this->open_);
    });
    connect(this->list_, &QListWidget::itemClicked, this,
            &WhisperConversationPicker::choose);
    connect(this->list_, &QListWidget::itemActivated, this,
            &WhisperConversationPicker::choose);
    connect(&this->reveal_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                this->progress_ = value.toReal();
                this->positionPanel();
                this->update();
            });
    connect(&this->reveal_, &QVariantAnimation::finished, this, [this] {
        if (!this->open_)
            this->dismiss();
    });
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *to) {
                if (this->open_ && to != this && to != this->list_ &&
                    (!to || !this->panel_->isAncestorOf(to)))
                    this->setOpen(false);
            });
}

int WhisperConversationPicker::rowHeight() const
{
    return this->fontMetrics().height() + qRound(10 * this->split_->scale());
}

void WhisperConversationPicker::refreshStyle()
{
    const auto scale = this->split_->scale();
    this->setFont(
        getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, scale));
    this->list_->setFont(this->font());
    this->setFixedHeight(this->rowHeight() + qRound(8 * scale));
    this->setMaximumWidth(qRound(300 * scale));
    const auto *theme = getApp()->getThemes();
    this->panel_->setStyleSheet(
        QStringLiteral(
            "QFrame#WhisperConversationPanel { background: %1; "
            "border: 1px solid %2; border-radius: 9px; }"
            "QListWidget { background: transparent; color: %3; "
            "border: none; outline: none; }"
            "QListWidget::item { height: %4px; padding: 0 8px; border-radius: "
            "5px; }"
            "QListWidget::item:hover { background: %5; }"
            "QListWidget::item:selected { background: %6; color: %3; }"
            "QScrollBar:vertical { background: transparent; width: 5px; }"
            "QScrollBar::handle:vertical { background: %7; min-height: 12px; "
            "border-radius: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { "
            "height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { "
            "background: transparent; }")
            .arg(theme->splits.input.background.name(),
                 theme->tabs.dividerLine.name(),
                 theme->messages.textColors.regular.name(),
                 QString::number(this->rowHeight()),
                 theme->tabs.regular.backgrounds.hover.name(),
                 theme->tabs.selected.backgrounds.regular.name(),
                 theme->scrollbars.thumb.name()));
    if (this->panel_->isVisible())
        this->positionPanel();
    this->update();
}

void WhisperConversationPicker::reset()
{
    this->dismiss();
    this->peer_.clear();
    this->peers_.clear();
    this->list_->clear();
    this->list_->addItem(uiText("All conversations"));
    this->setText(this->list_->item(0)->text());
}

void WhisperConversationPicker::addPeer(const QString &login,
                                        const QString &displayName)
{
    if (login.isEmpty())
        return;
    const auto key = login.toLower();
    auto *item = this->peers_.value(key);
    if (!item)
    {
        item = new QListWidgetItem(login, this->list_);
        item->setData(Qt::UserRole, key);
        this->peers_.insert(key, item);
    }
    if (displayName.compare(key, Qt::CaseInsensitive) == 0 &&
        displayName != key)
        item->setText(displayName);
    if (key == this->peer_)
        this->setText(item->text());
}

void WhisperConversationPicker::selectPeer(const QString &login)
{
    this->addPeer(login, login);
    this->peer_ = login.toLower();
    auto *item = this->peers_.value(this->peer_, this->list_->item(0));
    this->list_->setCurrentItem(item);
    this->setText(item->text());
}

const QString &WhisperConversationPicker::peer() const
{
    return this->peer_;
}

void WhisperConversationPicker::choose(QListWidgetItem *item)
{
    if (!this->open_ || !item)
        return;
    const auto next = item->data(Qt::UserRole).toString();
    this->setOpen(false);
    if (next != this->peer_)
        this->selected_(next);
    this->split_->setFocus(Qt::OtherFocusReason);
}

void WhisperConversationPicker::positionPanel()
{
    const int inset = std::max(3, qRound(5 * this->split_->scale()));
    const int top = this->y() + this->height();
    const int fullHeight = this->visibleRows_ * this->rowHeight() + 2 * inset;
    const int height =
        std::min(fullHeight, std::max(0, this->split_->height() - top - inset));
    const int width = std::max(0, this->width() - 2 * inset);
    // Clip the fully laid-out list by its parent. Rows and scrollbar keep the
    // same viewport throughout the reveal. The chat layout never participates.
    this->list_->setGeometry(inset, inset, std::max(0, width - 2 * inset),
                             std::max(0, height - 2 * inset));
    this->panel_->setGeometry(this->x() + inset, top, width,
                              qRound(height * this->progress_));
}

void WhisperConversationPicker::setOpen(bool open)
{
    if (this->open_ == open)
        return;
    this->open_ = open;
    if (open)
    {
        // An incoming whisper may add a row while the user is choosing.
        // Grow on the next opening; keep the current pointer target still.
        if (this->panel_->isHidden())
            this->visibleRows_ = std::clamp(this->list_->count(), 1, 9);
        this->positionPanel();
        this->panel_->show();
        this->panel_->raise();
        this->list_->setCurrentItem(
            this->peers_.value(this->peer_, this->list_->item(0)));
        this->list_->scrollToItem(this->list_->currentItem());
        qApp->installEventFilter(this);
        this->list_->setFocus(Qt::PopupFocusReason);
    }
    this->panel_->setAttribute(Qt::WA_TransparentForMouseEvents, !open);
    this->reveal_.setDirection(open ? QAbstractAnimation::Forward
                                    : QAbstractAnimation::Backward);
    if (this->reveal_.state() != QAbstractAnimation::Running)
        this->reveal_.start();
}

void WhisperConversationPicker::dismiss()
{
    this->reveal_.stop();
    qApp->removeEventFilter(this);
    this->open_ = false;
    this->progress_ = 0.0;
    this->panel_->hide();
    this->update();
}

void WhisperConversationPicker::hideEvent(QHideEvent *event)
{
    this->dismiss();
    QPushButton::hideEvent(event);
}

void WhisperConversationPicker::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Return ||
        event->key() == Qt::Key_Enter)
    {
        this->setOpen(true);
        event->accept();
        return;
    }
    QPushButton::keyPressEvent(event);
}

bool WhisperConversationPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate && watched == this->window())
        this->dismiss();
    if (this->open_ && watched == this->list_ &&
        (event->type() == QEvent::ShortcutOverride ||
         event->type() == QEvent::KeyPress))
    {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return ||
            key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space)
        {
            if (event->type() == QEvent::KeyPress)
            {
                if (key->key() == Qt::Key_Escape)
                {
                    this->setOpen(false);
                    this->setFocus(Qt::OtherFocusReason);
                }
                else
                    this->choose(this->list_->currentItem());
            }
            key->accept();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress && this->open_)
    {
        const auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && widget != this && widget != this->panel_ &&
            !this->panel_->isAncestorOf(widget))
            this->setOpen(false);
    }
    else if ((event->type() == QEvent::Resize ||
              event->type() == QEvent::Move) &&
             (watched == this || watched == this->split_))
        this->positionPanel();
    return QPushButton::eventFilter(watched, event);
}

void WhisperConversationPicker::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!this->isEnabled())
        painter.setOpacity(0.55);
    const auto *theme = getApp()->getThemes();
    const qreal scale = this->split_->scale();
    const QRectF bounds =
        QRectF(this->rect())
            .adjusted(5 * scale, 3 * scale, -5 * scale, -3 * scale);
    const auto &tab = theme->tabs.selected;
    painter.setPen(QPen(this->hasFocus() || this->open_
                            ? tab.line.regular
                            : theme->tabs.dividerLine,
                        1));
    painter.setBrush(this->underMouse() ? theme->tabs.regular.backgrounds.hover
                                        : theme->splits.input.background);
    painter.drawRoundedRect(bounds, 7 * scale, 7 * scale);
    const QPointF center(bounds.left() + 16 * scale, bounds.center().y());
    painter.save();
    painter.translate(center);
    painter.scale(scale, scale);
    painter.setPen(QPen(tab.line.regular, 1.4, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(-6.5, -4.5, 13, 9), 2, 2);
    QPainterPath flap;
    flap.moveTo(-6, -3);
    flap.lineTo(0, 1);
    flap.lineTo(6, -3);
    painter.drawPath(flap);
    painter.restore();
    const QRect textBounds =
        bounds.adjusted(31 * scale, 0, -27 * scale, 0).toAlignedRect();
    painter.setFont(this->font());
    painter.setPen(theme->messages.textColors.regular);
    painter.drawText(textBounds, Qt::AlignVCenter | Qt::AlignLeft,
                     this->fontMetrics().elidedText(
                         this->text(), Qt::ElideRight, textBounds.width()));
    painter.translate(bounds.right() - 14 * scale, center.y());
    painter.scale(scale, scale);
    painter.rotate(180.0 * this->progress_);
    painter.setPen(QPen(theme->messages.textColors.system, 1.5, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    QPainterPath arrow;
    arrow.moveTo(-3.5, -1.5);
    arrow.lineTo(0, 2);
    arrow.lineTo(3.5, -1.5);
    painter.drawPath(arrow);
}

}  // namespace chatterino
