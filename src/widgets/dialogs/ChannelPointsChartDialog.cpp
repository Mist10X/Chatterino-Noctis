#include "common/UiLanguage.hpp"
#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "Application.hpp"
#    include "providers/twitch/TwitchChannel.hpp"
#    include "singletons/Fonts.hpp"
#    include "singletons/Theme.hpp"
#    include "util/WidgetHelpers.hpp"
#    include "widgets/buttons/Button.hpp"
#    include "widgets/buttons/SvgButton.hpp"
#    include "widgets/dialogs/ChannelPointsChartDialog.hpp"
#    include "widgets/dialogs/ChannelPointsChartView.hpp"
#    include "widgets/helper/ChannelAvatarLabel.hpp"
#    include "widgets/helper/UsercardResizeGrip.hpp"

#    include <QCursor>
#    include <QGuiApplication>
#    include <QHBoxLayout>
#    include <QLabel>
#    include <QPainter>
#    include <QResizeEvent>
#    include <QScreen>
#    include <QShowEvent>
#    include <QVBoxLayout>

#    include <algorithm>
#    include <cmath>

namespace {

using namespace chatterino;

constexpr QSize DEFAULT_DIALOG_SIZE(565, 425);

// Do not make the popup larger than the screen at extreme app scales.
// Physical DPI is independently handled by Qt.
float contentScale(float scale)
{
    return std::clamp(scale, 0.85F, 1.7F);
}

int metric(float scale, int value)
{
    return std::max(1, qRound(scale * value));
}

QString channelDisplayName(const TwitchChannel &channel)
{
    const auto &displayName = channel.getDisplayName();
    return displayName.isEmpty() ? channel.getName() : displayName;
}

}  // namespace

namespace chatterino {

QVector<QPointer<ChannelPointsChartDialog>>
    ChannelPointsChartDialog::activeDialogs_;

ChannelPointsChartDialog::ChannelPointsChartDialog(TwitchChannel *channel,
                                                   QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel ? channel->sharedFromThis() : nullptr)
{
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setAutoFillBackground(false);
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("ChannelPointsChartDialog");
    this->setWindowTitle(uiText(QStringLiteral("Channel Points Chart")));

    auto *container = this->getLayoutContainer();
    container->setObjectName("ChannelPointsChartDialogRoot");
    this->mainLayout_ = new QVBoxLayout(container);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("ChannelPointsChartDialogHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto *headerTextLayout = new QVBoxLayout();
    headerTextLayout->setContentsMargins(0, 0, 0, 0);
    headerTextLayout->setSpacing(0);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(metric(contentScale(this->scale()), 5));

    this->headerTitleLabel_ =
        new QLabel(uiText("Channel Points"), this->headerWidget_);
    this->headerTitleLabel_->setObjectName("ChannelPointsChartHeaderTitle");
    this->headerTitleLabel_->setTextFormat(Qt::PlainText);
    titleRow->addWidget(this->headerTitleLabel_);

    this->headerChannelLabel_ = new QLabel(this->headerWidget_);
    this->headerChannelLabel_->setObjectName("ChannelPointsChartHeaderChannel");
    this->headerChannelLabel_->setTextFormat(Qt::PlainText);
    this->headerChannelLabel_->setWordWrap(true);
    this->headerAvatar_ = new ChannelAvatarLabel(this->headerChannelLabel_,
                                                 this->headerWidget_);
    titleRow->addWidget(this->headerAvatar_, 0, Qt::AlignVCenter);
    titleRow->addWidget(this->headerChannelLabel_, 1);
    headerTextLayout->addLayout(titleRow);

    this->headerSubtitleLabel_ = new QLabel(this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName(
        "ChannelPointsChartHeaderSubtitle");
    this->headerSubtitleLabel_->setTextFormat(Qt::PlainText);
    this->headerSubtitleLabel_->setWordWrap(true);
    headerTextLayout->addWidget(this->headerSubtitleLabel_);

    headerLayout->addLayout(headerTextLayout, 1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setObjectName("ChannelPointsChartPin");
    this->pinButton_->setToolTip(uiText(QStringLiteral("Pin chart popup")));
    this->pinButton_->setAccessibleName(
        uiText(QStringLiteral("Pin chart popup")));
    headerLayout->addWidget(this->pinButton_, 0, Qt::AlignTop);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setObjectName("ChannelPointsChartClose");
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setAccessibleName(uiText(QStringLiteral("Close")));
    this->closeButton_->setToolTip(uiText(QStringLiteral("Close")));
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_, 0, Qt::AlignTop);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new QWidget(container);
    separator->setObjectName("ChannelPointsChartDialogSeparator");
    separator->setFixedHeight(1);
    this->mainLayout_->addWidget(separator);

    this->chartView_ = new ChannelPointsChartView(container);
    this->chartView_->setObjectName("ChannelPointsChartView");
    this->chartView_->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Expanding);
    this->mainLayout_->addWidget(this->chartView_, 1);

    if (this->channel_ != nullptr)
    {
        this->channelPointsConnection_ =
            this->channel_->channelPointsChanged.connect([this] {
                this->reloadChart();
            });
    }

    this->refreshHeader();
    this->refreshStyle();
    this->applySizeConstraints();

    const auto scale = contentScale(this->scale());
    // Set the opening size, not a fixed scale-independent size. The user can
    // then freely resize within the actual layout and monitor constraints.
    this->resize(QSize(qRound(DEFAULT_DIALOG_SIZE.width() * scale),
                       qRound(DEFAULT_DIALOG_SIZE.height() * scale))
                     .expandedTo(this->minimumSize())
                     .boundedTo(this->maximumSize()));
    this->resizeGrip_ = new UsercardResizeGrip(this);
    this->resizeGrip_->syncGeometry(scale);

    this->signalHolder_.managedConnect(getApp()->getFonts()->fontChanged,
                                       [this] {
                                           this->refreshStyle();
                                           this->applySizeConstraints();
                                       });
}

void ChannelPointsChartDialog::showDialog(TwitchChannel *channel,
                                          QWidget *parent)
{
    if (channel == nullptr)
    {
        return;
    }

    const bool wasAutoPinned = DraggablePopup::pinParentIfNeeded(parent);

    ChannelPointsChartDialog *dialog = nullptr;

    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->channel_.get() == channel)
        {
            dialog = *it;
            dialog->raise();
            dialog->activateWindow();
            dialog->reloadChart();
            break;
        }
        ++it;
    }

