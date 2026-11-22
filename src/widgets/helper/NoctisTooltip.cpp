// SPDX-License-Identifier: MIT

#include "widgets/helper/NoctisTooltip.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/BaseWidget.hpp"

#include <QAbstractButton>
#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScopedValueRollback>
#include <QStyleOptionViewItem>
#include <QTabBar>
#include <QTextDocument>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>

namespace chatterino::noctistooltip {
namespace {

float tooltipScale(QWidget *owner)
{
    for (auto *widget = owner; widget; widget = widget->parentWidget())
    {
        if (auto *base = dynamic_cast<BaseWidget *>(widget))
        {
            return std::clamp(base->scale(), .75F, 3.F);
        }
    }
    return std::clamp(getSettings()->uiScale.getValue(), .75F, 3.F);
}

// The text document's word-or-anywhere wrap handles URLs/long tokens while
// retaining existing line breaks and rich tooltip text. QLabel still carries
// the actual string for accessible inspection.
class TooltipText final : public QLabel
{
public:
    explicit TooltipText(QWidget *parent) : QLabel(parent)
    {
        this->setObjectName("NoctisTooltipText");
        document_.setDocumentMargin(0);
        this->setAutoFillBackground(false);
    }

    void configure(const QString &text, const QFont &font, QColor color,
                   int maxWidth, int maxHeight)
    {
        this->setFont(font);
        this->setText(text);
        auto palette = this->palette();
        palette.setColor(QPalette::WindowText, color);
        this->setPalette(palette);
        document_.setDefaultFont(font);
        document_.setDefaultStyleSheet(QStringLiteral("body { color:%1; }")
                                           .arg(color.name()));
        auto option = document_.defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document_.setDefaultTextOption(option);
        if (Qt::mightBeRichText(text))
        {
            document_.setHtml(text);
        }
        else
        {
            document_.setPlainText(text);
        }
        document_.setTextWidth(-1);
        const int width = qBound(1, qCeil(document_.idealWidth()), maxWidth);
        document_.setTextWidth(width);
        this->setFixedSize(width,
                           qBound(1, qCeil(document_.size().height()), maxHeight));
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing);
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = this->palette();
        context.palette.setColor(QPalette::Text,
                                 this->palette().color(QPalette::WindowText));
        context.clip = this->rect();
        document_.documentLayout()->draw(&painter, context);
    }

private:
    QTextDocument document_;
};

class TextTooltip final : public QWidget
{
public:
    TextTooltip()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint |
                               Qt::WindowTransparentForInput |
                               Qt::WindowDoesNotAcceptFocus)
        , label_(new TooltipText(this))
    {
        this->setObjectName("NoctisTextTooltip");
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->setAttribute(Qt::WA_ShowWithoutActivating);
        this->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->setAutoFillBackground(false);
        this->setFocusPolicy(Qt::NoFocus);
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(label_);
        layout->setSizeConstraint(QLayout::SetFixedSize);
    }

    void configure(const QString &text, QWidget *owner, const QRect &available)
    {
        const auto scale = tooltipScale(owner);
        const auto *theme = getApp()->getThemes();
        background_ = theme->messages.backgrounds.alternate;
        background_.setAlpha(249);
        border_ = theme->tabs.dividerLine;
        radius_ = 8 * scale;
        const int horizontal = qRound(10 * scale);
        const int vertical = qRound(7 * scale);
        this->layout()->setContentsMargins(horizontal, vertical,
                                           horizontal, vertical);
        const int maxWidth = std::max(1, std::min(qRound(420 * scale),
                                                  available.width() - 2 * horizontal));
        const int maxHeight = std::max(1, available.height() - 2 * vertical);
        label_->configure(text,
                           getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, scale),
                           theme->messages.textColors.regular, maxWidth, maxHeight);
        this->adjustSize();
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(this->rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(border_, 1));
        painter.setBrush(background_);
        painter.drawRoundedRect(QRectF(this->rect()).adjusted(.5, .5, -.5, -.5),
                                radius_, radius_);
    }

private:
    TooltipText *label_;
    QColor background_;
    QColor border_;
    qreal radius_ = 8;
};

struct Tip {
    QString text;
    QRect region;
    bool redundant = false;
};

bool equalPlainText(const QString &text, const QString &visible)
{
    return !text.isEmpty() && text == visible && !Qt::mightBeRichText(text);
}

