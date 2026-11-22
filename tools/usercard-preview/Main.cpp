// SPDX-License-Identifier: MIT
// This executable is an offline UI fixture, never the normal application.

#include "common/Channel.hpp"
#include "providers/twitch/ChannelStats.hpp"
#include "widgets/dialogs/StreamStatsDialog.hpp"
#include "common/UiLanguage.hpp"
#include "common/network/NetworkManager.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "common/LinkParser.hpp"
#include "providers/twitch/ClipPreview.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageThread.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "widgets/helper/HighlightToast.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "OwnTimeoutChecks.hpp"
#include "PredictionDemo.hpp"
#include "LinkPreviewChecks.hpp"
#include "PublicLogsChecks.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/seventv/SeventvPersonalLookup.hpp"
#include "singletons/Logging.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/Resources.hpp"
#include "singletons/WindowManager.hpp"
#ifdef LEAFYRINO_BASELINE_USERCARD_PREVIEW
#    include "BaselineUserInfoPopup.hpp"
#else
#    include "providers/twitch/TwitchAccount.hpp"
#    include "widgets/dialogs/TwitchBadgePickerDialog.hpp"
#    include "widgets/dialogs/EmotePopup.hpp"
#    include "widgets/dialogs/SelectChannelDialog.hpp"
#    include "widgets/buttons/LabelButton.hpp"
#    include "widgets/helper/ChannelAvatarLabel.hpp"
#    include "widgets/Notebook.hpp"
#    include "messages/layouts/MessageLayoutElement.hpp"
#    include "widgets/TooltipWidget.hpp"
#    include <QHoverEvent>
#    include <QGraphicsOpacityEffect>
#    include "NoctisTooltipChecks.hpp"
#    include "widgets/helper/MenuStyle.hpp"
#    include "StartupHistoryChecks.hpp"
#    include "HistoryBadgeChecks.hpp"
#    include "widgets/helper/ServiceHealthCard.hpp"
#    include "HistoryChecks.hpp"
#    include "RolesChecks.hpp"
#    include "widgets/dialogs/UserInfoPopup.hpp"
#    include "widgets/helper/UsercardResizeGrip.hpp"
#    include "widgets/helper/UsercardChrome.hpp"
#    include "widgets/helper/RoundAvatar.hpp"
#    include "providers/seventv/paints/LinearGradientPaint.hpp"
#    include "singletons/Settings.hpp"
#endif
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/CornerBanners.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/Split.hpp"

#include <IrcMessage>
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include "singletons/Paths.hpp"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QCursor>
#include <QHelpEvent>
#include <QToolTip>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#endif

#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
namespace chatterino {
struct EmotePickerPreviewCheck {
    static bool run(const QString &path)
    {
        if (qEnvironmentVariableIsSet("NOCTIS_PICKER_UI_SCALE"))
            getSettings()->uiScale.setValue(qEnvironmentVariable("NOCTIS_PICKER_UI_SCALE").toFloat());
        EmotePopup popup;
        popup.setAttribute(Qt::WA_DontShowOnScreen);
        popup.loadChannel(std::make_shared<Channel>("dangerlyoha", Channel::Type::None));
        popup.resize(460, 600);
        popup.show();
        owntimeout::pump(120);
        const auto favourites = popup.favouritesView_->underlyingChannel()->getMessageSnapshot();
        const auto *hint = favourites.empty() || favourites.front()->elements.empty() ? nullptr :
            dynamic_cast<const TextElement *>(favourites.front()->elements.front().get());
        const bool favouritesLocalized = hint && hint->words().join(' ') ==
            uiText("No favourites. You can add them by Ctrl+clicking on an Emote or marking it as favourite in the context menu");
        const auto emojiRows = popup.viewEmojis_->underlyingChannel()->getMessageSnapshot();
        bool componentLocalized = false;
        for (const auto &row : emojiRows)
            if (!row->elements.empty())
                if (const auto *label = dynamic_cast<const TextElement *>(row->elements.front().get()))
                    componentLocalized |= label->words().join(' ') == uiText("Component");
        if (activeUiLanguage() == "ru")
            for (const auto *category : {"Activities", "Animals & Nature", "Component", "Flags", "Food & Drink",
                                          "Objects", "People & Body", "Smileys & Emotion", "Symbols", "Travel & Places"})
                componentLocalized &= uiText(category) != QString::fromLatin1(category);
        const bool title = !popup.windowTitle().contains('#') &&
            popup.titleChannel_->text() == "dangerlyoha" &&
            popup.titleAvatar_->isVisible() && !popup.titleAvatar_->pixmap().isNull() &&
            popup.titleAvatar_->geometry().right() < popup.titleChannel_->geometry().left();
        const bool searchStyle = popup.search_->styleSheet().contains("border-radius") &&
            popup.search_->font() == getApp()->getFonts()->getFont(FontStyle::ChatMedium, popup.scale());
        const bool compact = popup.searchRow_->height() <= popup.search_->height() + 12 &&
            popup.notebook_->height() >= popup.getLayoutContainer()->height() - popup.searchRow_->height() - 24
                - (popup.hasCustomWindowFrame() ? 0 : popup.titleRow_->height());
        popup.notebook_->select(popup.channelEmotesView_);
        const bool animated = popup.channelEmotesView_->findChild<QWidget *>("NotebookTabTransition") != nullptr;
        owntimeout::pump(150);
        const bool finished = !popup.channelEmotesView_->findChild<QWidget *>("NotebookTabTransition");
        TextElement creator("TestEmote", MessageElementFlag::AlwaysShow);
        ImageLayoutElement element(creator, getEmptyImagePtr(), QSizeF(20, 20));
        element.setLink(Link(Link::InsertText, "TestEmote"));
        QString inserted;
        auto connection = popup.linkClicked.connect([&](const Link &link) { inserted = link.value; });
        popup.channelEmotesView_->elementClicked.invoke(&element, Qt::NoModifier);
        const bool selection = inserted == "TestEmote" &&
            popup.channelEmotesView_->findChild<QWidget *>("EmotePickerSelectionFeedback");
        owntimeout::pump(240);
        const bool selectionFinished = !popup.channelEmotesView_->findChild<QWidget *>("EmotePickerSelectionFeedback");
        popup.search_->setText("TestEmote");
        const bool search = popup.searchView_->isVisible() && !popup.notebook_->isVisible();
        popup.search_->clear();
        const bool restored = popup.notebook_->isVisible() && !popup.searchView_->isVisible();
        popup.notebook_->select(popup.subEmotesView_);
        owntimeout::pump(150);
        static const QPixmap fixtureImage = [] {
            QPixmap pixmap(36, 36);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#7765b0"));
            painter.drawEllipse(QRect(2, 2, 32, 32));
            painter.setBrush(QColor("#ebd4a7"));
            painter.drawEllipse(QRect(9, 10, 6, 6));
            painter.drawEllipse(QRect(21, 10, 6, 6));
            painter.drawRoundedRect(10, 23, 16, 4, 2, 2);
            return pixmap;
        }();
        const auto image = Image::fromResourcePixmap(fixtureImage, 1);
        const auto emote = std::make_shared<Emote>(Emote{
            .name = EmoteName{"TestEmote"},
            .images = ImageSet{image, image, image},
            .tooltip = Tooltip{"TestEmote"}});
        const auto previousCatalog = getApp()->getBttvEmotes()->emotes();
        auto freshCatalog = std::make_shared<EmoteMap>();
        for (int i = 0; i < 121; ++i)
        {
            auto item = std::make_shared<Emote>(*emote);
            item->name = EmoteName{QStringLiteral("Fixture_%1").arg(i)};
            freshCatalog->emplace(item->name, item);
        }
        getApp()->getBttvEmotes()->setEmotes(freshCatalog);
        owntimeout::pump(420);
        size_t catalogCount = 0;
        size_t largestBatch = 0;
        for (const auto &row : popup.globalEmotesView_->underlyingChannel()->getMessageSnapshot())
        {
            size_t batch = 0;
            for (const auto &element : row->elements)
            {
                const auto *item = dynamic_cast<const EmoteElement *>(element.get());
                if (item && item->getEmote()->name.string.startsWith("Fixture_"))
                    ++batch;
            }
            catalogCount += batch;
            largestBatch = std::max(largestBatch, batch);
        }
        const bool lateCatalog = catalogCount == 121 && largestBatch <= 48;
        getApp()->getBttvEmotes()->setEmotes(previousCatalog);
        owntimeout::pump(420);
        auto sub = popup.subEmotesView_->underlyingChannel();
        sub->clearMessages();
        for (const auto &name : {"dangerlyoha", "morphe_ya", "kopsteep"})
        {
            MessageBuilder header;
            header->flags.set(MessageFlag::Centered);
            header.emplace<TextElement>(name, MessageElementFlag::Text);
            sub->addMessage(header.release(), MessageContext::Original);
            MessageBuilder row;
            row->flags.set(MessageFlag::Centered);
            row->flags.set(MessageFlag::DisableCompactEmotes);
            for (int i = 0; i < 19; ++i)
                row.emplace<EmoteElement>(emote, MessageElementFlags{MessageElementFlag::AlwaysShow, MessageElementFlag::Emote})
                    ->setLink(Link(Link::InsertText, "TestEmote"));
            sub->addMessage(row.release(), MessageContext::Original);
        }
        owntimeout::pump(120);
        popup.grab().save(path);
        const bool paused = activeUiLanguage() == "ru"
            ? uiText("Paused") == QString::fromUtf8("Пауза") : uiText("Paused") == "Paused";
        Split split(nullptr);
        split.setAttribute(Qt::WA_DontShowOnScreen);
        auto channel = std::make_shared<TwitchChannel>("room_modes_fixture");
        split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(channel)));
        TwitchChannelTestAccess::setSlowAndFollowerMode(*channel, 3, 60);
        split.resize(620, 280);
        split.show();
        owntimeout::pump(120);
        const auto *modes = dynamic_cast<LabelButton *>(split.findChild<QWidget *>("RoomModesButton"));
        const auto expected = activeUiLanguage() == "ru"
            ? QString::fromUtf8("Слоу-мод 3 с · Фолловеры 1 ч") : QStringLiteral("slow 3 s · followers 1 h");
        const bool roomModes = modes && modes->text() == expected && modes->isVisible();
        getSettings()->alwaysExpandPinnedMessages.setValue(false);
        MessageBuilder pinned;
        pinned->id = "alignment-fixture-message";
        pinned.emplace<TextElement>(QString("Pinned message with enough words to wrap. ").repeated(10),
                                   MessageElementFlag::Text);
        channel->addMessage(pinned.release(), MessageContext::Original);
        channel->setPinnedMessage(TwitchChannel::PinnedMessage{
            .pinId = "alignment-fixture",
            .messageId = "alignment-fixture-message",
            .text = QString("Pinned message with enough words to wrap. ").repeated(10),
            .authorLogin = "fixture_author",
            .pinnerName = "fixture_mod"});
        owntimeout::pump(150);
        const auto *pinner = split.findChild<QWidget *>("PinnedMessageAuthor");
        const auto *more = split.findChild<QLabel *>("PinnedMessageMore");
        const bool pinAligned = pinner && more && more->isVisible() &&
            std::abs(pinner->geometry().center().y() - more->geometry().center().y()) <= 1;
        split.grab().save(path + ".pin.png");
        split.getChannelView().pause(PauseReason::Mouse);
        split.grab().save(path + ".modes.png");
        getSettings()->bannerStackMode.setValue(0);
        getSettings()->pollBannerContentScale.setValue(1.25F);
        getSettings()->predictionBannerContentScale.setValue(1.25F);
        const auto now = QDateTime::currentDateTimeUtc();
        channel->setActivePoll(TwitchChannel::PollEvent{
            .id = "banner-font-poll", .title = "One < two", .status = "ACTIVE",
            .choices = {{.id = "a", .title = "A"}, {.id = "b", .title = "B"}},
            .durationSeconds = 90, .createdAt = now, .endsAt = now.addSecs(90)});
        channel->setActivePrediction(TwitchChannel::PredictionEvent{
            .id = "banner-font-prediction", .title = "Three > two", .status = "LOCKED",
            .outcomes = {{.id = "a", .title = "A"}, {.id = "b", .title = "B"}},
            .createdAt = now});
        owntimeout::pump(120);
        auto *pollTitle = split.findChild<QLabel *>("PollTitle");
        auto *predictionTitle = split.findChild<QLabel *>("PredictionTitle");
        const auto titleMatches = [&] {
            const auto expectedFont = getApp()->getFonts()->getFont(
                FontStyle::ChatMediumBold, split.scale() * 1.25F);
            for (const auto *label : {pollTitle, predictionTitle})
            {
                if (!label || !label->isVisible() || label->textFormat() != Qt::PlainText ||
                    label->font().family() != expectedFont.family() ||
                    label->font().pointSizeF() != expectedFont.pointSizeF() ||
                    label->height() < label->fontMetrics().height())
                    return false;
            }
            return true;
        };
        const bool bannerTitles = titleMatches();
        getSettings()->chatFontSize.setValue(getSettings()->chatFontSize.getValue() + 4);
        owntimeout::pump(200);
        const bool resizedBanners = titleMatches() && pinner && more &&
            pinner->font().pointSizeF() == more->font().pointSizeF();
        std::fprintf(stderr, "banner typography: initial=%d after-font-change=%d\n",
                     bannerTitles, resizedBanners);
        split.grab().save(path + ".banners.png");
        split.setChannel(IndirectChannel(Channel::getEmpty()));
        SelectChannelDialog selector;
        selector.setSelectedChannel({});
        selector.setAttribute(Qt::WA_DontShowOnScreen);
        selector.show();
        owntimeout::pump(100);
        const bool selectorStyled =
            selector.getLayoutContainer()->styleSheet().contains("border-radius") &&
            selector.getLayoutContainer()->font().family() ==
                getApp()->getFonts()->getFont(FontStyle::ChatMedium, 1).family() &&
            selector.width() <= 650 && selector.height() <= 750;
        selector.grab().save(path + ".channel-selector.png");
        selector.close();
        std::fprintf(stderr, "late emote catalog=%d count=%zu batch=%zu; selector styled=%d\n",
                     lateCatalog, catalogCount, largestBatch, selectorStyled);
        std::fprintf(stderr, "emote picker title=%d search-style=%d compact=%d tab=%d/%d insertion=%d/%d search=%d/%d pause=%d room-modes=%d\n",
                     title, searchStyle, compact, animated, finished, selection, selectionFinished, search, restored, paused, roomModes);
        return title && searchStyle && compact && animated && finished && selection && selectionFinished && search && restored && paused && roomModes &&
            favouritesLocalized && componentLocalized && pinAligned && bannerTitles && resizedBanners &&
            lateCatalog && selectorStyled;
    }
};
struct BadgePickerPreviewCheck {
    static bool checkAccountNames()
    {
        auto channel = std::make_shared<TwitchChannel>("badge_names_fixture");
        TwitchBadgePickerDialog dialog(channel.get());
        const auto addHistoryName = [&](const QString &id, const QString &login,
                                        const QString &name) {
            MessageBuilder builder;
            builder->userID = id;
            builder->loginName = login;
            builder->displayName = name;
            channel->addMessage(builder.release(), MessageContext::Repost);
        };

        TwitchAccount account("case_fixture", {}, {}, "900020");
        const bool loginFallback = dialog.resolveSelfDisplayName(account) ==
                                   "case_fixture";
        addHistoryName("900020", "case_fixture", "CASE_Fixture");
        addHistoryName("900020", "old_login", "Old_Login");
        addHistoryName("900021", "case_fixture", "Case_FIXTURE");
        const bool historyFallback = dialog.resolveSelfDisplayName(account) ==
                                     "CASE_Fixture";
        account.setDisplayName("900020", "case_fixture", "Case_Fixture");
        const bool verified = dialog.resolveSelfDisplayName(account) ==
                              "Case_Fixture" &&
                              account.getUserName() == "case_fixture";
        account.setDisplayName("900020", "case_fixture", "case_fixture");
        const bool lowercase = dialog.resolveSelfDisplayName(account) ==
                               "case_fixture";

        // The fixture's TwitchUsers returns a lowercase `forsen` for every ID.
        TwitchAccount cached("forsen", {}, {}, "900022");
        const bool cacheFallback = dialog.resolveSelfDisplayName(cached) ==
                                   "forsen";
        cached.setDisplayName("900022", "forsen", "Forsen");
        const bool cachePrecedence = dialog.resolveSelfDisplayName(cached) ==
                                     "Forsen";
        std::fprintf(stderr, "picker account names: login=%d history=%d verified=%d lowercase=%d cache=%d cache-precedence=%d\n",
                     loginFallback, historyFallback, verified, lowercase,
                     cacheFallback, cachePrecedence);
        return loginFallback && historyFallback && verified && lowercase &&
               cacheFallback && cachePrecedence;
    }