    if (dialog == nullptr)
    {
        // Keep using `parent` for auto-pin and placement, but do not make
        // another DraggablePopup the QObject owner. Otherwise closing that
        // popup (e.g. channel rewards) destroys a pinned chart with it.
        QWidget *ownershipParent = parent;
        if (qobject_cast<DraggablePopup *>(parent) != nullptr)
        {
            ownershipParent = nullptr;
        }

        dialog = new ChannelPointsChartDialog(channel, ownershipParent);
        activeDialogs_.push_back(dialog);

        QPoint center = QCursor::pos();
        if (parent != nullptr && parent->window() != nullptr)
        {
            center = parent->window()->geometry().center();
        }

        dialog->show();
        const auto size = dialog->size();
        dialog->showAndMoveTo(
            center - QPoint(size.width() / 2, size.height() / 2),
            widgets::BoundsChecking::DesiredPosition);
        dialog->raise();
        dialog->activateWindow();
        dialog->reloadChart();
    }

    if (wasAutoPinned)
    {
        dialog->scheduleUnpinParentOnClose(parent);
    }
}

void ChannelPointsChartDialog::scheduleUnpinParentOnClose(QWidget *parent)
{
    if (this->parentUnpinScheduled_ || parent == nullptr)
    {
        return;
    }

    this->parentUnpinScheduled_ = true;

    QPointer<QWidget> parentPtr(parent);
    QObject::connect(this, &QObject::destroyed, parent, [parentPtr] {
        if (parentPtr)
        {
            DraggablePopup::unpinParentIfNeeded(parentPtr);
        }
    });
}

void ChannelPointsChartDialog::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(this->theme->splits.header.border);
    painter.setBrush(this->theme->window.background);
    const auto radius = 10.0 * contentScale(this->scale());
    painter.drawRoundedRect(QRectF(this->rect()).adjusted(.5, .5, -.5, -.5),
                            radius, radius);
}

void ChannelPointsChartDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
    if (this->resizeGrip_)
    {
        this->resizeGrip_->syncGeometry(contentScale(this->scale()));
    }
}

void ChannelPointsChartDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void ChannelPointsChartDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->applySizeConstraints();
    if (this->resizeGrip_)
    {
        this->resizeGrip_->syncGeometry(contentScale(scale));
    }
}

void ChannelPointsChartDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    this->refreshHeader();
    this->applySizeConstraints();
    this->reloadChart();
}

void ChannelPointsChartDialog::refreshHeader()
{
    const auto channelName = this->channel_
                                 ? channelDisplayName(*this->channel_)
                                 : QString{};
    this->headerTitleLabel_->setText(uiText("Channel Points"));
    this->headerChannelLabel_->setText(channelName);
    this->headerChannelLabel_->setVisible(!channelName.isEmpty());
    this->headerAvatar_->setChannel(
        this->channel_ ? this->channel_->getName() : QString{});
    this->headerSubtitleLabel_->setText(
        uiText("Recorded while Noctis is open · local time"));
}

void ChannelPointsChartDialog::refreshStyle()
{
    if (this->chartView_ == nullptr || this->headerAvatar_ == nullptr)
    {
        return;
    }
    auto *fonts = getApp()->getFonts();
    const auto scale = contentScale(this->scale());
    this->headerTitleLabel_->setFont(
        fonts->getFont(FontStyle::ChatMediumBold, scale));
    this->headerChannelLabel_->setFont(
        fonts->getFont(FontStyle::ChatMediumBold, scale));
    this->headerSubtitleLabel_->setFont(
        fonts->getFont(FontStyle::ChatMediumSmall, scale));
    this->chartView_->setFont(fonts->getFont(FontStyle::ChatMedium, scale));
    this->chartView_->setChartScale(scale);
    // The invisible grip sits outside the contents, not over the header
    // buttons or an axis/plot interaction area.
    const auto inset = UsercardResizeGrip::edgeBand(scale) + metric(scale, 5);
    this->mainLayout_->setContentsMargins(inset, inset, inset, inset);
    this->mainLayout_->setSpacing(metric(scale, 8));
    this->headerWidget_->layout()->setSpacing(metric(scale, 5));
    this->pinButton_->setOverrideScale(scale);
    this->closeButton_->setOverrideScale(scale);
    this->pinButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setScaleIndependentSize(18, 18);

    const auto *theme = this->theme;
    const auto text = theme->window.text.name(QColor::HexArgb);
    auto mutedText = theme->window.text;
    mutedText.setAlpha(186);
    auto separatorColor = theme->splits.header.border;
    separatorColor.setAlpha(120);

    this->getLayoutContainer()->setStyleSheet(
        QStringLiteral(R"(
        QLabel#ChannelPointsChartHeaderTitle,
        QLabel#ChannelPointsChartHeaderChannel {
            background: transparent;
            color: %1;
        }
        QLabel#ChannelPointsChartHeaderSubtitle {
            background: transparent;
            color: %2;
        }
        QWidget#ChannelPointsChartDialogSeparator {
            background: %3;
        }
    )").arg(text, mutedText.name(QColor::HexArgb),
             separatorColor.name(QColor::HexArgb)));

    this->chartView_->applyTheme(*theme);
    this->update();
}

void ChannelPointsChartDialog::applySizeConstraints()
{
    if (this->mainLayout_ == nullptr)
    {
        return;
    }
    const auto scale = contentScale(this->scale());
    const auto layoutMinimum = this->mainLayout_->minimumSize();
    const int minWidth = std::max(metric(scale, 392), layoutMinimum.width());
    const int minHeight = std::max(metric(scale, 325), layoutMinimum.height());

    int maxWidth = QWIDGETSIZE_MAX;
    int maxHeight = QWIDGETSIZE_MAX;
    auto *screen = this->screen();
    if (screen == nullptr)
    {
        screen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr)
    {
        const auto available = screen->availableGeometry();
        const int margin = std::max(12, metric(scale, 12));
        maxWidth = std::max(minWidth, available.width() - margin * 2);
        maxHeight = std::max(minHeight, available.height() - margin * 2);
    }

    this->setMinimumSize(minWidth, minHeight);
    this->setMaximumSize(maxWidth, maxHeight);
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    this->resize(qBound(minWidth, this->width(), maxWidth),
                 qBound(minHeight, this->height(), maxHeight));
}

void ChannelPointsChartDialog::reloadChart()
{
    if (this->channel_ == nullptr || this->chartView_ == nullptr)
    {
        return;
    }

    this->chartView_->reloadFromStore(this->channel_->roomId());
}

}  // namespace chatterino

#endif