// If callers set a tooltip to reveal a clipped label/row, the popup is useful
// only while its displayed text is actually clipped.
Tip tooltipAt(QWidget *widget, const QPoint &position)
{
    if (auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget());
        view && view->viewport() == widget)
    {
        if (auto *header = qobject_cast<QHeaderView *>(view))
        {
            const int section = header->logicalIndexAt(position);
            if (section >= 0 && header->model())
            {
                const auto text = header->model()->headerData(
                    section, header->orientation(), Qt::ToolTipRole).toString();
                if (!text.isEmpty())
                {
                    const auto start = header->sectionViewportPosition(section);
                    const auto size = header->sectionSize(section);
                    const bool horizontal = header->orientation() == Qt::Horizontal;
                    const auto region = horizontal
                                            ? QRect(start, 0, size, widget->height())
                                            : QRect(0, start, widget->width(), size);
                    return {text, region};
                }
            }
        }
        else
        {
            const auto index = view->indexAt(position);
            const auto text = index.data(Qt::ToolTipRole).toString();
            if (!text.isEmpty())
            {
                const auto region = view->visualRect(index);
                bool fits = false;
                if (equalPlainText(text, index.data(Qt::DisplayRole).toString()))
                {
                    QStyleOptionViewItem option;
                    option.initFrom(view);
                    option.rect = region;
                    option.font = view->font();
                    if (const auto *delegate = view->itemDelegateForIndex(index))
                    {
                        const auto wanted = delegate->sizeHint(option, index);
                        fits = wanted.width() <= region.width() &&
                               wanted.height() <= region.height() &&
                               widget->rect().contains(region);
                    }
                }
                return {text, region, fits};
            }
        }
    }
    if (auto *bar = qobject_cast<QTabBar *>(widget))
    {
        const int index = bar->tabAt(position);
        if (index >= 0 && !bar->tabToolTip(index).isEmpty())
        {
            return {bar->tabToolTip(index), bar->tabRect(index)};
        }
    }
    if (auto *menu = qobject_cast<QMenu *>(widget); menu && menu->toolTipsVisible())
    {
        if (auto *action = menu->actionAt(position); action && !action->toolTip().isEmpty())
        {
            auto visible = action->text();
            visible.remove('&');
            const auto text = action->toolTip();
            return {text, menu->actionGeometry(action), equalPlainText(text, visible)};
        }
    }
    const auto text = widget->toolTip();
    bool fits = false;
    if (const auto *label = qobject_cast<QLabel *>(widget);
        label && equalPlainText(text, label->text()))
    {
        const auto bounds = label->contentsRect().adjusted(label->margin(), label->margin(),
                                                          -label->margin(), -label->margin());
        const auto metrics = label->fontMetrics();
        const auto flags = (label->wordWrap() ? Qt::TextWordWrap : 0) |
                           Qt::TextExpandTabs;
        const auto wanted = metrics.boundingRect(bounds, flags, text);
        fits = wanted.width() <= bounds.width() && wanted.height() <= bounds.height();
    }
    else if (const auto *button = qobject_cast<QAbstractButton *>(widget);
             button && equalPlainText(text, button->text()))
    {
        const auto reserve = button->icon().isNull() ? 10 : button->iconSize().width() + 16;
        fits = button->fontMetrics().horizontalAdvance(text) <=
               button->contentsRect().width() - reserve;
    }
    return {text, widget->rect(), fits};
}