    static bool run(const QString &path)
    {
        if (qEnvironmentVariableIsSet("NOCTIS_PICKER_UI_SCALE"))
            getSettings()->uiScale.setValue(qEnvironmentVariable("NOCTIS_PICKER_UI_SCALE").toFloat());
        if (qEnvironmentVariableIsSet("NOCTIS_PICKER_FONT_SIZE"))
            getSettings()->chatFontSize.setValue(qEnvironmentVariableIntValue("NOCTIS_PICKER_FONT_SIZE"));
        if (!checkAccountNames())
            return false;
        QPointer<TwitchBadgePickerDialog> dialog(new TwitchBadgePickerDialog(nullptr));
        dialog->initialFetchDone_ = true;
        GqlBadge badge;
        badge.setID = "fixture-owned";
        badge.version = "1";
        badge.title = "Fixture badge";
        badge.image4x = QUrl::fromLocalFile(path + ".asset.png").toString();
        QImage asset(72, 72, QImage::Format_ARGB32);
        asset.fill(QColor("#529bea"));
        if (!asset.save(path + ".asset.png"))
            return false;
        dialog->badges_.availableGlobal.append(badge);
        dialog->rebuildContent();
        dialog->show();
        owntimeout::pump(120);
        const auto surface = dialog->grab().toImage();
        surface.save(path);
        const bool rounded = surface.pixelColor(0, 0).alpha() == 0 &&
            surface.pixelColor(surface.width()/2, surface.height()/2).alpha() == 255;
        for (auto *tile : dialog->findChildren<QPushButton *>())
        {
            if (!tile->isVisible() || tile->accessibleName() != badge.title)
                continue;
            QHoverEvent enter(QEvent::HoverEnter, QPointF(5,5), tile->mapToGlobal(QPoint(5,5)), QPointF(-1,-1));
            QApplication::sendEvent(tile, &enter);
            owntimeout::pump(250);
            if (!dialog) return false;
            const auto preview = dialog->badgePreview_->grab().toImage();
            preview.save(path + ".preview.png");
            bool imageShown = false;
            for (int y = 0; y < preview.height() && !imageShown; ++y)
                for (int x = 0; x < preview.width() && !imageShown; ++x)
                    imageShown = preview.pixelColor(x, y) == QColor("#529bea");
            const bool shown = dialog->badgePreview_->isVisible() && imageShown;
            QEvent leave(QEvent::HoverLeave);
            QApplication::sendEvent(tile, &leave);
            const bool previewClosed = !dialog->badgePreview_->isVisible();
            dialog->badges_.selectedGlobalBadge = badge;
            dialog->animateBadgeApplied(badge, TwitchBadgePickerDialog::View::GlobalBadges);
            const bool applying = tile->graphicsEffect() && tile->graphicsEffect()->isEnabled();
            owntimeout::pump(260);
            const bool applied = !tile->graphicsEffect();
            dialog->switchView(TwitchBadgePickerDialog::View::ChannelBadges);
            const bool tabAnimation = dialog->contentWidget_->graphicsEffect() != nullptr;
            const auto *setting = dialog->findChild<QLabel *>("TwitchBadgePickerSettingLabel");
            const bool translated = setting && setting->text() == uiText("Use Custom Badge for This Channel");
            const bool fontMatches = setting && setting->font().family() ==
                getApp()->getFonts()->getFont(FontStyle::ChatMedium, dialog->scale()).family() &&
                dialog->globalTabButton_->font().family() == setting->font().family();
            dialog->eventBadgesLoaded_ = true;
            dialog->eventBadgesCacheTime_ = QDateTime::currentDateTimeUtc();
            for (int i = 0; i < 32; ++i)
            {
                dialog->eventBadgesCache_.append(EventBadge{
                    .id = QString("fixture-event-%1").arg(i),
                    .name = QString("Fixture event badge %1").arg(i),
                    .imageUrl = badge.image4x,
                    .startAt = QDateTime::currentDateTimeUtc().addDays(-1),
                    .endAt = QDateTime::currentDateTimeUtc().addDays(1),
                    .free = i % 2 == 0});
            }
            dialog->switchView(TwitchBadgePickerDialog::View::EventBadges);
            owntimeout::pump(240);
            const auto *price = dialog->findChild<QLabel *>("TwitchBadgePickerEventBadgePrice");
            const bool eventTranslated = price && price->text() == uiText("Free");
            const auto priceLabels = dialog->findChildren<QLabel *>("TwitchBadgePickerEventBadgePrice");
            const int viewportWidth = dialog->scrollArea_->viewport()->width();
            bool priceFits = priceLabels.size() == 32;
            for (const auto *label : priceLabels)
            {
                const auto left = label->mapTo(dialog->scrollArea_->viewport(), QPoint(0, 0)).x();
                priceFits &= label->contentsRect().width() >= QFontMetrics(label->font()).horizontalAdvance(label->text()) &&
                             left >= 0 && left + label->width() <= viewportWidth;
            }
            const bool finished = !dialog->contentWidget_->graphicsEffect();
            dialog->grab().save(path + ".event.png");
            std::fprintf(stderr, "picker: rounded=%d hover=%d closed=%d animation=%d/%d translated=%d font=%d event=%d tab=%d/%d 32-price-labels-fit=%d columns=%d\n",
                         rounded, shown, previewClosed, applying, applied, translated, fontMatches, eventTranslated, tabAnimation, finished, priceFits, dialog->badgeGridColumns());
            const bool ok = rounded && shown && previewClosed && applying && applied &&
                translated && fontMatches && eventTranslated && priceFits && tabAnimation && finished;
            delete dialog.data();
            return ok;
        }
        return false;
    }
};
}  // namespace chatterino
#endif

namespace {

using namespace chatterino;

#ifdef LEAFYRINO_BASELINE_USERCARD_PREVIEW
using PreviewPopup = BaselineUserInfoPopup;
using PreviewData = BaselineUserInfoPreviewData;
#else
using PreviewPopup = UserInfoPopup;
using PreviewData = UserInfoPreviewData;
#endif

struct GuiLifecycleGuard {
    ~GuiLifecycleGuard()
    {
        // Also covers argument validation failures before exec() was entered.
        markAppAboutToQuit();
    }
};

bool clipDemo = false;  // --clip-demo: last message carries a clip link

class OfflineApplication final : public mock::BaseApplication
{
public:
    OfflineApplication()
        : windows(this->args, this->paths_, this->settings, this->theme,
                  this->fonts)
        , commands(this->paths_)
    {
        // No AccountController::load(), provider initialize(), NetworkManager,
        // credential store, Twitch IRC, EventSub, or PLUS connection is started.
        this->settings.disableSave();
        this->settings.alwaysLoadMoreUsercardMessages = false;
        this->settings.scrollbackSplitLimit = 6000;
        this->settings.scrollbackUsercardLimit = 6000;
        this->settings.showUsercardSevenTVPaint = false;
        this->settings.showFollowButtonInUsercard = false;
        this->settings.showPronouns = true;
    }

    WindowManager *getWindows() override
    {
        return &this->windows;
    }
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }
    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }
    CommandController *getCommands() override
    {
        return &this->commands;
    }
    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }
    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }
    TwitchBadges *getTwitchBadges() override
    {
        return &this->twitchBadges;
    }
    IUserDataController *getUserData() override
    {
        return &this->userData;
    }
    SeventvPaints *getSeventvPaints() override
    {
        return &this->paints;
    }
    BttvEmotes *getBttvEmotes() override
    {
        return &this->bttv;
    }
    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffz;
    }
    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventv;
    }
    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &this->personalEmotes;
    }
    SeventvAPI *getSeventvAPI() override
    {
        return &this->seventvApi;
    }
    ILogging *getChatLogger() override
    {
        return &this->chatLog;
    }
    SpellChecker *getSpellChecker() override
    {
        return &this->spellChecker;
    }
    ILinkResolver *getLinkResolver() override
    {
        return &this->linkResolver;
    }
    PubSub *getTwitchPubSub() override
    {
        // Never connected: nothing in this fixture asks it to listen.
        return &this->pubSub;
    }
    ImageUploader *getImageUploader() override
    {
        // Nothing is uploaded: the checks only drive its progress signal.
        return &this->imageUploader;
    }

    WindowManager windows;
    AccountController accounts;
    HotkeyController hotkeys;
    CommandController commands;
    mock::EmoteController emotes;
    mock::MockTwitchIrcServer twitch;
    TwitchBadges twitchBadges;
    mock::UserDataController userData;
    SeventvPaints paints;
    BttvEmotes bttv;
    FfzEmotes ffz;
    SeventvEmotes seventv;
    SeventvPersonalEmotes personalEmotes;
    SeventvAPI seventvApi;
    /// system messages on a synthetic channel are written nowhere
    struct NoChatLog final : ILogging {
        void addMessage(const QString &, MessagePtr, const QString &,
                        const QString &) override
        {
        }
        void closeChannel(const QString &, const QString &) override
        {
        }
    } chatLog;
    SpellChecker spellChecker;
    PubSub pubSub{QStringLiteral("wss://127.0.0.1:9")};
    LinkResolver linkResolver;
    ImageUploader imageUploader;
};

