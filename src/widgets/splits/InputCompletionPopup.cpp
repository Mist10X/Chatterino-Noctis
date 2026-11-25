#include "widgets/splits/InputCompletionPopup.hpp"

#include "controllers/completion/sources/UserSource.hpp"
#include "controllers/completion/strategies/ClassicEmoteStrategy.hpp"
#include "controllers/completion/strategies/ClassicUserStrategy.hpp"
#include "controllers/completion/strategies/SmartEmoteStrategy.hpp"
#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <QLayout>
#include <QPainter>
#include <QScreen>

#include <algorithm>

namespace chatterino {

InputCompletionPopup::InputCompletionPopup(QWidget *parent)
    : BasePopup({BasePopup::EnableCustomFrame, BasePopup::Frameless,
                 BasePopup::DontFocus, BaseWindow::DisableLayoutSave},
                parent)
    , model_(this)
{
    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setAutoFillBackground(false);
    this->initLayout();
    this->themeChangedEvent();

    QObject::connect(&this->redrawTimer_, &QTimer::timeout, this, [this] {
        if (this->isVisible())
        {
            this->ui_.listView->doItemsLayout();
        }
    });
    this->redrawTimer_.setInterval(33);
}

void InputCompletionPopup::updateCompletion(const QString &text,
                                            CompletionKind kind,
                                            ChannelPtr channel)
{
    if (this->currentKind_ != kind || this->currentChannel_ != channel)
    {
        this->beginCompletion(kind, std::move(channel));
    }

    assert(this->model_.hasSource());
    this->model_.updateResults(text, MAX_ENTRY_COUNT);
    if (this->model_.rowCount() == 0)
    {
        this->hide();
        return;
    }

    if (this->model_.rowCount() != 0)
    {
        this->ui_.listView->setCurrentIndex(this->model_.index(0));
        this->updatePopupSize();
    }
}

std::unique_ptr<completion::Source> InputCompletionPopup::getSource() const
{
    assert(this->currentChannel_ != nullptr);

    if (!this->currentKind_)
    {
        return nullptr;
    }

    switch (*this->currentKind_)
    {
        case CompletionKind::Emote:
            if (getSettings()->useSmartEmoteCompletion)
            {
                return std::make_unique<completion::EmoteSource>(
                    this->currentChannel_.get(),
                    std::make_unique<completion::SmartEmoteStrategy>(),
                    this->callback_);
            }
            return std::make_unique<completion::EmoteSource>(
                this->currentChannel_.get(),
                std::make_unique<completion::ClassicEmoteStrategy>(),
                this->callback_);
        case CompletionKind::User:
            return std::make_unique<completion::UserSource>(
                this->currentChannel_.get(),
                std::make_unique<completion::ClassicUserStrategy>(),
                this->callback_);
        default:
            return nullptr;
    }
}

void InputCompletionPopup::beginCompletion(CompletionKind kind,
                                           ChannelPtr channel)
{
    this->currentKind_ = kind;
    this->currentChannel_ = std::move(channel);
    this->model_.setSource(this->getSource());
}

void InputCompletionPopup::endCompletion()
{
    this->currentKind_ = std::nullopt;
    this->currentChannel_ = nullptr;
    this->model_.setSource(nullptr);
}

void InputCompletionPopup::setInputAction(ActionCallback callback)
{
    this->callback_ = std::move(callback);
}

bool InputCompletionPopup::eventFilter(QObject *watched, QEvent *event)
{
    return this->ui_.listView->eventFilter(watched, event);
}

bool InputCompletionPopup::hasResults() const
{
    return this->model_.rowCount() > 0;
}

void InputCompletionPopup::showEvent(QShowEvent *)
{
    this->redrawTimer_.start();
}

void InputCompletionPopup::hideEvent(QHideEvent *)
{
    this->redrawTimer_.stop();
    this->endCompletion();
}

void InputCompletionPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->ui_.listView->refreshTheme(*getTheme());
    this->update();
}

void InputCompletionPopup::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(getTheme()->splits.header.border);
    painter.setBrush(getTheme()->window.background);
    const auto radius = 9.0 * std::max(1.F, this->scale());
    painter.drawRoundedRect(QRectF(this->rect()).adjusted(.5, .5, -.5, -.5), radius, radius);
}

void InputCompletionPopup::scaleChangedEvent(float scale)
{
    BasePopup::scaleChangedEvent(scale);
    if (this->ui_.listView)
        this->updatePopupSize();
}

void InputCompletionPopup::updatePopupSize()
{
    const auto scale = std::max(1.F, this->scale());
    auto *list = this->ui_.listView;
    auto font = getApp()->getFonts()->getFont(FontStyle::UiMedium, scale);
    font.setWeight(std::max(font.weight(), QFont::Medium));
    list->setFont(font);
    const int inset = qRound(7 * scale);
    this->layout()->setContentsMargins(inset, inset, inset, inset);
    const int rows = std::min(6, this->model_.rowCount());
    int height = 2 * inset;
    for (int row = 0; row < rows; ++row)
        height += std::max(list->sizeHintForRow(row), list->fontMetrics().height() + 8);
    const auto *screen = this->parentWidget() ? this->parentWidget()->screen() : this->screen();
    if (screen)
    {
        const auto available = screen->availableGeometry();
        height = std::min(height, std::max(1, available.height() / 2));
        this->resize(std::min(qRound(300 * scale), available.width()), height);
    }
    else
        this->resize(qRound(300 * scale), height);
}

void InputCompletionPopup::initLayout()
{
    LayoutCreator creator = {this};

    auto listView =
        creator.emplace<GenericListView>().assign(&this->ui_.listView);
    listView->setInvokeActionOnTab(true);
    listView->setProperty("noctisInputCompletion", true);
    listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    listView->setSpacing(0);

    listView->setModel(&this->model_);
    QObject::connect(listView.getElement(), &GenericListView::closeRequested,
                     this, [this] {
                         this->close();
                     });
}

}  // namespace chatterino
