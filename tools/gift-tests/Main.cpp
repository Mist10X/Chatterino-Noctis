// SPDX-License-Identifier: MIT
// Actual IRC parser and chat rendering; synthetic data, no provider startup.
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "mocks/UserData.hpp"
#include "singletons/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "singletons/Resources.hpp"
#include "singletons/WindowManager.hpp"
#include "util/VectorMessageSink.hpp"
#include "widgets/helper/ChannelView.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace chatterino;
class OfflineApp : public mock::BaseApplication
{
public:
    OfflineApp()
        : mock::BaseApplication(QStringLiteral("{}"))
        , windows(this->args, this->paths_, this->settings, this->theme,
                  this->fonts)
    {
        this->settings.disableSave();
    }
    IUserDataController *getUserData() override
    {
        return &this->users;
    }
    SeventvPaints *getSeventvPaints() override { return &this->paints; }
    EmoteController *getEmotes() override { return &this->emotes; }
    SeventvPaints paints;
    mock::EmoteController emotes;
    WindowManager *getWindows() override
    {
        return &this->windows;
    }
    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }
    HighlightController *getHighlights() override { return &this->highlights; }
    ITwitchIrcServer *getTwitch() override { return &this->twitch; }
    PubSub *getTwitchPubSub() override { return &this->pubSub; }
    ILogging *getChatLogger() override { return &this->logging; }
    struct NoLog : ILogging {
        void addMessage(const QString &, MessagePtr, const QString &,
                        const QString &) override {}
        void closeChannel(const QString &, const QString &) override {}
    } logging;
    PubSub pubSub{QStringLiteral("wss://127.0.0.1:9")};
    mock::MockTwitchIrcServer twitch;
    WindowManager windows;
    HotkeyController hotkeys;
    AccountController accounts;
    HighlightController highlights{this->settings, &this->accounts};
    mock::UserDataController users;
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication qt(argc, argv);
    if (argc != 2 || !QDir::isAbsolutePath(QString::fromLocal8Bit(argv[1])))
    {
        return 20;
    }
    auto progress = [&](const QString &stage) {
        QFile log(QString::fromLocal8Bit(argv[1]) + ".progress");
        if (log.open(QIODevice::WriteOnly | QIODevice::Append))
            log.write(stage.toUtf8() + '\n');
    };
    progress("resources");
    initResources();
    progress("app");
    OfflineApp app;
    progress("channels");
    auto channel = std::make_shared<TwitchChannel>("offline-gifts");
    auto other = std::make_shared<TwitchChannel>("other-gifts");
    QJsonArray checks;
    int failures = 0;
    auto check = [&](bool ok, const QString &name) {
        progress(name + (ok ? " PASS" : " FAIL"));
        checks.append(QJsonObject{{"name", name}, {"pass", ok}});
        failures += !ok;
    };
    {
        // Settings connect() invokes its callback immediately, before start().
        StreamerMode mode;
        check(!mode.isEnabled(), "streamer-mode construction before worker start");
    }
    auto parse = [&](const QByteArray &tags, TwitchChannel *c = nullptr,
                     bool giftText = true) {
        auto *irc = Communi::IrcMessage::fromData(
            QByteArray("@login=alice;display-name=Alice;user-id=42;room-id=11;") +
                (giftText ? "system-msg=Alice\\sgifted\\sa\\ssub\\sto\\sBob!;" : "") +
                tags + " :tmi.twitch.tv USERNOTICE #offline-gifts",
            nullptr);
        VectorMessageSink sink;
        IrcMessageHandler::parseUserNoticeMessageInto(irc, sink,
                                                      c ? c : channel.get());
        delete irc;
        return sink.messages();
    };
    const QByteArray raidTags = "msg-id=raid;msg-param-login=raider;"
                                "msg-param-displayName=RaiDer";
    check(!app.settings.hideRaidMessages && app.settings.showIncomingRaidBanner,
          "chat line and corner banner are both enabled by default");
    initializeUiLanguage("ru");
    const std::unique_ptr<Communi::IrcMessage> resub(Communi::IrcMessage::fromData(
        "@display-name=Alice;msg-id=resub;msg-param-sub-plan=1000;"
        "msg-param-cumulative-months=28;msg-param-should-share-streak=1;"
        "msg-param-streak-months=5 :tmi.twitch.tv USERNOTICE #offline-gifts", nullptr));
    check(resub && MessageBuilder::formatSubscriptionNotice(resub->tags(), "alice", true, {}) ==
              QStringLiteral("Alice продлил(а) подписку T1 · Всего 28 мес. · Серия 5 мес."),
          "resubscription notice retains plan, cumulative months and shared streak in Russian");
    const auto raid = parse(raidTags + ";msg-param-viewerCount=21", nullptr, false);
    check(raid.size() == 1 && raid.front()->flags.has(MessageFlag::Raid) &&
              !raid.front()->flags.has(MessageFlag::UncategorizedNotification) &&
              raid.front()->messageText ==
                  QStringLiteral("Рейд с канала RaiDer · Зрителей: 21") &&
              raid.front()->searchText == raid.front()->messageText,
          "incoming raid with structured tags only, localized RU and styled");
    initializeUiLanguage("en");
    const auto historicalRaid = parse(raidTags + ";historical=1;msg-param-viewerCount=1");
    check(historicalRaid.size() == 1 &&
              historicalRaid.front()->clone()->flags.has(MessageFlag::Raid) &&
              historicalRaid.front()->messageText == "Raid from RaiDer · Viewers: 1",
          "historical IRC raid uses EN UI, ignores upstream prose");
    const auto unknownCount = parse(raidTags + ";msg-param-viewerCount=bad");
    check(unknownCount.size() == 1 && unknownCount.front()->messageText == "Raid from RaiDer",
          "unknown raid size leaves the count out");

    app.twitch.mockChannels[channel->getName()] = channel;
    int raidBanners = 0;
    auto bannerConnection = channel->incomingRaid.connect(
        [&](const QString &login, const QString &display, int viewers) {
            if (login == "raider" && display == "RaiDer" && viewers == 21)
            {
                ++raidBanners;
            }
        });
    auto liveRaid = [&](const QByteArray &id) {
        std::unique_ptr<Communi::IrcMessage> irc(Communi::IrcMessage::fromData(
            "@" + raidTags + ";id=" + id +
                ";msg-param-viewerCount=21 :tmi.twitch.tv USERNOTICE #offline-gifts",
            nullptr));
        IrcMessageHandler::instance().handleUserNoticeMessage(irc.get(), app.twitch);
    };
    const auto previousMessages = channel->countMessages();
    liveRaid("first-raid");
    check(channel->countMessages() == previousMessages + 1 &&
              channel->findMessageByID(u"first-raid") && raidBanners == 1,
          "live IRC emits one raid line plus one separate corner notification");
    check(parse(raidTags + ";id=first-raid;historical=1").empty() && raidBanners == 1,
          "history overlapping live IRC does not repeat the line or banner");

    app.settings.showIncomingRaidBanner = false;
    liveRaid("second-raid");
    check(channel->countMessages() == previousMessages + 2 &&
              channel->findMessageByID(u"second-raid"),
          "a distinct raid from the same channel stays visible with banner off");
    app.settings.showIncomingRaidBanner = true;
    app.settings.hideRaidMessages = true;
    check(parse(raidTags).empty(), "hidden incoming raid stays out of chat");
    liveRaid("hidden-raid");
    check(channel->countMessages() == previousMessages + 2 && raidBanners == 3,
          "explicitly hiding the chat line preserves the live banner");
    app.settings.hideCommunityGiftMessages = true;
    app.settings.showMassGiftBanner = true;
    app.settings.massGiftBannerMinimum = 5;
    auto small =
        parse("msg-id=submysterygift;id=small;msg-param-mass-gift-count=4");
    check(small.size() == 1 && small[0]->giftNotice,
          "below minimum: styled summary");
    const QByteArray recipient = "msg-id=subgift;msg-param-recipient-id=43;"
                                 "msg-param-recipient-user-name=bob;msg-param-"
                                 "recipient-display-name=Bob;";
    auto child = parse(recipient + "msg-param-community-gift-id=small");
    check(child.size() == 1 && child[0]->giftNotice,
          "small gift: recipient visible");
    check(parse("msg-id=submysterygift;id=large;msg-param-mass-gift-count=5")
              .empty(),
          "at minimum: summary replaced by banner");
    check(parse(recipient + "msg-param-community-gift-id=large").empty(),
          "large gift: no recipient flood");
    check(parse(recipient + "msg-param-community-gift-id=large", other.get())
                  .size() == 1,
          "parent state is channel-local");
    check(parse(recipient + "msg-param-community-gift-id=missing").size() == 1,
          "unknown parent: never lose notice");
    check(parse(recipient).size() == 1, "individual gift remains visible");
    check(parse("msg-id=standardpayforward").size() == 1,
          "direct pay-forward notice remains visible");
    check(parse("msg-id=communitypayforward").size() == 1,
          "community pay-forward notice remains visible");
    check(parse("msg-id=giftpaidupgrade;msg-param-sender-login=bob").size() == 1,
          "gift subscription continuation remains visible");
    check(parse("msg-id=anongiftpaidupgrade").size() == 1,
          "anonymous gift subscription continuation remains visible");
    auto anon =
        parse("msg-id=anonsubmysterygift;id=anon;msg-param-mass-gift-count=2");
    check(anon.size() == 1 && anon[0]->giftNotice,
          "small anonymous gift visible");
    app.settings.showMassGiftBanner = false;
    check(parse("msg-id=submysterygift;id=off;msg-param-mass-gift-count=50")
                  .size() == 1,
          "disabled banners: large summary visible");
    check(parse(recipient + "msg-param-community-gift-id=large").size() == 1,
          "disabled banners: recipients visible");
    app.settings.showMassGiftBanner = true;
    app.settings.hideCommunityGiftMessages = false;
    check(
        parse("msg-id=submysterygift;id=unhidden;msg-param-mass-gift-count=50")
                .size() == 1,
        "hide switch off: preserve ordinary notices");
    check(small[0]->clone()->giftNotice, "cloning preserves event style");
    for (int i = 0; i < 150; ++i)
    {
        parse("msg-id=submysterygift;msg-param-mass-gift-count=10;id=bounded-" +
              QByteArray::number(i));
    }
    check(channel->communityGiftBanners.size() <= 128, "parent cache bounded");

    std::vector<MessagePtr> noticePreviewRows;
    for (const auto &language : {QStringLiteral("ru"), QStringLiteral("en")})
    {
        initializeUiLanguage(language);
        const auto russian = language == "ru";
        const QByteArray streakTags = "msg-id=viewermilestone;msg-param-category=watch-streak;";
        const auto streak = parse(streakTags + "msg-param-value=30;system-msg=Alice\\swatched\\s30\\sconsecutive\\sstreams\\sand\\ssparked\\sa\\swatch\\sstreak!", nullptr, false);
        const auto streakText = russian
            ? QStringLiteral("Alice · Серия просмотров: 30 стримов подряд")
            : QStringLiteral("Alice watched 30 consecutive streams and sparked a watch streak!");
        check(streak.size() == 1 &&
                  streak.front()->channelNotice == Message::ChannelNotice::WatchStreak &&
                  streak.front()->flags.has(MessageFlag::WatchStreak) &&
                  streak.front()->flags.has(MessageFlag::System) &&
                  streak.front()->messageText == streakText &&
                  streak.front()->searchText == streakText,
              language + " actual IRC streak notice retains casing, count and style");

        const std::unique_ptr<Communi::IrcMessage> heldIrc(Communi::IrcMessage::fromData(
            "@msg-id=msg_automod_held :tmi.twitch.tv NOTICE #offline-gifts :Your message has been held for review by Automod.",
            nullptr));
        VectorMessageSink heldRows;
        IrcMessageHandler::parseMessageInto(heldIrc.get(), heldRows, channel.get());
        const auto apiHeld = makeChatSendDropMessage("Your message has been held for review by Automod.", "automod_held");
        const auto expectedHeld = russian
            ? QStringLiteral("Сообщение задержано AutoMod · ожидает проверки модераторами.")
            : QStringLiteral("Your message has been held for review by AutoMod.");
        check(heldRows.messages().size() == 1 && apiHeld &&
                  heldRows.messages().front()->channelNotice == Message::ChannelNotice::AutoModHeld &&
                  apiHeld->channelNotice == Message::ChannelNotice::AutoModHeld &&
                  heldRows.messages().front()->messageText == expectedHeld &&
                  apiHeld->messageText == expectedHeld && apiHeld->searchText == expectedHeld &&
                  apiHeld->flags.has(MessageFlag::DoNotTriggerNotification),
              language + " held sender notice localized and styled in both IRC and API paths");
        const auto rejected = makeChatSendDropMessage("Your message was not sent.", "automod_rejected");
        check(rejected && rejected->channelNotice == Message::ChannelNotice::None &&
                  rejected->messageText == uiText("Your message was not sent."),
              language + " rejected message is not incorrectly marked held");
        if (russian && !streak.empty() && apiHeld)
        {
            noticePreviewRows.push_back(streak.front());
            noticePreviewRows.push_back(apiHeld);
        }
        auto pins = std::make_shared<TwitchChannel>("offline-pins");
        const QString quote = QStringLiteral("AbC %1 <3");
        const QJsonObject pinData{
            {"id", "pin-1"},
            {"pinnedBy", QJsonObject{{"displayName", "MoD"}}},
            {"message", QJsonObject{{"content", QJsonObject{{"text", quote}}}}},
        };
        const QJsonObject pinEvent{{"type", "pin-message"}, {"data", pinData}};
        pins->handlePinnedChatUpdate(pinEvent);
        pins->handlePinnedChatUpdate(pinEvent);
        const auto pinned = pins->getLastMessage();
        check(pinned && pinned->channelNotice == Message::ChannelNotice::Pinned &&
                  pins->getMessageSnapshot().size() == 1 &&
                  pinned->messageText == uiText("%1 pinned: \"%2\"").arg("MoD", quote) &&
                  pinned->messageText.contains(quote) &&
                  pinned->messageText.contains("MoD") &&
                  pinned->messageText.contains("pinned:") != russian &&
                  pinned->flags.has(MessageFlag::DoNotTriggerNotification),
              language + " pin from PubSub retains quote, moderator and deduplication");
        pins->setPinnedMessage(TwitchChannel::PinnedMessage{.pinId = "pin-1"});
        pins->handlePinnedChatUpdate(QJsonObject{
            {"type", "unpin-message"}, {"data", QJsonObject{{"id", "old"}}}});
        check(pins->getLastMessage() == pinned && pins->accessPinnedMessage()->has_value(),
              language + " stale removal ignored");
        const QJsonObject unpinEvent{
            {"type", "unpin-message"}, {"data", QJsonObject{{"id", "pin-1"}}}};
        pins->handlePinnedChatUpdate(unpinEvent);
        pins->handlePinnedChatUpdate(unpinEvent);
        const auto unpinned = pins->getLastMessage();
        check(unpinned && unpinned->channelNotice == Message::ChannelNotice::Unpinned &&
                  pins->getMessageSnapshot().size() == 2 &&
                  unpinned->messageText == uiText("Pinned message was removed.") &&
                  unpinned->messageText.contains("was removed") != russian &&
                  !pins->accessPinnedMessage()->has_value(),
              language + " unpin without moderator is localized and appears once");

        const HelixMinimalUser user{.id = "11", .login = "live_demo",
                                    .displayName = "Live_Demo"};
        const auto name = user.formatted(app.settings.usernameDisplayMode.getEnum());
        app.settings.showTitleInLiveMessage = false;
        const auto live = MessageBuilder::makeLiveMessage(
            user, quote, {MessageFlag::System, MessageFlag::DoNotTriggerNotification});
        check(live->channelNotice == Message::ChannelNotice::StreamLive &&
                  live->messageText == name + ' ' + uiText("is live!") &&
                  live->searchText == live->messageText &&
                  live->messageText.contains("is live!") != russian &&
                  live->id == user.id &&
                  live->flags.has(MessageFlag::DoNotTriggerNotification) &&
                  live->elements.at(1)->getLink().value == user.login,
              language + " live row preserves channel link, id and flags");
        app.settings.showTitleInLiveMessage = true;
        const auto titled = MessageBuilder::makeLiveMessage(user, quote);
        check(titled->messageText == name + ' ' + uiText("is live: %1").arg(quote) &&
                  titled->messageText.endsWith(quote) &&
                  titled->messageText.contains("is live:") != russian &&
                  !titled->flags.has(MessageFlag::DoNotTriggerNotification),
              language + " /live tab keeps title verbatim and independent flags");
        const auto offline = MessageBuilder::makeOfflineSystemMessage(user);
        check(offline->channelNotice == Message::ChannelNotice::StreamOffline &&
                  offline->messageText == name + ' ' + uiText("is now offline.") &&
                  offline->searchText == offline->messageText &&
                  offline->messageText.contains("is now offline.") != russian &&
                  offline->elements.at(1)->getLink().value == user.login,
              language + " offline row preserves link and searchable status");
        if (russian && pinned && unpinned)
        {
            noticePreviewRows.insert(noticePreviewRows.end(),
                                     {pinned, unpinned, live, offline});
        }
    }

    auto author = std::make_shared<Message>();
    author->userID = "101";
    author->loginName = "fixture_viewer";
    auto nick = std::make_unique<TextElement>("fixture_viewer:",
                                              MessageElementFlag::Username);
    nick->setLink({Link::UserInfo, author->loginName});
    author->elements.push_back(std::move(nick));
    author->elements.push_back(std::make_unique<TextElement>(
        QString("test message ").repeated(24), MessageElementFlag::Text));
    MessageLayout authorLayout(author);
    const MessageColors colors{};
    MessageLayoutContext authorContext{.messageColors = colors,
                                             .flags = {MessageElementFlag::Username,
                                                       MessageElementFlag::Text},
                                             .width = 400,
                                             .message = *author};
    const auto findLink = [&](Link::Type type) {
        for (int y = 0; y < authorLayout.getHeight(); ++y)
        {
            for (int x = 0; x < authorContext.width; ++x)
            {
                if (const auto *element = authorLayout.getElementAt({double(x), double(y)});
                    element && element->getLink().type == type)
                    return element;
            }
        }
        return static_cast<const MessageLayoutElement *>(nullptr);
    };
    authorLayout.layout(authorContext, false);
    const auto *username = findLink(Link::UserInfo);
    check(username && username->getLink().value == author->loginName,
          "wrapped chat message keeps the username clickable");
    authorContext.width = 245;
    authorContext.scale = 1.25;
    authorLayout.layout(authorContext, false);
    username = findLink(Link::UserInfo);
    check(username && username->getLink().value == author->loginName,
          "username remains clickable after resize and scaling");

    // Same ChannelView as real chat, with the actual parsed notices.
    progress("preview channel");
    auto preview =
        std::make_shared<Channel>("gift-preview", Channel::Type::Misc);
    progress("adding preview messages");
    preview->addMessage(small[0], MessageContext::Original);
    preview->addMessage(child[0], MessageContext::Original);
    if (!raid.empty())
    {
        preview->addMessage(raid.front(), MessageContext::Original);
    }
    progress("constructing view");
    ChannelView view(nullptr, ChannelView::Context::UserCard);
    view.resize(700, 210);
    progress("setting preview channel");
    view.setChannel(preview);
    progress("showing view");
    view.show();
    qt.processEvents();
    progress("grabbing view");
    const auto rendered = view.grab();
    const auto background = rendered.toImage().pixelColor(650, 10);
    check(qAbs(background.red() - background.blue()) < 20,
          "gift notice has no full-width subscription highlight");
    check(rendered.save(QString::fromLocal8Bit(argv[1]) + ".png"),
          "real chat rendering saved");

    initializeUiLanguage("ru");
    app.settings.showPredictionSystemMessages = true;
    auto events = std::make_shared<TwitchChannel>("offline-channel-events");
    for (const auto &outcome : {"WIN", "LOSE", "REFUND"})
    {
        const QJsonObject bet{{"event_id", QString::fromLatin1(outcome)},
                              {"points", 5000}, {"outcome_id", "A"},
                              {"result", QJsonObject{{"type", outcome},
                                                     {"points_won", 7000}}}};
        events->handlePredictionUserEvent(
            QJsonObject{{"type", "prediction-result"},
                        {"data", QJsonObject{{"prediction", bet}}}});
    }
    for (const auto &message : events->getMessageSnapshot())
    {
        noticePreviewRows.push_back(message);
    }
    if (!raid.empty())
    {
        noticePreviewRows.push_back(raid.front());
    }
    MessageBuilder subscription;
    subscription->flags.set(MessageFlag::System);
    subscription->flags.set(MessageFlag::Subscription);
    subscription->messageText = MessageBuilder::formatSubscriptionNotice(resub->tags(), "alice", true, {});
    subscription.emplace<TextElement>(subscription->messageText, MessageElementFlag::Text, MessageColor::System);
    noticePreviewRows.push_back(subscription.release());
    auto noticePreview = std::make_shared<Channel>("notice-preview", Channel::Type::Misc);
    check(noticePreviewRows.size() == 11, "all eleven notice variants produced for rendering");
    for (const auto &message : noticePreviewRows)
    {
        if (!message)
        {
            check(false, "missing notice");
            continue;
        }
        noticePreview->addMessage(message, MessageContext::Original);
        MessageLayout layout(message);
        MessageLayoutContext ctx{.messageColors = colors,
                                 .flags = {MessageElementFlag::Text, MessageElementFlag::Username},
                                 .width = 500, .message = *message};
        layout.layout(ctx, false);
        bool foundIcon = false;
        for (int y = 0; !foundIcon && y < layout.getHeight(); ++y)
        {
            for (int x = 0; !foundIcon && x < 35; ++x)
            {
                foundIcon = dynamic_cast<const NoticeIconLayoutElement *>(
                                layout.getElementAt({double(x), double(y)})) != nullptr;
            }
        }
        check(foundIcon, "notice has a leading drawn icon with timestamps hidden: " +
                             message->messageText);
    }
    view.resize(600, 360);
    view.setChannel(noticePreview);
    qt.processEvents();
    check(view.grab().save(QString::fromLocal8Bit(argv[1]) + ".notices.png"),
          "saved actual chat rows for all channel events");
    auto subscriptionPreview = std::make_shared<Channel>("subscription-preview", Channel::Type::Misc);
    subscriptionPreview->addMessage(noticePreviewRows.back(), MessageContext::Original);
    view.resize(700, 100);
    view.setChannel(subscriptionPreview);
    qt.processEvents();
    const auto subscriptionImage = view.grab().toImage();
    const auto subscriptionBackground = subscriptionImage.pixelColor(subscriptionImage.width() - 20, 10);
    check(qAbs(subscriptionBackground.red() - subscriptionBackground.blue()) < 20,
          "subscription event has a soft notice frame without a full-width purple highlight");
    QFile report(QString::fromLocal8Bit(argv[1]));
    if (!report.open(QIODevice::WriteOnly))
        return 21;
    report.write(
        QJsonDocument(QJsonObject{{"checks", checks},
                                  {"failed", failures},
                                  {"passed", checks.size() - failures}})
            .toJson());
    markAppAboutToQuit();
    return failures ? 1 : 0;
}