#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
std::vector<EmotePtr> fixtureBadgeCollection(int count)
{
    // Image::fromResourcePixmap caches by the address of the source pixmap.
    // Keep distinct, process-owned sample icons for all entries.
    static const std::array<QPixmap, 96> pixmaps = [] {
        std::array<QPixmap, 96> icons;
        for (size_t i = 0; i < icons.size(); ++i)
        {
            auto &icon = icons[i];
            icon = QPixmap(36, 36);
            icon.fill(Qt::transparent);
            QPainter painter(&icon);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor::fromHsv(int(i * 43 % 360), 155, 245));
            painter.drawRoundedRect(3, 3, 30, 30, 7, 7);
            painter.setPen(Qt::white);
            auto font = painter.font();
            font.setPixelSize(i < 9 ? 22 : 16);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(icon.rect(), Qt::AlignCenter,
                             QString::number(i + 1));
        }
        return icons;
    }();
    static const std::array<QPixmap, 96> largePixmaps = [] {
        std::array<QPixmap, 96> icons;
        for (size_t i = 0; i < icons.size(); ++i)
        {
            auto &icon = icons[i];
            icon = QPixmap(72,72);
            icon.fill(Qt::transparent);
            QPainter painter(&icon);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(i == 0 ? QColor("#12b9e6") : QColor::fromHsv(int(i*43%360),155,245));
            painter.drawRoundedRect(6,6,60,60,14,14);
            painter.setPen(Qt::white);
            auto font = painter.font();
            font.setPixelSize(i < 9 ? 44 : 32);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(icon.rect(), Qt::AlignCenter, QString::number(i+1));
        }
        return icons;
    }();
    std::vector<EmotePtr> badges;
    for (int i = 0; i < std::min(count, int(pixmaps.size())); ++i)
    {
        badges.push_back(std::make_shared<Emote>(Emote{
            .name = EmoteName{QStringLiteral("Synthetic badge %1").arg(i + 1)},
            .images = ImageSet{Image::fromResourcePixmap(pixmaps[size_t(i)], .5),
                Image::fromResourcePixmap(pixmaps[size_t(i)], .5),
                Image::fromResourcePixmap(largePixmaps[size_t(i)], .25)},
            .tooltip = Tooltip{QStringLiteral("Synthetic badge %1<br>ACQUISITION SENTINEL").arg(i + 1)},
            .id = EmoteId{QStringLiteral("fixture-earned-%1").arg(i + 1)},
        }));
    }
    return badges;
}

void clickBadgeChip(QWidget *strip, float scale)
{
    if (!strip)
        return;
    const auto x = strip->property("firstRowBadgeCount").toInt() *
                   std::max(1, int(24 * scale)) + 5;
    const QPoint point(x, std::max(1, int(12 * scale)));
    const auto global = strip->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point),
                      QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                        QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(strip, &press);
    QApplication::sendEvent(strip, &release);
}

#endif

PreviewData fixture(const QString &scenario)
{
    const bool secondIdentity = scenario == QStringLiteral("b");
    const bool empty = scenario == QStringLiteral("empty");
    const bool longHistory = scenario == QStringLiteral("long");
    PreviewData data;
    data.login = QStringLiteral("fixture_viewer");
    data.userId = secondIdentity ? QStringLiteral("910000002")
                  : empty        ? QStringLiteral("910000003")
                                 : QStringLiteral("910000001");
    data.displayName = secondIdentity ? QStringLiteral("Fixture_Viewer B")
                                      : QStringLiteral("Fixture_Viewer A");
    if (empty)
    {
        data.displayName = QStringLiteral("Fixture_Viewer • empty");
    }
    data.createdText = QStringLiteral("Создан 2021-04-15 · тестовые данные");
    data.followersText = QStringLiteral("Фолловеров 1 234 · тестовые данные");
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    data.chatColor = QStringLiteral("#E07700");
#endif
    data.pronouns = QStringLiteral("Местоимения: they/them · тестовые данные");
    data.avatar = QPixmap(128, 128);
    data.avatar.fill(QColor(secondIdentity ? "#355b77" : "#67418b"));
    {
        QPainter painter(&data.avatar);
        painter.setPen(Qt::white);
        auto font = painter.font();
        font.setPixelSize(60);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(
            data.avatar.rect(), Qt::AlignCenter,
            secondIdentity ? QStringLiteral("B") : QStringLiteral("A"));
    }
    data.banner = QPixmap(760, 240);
    data.banner.fill(QColor(secondIdentity ? "#1d4555" : "#35264b"));
    {
        QPainter painter(&data.banner);
        painter.setPen(QColor("#a98acb"));
        for (int x = -240; x < data.banner.width(); x += 60)
        {
            painter.drawLine(x, 240, x + 240, 0);
        }
    }
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    data.earnedBadges = fixtureBadgeCollection(8);
#endif
    // Type::None has no logger or 7TV lookups. This is still the real Channel
    // queue and the real ChannelView message renderer used by UserInfoPopup.
    data.messages = std::make_shared<Channel>(QStringLiteral("fixture_room"),
                                              Channel::Type::None);
    const auto count = empty ? 0 : longHistory ? 5000 : 18;
    const QStringList texts{
        QStringLiteral("Это синтетическое сообщение для проверки карточки."),
        QStringLiteral(
            "Русский и English text stay readable in the same line."),
        QStringLiteral("Сохранённый текст является контекстом, а не доказанной "
                       "причиной наказания."),
        QStringLiteral("Очень длинная строка: ") + QString(380, QChar(0x044F)),
        QStringLiteral(
            "<b>Это обычный текст, а не HTML.</b> & quote \"example\""),
        QStringLiteral("Reply example: ответ на ранее сохранённое сообщение.")};
    const auto start = QDateTime::fromString(
        QStringLiteral("2026-10-20T12:00:00Z"), Qt::ISODate);
    QPixmap emotePixmap(28, 28);
    emotePixmap.fill(Qt::transparent);
    {
        QPainter painter(&emotePixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#b397f4"));
        painter.drawEllipse(1, 1, 26, 26);
        painter.setBrush(QColor("#21182d"));
        painter.drawEllipse(7, 8, 4, 5);
        painter.drawEllipse(17, 8, 4, 5);
        painter.drawRoundedRect(8, 18, 12, 3, 1, 1);
    }
    const auto emote = std::make_shared<Emote>(Emote{
        .name = EmoteName{QStringLiteral("PreviewSmile")},
        .images = ImageSet{Image::fromResourcePixmap(emotePixmap)},
        .tooltip = {},
        .homePage = {},
        .id = {},
        .author = {},
        .baseName = {},
    });
    MessagePtr parent;
    std::shared_ptr<MessageThread> thread;
    for (int i = 0; i < count; ++i)
    {
        MessageBuilder builder;
        auto &message = builder.message();
        message.id = QStringLiteral("synthetic-%1-%2").arg(data.userId).arg(i);
        message.userID = data.userId;
        message.loginName = data.login;
        message.displayName = data.displayName;
        message.channelName = QStringLiteral("fixture_room");
        message.serverReceivedTime = start.addSecs(i * 17);
        message.messageText = texts.at(i % texts.size());
        if (i % 6 == 5 && parent)
        {
            message.replyParent = parent;
            message.replyThread = thread;
            builder.emplace<ReplyCurveElement>();
            builder.emplace<TextElement>(
                QStringLiteral("Replying to Fixture_Viewer"),
                MessageElementFlag::RepliedMessage, MessageColor::System);
            builder.emplace<LinebreakElement>(
                MessageElementFlag::RepliedMessage);
        }
        builder.emplace<TimestampElement>(message.serverReceivedTime.time());
        builder.emplace<TextElement>(data.displayName + QStringLiteral(": "),
                                     MessageElementFlag::Username,
                                     QColor(QStringLiteral("#b397f4")));
        builder.emplace<TextElement>(message.messageText,
                                     MessageElementFlag::Text);
        if (i % 3 == 2)
        {
            builder.emplace<EmoteElement>(emote, MessageElementFlag::Emote);
        }
        if (clipDemo && i == count - 1)
        {
            QPixmap thumb(480, 272);
            {
                QPainter painter(&thumb);
                QLinearGradient gradient(0, 0, 480, 272);
                gradient.setColorAt(0, QColor("#3b6d3a"));
                gradient.setColorAt(1, QColor("#1d2b4a"));
                painter.fillRect(thumb.rect(), gradient);
            }
            clippreview::insertForTesting({
                .slug = QStringLiteral("FakeClipDemo"),
                .url = QStringLiteral(
                    "https://clips.twitch.tv/FakeClipDemo"),
                .title = QStringLiteral("Synthetic clip title"),
                .broadcaster = QStringLiteral("morphe_ya"),
                .creator = QStringLiteral("Deux_Visages"),
                .gameName = QStringLiteral("Spore"),
                .thumbnail = Image::fromResourcePixmap(thumb),
                .views = 1,
                .durationSeconds = 6,
                .createdAt = QDateTime::currentDateTimeUtc(),
            });
            const QString link = QStringLiteral(
                "https://www.twitch.tv/morphe_ya/clip/FakeClipDemo");
            builder.addLink(*linkparser::parse(link), link);
        }
        auto built = builder.release();
        if (!parent)
        {
            parent = built;
            thread = std::make_shared<MessageThread>(parent);
        }
        else if (built->replyThread)
        {
            thread->addToThread(MessagePtr{built});
        }
        data.messages->addMessage(std::move(built), MessageContext::Repost);
    }
    return data;
}

QScreen *previewScreen()
{
    for (auto *screen : QGuiApplication::screens())
    {
        if (screen != QGuiApplication::primaryScreen())
        {
            return screen;
        }
    }
    return nullptr;
}

QJsonObject nativeHitTests(PreviewPopup &popup)
{
    QJsonObject result;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
    {
        return result;
    }
    auto *tabs =
        popup.findChild<QTabWidget *>(QStringLiteral("UsercardContent"));
    auto *profile =
        popup.findChild<QScrollArea *>(QStringLiteral("UsercardProfileScroll"));
    if (!tabs || !profile)
    {
        return result;
    }
    const auto window = reinterpret_cast<HWND>(popup.winId());
    DWORD ownerProcess = 0;
    if (!IsWindow(window) ||
        GetWindowThreadProcessId(window, &ownerProcess) == 0 ||
        ownerProcess != GetCurrentProcessId())
    {
        return result;
    }
    auto checkPoint = [&](const QString &name, QWidget *widget, QPoint point) {
        const auto local = widget->mapTo(&popup, point);
        POINT nativePoint{qRound(local.x() * popup.devicePixelRatioF()),
                          qRound(local.y() * popup.devicePixelRatioF())};
        // Only this process's synthetic popup HWND is queried. No input is
        // injected and no other window or process is inspected.
        ClientToScreen(window, &nativePoint);
        const auto hit = SendMessageW(window, WM_NCHITTEST, 0,
                                      MAKELPARAM(nativePoint.x, nativePoint.y));
        result[name] = int(hit);
    };
    checkPoint(QStringLiteral("content-page"), tabs->currentWidget(),
               tabs->currentWidget()->rect().center());
    checkPoint(QStringLiteral("profile-viewport"), profile->viewport(),
               profile->viewport()->rect().center());
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    for (const auto &name : {"UsercardQuickWhisper", "UsercardQuickProfile",
                             "UsercardQuickCopyId"})
    {
        if (auto *button = popup.findChild<QWidget *>(name);
            button && button->isVisible())
            checkPoint(QString::fromLatin1(name), button, button->rect().center());
    }
    if (auto *badges = popup.findChild<QWidget *>("UsercardObservedBadges");
        badges && badges->isVisible() &&
        badges->property("firstRowBadgeCount").toInt() <
            badges->property("badgeCount").toInt())
        checkPoint("badge-count-chip", badges,
                   QPoint(badges->property("firstRowBadgeCount").toInt() *
                              int(24 * popup.scale()) + 5,
                          int(12 * popup.scale())));
    if (auto *grip = popup.findChild<QWidget *>("UsercardResizeGrip"))
    {
        checkPoint("resize-left", grip, QPoint(2, grip->height() / 2));
        checkPoint("resize-right", grip, QPoint(grip->width() - 3, grip->height() / 2));
        checkPoint("resize-top", grip, QPoint(grip->width() / 2, 2));
        checkPoint("resize-bottom", grip, QPoint(grip->width() / 2, grip->height() - 3));
    }
#endif
#else
    (void)popup;
#endif
    return result;
}

bool writeReport(const QString &path, const OfflineApplication &app,
                 const PreviewData &data, const PreviewPopup &popup,
                 const QJsonArray &checks)
{
    auto *tabs =
        popup.findChild<QTabWidget *>(QStringLiteral("UsercardContent"));
    auto *profile =
        popup.findChild<QScrollArea *>(QStringLiteral("UsercardProfileScroll"));
    QJsonObject result{
        {QStringLiteral("mode"), QStringLiteral("offline-actual-usercard")},
#ifdef LEAFYRINO_BASELINE_USERCARD_PREVIEW
        {QStringLiteral("layout"),
         QStringLiteral("baseline-source-offline-seam")},
#else
        {QStringLiteral("layout"),
         QStringLiteral("reference-usercard-stage-2")},
#endif
        {QStringLiteral("normalApplicationInitialized"), false},
        {QStringLiteral("networkManagerInitialized"),
         NetworkManager::workerThread != nullptr},
        {QStringLiteral("accountsLoaded"), false},
        {QStringLiteral("qtRuntime"), QString::fromLatin1(qVersion())},
        {QStringLiteral("autoClose"),
         popup.property("previewAutoClose").toBool()},
        {QStringLiteral("mouseTracking"),
         QJsonObject{{QStringLiteral("tabs"), tabs && tabs->hasMouseTracking()},
                     {QStringLiteral("tabBar"),
                      tabs && tabs->tabBar()->hasMouseTracking()},
                     {QStringLiteral("contentPage"),
                      tabs && tabs->currentWidget()->hasMouseTracking()},
                     {QStringLiteral("profileScroll"),
                      profile && profile->hasMouseTracking()},
                     {QStringLiteral("profileViewport"),
                      profile && profile->viewport()->hasMouseTracking()}}},
        {QStringLiteral("userId"), data.userId},
        {QStringLiteral("login"), data.login},
        {QStringLiteral("messageCount"),
         int(data.messages->getMessageSnapshot().size())},
        {QStringLiteral("width"), popup.width()},
        {QStringLiteral("height"), popup.height()},
        {QStringLiteral("devicePixelRatio"), popup.devicePixelRatioF()},
        {QStringLiteral("settings"), app.settingsDir.path()},
        {QStringLiteral("checks"), checks},
    };
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(result).toJson(QJsonDocument::Indented)) >
               0 &&
           file.commit();
}

}  // namespace