class Controller final : public QObject
{
public:
    explicit Controller(QApplication *app) : QObject(app)
    {
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] { this->hideTip(); });
        connect(app, &QCoreApplication::aboutToQuit, this, [this] { this->hideTip(); });
        app->installEventFilter(this);
    }

    ~Controller() override
    {
        delete tooltip_.data();
    }

    void showTip(const QPoint &position, const QString &text, QWidget *owner,
                 const QRect &region, int durationMs = -1)
    {
        if (text.isEmpty() || !owner || !owner->isVisible() ||
            !tryGetApp() || isAppAboutToQuit())
        {
            this->hideTip();
            return;
        }
        const auto bounds = region.isEmpty() ? owner->rect() : region;
        if (tooltip_ && tooltip_->isVisible() && owner_ == owner &&
            text_ == text && region_ == bounds && durationMs <= 0 &&
            !transient_)
        {
            // Repeated mouse-move handlers must not resize/move an unchanged
            // popup. Keep it at the original cursor anchor.
            return;
        }
        if (!tooltip_)
        {
            tooltip_ = new TextTooltip;
        }
        QScopedValueRollback<bool> updating(updating_, true);
        QObject::disconnect(ownerDestroyed_);
        owner_ = owner;
        ownerDestroyed_ = connect(owner, &QObject::destroyed, this,
                                  [this] { this->hideTip(); });
        text_ = text;
        region_ = bounds;
        transient_ = durationMs > 0;
        auto *screen = QGuiApplication::screenAt(position);
        if (!screen)
        {
            screen = owner->screen();
        }
        const auto available = screen->availableGeometry();
        tooltip_->configure(text, owner, available);
        auto place = position + QPoint(12, 19);
        if (place.y() + tooltip_->height() > available.bottom())
        {
            place.setY(position.y() - tooltip_->height() - 8);
        }
        place.setX(qBound(available.left(), place.x(),
                          std::max(available.left(), available.right() - tooltip_->width() + 1)));
        place.setY(qBound(available.top(), place.y(),
                          std::max(available.top(), available.bottom() - tooltip_->height() + 1)));
        tooltip_->move(place);
        tooltip_->show();
        tooltip_->raise();
        const int hoverDuration = 3000 + int(std::min(qsizetype(400), text.size())) * 45;
        timer_.start(transient_ ? durationMs : std::clamp(hoverDuration, 7000, 18000));
    }

    void hideTip()
    {
        timer_.stop();
        QObject::disconnect(ownerDestroyed_);
        owner_.clear();
        region_ = {};
        text_.clear();
        transient_ = false;
        if (tooltip_)
        {
            tooltip_->hide();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (updating_ || (!owner_ && event->type() != QEvent::ToolTip))
        {
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && tooltip_ &&
            (widget == tooltip_ || tooltip_->isAncestorOf(widget)))
        {
            return false;
        }
        if (event->type() == QEvent::ToolTip && widget && tryGetApp() &&
            !isAppAboutToQuit())
        {
            auto *help = static_cast<QHelpEvent *>(event);
            const auto tip = tooltipAt(widget, help->pos());
            if (!tip.text.isEmpty())
            {
                if (transient_)
                {
                    event->accept();
                    return true;
                }
                if (tip.redundant)
                {
                    this->hideTip();
                }
                else
                {
                    this->showTip(help->globalPos(), tip.text, widget, tip.region);
                }
                event->accept();
                return true;
            }
            // Custom hover widgets may handle an otherwise-empty help event
            // themselves, notably the rich badge previews.
            if (!transient_ && owner_ != widget)
            {
                this->hideTip();
            }
            return false;
        }
        if (!owner_)
        {
            return false;
        }
        switch (event->type())
        {
            case QEvent::MouseButtonPress:
            case QEvent::Wheel:
            case QEvent::KeyPress:
            case QEvent::ApplicationDeactivate:
                this->hideTip();
                break;
            case QEvent::MouseMove: {
                const auto position = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
                if (!transient_ &&
                    (!region_.contains(owner_->mapFromGlobal(position)) ||
                     (widget && widget != owner_ &&
                      !owner_->isAncestorOf(widget) && !widget->isAncestorOf(owner_))))
                {
                    this->hideTip();
                }
                break;
            }
            case QEvent::Leave:
                if (!transient_ && widget == owner_)
                {
                    this->hideTip();
                }
                break;
            case QEvent::Hide:
            case QEvent::Close:
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::WindowDeactivate:
                if (widget && (widget == owner_ || widget->isAncestorOf(owner_)))
                {
                    this->hideTip();
                }
                break;
            case QEvent::ToolTipChange:
                if (widget == owner_)
                {
                    this->hideTip();
                }
                break;
            default:
                break;
        }
        return false;
    }

private:
    QPointer<TextTooltip> tooltip_;
    QPointer<QWidget> owner_;
    QMetaObject::Connection ownerDestroyed_;
    QRect region_;
    QString text_;
    QTimer timer_;
    bool transient_ = false;
    bool updating_ = false;
};

QPointer<Controller> controller;

}  // namespace

void install()
{
    if (!controller && qApp)
    {
        controller = new Controller(qApp);
    }
}

void showText(const QPoint &globalPosition, const QString &text, QWidget *owner,
              const QRect &region, int durationMs)
{
    install();
    if (controller)
    {
        controller->showTip(globalPosition, text, owner, region, durationMs);
    }
}

void hideText()
{
    if (controller)
    {
        controller->hideTip();
    }
}

}  // namespace chatterino::noctistooltip