int main(int argc, char **argv)
{
    QApplication qapp(argc, argv);
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen")
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    GuiLifecycleGuard lifecycleGuard;
    qapp.setApplicationName(QStringLiteral("Leafyrino-usercard-preview"));
    qapp.setOrganizationName(QStringLiteral("LeafyrinoSyntheticPreview"));
    qapp.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QObject::connect(&qapp, &QApplication::aboutToQuit, &qapp,
                     &markAppAboutToQuit);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {{QStringLiteral("case")},
         QStringLiteral("a, b, empty, or long"),
         QStringLiteral("case"),
         QStringLiteral("a")},
        {{QStringLiteral("width")},
         QStringLiteral("Card width in logical pixels"),
         QStringLiteral("width"),
         QStringLiteral("860")},
        {{QStringLiteral("height")},
         QStringLiteral("Card height in logical pixels"),
         QStringLiteral("height"),
         QStringLiteral("420")},
        {{QStringLiteral("scale")},
         QStringLiteral("Widget scale override"),
         QStringLiteral("scale"),
         QStringLiteral("1.0")},
        {{QStringLiteral("report")},
         QStringLiteral("Write fixture-only diagnostics JSON"),
         QStringLiteral("path")},
        {{QStringLiteral("screenshot")},
         QStringLiteral("Save this fixture widget's own rendered PNG (not an "
                        "OS screenshot)"),
         QStringLiteral("path")},
        {{QStringLiteral("banner-demo")},
         QStringLiteral("Show a raid and a mass-gift corner banner on the "
                        "card's chat view (synthetic, no avatar download)")},
        {{QStringLiteral("show-moderation")},
         QStringLiteral("Show the moderation strip as for a moderator")},
        {{QStringLiteral("toast-demo")},
         QStringLiteral("Render two highlight cards on a plain widget")},
        {{QStringLiteral("stats-demo")},
         QStringLiteral("Check stream statistics and render an offline fixture")},
        {{QStringLiteral("own-timeout-checks")},
         QStringLiteral("Drive the own-timeout input states on a synthetic "
                        "channel and exit")},
        {{QStringLiteral("prediction-demo")},
         QStringLiteral("Render the prediction banner and dialog with "
                        "synthetic predictions and exit")},
        {{QStringLiteral("clip-demo")},
         QStringLiteral("Last synthetic message carries a clip link with a "
                        "ready card (no network)")},
        {{QStringLiteral("link-preview-checks")},
         QStringLiteral("Run the link-preview checks against a local stand-in "
                        "resolver and exit")},
        {{QStringLiteral("public-logs-checks")},
         QStringLiteral("Run the public chat log checks against local stand-in "
                        "mirrors and exit")},
        {{QStringLiteral("public-logs-live")},
         QStringLiteral("One real page from the public mirrors: "
                        "user-id,login,channel (network!)"),
         QStringLiteral("who")},
        {{QStringLiteral("seventv-personal-live")},
         QStringLiteral("Look up 7TV personal sets of user-id:login,... "
                        "(network!)"),
         QStringLiteral("users")},
        {{QStringLiteral("viewer-counts-live")},
         QStringLiteral("Real viewer counts for logins a,b,c (network!)"),
         QStringLiteral("logins")},
        {{QStringLiteral("badge-demo")},
         QStringLiteral("Show a synthetic expanded badge collection inline")},
        {{QStringLiteral("menu-demo")},
         QStringLiteral("Render the compact production actions menu (offscreen only)")},
        {{QStringLiteral("activity-demo")},
         QStringLiteral("Compatibility flag: the actual activity panel is always shown")},
        {{QStringLiteral("render-hidden")},
         QStringLiteral("Render with the native platform without showing "
                        "fixture windows")},
        {{QStringLiteral("history-checks")},
         QStringLiteral("Exercise actual history widgets against a "
                        "process-owned synthetic local server")},
        {{QStringLiteral("startup-history-checks")}, QStringLiteral("Verify startup history over loopback")},
        {{QStringLiteral("history-badge-checks")}, QStringLiteral("Verify late-arriving badges in historical chat rows")},
        {{QStringLiteral("noctis-tooltip-checks")}, QStringLiteral("Verify styled text tooltips offline")},
        {{QStringLiteral("service-health-checks")}, QStringLiteral("Check service indicator with a local endpoint")},
        {{QStringLiteral("badge-picker-check")}, QStringLiteral("Render and check the badge picker hover preview")},
        {{QStringLiteral("emote-picker-check")}, QStringLiteral("Check the emote picker title, search and motion")},
        {{QStringLiteral("roles-checks")},
         QStringLiteral("Exercise the actual role page and provider against a "
                        "process-owned synthetic local server")},
        {{QStringLiteral("scripted-checks")},
         QStringLiteral(
             "Exercise A-B-A, empty/long and responsive actual widgets")},
        {{QStringLiteral("auto-close")},
         QStringLiteral("Use the actual auto-closing frameless popup variant")},
        {{QStringLiteral("native-hit-test-checks")},
         QStringLiteral("Query only this synthetic popup's Windows hit tests")},
        {{QStringLiteral("quit-after")},
         QStringLiteral("Exit the test copy after milliseconds"),
         QStringLiteral("milliseconds")},
    });
    parser.process(qapp);
    clipDemo = parser.isSet(QStringLiteral("clip-demo"));
    const auto scenario = parser.value(QStringLiteral("case"));
    if (!QStringList{QStringLiteral("a"), QStringLiteral("b"),
                     QStringLiteral("empty"), QStringLiteral("long")}
             .contains(scenario))
    {
        return 22;
    }
    auto *screen = previewScreen();
    if (!screen &&
        qEnvironmentVariable("QT_QPA_PLATFORM") != QStringLiteral("offscreen"))
    {
        // Never fall back to the user's primary monitor for visible tests.
        return 23;
    }
    initResources();
    initializeUiLanguage(qEnvironmentVariable("LEAFYRINO_PREVIEW_LANGUAGE", "ru"));
    auto appOwner = std::make_unique<OfflineApplication>();
    auto &app = *appOwner;
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    installMenuStyle();
    noctistooltip::install();
    if (parser.isSet(QStringLiteral("noctis-tooltip-checks")))
        return preview::checkNoctisTooltips(parser.value(QStringLiteral("report")),
                                            parser.value(QStringLiteral("screenshot")));
    if (parser.isSet(QStringLiteral("startup-history-checks")))
        return preview::checkStartupHistory(parser.value(QStringLiteral("report")));
    if (parser.isSet(QStringLiteral("history-badge-checks")))
        return preview::checkHistoryBadges(parser.value(QStringLiteral("report")));
    if (parser.isSet(QStringLiteral("service-health-checks")))
    {
        using namespace servicehealth;
        NetworkManager::init();
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) return 40;
        bool healthy = false;
        int hits = 0;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                socket->readAll();
                ++hits;
                socket->write(healthy ? "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 11\r\n\r\n{\"id\":\"ok\"}"
                                      : "HTTP/1.1 503 Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
            });
        });
        PixmapButton anchor;
        ServiceHealthCard card(&anchor);
        pajlada::Signals::SignalHolder holder;
        holder.managedConnect(changed, [&] { card.refresh(); });
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/test").arg(server.serverPort()));
        const NetworkResult timeout(QNetworkReply::TimeoutError, {}, {});
        for (int i = 0; i < 8; ++i) failed(SevenTV, url, timeout);
        const bool quiet = anchor.isHidden() && !entries[SevenTV].url.isEmpty() && hits == 0;
        const bool visible = publiclogchecks::waitUntil([&] { return !anchor.isHidden(); }, 9000);
        const bool singleProbe = hits == 1;
        card.open();
        const bool shown = card.isVisible();
        card.grab().save(parser.value(QStringLiteral("screenshot")));
        healthy = true;
        card.findChild<QPushButton *>()->click();
        const bool recovered = publiclogchecks::waitUntil([&] { return anchor.isHidden() && !card.isVisible(); }, 5000);
        failed(History, url, timeout);
        success(History);
        QEventLoop wait;
        QTimer::singleShot(4400, &wait, &QEventLoop::quit);
        wait.exec();
        const bool cancelled = hits == 2 && !entries[History].unavailable;
        QFile report(parser.value(QStringLiteral("report")));
        if (report.open(QIODevice::WriteOnly))
            report.write(QJsonDocument(QJsonObject{{"quiet", quiet}, {"visible", visible},
                {"singleProbe", singleProbe}, {"card", shown}, {"recovered", recovered},
                {"cancelled", cancelled}, {"requests", hits}}).toJson());
        NetworkManager::deinit();
        return quiet && visible && singleProbe && shown && recovered && cancelled ? 0 : 41;
    }
    if (parser.isSet(QStringLiteral("emote-picker-check")))
        return EmotePickerPreviewCheck::run(parser.value(QStringLiteral("screenshot"))) ? 0 : 43;
    if (parser.isSet(QStringLiteral("badge-picker-check")))
    {
        NetworkManager::init();
        const bool ok = BadgePickerPreviewCheck::run(parser.value(QStringLiteral("screenshot")));
        NetworkManager::deinit();
        return ok ? 0 : 42;
    }
#endif
    if (clipDemo)
    {
        app.settings.showLinkTitles = false;  // offline: no resolver request
    }
    if (parser.isSet(QStringLiteral("link-preview-checks")))
    {
        QJsonArray checks;
        const int failed = linkpreview::runLinkPreviewChecks(app.settings, checks);
        QFile out(parser.value(QStringLiteral("report")));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(QJsonObject{{"mode", "link-preview"},
                                                {"checks", checks}})
                          .toJson());
        }
        return failed == 0 ? 0 : 30;
    }
    if (parser.isSet(QStringLiteral("public-logs-checks")))
    {
        QJsonArray checks;
        const int failed = publiclogchecks::runPublicLogChecks(checks);
        QFile out(parser.value(QStringLiteral("report")));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(QJsonObject{{"mode", "public-logs"},
                                                {"checks", checks}})
                          .toJson());
        }
        return failed == 0 ? 0 : 32;
    }
    if (parser.isSet(QStringLiteral("viewer-counts-live")))
    {
        NetworkManager::init();
        std::optional<QHash<QString, unsigned>> counts;
        chatterino::TwitchGql::getViewerCounts(
            parser.value(QStringLiteral("viewer-counts-live")).split(','),
            [&](QHash<QString, unsigned> value) {
                counts = value;
            });
        publiclogchecks::waitUntil([&] {
            return counts.has_value();
        }, 20000);
        QJsonObject result;
        if (counts)
        {
            for (auto it = counts->begin(); it != counts->end(); ++it)
            {
                result.insert(it.key(), double(it.value()));
            }
        }
        QFile out(parser.value(QStringLiteral("report")));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(QJsonObject{{"answered", counts.has_value()},
                                                {"counts", result}})
                          .toJson());
        }
        return counts ? 0 : 35;
    }
    if (parser.isSet(QStringLiteral("seventv-personal-live")))
    {
        NetworkManager::init();
        auto &lookup = SeventvPersonalLookup::instance();
        const auto store = QDir(QDir::tempPath())
                               .filePath(QStringLiteral("seventv-personal-test.json"));
        QFile::remove(store);
        lookup.setStorePath(store);
        const auto users =
            parser.value(QStringLiteral("seventv-personal-live")).split(',');
        for (const auto &user : users)
        {
            const auto parts = user.split(':');
            lookup.noteChatter(parts.value(0), parts.value(1));
        }
        QElapsedTimer timer;
        timer.start();
        // every user answered (a set or "none") and every found set loaded
        auto done = [&] {
            for (const auto &user : users)
            {
                const auto id = user.split(':').value(0);
                const auto set = lookup.rememberedSet(id);
                if (lookup.queued() > 0 ||
                    (!set.isEmpty() &&
                     app.personalEmotes.getEmoteSetsForTwitchUser(id).isEmpty()))
                {
                    return false;
                }
            }
            return true;
        };
        publiclogchecks::waitUntil([&] {
            return timer.elapsed() > 1500 && done();
        }, 30000);
        lookup.save();
        lookup.setStorePath(store);  // read back from disk
        QJsonObject result;
        for (const auto &user : users)
        {
            const auto id = user.split(':').value(0);
            QStringList names;
            for (const auto &set :
                 app.personalEmotes.getEmoteSetsForTwitchUser(id))
            {
                for (const auto &[name, emote] : *set)
                {
                    names.append(name.string);
                }
            }
            result.insert(id, QJsonObject{
                                  {"rememberedSet", lookup.rememberedSet(id)},
                                  {"emotes", names.join(' ')},
                              });
        }
        std::fprintf(stderr, "%s\n",
                     QJsonDocument(result).toJson().constData());
        QFile::remove(store);
        return 0;
    }
    if (parser.isSet(QStringLiteral("public-logs-live")))
    {
        const auto who = parser.value(QStringLiteral("public-logs-live")).split(',');
        if (who.size() != 3)
        {
            return 33;
        }
        NetworkManager::init();
        QJsonArray pages;
        QJsonObject cursor;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 2; ++i)
        {
            std::optional<QJsonObject> result;
            chatterino::publiclogs::requestPage(
                who[0], who[1], who[2], cursor, nullptr,
                [&](const QJsonObject &value) {
                    result = value;
                });
            publiclogchecks::waitUntil([&] {
                return result.has_value();
            }, 40000);
            if (!result)
            {
                break;
            }
            auto rows = result->value("rows").toArray();
            QJsonObject summary{{"status", result->value("status")},
                                {"error", result->value("error")},
                                {"more", result->value("more")},
                                {"rows", rows.size()},
                                {"ms", double(timer.elapsed())}};
            if (!rows.isEmpty())
            {
                summary.insert("newest", rows.first().toObject().value("time_utc"));
                summary.insert("oldest", rows.last().toObject().value("time_utc"));
            }
            pages.append(summary);
            cursor = result->value("next_cursor").toObject();
            if (!result->value("more").toBool())
            {
                break;
            }
        }
        QFile out(parser.value(QStringLiteral("report")));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(QJsonObject{{"mode", "public-logs-live"},
                                                {"pages", pages}})
                          .toJson());
        }
        return pages.isEmpty() ? 34 : 0;
    }
    const bool historyChecks = parser.isSet(QStringLiteral("history-checks"));
#ifdef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    if (historyChecks)
    {
        return 26;
    }
#endif
    auto splitOwner = std::make_unique<Split>(nullptr);
    auto &split = *splitOwner;
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    if (parser.isSet(QStringLiteral("roles-checks")))
    {
        QJsonArray checks;
        const int failed = roleschecks::runRolesChecks(
            split, fixture(scenario),
            std::clamp(parser.value(QStringLiteral("scale")).toFloat(), 0.75F, 2.0F),
            parser.value(QStringLiteral("screenshot")), checks);
        QFile out(parser.value(QStringLiteral("report")));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(QJsonObject{{"mode", "roles"},
                                                {"checks", checks}})
                          .toJson());
        }
        splitOwner.reset();
        return failed == 0 ? 0 : 33;
    }
#endif
    if (parser.isSet(QStringLiteral("stats-demo")))
    {
        auto checks = ChannelStats::fixtureChecks();
        const auto name = QStringLiteral("stats_fixture");
        auto channel = std::make_shared<Channel>(name, Channel::Type::None);
        const auto start = QDateTime::currentDateTime().addSecs(-1800);
        auto make = [&](int i, int offset) {
            auto message = std::make_shared<Message>();
            message->id = QString::number(i);
            message->loginName = QStringLiteral("viewer_%1").arg(i % 12);
            message->displayName = QStringLiteral("Viewer_%1").arg(i % 12);
            message->usernameColor = QColor::fromHsv((i % 12) * 27, 120, 230);
            message->messageText = QStringLiteral("Synthetic chat");
            message->serverReceivedTime = start.addSecs(offset);
            if (i < 40)
                message->flags.set(MessageFlag::FirstMessage);
            return MessagePtr(message);
        };
        std::vector<MessagePtr> messages;
        for (int i = 0; i < 1200; ++i)
            messages.push_back(make(i, (i * 19) % 1700));
        channel->addMessagesAtStart(messages);
        ChannelStats::attach(channel);
        const int before = ChannelStats::get(name).messages;
        channel->fillInMissingMessages({make(800, 400), make(1500, 550)});
        const auto data = ChannelStats::get(name);
        checks.append(QJsonObject{{"name", "reconnect signal counts newly recovered messages exactly once"},
                                  {"ok", before == 1200 && data.messages == before + 1}});
        showStreamStatsDialog(nullptr, name);
        QCoreApplication::processEvents();
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->windowTitle().contains(name))
            {
                widget->grab().save(parser.value(QStringLiteral("screenshot")), "PNG");
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
                noctistooltip::install();
                const auto rows = widget->findChildren<QWidget *>("StreamStatsRankRow");
                const bool noRedundant = !rows.isEmpty() &&
                    std::ranges::all_of(rows, [](const QWidget *row) {
                        return row->toolTip().isEmpty();
                    });
                checks.append(QJsonObject{{"name", "fully visible ranking rows have no redundant tooltip"},
                                          {"ok", noRedundant}});
                auto *chart = widget->findChild<QWidget *>("StreamStatsActivityChart");
                if (chart)
                {
                    const auto point = chart->rect().center();
                    QMouseEvent move(QEvent::MouseMove, point, chart->mapToGlobal(point),
                                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(chart, &move);
                    QCoreApplication::processEvents();
                    if (auto *tip = preview::visibleTextTooltip())
                        tip->grab().save(parser.value(QStringLiteral("screenshot")) + ".tip.png");
                    checks.append(QJsonObject{{"name", "chart hover reveals additional details in styled popup"},
                                              {"ok", preview::visibleTextTooltip() != nullptr}});
                    noctistooltip::hideText();
                }
                else
                    checks.append(QJsonObject{{"name", "activity chart is present"}, {"ok", false}});
#endif
            }
        QFile report(parser.value(QStringLiteral("report")));
        if (report.open(QIODevice::WriteOnly))
            report.write(QJsonDocument(checks).toJson());
        for (const auto &value : checks)
            if (!value.toObject().value("ok").toBool())
                return 44;
        return checks.isEmpty() ? 45 : 0;
    }
    if (parser.isSet(QStringLiteral("toast-demo")))
    {
        QWidget host;
        host.resize(900, 300);
        host.setStyleSheet(QStringLiteral("background:#18171c;"));
        host.show();
        const QDir avatars(getApp()->getPaths().twitchProfileAvatars);
        QDir().mkpath(avatars.absolutePath());
        for (const auto &login : {QStringLiteral("morphe_ya"), QStringLiteral("dangerlyoha")})
        {
            QPixmap avatar(48, 48);
            avatar.fill(login == QStringLiteral("morphe_ya") ? QColor("#9466bd") : QColor("#486bba"));
            QPainter profile(&avatar);
            profile.setPen(Qt::white);
            profile.drawText(avatar.rect(), Qt::AlignCenter, login.left(1).toUpper());
            profile.end();
            avatar.save(avatars.filePath(login + QStringLiteral(".png")));
        }
        auto make = [](const QString &login, const QString &text, QColor color) {
            auto message = std::make_shared<Message>();
            message->loginName = login.toLower();
            message->displayName = login;
            message->messageText = text;
            message->usernameColor = color;
            QPixmap pixmap(24, 24);
            pixmap.fill(Qt::transparent);
            {
                QPainter painter(&pixmap);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor("#ecc861"));
                painter.drawEllipse(1, 1, 22, 22);
                painter.setBrush(QColor("#342936"));
                painter.drawEllipse(6, 7, 3, 4);
                painter.drawEllipse(15, 7, 3, 4);
                painter.drawRoundedRect(7, 15, 10, 3, 1, 1);
            }
            const auto emote = std::make_shared<Emote>(Emote{
                .name = EmoteName{QStringLiteral("PreviewSmile")},
                .images = ImageSet{Image::fromResourcePixmap(pixmap)},
            });
            message->elements.push_back(std::make_unique<TextElement>(QStringLiteral("@Mist1X_X"), MessageElementFlag::Text));
            message->elements.push_back(std::make_unique<EmoteElement>(emote, MessageElementFlag::Emote));
            message->elements.push_back(std::make_unique<TextElement>(text, MessageElementFlag::Text));
            return MessagePtr(message);
        };
        showHighlightToastOn(&host, QStringLiteral("morphe_ya"),
                             make(QStringLiteral("Some_Viewer"),
                                  QStringLiteral("@Mist1X_X glad to see you here, synthetic text"),
                                  QColor("#ff7f50")));
        showHighlightToastOn(&host, QStringLiteral("dangerlyoha"),
                             make(QStringLiteral("Another_One"),
                                  QStringLiteral("mist1x what do you think about this very long synthetic message that should be cut"),
                                  QColor("#1e90ff")));
        auto whisper = std::make_shared<Message>();
        whisper->loginName = "morphe_ya";
        whisper->displayName = "Morphe_ya";
        whisper->messageText = "Привет! Это тест личного сообщения.";
        whisper->usernameColor = QColor("#70c6af");
        whisper->elements.push_back(std::make_unique<TextElement>(
            whisper->displayName + ":", MessageElementFlag::Username));
        whisper->elements.push_back(std::make_unique<TextElement>(
            whisper->messageText, MessageElementFlag::Text));
        MessageBuilder::makeWhisperCard(*whisper, uiText("Whisper"),
                                       whisper->loginName);
        showHighlightToastOn(&host, QStringLiteral("/whispers"), whisper);
        QEventLoop animation;
        QTimer::singleShot(220, &animation, &QEventLoop::quit);
        animation.exec();
        host.grab().save(parser.value(QStringLiteral("screenshot")), "PNG");
        return 0;
    }
    if (parser.isSet(QStringLiteral("own-timeout-checks")))
    {
        const int failed = owntimeout::runOwnTimeoutChecks(
            split, parser.value(QStringLiteral("report")),
            parser.value(QStringLiteral("screenshot")));
        splitOwner.reset();
        return failed == 0 ? 0 : 31;
    }
    if (parser.isSet(QStringLiteral("prediction-demo")))
    {
        const int failed = predictiondemo::runPredictionDemo(
            split, parser.value(QStringLiteral("screenshot")));
        splitOwner.reset();
        return failed;
    }
    auto controlsOwner = std::make_unique<QWidget>();
    auto &controls = *controlsOwner;
    controls.setObjectName(QStringLiteral("UsercardPreviewControls"));
    controls.setAttribute(Qt::WA_DontShowOnScreen,
                          parser.isSet(QStringLiteral("render-hidden")));
    controls.setWindowTitle(
        QStringLiteral("Leafyrino offline synthetic preview controls"));
    controls.setStyleSheet(QStringLiteral(
        "QWidget#UsercardPreviewControls {background:#17151d;color:#ddd;}"));
    auto *layout = new QVBoxLayout(&controls);
    layout->addWidget(new QLabel(QStringLiteral(
        "Actual UserInfoPopup • synthetic only • no Twitch / PLUS")));
    auto *row = new QHBoxLayout;
    layout->addLayout(row);
    const bool autoClose = parser.isSet(QStringLiteral("auto-close"));
    auto popupOwner = std::make_unique<PreviewPopup>(autoClose, &split, true);
    QPointer<PreviewPopup> popupGuard(popupOwner.get());
    auto &popup = *popupOwner;
    popup.setAttribute(Qt::WA_DontShowOnScreen,
                       parser.isSet(QStringLiteral("render-hidden")));
    popup.setProperty("previewAutoClose", autoClose);
    QObject::connect(&popup, &QObject::destroyed, &controls, [&] {
        // The actual auto-close mode self-deletes on window deactivation.
        // Keep its visible owner alive, as the normal main window would be.
        popupOwner.release();
        for (auto *button : controls.findChildren<QPushButton *>())
        {
            button->setEnabled(false);
        }
        controls.setWindowTitle(
            QStringLiteral("Leafyrino synthetic controls — popup closed"));
    });
    // Native popup ownership must follow a visible window. The hidden Split
    // still supplies the real ChannelView context, but cannot own a visible
    // transient window reliably on Windows.
    popup.setParent(&controls, popup.windowFlags());
    // The popup normally owns itself after close. This fixture owns it on the
    // stack so that the mock application always outlives all of its widgets.
    popup.setAttribute(Qt::WA_DeleteOnClose, false);
    popup.setOverrideScale(std::clamp(
        parser.value(QStringLiteral("scale")).toFloat(), 0.75F, 2.0F));
    auto current = fixture(scenario);
    auto showCase = [&](const QString &next) {
        if (!popupGuard)
        {
            return;
        }
        current = fixture(next);
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
        if (parser.isSet("badge-demo"))
            current.earnedBadges = fixtureBadgeCollection(30);
#endif
        popupGuard->setOfflinePreviewData(current);
        popupGuard->show();
    };
    for (const auto &name : {QStringLiteral("a"), QStringLiteral("b"),
                             QStringLiteral("empty"), QStringLiteral("long")})
    {
        auto *button = new QPushButton(name.toUpper(), &controls);
        button->setObjectName(QStringLiteral("preview-case-") + name);
        row->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, &controls, [&, name] {
            showCase(name);
        });
    }
    const auto width =
        std::clamp(parser.value(QStringLiteral("width")).toInt(), 480, 1400);
    const auto height =
        std::clamp(parser.value(QStringLiteral("height")).toInt(), 400, 1000);
    if (screen)
    {
        const auto bounds = screen->availableGeometry();
        controls.move(bounds.topLeft() + QPoint(24, 24));
        popup.move(bounds.topLeft() + QPoint(40, 130));
    }
    controls.show();
    showCase(scenario);
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    if (parser.isSet("badge-demo"))
        QTimer::singleShot(330, &popup, [&] {
            clickBadgeChip(popup.findChild<QWidget *>("UsercardObservedBadges"),
                           popup.scale());
        });
#endif
    // A queued size restores the requested test geometry after native DPI
    // notifications and the existing popup's scale/layout initialization.
    QTimer::singleShot(180, &popup, [&] {
#ifdef LEAFYRINO_BASELINE_USERCARD_PREVIEW
        // Historical comparison fixture only. Never override the actual
        // usercard's constraints: doing so concealed a fixed-size regression.
        popup.setMinimumSize(0, 0);
        popup.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
#endif
        popup.resize(width, height);
    });
    const auto report = parser.value(QStringLiteral("report"));
    QJsonArray checks;
    bool checksPassed = true;
    if (parser.isSet(QStringLiteral("scripted-checks")) && !historyChecks)
    {
        QTimer::singleShot(400, &popup, [&] {
            auto check = [&](const QString &name, bool ok) {
                checksPassed &= ok;
                checks.append(QJsonObject{{QStringLiteral("name"), name},
                                          {QStringLiteral("ok"), ok}});
            };
            for (const auto &name :
                 {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a"),
                  QStringLiteral("empty"), QStringLiteral("long")})
            {
                showCase(name);
                bool identityVisible = false;
                for (auto *widget : popup.findChildren<QWidget *>())
                {
                    if (auto *label = dynamic_cast<Label *>(widget))
                    {
                        identityVisible |=
                            label->getText() ==
                            QStringLiteral("ID: ") + current.userId;
                    }
                }
                check(name + QStringLiteral(": actual UID label"),
                      identityVisible);
                auto *view = popup.findChild<ChannelView *>();
                check(name + QStringLiteral(": actual ChannelView data"),
                      view && view->underlyingChannel() == current.messages &&
                          view->channel()->getMessageSnapshot().size() ==
                              size_t(name == QStringLiteral("long")    ? 5000
                                     : name == QStringLiteral("empty") ? 0
                                                                       : 18));
            }
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
            {
                auto mixed = fixture(QStringLiteral("a"));
                auto appendIdentityRow = [&](const QString &uid,
                                             const QString &login) {
                    MessageBuilder builder;
                    auto &message = builder.message();
                    message.id =
                        QStringLiteral("synthetic-mixed-") + uid + login;
                    message.userID = uid;
                    message.loginName = login;
                    message.displayName = login;
                    message.channelName = QStringLiteral("fixture_room");
                    message.messageText =
                        QStringLiteral("Identity filtering fixture");
                    message.serverReceivedTime =
                        QDateTime::currentDateTimeUtc();
                    builder.emplace<TextElement>(login + QStringLiteral(": "),
                                                 MessageElementFlag::Username);
                    builder.emplace<TextElement>(message.messageText,
                                                 MessageElementFlag::Text);
                    mixed.messages->addMessage(builder.release(),
                                               MessageContext::Repost);
                };
                appendIdentityRow(QStringLiteral("910000002"), mixed.login);
                appendIdentityRow(mixed.userId,
                                  QStringLiteral("fixture_old_login"));
                appendIdentityRow({}, mixed.login);
                mixed.sourceMessages = mixed.messages;
                popup.setOfflinePreviewData(mixed);
                auto *view = popup.findChild<ChannelView *>();
                const auto visible = view
                                         ? view->channel()->getMessageSnapshot()
                                         : std::vector<MessagePtr>{};
                check(QStringLiteral("normal UID filter excludes same-login "
                                     "other UID and includes old login"),
                      visible.size() == 19 &&
                          std::all_of(visible.begin(), visible.end(),
                                      [&](const auto &message) {
                                          return message->userID ==
                                                 mixed.userId;
                                      }) &&
                          std::any_of(visible.begin(), visible.end(),
                                      [](const auto &message) {
                                          return message->loginName ==
                                                 QStringLiteral(
                                                     "fixture_old_login");
                                      }));
                const auto *activityCount = popup.findChild<QLabel *>("UsercardActivityCount");
                check("activity counts only the target UID including older login", activityCount &&
                      activityCount->text() == QStringLiteral("19"));
            }

            {
                // Punishments about the person: shown even with no messages,
                // tied by Twitch ID, never from another channel. The notices
                // go through the real IRC parser (CLEARCHAT / CLEARMSG).
                auto feed = [](const ChannelPtr &sink, const QString &raw) {
                    std::unique_ptr<Communi::IrcMessage> message(
                        Communi::IrcMessage::fromData(raw.toUtf8(), nullptr));
                    IrcMessageHandler::parseMessageInto(message.get(), *sink,
                                                        nullptr);
                };
                auto clearchat = [&](const ChannelPtr &sink,
                                     const QString &login, const QString &uid,
                                     const QString &duration) {
                    QStringList tags{
                        QStringLiteral("room-id=900000001"),
                        QStringLiteral("tmi-sent-ts=%1")
                            .arg(QDateTime::currentMSecsSinceEpoch())};
                    if (!uid.isEmpty())
                    {
                        tags << QStringLiteral("target-user-id=") + uid;
                    }
                    if (!duration.isEmpty())
                    {
                        tags << QStringLiteral("ban-duration=") + duration;
                    }
                    feed(sink, u'@' + tags.join(u';') +
                                   QStringLiteral(" :tmi.twitch.tv CLEARCHAT "
                                                  "#fixture_room :") +
                                   login);
                };
                auto notices = [](const std::vector<MessagePtr> &rows) {
                    return std::count_if(rows.begin(), rows.end(),
                                         [](const auto &m) {
                                             return !m->timeoutUser.isEmpty();
                                         });
                };
                auto shown = [&](const PreviewData &data) {
                    popup.setOfflinePreviewData(data);
                    auto *view = popup.findChild<ChannelView *>();
                    return view ? view->channel()->getMessageSnapshot()
                                : std::vector<MessagePtr>{};
                };
                auto person = [&](const QString &login, const QString &uid) {
                    auto data = fixture(QStringLiteral("empty"));
                    data.login = login;
                    data.displayName = login;
                    data.userId = uid;
                    data.messages = std::make_shared<Channel>(
                        QStringLiteral("fixture_room"), Channel::Type::None);
                    data.sourceMessages = data.messages;
                    return data;
                };

                // 1. messages, then a ban and one deleted message
                auto writer = fixture(QStringLiteral("a"));
                const auto own =
                    writer.messages->getMessageSnapshot().back()->id;
                feed(writer.messages,
                     QStringLiteral("@login=fixture_viewer;target-msg-id=") +
                         own +
                         QStringLiteral(" :tmi.twitch.tv CLEARMSG "
                                        "#fixture_room :deleted text"));
                clearchat(writer.messages, writer.login, writer.userId, {});
                clearchat(writer.messages, QStringLiteral("someone_else"),
                          QStringLiteral("920000001"), {});
                feed(writer.messages,
                     QStringLiteral("@room-id=900000001 :tmi.twitch.tv "
                                    "CLEARCHAT #fixture_room"));
                writer.sourceMessages = writer.messages;
                auto rows = shown(writer);
                check(QStringLiteral("messages plus own ban and deletion, "
                                     "not other people's or chat clear"),
                      notices(rows) == 2 &&
                          std::any_of(rows.begin(), rows.end(),
                                      [](const auto &m) {
                                          return m->messageText.contains(
                                              QStringLiteral(
                                                  "permanently banned"));
                                      }) &&
                          rows.size() == size_t(18 + 2));

                // 2. no messages, banned
                auto silent = person(QStringLiteral("silent_user"),
                                     QStringLiteral("910000006"));
                clearchat(silent.messages, silent.login, silent.userId, {});
                rows = shown(silent);
                check(QStringLiteral("ban shown without any messages"),
                      rows.size() == 1 && notices(rows) == 1 &&
                          rows.front()->messageText.contains(
                              QStringLiteral("silent_user has been "
                                             "permanently banned")));

                // 6. reopening does not duplicate
                rows = shown(silent);
                check(QStringLiteral("reopening shows the ban once"),
                      rows.size() == 1);

                // 3. no messages, timed out
                auto muted = person(QStringLiteral("muted_user"),
                                    QStringLiteral("910000007"));
                clearchat(muted.messages, muted.login, muted.userId,
                          QStringLiteral("600"));
                rows = shown(muted);
                check(QStringLiteral("timeout shown without any messages"),
                      rows.size() == 1 &&
                          rows.front()->messageText.contains(
                              QStringLiteral("has been timed out")));

                // 4. nothing at all
                auto nobody = person(QStringLiteral("nobody_user"),
                                     QStringLiteral("910000008"));
                clearchat(nobody.messages, QStringLiteral("someone_else"),
                          QStringLiteral("920000001"), {});
                rows = shown(nobody);
                check(QStringLiteral("no messages and no punishment stays "
                                     "empty"),
                      rows.empty());

                // 5. other channel (shared chat) and a reused login
                auto shared = person(QStringLiteral("silent_user"),
                                     QStringLiteral("910000006"));
                {
                    MessageBuilder builder;
                    builder->flags.set(MessageFlag::System,
                                       MessageFlag::Timeout,
                                       MessageFlag::ModerationAction,
                                       MessageFlag::SharedMessage);
                    builder->channelName = QStringLiteral("other_room");
                    builder->timeoutUser = QStringLiteral("silent_user");
                    builder->timeoutUserID = QStringLiteral("910000006");
                    builder->messageText = QStringLiteral(
                        "silent_user has been banned in other_room.");
                    builder.emplace<TextElement>(builder->messageText,
                                                 MessageElementFlag::Text);
                    shared.messages->addMessage(builder.release(),
                                                MessageContext::Original);
                }
                rows = shown(shared);
                check(QStringLiteral("ban in another shared-chat channel is "
                                     "not shown"),
                      rows.empty());
                auto reused = person(QStringLiteral("silent_user"),
                                     QStringLiteral("910000006"));
                clearchat(reused.messages, QStringLiteral("silent_user"),
                          QStringLiteral("930000001"), {});
                rows = shown(reused);
                check(QStringLiteral("ban of another Twitch ID with the same "
                                     "login is not shown"),
                      rows.empty());

                // 7. switching people drops the previous card's notices
                rows = shown(silent);
                const auto silentRows = rows.size();
                rows = shown(muted);
                check(QStringLiteral("switching user shows only that user's "
                                     "punishment"),
                      silentRows == 1 && rows.size() == 1 &&
                          rows.front()->timeoutUser ==
                              QStringLiteral("muted_user"));

                // 8. "chat cleared by <moderator>" is not about the moderator
                auto moderator = person(QStringLiteral("mod_user"),
                                        QStringLiteral("910000009"));
                {
                    MessageBuilder builder;
                    builder->flags.set(MessageFlag::System,
                                       MessageFlag::ClearChat,
                                       MessageFlag::ModerationAction);
                    builder->timeoutUser = QStringLiteral("mod_user");
                    builder->messageText =
                        QStringLiteral("mod_user cleared the chat.");
                    builder.emplace<TextElement>(builder->messageText,
                                                 MessageElementFlag::Text);
                    moderator.messages->addMessage(builder.release(),
                                                   MessageContext::Original);
                }
                rows = shown(moderator);
                check(QStringLiteral("chat clear by a moderator is not shown "
                                     "as their punishment"),
                      rows.empty());
            }
#endif
            showCase(scenario);
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
            {
                bool storedOutsideProfile = false;
                for (auto *widget : popup.findChildren<QWidget *>())
                {
                    if (auto *label = dynamic_cast<Label *>(widget))
                    {
                        storedOutsideProfile |=
                            label->getText() ==
                                QStringLiteral("ID: ") + current.userId &&
                            !label->isVisible() &&
                            label->property("copy-text").toString() ==
                                current.userId;
                    }
                }
                check(QStringLiteral("UID remains stored and copyable without "
                                     "a technical profile row"),
                      storedOutsideProfile);
                auto *badges = popup.findChild<QWidget *>(
                    QStringLiteral("UsercardObservedBadges"));
                check(QStringLiteral("provided earned badge collection is "
                                     "visible (eight synthetic icons)"),
                      current.earnedBadges.size() == 8 && badges &&
                          badges->isVisible());
                const auto *whisper = popup.findChild<QToolButton *>("UsercardQuickWhisper");
                const auto *profile = popup.findChild<QToolButton *>("UsercardQuickProfile");
                const auto *copy = popup.findChild<QToolButton *>("UsercardQuickCopyId");
                check("quick actions are visible inside the profile",
                      whisper && profile && copy && whisper->isVisible() &&
                          profile->isVisible() && copy->isVisible());
                check("labelled quick actions have no hover tooltip but retain accessible descriptions",
                      whisper && profile && copy && whisper->toolTip().isEmpty() &&
                      profile->toolTip().isEmpty() && copy->toolTip().isEmpty() &&
                      !profile->accessibleName().isEmpty());
                check("production usercard constructor does not lock width or height",
                      popup.maximumWidth() > popup.width() && popup.maximumHeight() > popup.height());
                auto extended = fixture(scenario);
                constexpr int largeBadgeCount = 82;
                extended.earnedBadges = fixtureBadgeCollection(largeBadgeCount);
                popup.setOfflinePreviewData(extended);
                popup.resize(780, 540);
                owntimeout::pump(100);
                const auto *narrowChannel = popup.findChild<QLabel *>("UsercardHeadingChannel");
                check("narrow channel heading prioritizes complete channel login",
                      narrowChannel && narrowChannel->text() == QStringLiteral("fixture_room") &&
                      narrowChannel->width() >= QFontMetrics(narrowChannel->font()).horizontalAdvance(narrowChannel->text()));
                popup.resize(880, 315);
                owntimeout::pump(180);
                const auto *initialScroll = popup.findChild<QScrollArea *>("UsercardProfileScroll");
                check("unresized usercard fits collapsed profile instead of opening with scrollbar",
                      popup.height() > 315 && initialScroll && initialScroll->verticalScrollBar()->maximum() == 0 &&
                      !initialScroll->verticalScrollBar()->isVisible());
                const int baseHeight = badges->height();
                const int basePopupHeight = popup.height();
                const int prefix = badges->property("firstRowBadgeCount").toInt();
                const auto count = popup.findChild<QLabel *>("UsercardActivityCount");
                const auto latest = popup.findChild<QLabel *>("UsercardActivityLatest");
                check("activity shows loaded messages and last timestamp", count && latest &&
                      count->text() == QStringLiteral("18") && latest->text() != QStringLiteral("—"));
                const auto *activity = popup.findChild<QWidget *>("UsercardActivity");
                check("activity does not show intrusive tooltips", activity && count && latest &&
                      activity->toolTip().isEmpty() && count->toolTip().isEmpty() && latest->toolTip().isEmpty());
                const auto *headingChannel = popup.findChild<QLabel *>("UsercardHeadingChannel");
                const auto *headingAvatar = popup.findChild<QLabel *>("ChannelIdentityAvatar");
                check("channel identity shows name without hash and a round channel avatar", headingChannel && headingAvatar &&
                    headingChannel->text() == QStringLiteral("fixture_room") && headingAvatar->isVisible() && !headingAvatar->pixmap().isNull());

                // Show the actual production image-tooltip on a real badge,
                // using only the fixture's loaded synthetic asset.
                const QPoint hoverPoint(10, std::max(1, int(12 * popup.scale())));
                // The hidden native fixture can be on a different monitor
                // from the physical cursor. ChannelView's CursorPosition
                // bound check deliberately chooses that cursor monitor;
                // synthesize an interior global point there, not a clamped
                // point from the fixture's other screen.
                const auto *cursorScreen = QGuiApplication::screenAt(QCursor::pos());
                const auto hoverGlobal = cursorScreen
                    ? cursorScreen->availableGeometry().center() - QPoint(150,100)
                    : badges->mapToGlobal(hoverPoint);
                QMouseEvent hover(QEvent::MouseMove, hoverPoint, hoverGlobal, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(badges, &hover);
                owntimeout::pump(60);
                QWidget *hoverPreview = nullptr;
                for (auto *candidate : QApplication::topLevelWidgets())
                    if (candidate->objectName() == "UsercardBadgeHoverPreview") hoverPreview = candidate;
                bool enlarged = false;
                bool highResolution = false;
                bool shortTitle = false;
                bool acquisitionText = false;
                if (hoverPreview)
                {
                    for (auto *label : hoverPreview->findChildren<QLabel *>())
                    {
                        enlarged |= label->isVisible() && !label->pixmap().isNull() &&
                                    label->pixmap().deviceIndependentSize().width() > 26;
                        if (!label->pixmap().isNull())
                        {
                            const auto pixels = label->pixmap().toImage();
                            const auto marker = pixels.pixelColor(pixels.width()/4,pixels.height()/2);
                            highResolution |= marker.blue() > 180 && marker.red() < 90 && pixels.width() >= 36;
                        }
                        shortTitle |= label->text().contains(QStringLiteral("Synthetic badge 1"));
                        acquisitionText |= label->text().contains(QStringLiteral("ACQUISITION SENTINEL"));
                    }
                }
                check("usercard badge hover uses the shared enlarged image preview", hoverPreview && hoverPreview->isVisible() && enlarged);
                check("badge preview selects actual high-resolution source and omits acquisition description",
                      highResolution && shortTitle && !acquisitionText);
                if (hoverPreview)
                {
                    const auto anchor = hoverPreview->pos();
                    const QPoint delta(3, 2);
                    QMouseEvent tinyMove(QEvent::MouseMove, hoverPoint + delta, hoverGlobal + delta, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(badges, &tinyMove);
                    QHelpEvent delayed(QEvent::ToolTip, hoverPoint + delta, hoverGlobal + delta);
                    QApplication::sendEvent(badges, &delayed);
                    owntimeout::pump(30);
                    const auto tooltipImage = hoverPreview->grab().toImage();
                    check("shared tooltip corners are translucent and preview follows cursor like chat",
                          hoverPreview->isVisible() && hoverPreview->pos() == anchor + delta && !QToolTip::isVisible() &&
                          tooltipImage.pixelColor(0,0).alpha() < 20 &&
                          tooltipImage.pixelColor(tooltipImage.width()/2,1).alpha() > 100);
                    if (!report.isEmpty())
                        hoverPreview->grab().save(QFileInfo(report).absoluteDir().filePath("badge-hover.png"));
                }
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(badges, &leave);
                check("badge preview closes on leave", hoverPreview && !hoverPreview->isVisible());

                // Force expansion against the bottom of the monitor. The
                // collapse must not move the already visible first row back
                // down to the former opening position.
                if (auto *screen = popup.screen())
                    popup.move(popup.x(), screen->availableGeometry().bottom() - popup.height() + 1);
                struct ScrollVisibilityProbe : QObject
                {
                    int shows = 0;
                    bool eventFilter(QObject *, QEvent *event) override
                    {
                        if (event->type() == QEvent::Show)
                            ++shows;
                        return false;
                    }
                } scrollProbe;
                struct MotionProbe : QObject
                {
                    QWidget *strip = nullptr;
                    QWidget *view = nullptr;
                    QElapsedTimer elapsed;
                    qint64 previous = 0;
                    int resizes = 0;
                    bool running = false;
                    std::vector<double> intervals;
                    void start()
                    {
                        intervals.clear(); previous = 0; resizes = 0;
                        elapsed.start(); running = true;
                    }
                    void stop(const char *direction)
                    {
                        running = false;
                        std::sort(intervals.begin(), intervals.end());
                        if (intervals.empty()) return;
                        const auto p95 = intervals[std::min(intervals.size()-1, size_t(intervals.size()*.95))];
                        const auto slow = std::count_if(intervals.begin(), intervals.end(), [](double dt){return dt>32.;});
                        std::fprintf(stderr, "badge-motion %s: frames=%zu p95_ms=%.2f max_ms=%.2f gaps_over_32ms=%td view_resizes=%d\n",
                            direction, intervals.size(), p95, intervals.back(), slow, resizes);
                    }
                    bool eventFilter(QObject *watched, QEvent *event) override
                    {
                        if (!running) return false;
                        if (watched == view && event->type() == QEvent::Resize) ++resizes;
                        if (watched == strip && event->type() == QEvent::DynamicPropertyChange &&
                            static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "badgeRevealProgress")
                        {
                            const auto now = elapsed.nsecsElapsed();
                            if (previous != 0) intervals.push_back((now-previous)/1000000.);
                            previous = now;
                        }
                        return false;
                    }
                } motionProbe;
                motionProbe.strip = badges;
                motionProbe.view = popup.findChild<ChannelView *>();
                badges->installEventFilter(&motionProbe);
                if (motionProbe.view) motionProbe.view->installEventFilter(&motionProbe);
                auto *bar = initialScroll->verticalScrollBar();
                bar->installEventFilter(&scrollProbe);
                QTimer sampleBar;
                bool flashed = false;
                bool trackCollapse = false;
                QPoint collapsedAnchor;
                bool stableRow = true;
                int lastHeight = 0;
                bool monotone = true;
                int collapseResizes = 0;
                int largestDrop = 0;
                QObject::connect(&sampleBar, &QTimer::timeout, &scrollProbe,
                                 [&] {
                    flashed |= bar->isVisible();
                    if (trackCollapse)
                    {
                        stableRow &= badges->mapToGlobal(QPoint(0,0)) == collapsedAnchor &&
                                     badges->property("firstRowBadgeCount").toInt() == prefix;
                        collapseResizes += popup.height() != lastHeight;
                        largestDrop = std::max(largestDrop, lastHeight - popup.height());
                        monotone &= popup.height() <= lastHeight;
                        lastHeight = popup.height();
                    }
                });
                sampleBar.start(8);
                motionProbe.start();
                clickBadgeChip(badges, popup.scale());
                owntimeout::pump(75);
                const double progress = badges->property("badgeRevealProgress").toDouble();
                check("badge expansion animates rather than jumping",
                      badges->property("expanded").toBool() &&
                      progress > 0 && progress < 1 && badges->property("badgeFirstExtraProgress").toDouble() == 0);
                const int duration = std::clamp(290 + 38 * (largeBadgeCount - prefix), 460, 1850);
                owntimeout::pump(int(duration * 0.46));
                check("badge icons stagger independently and first row stays fixed",
                      badges->property("badgeFirstExtraProgress").toDouble() > 0 &&
                      badges->property("badgeLastExtraProgress").toDouble() == 0 &&
                      badges->property("firstRowBadgeCount").toInt() == prefix);
                owntimeout::pump(1900);
                motionProbe.stop("expand");
                check("right chat is laid out at the motion endpoint, not every expansion frame", motionProbe.resizes <= 3);
                check("remaining badges appear below unchanged first row",
                      badges->height() > baseHeight &&
                      badges->property("firstRowBadgeCount").toInt() == prefix &&
                      badges->property("badgeCount").toInt() == largeBadgeCount &&
                      badges->property("badgeRevealProgress").toDouble() == 1);
                auto *scroll = popup.findChild<QScrollArea *>("UsercardProfileScroll");
                check("expanded card fits its profile and hides unnecessary scrollbar",
                      popup.height() > basePopupHeight && scroll &&
                      scroll->verticalScrollBar()->maximum() == 0 &&
                      !scroll->verticalScrollBar()->isVisible());
                const int expandedHeight = popup.height();
                const int expandedBadgeHeight = badges->height();
                // During collapse the inner strip can already be shorter
                // while its parent still has the previous frame's height.
                // Sample actual pixels before the queued parent layout runs.
                auto *badgeHost = badges->parentWidget();
                const QSize hostSize = badgeHost->size();
                const QRect firstIcons(badges->pos(), QSize(prefix * baseHeight, baseHeight));
                const auto firstRowPixels = badgeHost->grab(firstIcons).toImage();
                bool pixelsAnchored = !firstRowPixels.isNull() && prefix > 0;
                for (int slack : {2, 8, 13, 4, 0})
                {
                    badgeHost->resize(hostSize.width(), hostSize.height() + slack);
                    pixelsAnchored &= badgeHost->grab(firstIcons).toImage() == firstRowPixels;
                }
                check("first-row pixels stay anchored between strip and parent resize events", pixelsAnchored);
                collapsedAnchor = badges->mapToGlobal(QPoint(0,0));
                lastHeight = popup.height();
                trackCollapse = true;
                motionProbe.start();
                clickBadgeChip(badges, popup.scale());
                check("collapse configuration emits no transient endpoint resize", popup.height() == expandedHeight &&
                      badges->height() == expandedBadgeHeight && badges->property("badgeRevealProgress").toDouble() == 1.0);
                owntimeout::pump(2050);
                motionProbe.stop("collapse");
                check("right chat is not relaid out on every collapse frame", motionProbe.resizes <= 3);
                badges->removeEventFilter(&motionProbe);
                if (motionProbe.view) motionProbe.view->removeEventFilter(&motionProbe);
                check("second click smoothly collapses badges",
                      !badges->property("expanded").toBool() &&
                      badges->height() == baseHeight && popup.height() == basePopupHeight);
                check("82-badge collapse keeps first-row position and height monotone throughout", stableRow && monotone &&
                      badges->mapToGlobal(QPoint(0,0)) == collapsedAnchor);
                check("card shrinks through intermediate heights without a final snap",
                      collapseResizes >= 3 && largestDrop < (expandedHeight - basePopupHeight) / 2);
                trackCollapse = false;
                sampleBar.stop();
                bar->removeEventFilter(&scrollProbe);
                check("profile scrollbar never appears on any animation frame when both endpoints fit",
                      !flashed && scrollProbe.shows == 0 && !bar->isVisible() &&
                      initialScroll->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);
#ifdef Q_OS_WIN
                if (QGuiApplication::platformName() == "windows")
                {
                    for (const bool opening : {true, false})
                    {
                        clickBadgeChip(badges, popup.scale());
                        owntimeout::pump(opening ? 200 : 1250);
                        const QPoint local(32, 15);
                        auto global = popup.mapToGlobal(local);
                        auto target = popup.pos();
                        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(&popup, &press);
                        const int initialHeight = popup.height();
                        const auto initialProgress = badges->property("badgeRevealProgress").toDouble();
                        bool follows = true;
                        for (int step = 0; step < 8; ++step)
                        {
                            const QPoint delta(2, -2);
                            global += delta;
                            target += delta;
                            QMouseEvent move(QEvent::MouseMove, popup.mapFromGlobal(global), global, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                            QApplication::sendEvent(&popup, &move);
                            owntimeout::pump(35);
                            follows &= popup.pos() == target;
                        }
                        const auto movingProgress = badges->property("badgeRevealProgress").toDouble();
                        const bool animates = opening ? movingProgress > initialProgress && popup.height() > initialHeight
                                                     : movingProgress < initialProgress && popup.height() < initialHeight;
                        owntimeout::pump(2000);
                        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(&popup, &release);
                        check(opening ? "expansion continues while card follows drag" :
                                        "collapse continues while card follows drag",
                              follows && animates && popup.pos() == target &&
                              popup.height() == (opening ? expandedHeight : basePopupHeight));
                    }
                }
#endif
                clickBadgeChip(badges, popup.scale());
                owntimeout::pump(60);
                showCase("b");
                owntimeout::pump(240);
                check("changing identity cancels animation and clears old badge rows",
                      !badges->property("expanded").toBool() &&
                      badges->property("badgeCount").toInt() == 8 &&
                      badges->property("badgeRevealProgress").toDouble() == 0);
                showCase(scenario);
                if (auto *grip = popup.findChild<QWidget *>("UsercardResizeGrip"))
                {
                    const auto original = popup.geometry();
                    const QPoint p(2, grip->height() / 2);
                    const auto g = grip->mapToGlobal(p);
                    QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(grip, &press);
                    const bool started = static_cast<UsercardResizeGrip *>(grip)->isResizing();
                    QMouseEvent move(QEvent::MouseMove, p, g + QPoint(-25, 0), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(grip, &move);
                    const auto moved = popup.geometry();
                    QMouseEvent release(QEvent::MouseButtonRelease, p, g + QPoint(-25, 0), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(grip, &release);
                    check(QStringLiteral("left resize edge expands width with opposite edge fixed (%1 x %2 => %3 x %4, right %5 => %6, started %7)").arg(original.width()).arg(original.height()).arg(moved.width()).arg(moved.height()).arg(original.right()).arg(moved.right()).arg(started), moved.width() == original.width() + 25 && moved.right() == original.right());
                    popup.setGeometry(original);
                    const QPoint bottom(grip->width() / 2, grip->height() - 3);
                    const auto global = grip->mapToGlobal(bottom);
                    QMouseEvent bottomPress(QEvent::MouseButtonPress, bottom, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(grip, &bottomPress);
                    QMouseEvent bottomMove(QEvent::MouseMove, bottom, global + QPoint(0, 34), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(grip, &bottomMove);
                    const auto taller = popup.geometry();
                    QMouseEvent bottomRelease(QEvent::MouseButtonRelease, bottom, global + QPoint(0, 34), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(grip, &bottomRelease);
                    check("bottom edge increases real card height with top edge fixed", taller.height() == original.height() + 34 && taller.top() == original.top());
                    popup.setGeometry(original);
                    check("edge resize mask leaves central controls accessible", grip->mask().contains(p) && !grip->mask().contains(grip->rect().center()));
                    const QRect rect(20, 40, 400, 400);
                    check("corner resize anchors opposite corner and clamps minimum", UsercardResizeGrip::resizedGeometry(rect, Qt::LeftEdge | Qt::TopEdge, QPoint(350, 350), QSize(200, 200), QSize(800, 800)) == QRect(220, 240, 200, 200));
                }
                else
                    check("resize grip exists", false);
                if (autoClose)
                {
                    const auto image = popup.grab().toImage();
                    check("frameless usercard uses translucent antialiased shadow instead of binary mask",
                          popup.testAttribute(Qt::WA_TranslucentBackground) && popup.mask().isEmpty() &&
                          image.pixelColor(0, 0).alpha() < 40 && image.pixelColor(image.width()/2, image.height()/2).alpha() > 240);
                    auto *columns = popup.findChild<QSplitter *>("UsercardColumns");
                    const auto content = QRect(columns->mapTo(&popup, QPoint()), columns->size());
                    const auto inset = usercardChrome::shadowInset(popup.scale());
                    const auto radius = usercardChrome::bodyRadius(popup.scale());
                    const auto safe = int(std::ceil(radius * (1 - 1 / std::sqrt(2.0)) + 1.5 * popup.scale()));
                    check("all opaque child corners stay inside the antialiased body geometry",
                         content.left() >= inset + safe && content.top() >= inset + safe &&
                         popup.width() - content.right() - 1 >= inset + safe && popup.height() - content.bottom() - 1 >= inset + safe);
                    QPixmap rectangular(120, 60);
                    rectangular.fill(Qt::white);
                    const auto circle = roundAvatarPixmap(rectangular, avatarPixelSize(64, 1.3)).toImage();
                    int fractional = 0;
                    for (int y = 0; y < circle.height(); ++y)
                        for (int x = 0; x < circle.width(); ++x)
                        {
                            const int a = circle.pixelColor(x,y).alpha();
                            fractional += a > 0 && a < 255;
                        }
                    check("round avatar mask has fractional antialiased edge coverage at 130% DPI",
                          circle.width() == 83 && circle.height() == 83 && circle.pixelColor(0,0).alpha() == 0 &&
                          circle.pixelColor(41,41).alpha() == 255 && fractional > 32);
                    int edgeAlpha = 0;
                    for (int x = 0; x < image.width(); ++x)
                        edgeAlpha = std::max({edgeAlpha, image.pixelColor(x, 0).alpha(), image.pixelColor(x, image.height() - 1).alpha()});
                    for (int y = 0; y < image.height(); ++y)
                        edgeAlpha = std::max({edgeAlpha, image.pixelColor(0, y).alpha(), image.pixelColor(image.width() - 1, y).alpha()});
                    int shadowPeak = 0;
                    for (int y = image.height() - 1; y >= image.height() - std::max(2, int(inset * popup.devicePixelRatioF())) + 1; --y)
                        shadowPeak = std::max(shadowPeak, image.pixelColor(image.width()/2,y).alpha());
                    check("real window shadow fades to near transparency at every outside edge", edgeAlpha <= 2 && shadowPeak > 10);
                    const auto oldShadow = getSettings()->displaySevenTVPaintShadows.getValue();
                    getSettings()->displaySevenTVPaintShadows = true;
                    LinearGradientPaint paint(QStringLiteral("fixture"), QStringLiteral("fixture-paint"), std::nullopt,
                        QGradientStops{{0, Qt::white}, {1, QColor("#b59be5")}}, false, 90,
                        {PaintDropShadow(0,0,12,QColor(255,180,40,230))});
                    auto font = popup.font();
                    font.setPixelSize(24);
                    font.setBold(true);
                    const QString name = QStringLiteral("TestNick");
                    const auto metrics = QFontMetricsF(font);
                    const QSizeF paintSize(metrics.horizontalAdvance(name) + 1, metrics.height());
                    constexpr qreal ratio = 1.3;
                    qreal padding = 0;
                    const QRectF clip(0,0,paintSize.width()+10,paintSize.height());
                    const auto rendered = paint.getPixmap(name, font, Qt::white, paintSize, 1.3, ratio, true, &padding, &clip).toImage();
                    int borderAlpha = 0, glyphPixels = 0;
                    for (int y=0; y<rendered.height(); ++y)
                        for (int x=0; x<rendered.width(); ++x)
                        {
                            const int alpha = rendered.pixelColor(x,y).alpha();
                            if (x==0 || y==0 || x==rendered.width()-1 || y==rendered.height()-1)
                                borderAlpha = std::max(borderAlpha, alpha);
                            glyphPixels += alpha >= 240;
                        }
                    getSettings()->displaySevenTVPaintShadows = oldShadow;
                    check("7TV glyph glow has padded transparent borders and opaque painted letters", padding > 0 && borderAlpha == 0 && glyphPixels > 20);
                }
            }
#endif
            for (const auto &size : {QSize(960, 670), QSize(600, 760)})
            {
                popup.resize(size);
                auto *columns = popup.findChild<QSplitter *>(
                    QStringLiteral("UsercardColumns"));
                check(QStringLiteral("responsive %1x%2")
                          .arg(size.width())
                          .arg(size.height()),
                      columns && popup.width() <= size.width() &&
                          columns->orientation() ==
                              (popup.getLayoutContainer()->width() <
                                       int(520 * popup.scale())
                                   ? Qt::Vertical
                                   : Qt::Horizontal));
            }
            popup.resize(width, height);
            if (autoClose)
            {
                auto *tabs = popup.findChild<QTabWidget *>(
                    QStringLiteral("UsercardContent"));
                auto *profile = popup.findChild<QScrollArea *>(
                    QStringLiteral("UsercardProfileScroll"));
                check(
                    QStringLiteral(
                        "auto-close native containers retain mouse input"),
                    tabs && profile && tabs->hasMouseTracking() &&
                        profile->hasMouseTracking() &&
                        popup.windowFlags().testFlag(Qt::FramelessWindowHint));
                check(QStringLiteral("public card has no private history tab"),
                      tabs && tabs->count() <= 2 &&
                          !popup.findChild<QWidget *>(QStringLiteral("UsercardArchiveToggle")) &&
                          !popup.findChild<QWidget *>(QStringLiteral("archiveUserCard")));
            }
            check(QStringLiteral("no external network manager thread"),
                  NetworkManager::workerThread == nullptr);
            // Animation checks pump the event loop; a fixed 650 ms report
            // timer would otherwise write only a partial set of checks.
            if (!report.isEmpty())
                checksPassed &= writeReport(report, app, current, popup, checks);
        });
    }
    if (parser.isSet(QStringLiteral("native-hit-test-checks")))
    {
        QTimer::singleShot(500, &popup, [&] {
            const auto hits = nativeHitTests(popup);
            checksPassed &= !hits.isEmpty();
            for (auto it = hits.begin(); it != hits.end(); ++it)
            {
                const bool ok = it.value().toInt() == 1;  // HTCLIENT
                checksPassed &= ok;
                checks.append(
                    QJsonObject{{QStringLiteral("name"),
                                 QStringLiteral("native HTCLIENT ") + it.key()},
                                {QStringLiteral("ok"), ok},
                                {QStringLiteral("hit"), it.value()}});
            }
        });
    }
    if (parser.isSet("menu-demo") && qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen")
    {
        QTimer::singleShot(450, &popup, [&] {
            if (auto *actions = popup.findChild<QToolButton *>("UsercardActions"))
                actions->click();
        });
    }
    const auto screenshot = parser.value(QStringLiteral("screenshot"));
    if (!screenshot.isEmpty())
    {
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
        if (parser.isSet(QStringLiteral("show-moderation")))
        {
            // after the synthetic profile data (which hides the strip) is set
            QTimer::singleShot(500, &popup, [&] {
                popup.previewModerationStrip();
            });
        }
#endif
        if (parser.isSet(QStringLiteral("banner-demo")))
        {
            QTimer::singleShot(300, &popup, [&] {
                if (auto *view = popup.findChild<ChannelView *>())
                {
                    auto *stack = new CornerBannerStack(view);
                    stack->setScale(popup.scale());
                    stack->show({.kind = CornerBannerData::Kind::GiftSubs,
                                 .displayName = QStringLiteral("doctorbrinen"),
                                 .amount = 5});
                    stack->show({.kind = CornerBannerData::Kind::Raid,
                                 .displayName = QStringLiteral("ZLOYn"),
                                 .amount = 1533});
                }
            });
        }
        QTimer::singleShot(parser.isSet("badge-demo") ? 2400 : 650, &popup, [&, screenshot] {
            QWidget *target = &popup;
            if (parser.isSet("menu-demo"))
            {
                auto *menu = popup.findChild<QMenu *>("UsercardActionsMenu");
                const auto entries = menu ? menu->actions() : QList<QAction *>{};
                const auto count = std::count_if(entries.begin(), entries.end(), [](const auto *item) { return !item->isSeparator(); });
                const bool compact = menu && menu->property("mist1xStyled").toBool() && count > 0 && count <= 6 &&
                    std::none_of(entries.begin(), entries.end(), [](const auto *item) { return item->text().contains("910000001") || item->text().contains("fixture_viewer"); });
                checksPassed &= compact;
                checks.append(QJsonObject{{"name", "compact Noctis usercard menu with no exposed technical duplicates"}, {"ok", compact}});
                if (menu)
                    target = menu;
            }
            const bool saved = target->grab().save(screenshot, "PNG");
            checksPassed &= saved;
            checks.append(
                QJsonObject{{QStringLiteral("name"),
                             QStringLiteral("own fixture widget rendered PNG")},
                            {QStringLiteral("ok"), saved}});
        });
    }
#ifndef LEAFYRINO_BASELINE_USERCARD_PREVIEW
    if (historyChecks)
    {
        QTimer::singleShot(400, &popup, [&] {
            preview::runHistoryChecks(
                popup, fixture,
                [&](const QString &name, bool ok) {
                    checksPassed &= ok;
                    checks.append(QJsonObject{{QStringLiteral("name"), name},
                                              {QStringLiteral("ok"), ok}});
                });
            if (!report.isEmpty())
            {
                checksPassed &= writeReport(report, app, current, popup, checks);
            }
            qapp.exit(checksPassed ? 0 : 30);
        });
        QTimer::singleShot(20000, &qapp, [&] {
            checksPassed = false;
            checks.append(
                QJsonObject{{QStringLiteral("name"),
                             QStringLiteral("history checks watchdog")},
                            {QStringLiteral("ok"), false}});
            if (!report.isEmpty())
            {
                writeReport(report, app, current, popup, checks);
            }
            qapp.exit(31);
        });
    }
#endif
    if (!report.isEmpty() && !historyChecks &&
        !parser.isSet(QStringLiteral("scripted-checks")))
    {
        QTimer::singleShot(650, &popup, [&] {
            writeReport(report, app, current, popup, checks);
        });
    }
    if (parser.isSet(QStringLiteral("quit-after")) && !historyChecks)
    {
        QTimer::singleShot(
            std::clamp(parser.value(QStringLiteral("quit-after")).toInt(), 1000,
                       300000),
            &qapp, [&] {
                qapp.exit(checksPassed ? 0 : 30);
            });
    }
    std::ignore = popup.closing.connect([&controls, autoClose] {
        if (!autoClose)
        {
            controls.close();
        }
    });
    QElapsedTimer shutdownTimer;
    QObject::connect(&qapp, &QApplication::aboutToQuit, &qapp,
                     [&shutdownTimer] {
                         shutdownTimer.start();
                     });
    const int result = qapp.exec();
    std::fputs("offline preview: event loop exited\n", stderr);
    popupOwner.reset();
    std::fputs("offline preview: popup released\n", stderr);
    controlsOwner.reset();
    std::fputs("offline preview: controls released\n", stderr);
    splitOwner.reset();
    std::fputs("offline preview: split released\n", stderr);
    // The real app uses the same final deferred-delete drain. Keep the mock
    // controllers alive until all of the preview widgets have been released.
    for (int i = 0; i < 16; ++i)
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    appOwner.reset();
    std::fprintf(
        stderr,
        "offline preview: controllers released; shutdown cleanup %lld ms\n",
        static_cast<long long>(shutdownTimer.elapsed()));
    return result;
}
