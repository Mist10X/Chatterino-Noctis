// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/AutoModeration.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include <QComboBox>
#include <QTableWidget>
#include "providers/twitch/AutoModerationRules.hpp"
#include <QCheckBox>
#include <QPushButton>
#include <QTabWidget>

#include "providers/twitch/ModeratorAutoReplies.hpp"
#include "providers/twitch/ModeratorModule.hpp"
#include <QThread>
#include "providers/twitch/ModeratorReplyFeatures.hpp"

#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "controllers/sound/NullBackend.hpp"
#include "lib/Snapshot.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/ChatterinoBadges.hpp"
#include "mocks/DisabledStreamerMode.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Helix.hpp"
#include "mocks/LinkResolver.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/folhinha/FolhinhaBadges.hpp"
#include "providers/homies/HomiesBadges.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "TwitchChannelTestAccess.hpp"
#include "util/IrcHelpers.hpp"
#include "util/VectorMessageSink.hpp"

#include <IrcConnection>
#include <QDebug>
#include <QApplication>
#include <QDialog>
#include <QTabBar>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringBuilder>

#include <unordered_map>
#include <vector>

using namespace chatterino;
using namespace literals;

namespace chatterino {
class ModeratorAutoRepliesTestAccess
{
public:
    static void configure(ModeratorAutoReplies &module, const moderator::Policy &policy,
                          const QString &roleAccount, const QStringList &channels,
                          const QString &ruleChannel, unsigned senderMask = 1,
                          const QString &reply = "fixture answer", bool allWords = false,
                          int cooldownSeconds = 5)
    {
        module.configureFixture(policy, roleAccount, channels, ruleChannel, senderMask, reply, allWords, cooldownSeconds);
    }
};
}

namespace {

/// Controls whether snapshots will be updated (true) or verified (false)
///
/// In CI, all snapshots must be verified, thus the integrity tests checks for
/// this constant.
///
/// When adding a test, start with `{ "input": "..." }` and set this to `true`
/// to generate an initial snapshot. Make sure to verify the output!
constexpr bool UPDATE_SNAPSHOTS = false;

const QString IRC_CATEGORY = u"IrcMessageHandler"_s;

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication(const QString &settingsData)
        : mock::BaseApplication(settingsData, /*runMigrations*/ true)
        , highlights(this->settings, &this->accounts)
        , windows(this->args, this->paths_, this->settings, this->theme,
                  this->fonts)
    {
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    IUserDataController *getUserData() override
    {
        return &this->userData;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    IChatterinoBadges *getChatterinoBadges() override
    {
        return &this->chatterinoBadges;
    }

    FfzBadges *getFfzBadges() override
    {
        return &this->ffzBadges;
    }

    BttvBadges *getBttvBadges() override
    {
        return &this->bttvBadges;
    }

    SeventvBadges *getSeventvBadges() override
    {
        return &this->seventvBadges;
    }

    FolhinhaBadges *getFolhinhaBadges() override
    {
        return &this->folhinhaBadges;
    }

    HomiesBadges *getHomiesBadges() override
    {
        return &this->homiesBadges;
    }

    HighlightController *getHighlights() override
    {
        return &this->highlights;
    }

    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &this->personalEmotes;
    }

    BttvEmotes *getBttvEmotes() override
    {
        return &this->bttvEmotes;
    }

    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffzEmotes;
    }

    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventvEmotes;
    }

    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    TwitchBadges *getTwitchBadges() override
    {
        return &this->twitchBadges;
    }

    ILinkResolver *getLinkResolver() override
    {
        return &this->linkResolver;
    }

    ISoundController *getSound() override
    {
        return &this->sound;
    }

    WindowManager *getWindows() override
    {
        return &this->windows;
    }

    mock::EmptyLogging logging;
    AccountController accounts;
    mock::EmoteController emotes;
    mock::Helix helix;
    mock::UserDataController userData;
    mock::MockTwitchIrcServer twitch;
    mock::ChatterinoBadges chatterinoBadges;
    FfzBadges ffzBadges;
    BttvBadges bttvBadges;
    SeventvBadges seventvBadges;
    FolhinhaBadges folhinhaBadges;
    HomiesBadges homiesBadges;
    HighlightController highlights;
    SeventvPersonalEmotes personalEmotes;
    BttvEmotes bttvEmotes;
    FfzEmotes ffzEmotes;
    SeventvEmotes seventvEmotes;
    TwitchBadges twitchBadges;
    mock::EmptyLinkResolver linkResolver;
    NullBackend sound;
    WindowManager windows;
};

std::pair<const EmoteName, EmotePtr> makeEmote(Emote &&emote)
{
    auto ptr = std::make_shared<Emote>(std::move(emote));
    ptr->homePage = {u"https://chatterino.com/" % ptr->name.string};
    ptr->tooltip = {ptr->name.string % u" Tooltip"_s};
    ptr->author = {u"Chatterino"_s};
    ptr->images = {
        Url{u"https://chatterino.com/" % ptr->name.string % u".png"}};
    return {ptr->name, ptr};
}

using EmoteMapPtr = std::shared_ptr<const EmoteMap>;

EmoteMapPtr makeEmotes(auto &&...emotes)
{
    auto map = std::make_shared<EmoteMap>();
    ((map->emplace(makeEmote(std::forward<decltype(emotes)>(emotes)))), ...);
    return map;
}

QT_WARNING_PUSH
QT_WARNING_DISABLE_CLANG("-Wmissing-field-initializers")

struct MockEmotes {
    EmoteMapPtr seventv;
    EmoteMapPtr bttv;
    EmoteMapPtr ffz;
    EmoteMapPtr twitchAccount;

    static MockEmotes channel()
    {
        return {
            .seventv = makeEmotes(
                Emote{
                    .name = {u"7TVEmote"_s},
                    .id = {u"1"_s},
                },
                Emote{
                    .name = {u"7TVEmote0w"_s},
                    .zeroWidth = true,
                    .id = {u"2"_s},
                    .baseName = EmoteName{u"ZeroWidth"_s},
                },
                Emote{
                    .name = {u"PogChamp"_s},
                    .id = {u"3"_s},
                }),
            .bttv = makeEmotes(
                Emote{
                    .name = {u"BTTVEmote"_s},
                },
                Emote{
                    .name = {u"Kappa"_s},
                }),
            .ffz = makeEmotes(
                Emote{
                    .name = {u"FFZEmote"_s},
                },
                Emote{
                    .name = {u"Keepo"_s},
                }),
        };
    }

    static MockEmotes twitchdev()
    {
        return {
            .seventv = makeEmotes(Emote{
                .name = {u"7TVTwitchDev"_s},
                .id = {u"t5"_s},
            }),
            .bttv = makeEmotes(Emote{
                .name = {u"BTTVTwitchDev"_s},
            }),
            .ffz = makeEmotes(Emote{
                .name = {u"FFZTwitchDev"_s},
            }),
        };
    }

    static MockEmotes global()
    {
        return {
            .seventv = makeEmotes(Emote{
                .name = {u"7TVGlobal"_s},
                .id = {u"G1"_s},
            }),
            .bttv = makeEmotes(Emote{
                .name = {u"BTTVGlobal"_s},
            }),
            .ffz = makeEmotes(Emote{
                .name = {u"FFZGlobal"_s},
            }),
            .twitchAccount = makeEmotes(Emote{
                .name = {u"MyCoolTwitchEmote"_s},
                .id = {u"5678"_s},
            }),
        };
    }
};

const QByteArray CHEERMOTE_JSON{R"({
    "prefix": "Cheer",
    "tiers": [
    {
        "min_bits": 1,
        "id": "1",
        "color": "#979797",
        "images": {
            "dark": {
                "animated": {
                    "1": "https://chatterino.com/bits/1.gif",
                    "2": "https://chatterino.com/bits/2.gif",
                    "4": "https://chatterino.com/bits/4.gif"
                },
                "static": {
                    "1": "https://chatterino.com/bits/1.png",
                    "2": "https://chatterino.com/bits/2.png",
                    "4": "https://chatterino.com/bits/4.png"
                }
            }
        },
        "can_cheer": true,
        "show_in_bits_card": true
    },
    {
        "min_bits": 100,
        "id": "100",
        "color": "#9c3ee8",
        "images": {
            "dark": {
                "animated": {
                    "1": "https://chatterino.com/bits/1.gif",
                    "2": "https://chatterino.com/bits/2.gif",
                    "4": "https://chatterino.com/bits/4.gif"
                },
                "static": {
                    "1": "https://chatterino.com/bits/1.png",
                    "2": "https://chatterino.com/bits/2.png",
                    "4": "https://chatterino.com/bits/4.png"
                }
            }
        },
        "can_cheer": true,
        "show_in_bits_card": true
    }
    ],
    "type": "global_first_party",
    "order": 1,
    "last_updated": "2018-05-22T00:06:04Z",
    "is_charitable": false
})"_ba};

const QByteArray LOCAL_BADGE_JSON{R"({
    "data": [
        {
            "set_id": "subscriber",
            "versions": [
                {
                "click_url": null,
                "description": "Subscriber",
                "id": "3072",
                "image_url_1x": "https://chatterino.com/tb-1",
                "image_url_2x": "https://chatterino.com/tb-2",
                "image_url_4x": "https://chatterino.com/tb-3",
                "title": "Subscriber"
                }
            ]
        }
    ]
})"_ba};

const QByteArray SETTINGS_DEFAULT{"{}"_ba};

std::shared_ptr<TwitchChannel> makeMockTwitchChannel(
    const QString &name, const testlib::Snapshot &snapshot)
{
    auto chan = std::make_shared<TwitchChannel>(name);
    auto mocks = MockEmotes::channel();
    chan->setSeventvEmotes(std::move(mocks.seventv));
    chan->setBttvEmotes(std::move(mocks.bttv));
    chan->setFfzEmotes(std::move(mocks.ffz));

    QJsonObject defaultImage{
        {u"url_1x"_s, u"https://chatterino.com/reward1x.png"_s},
        {u"url_2x"_s, u"https://chatterino.com/reward2x.png"_s},
        {u"url_4x"_s, u"https://chatterino.com/reward4x.png"_s},
    };
    chan->addKnownChannelPointReward({{
        {u"channel_id"_s, u"11148817"_s},
        {u"id"_s, u"unused"_s},
        {u"reward"_s,
         {{
             {u"channel_id"_s, u"11148817"_s},
             {u"cost"_s, 1},
             {u"id"_s, u"31a2344e-0fce-4229-9453-fb2e8b6dd02c"_s},
             {u"is_user_input_required"_s, true},
             {u"title"_s, u"my reward"_s},
             {u"image"_s, defaultImage},
         }}},
    }});
    chan->addKnownChannelPointReward({{
        {u"channel_id"_s, u"11148817"_s},
        {u"id"_s, u"unused"_s},
        {u"reward"_s,
         {{
             {u"channel_id"_s, u"11148817"_s},
             {u"cost"_s, 1},
             {u"id"_s, u"dc8d1dac-256e-42b9-b7ba-40b32e5294e2"_s},
             {u"is_user_input_required"_s, false},
             {u"title"_s, u"test"_s},
             {u"image"_s, defaultImage},
         }}},
    }});
    chan->addKnownChannelPointReward({{
        {u"channel_id"_s, u"11148817"_s},
        {u"id"_s, u"unused"_s},
        {u"reward"_s,
         {{
             {u"channel_id"_s, u"11148817"_s},
             {u"cost"_s, 1},
             {u"default_bits_cost"_s, 2},
             {u"bits_cost"_s, 0},
             {u"pricing_type"_s, u"BITS"_s},
             {u"reward_type"_s, u"CELEBRATION"_s},
             {u"is_user_input_required"_s, false},
             {u"title"_s, u"BitReward"_s},
             {u"image"_s, defaultImage},
         }}},
        {u"redemption_metadata"_s,
         QJsonObject{
             {u"celebration_emote_metadata"_s,
              QJsonObject{
                  {u"emote"_s,
                   {{
                       {u"id"_s, u"42"_s},
                       {u"token"_s, u"MyBitsEmote"_s},
                   }}},
              }},
         }},
    }});

    chan->setUserColor("UserColor", {1, 2, 3, 4});
    chan->setUserColor("UserColor2", {5, 6, 7, 8});
    chan->addRecentChatter("UserChatter");
    chan->addRecentChatter("UserColor");

    chan->setCheerEmoteSets({
        HelixCheermoteSet{QJsonDocument::fromJson(CHEERMOTE_JSON).object()},
    });

    chan->setFfzChannelBadges({{u"123456"_s, {3, 4}}});

    chan->addTwitchBadgeSets(HelixChannelBadges{
        QJsonDocument::fromJson(LOCAL_BADGE_JSON).object(),
    });

    if (snapshot.param("ffzCustomVipBadge").toBool())
    {
        chan->setFfzCustomVipBadge(std::make_shared<Emote>(Emote{
            .name = {},
            .images = {Url{"https://chatterino.com/ffz-vip1x.png"}},
            .tooltip = {"VIP"},
            .homePage = {},
        }));
    }
    if (snapshot.param("ffzCustomModBadge").toBool())
    {
        chan->setFfzCustomModBadge(std::make_shared<Emote>(Emote{
            .name = {},
            .images = {Url{"https://chatterino.com/ffz-mod1x.png"}},
            .tooltip = {"Moderator"},
            .homePage = {},
        }));
    }

    return chan;
}

QT_WARNING_POP

}  // namespace

class TestIrcMessageHandlerP : public ::testing::TestWithParam<QString>
{
public:
    void SetUp() override
    {
        auto param = TestIrcMessageHandlerP::GetParam();
        this->snapshot = testlib::Snapshot::read(IRC_CATEGORY, param);

        this->mockApplication =
            std::make_unique<MockApplication>(QString::fromUtf8(
                this->snapshot->mergedSettings(SETTINGS_DEFAULT)));
        auto mocks = MockEmotes::global();
        this->mockApplication->seventvEmotes.setGlobalEmotes(mocks.seventv);
        this->mockApplication->bttvEmotes.setEmotes(mocks.bttv);
        this->mockApplication->ffzEmotes.setEmotes(mocks.ffz);
        this->mockApplication->getAccounts()->twitch.getCurrent()->setEmotes(
            mocks.twitchAccount);
        this->mockApplication->getUserData()->setUserColor(u"117691339"_s,
                                                           u"#DAA521"_s);

        this->mockApplication->getAccounts()
            ->twitch.getCurrent()
            ->blockUserLocally(u"12345"_s, u"blocked"_s);

        auto makeBadge = [](QStringView platform, QStringView name) {
            return std::make_shared<Emote>(Emote{
                .name = {platform.toString().toLower() % ':' % name},
                .images = {Url{u"https://chatterino.com/" % name % u".png"}},
                .tooltip = {name % u" badge"},
                .homePage = {},
                .zeroWidth = false,
                .id = {},
                .author = {},
                .baseName = {},
            });
        };

        // Chatterino
        this->mockApplication->chatterinoBadges.setBadge(
            {u"123456"_s}, makeBadge(u"chatterino", u"Chatterino"));

        // FrankerFaceZ
        this->mockApplication->ffzBadges.registerBadge(
            1, {.emote = makeBadge(u"frankerfacez", u"FFZ1"),
                .color = {9, 10, 11, 12}});
        this->mockApplication->ffzBadges.registerBadge(
            2, {.emote = makeBadge(u"frankerfacez", u"FFZ2"),
                .color = {13, 14, 15, 16}});
        this->mockApplication->ffzBadges.registerBadge(
            3, {.emote = makeBadge(u"frankerfacez", u"FFZ2"),
                .color = {17, 18, 19, 20}});
        this->mockApplication->ffzBadges.registerBadge(
            4, {.emote = makeBadge(u"frankerfacez", u"FFZ2"),
                .color = {21, 22, 23, 24}});
        this->mockApplication->getFfzBadges()->assignBadgeToUser({u"123456"_s},
                                                                 1);
        this->mockApplication->getFfzBadges()->assignBadgeToUser({u"123456"_s},
                                                                 2);

        // 7TV
        this->mockApplication->getSeventvBadges()->registerBadge({
            {u"id"_s, u"1"_s},
            {u"name"_s, u"7TV badge name"_s},
            {u"tooltip"_s, u"7TV badge"_s},
            {
                u"host"_s,
                {{
                    {u"url"_s, u"//chatterino.com/7tv/"_s},
                    {u"files"_s,
                     QJsonArray{
                         {{
                             {u"name"_s, u"1x"_s},
                             {u"format"_s, u"WEBP"_s},
                             {u"width"_s, 16},
                         }},
                     }},
                }},
            },
        });
        this->mockApplication->getSeventvBadges()->assignBadgeToUser(
            u"1"_s, {u"123456"_s});

        // BetterTTV
        this->mockApplication->getBttvBadges()->registerBadge({
            {u"startedAt"_s, u"2017-01-11T09:54:10.000Z"_s},
            {u"url"_s, u"https://chatterino.com/betterttv/test.png"_s},
        });
        this->mockApplication->getBttvBadges()->assignBadgeToUser(
            u"https://chatterino.com/betterttv/test.png"_s, {u"123456"_s});

        // Twitch
        this->mockApplication->getTwitchBadges()->loadLocalBadges();

        this->twitchdevChannel = std::make_shared<TwitchChannel>("twitchdev");
        this->twitchdevChannel->setRoomId("141981764");

        auto tdMocks = MockEmotes::twitchdev();
        this->twitchdevChannel->setSeventvEmotes(std::move(tdMocks.seventv));
        this->twitchdevChannel->setBttvEmotes(std::move(tdMocks.bttv));
        this->twitchdevChannel->setFfzEmotes(std::move(tdMocks.ffz));

        this->mockApplication->twitch.mockChannels.emplace(
            "twitchdev", this->twitchdevChannel);

        const auto helixExpectations =
            this->snapshot->param("helixExpectations").toObject();
        if (!helixExpectations.isEmpty())
        {
            initializeHelix(&this->mockHelix);

            int nCalls =
                helixExpectations.value("getSharedChatSession").toInt();
            if (nCalls > 0)
            {
                EXPECT_CALL(this->mockHelix, getSharedChatSession)
                    .Times(nCalls);
            }
        }
    }

    void TearDown() override
    {
        this->twitchdevChannel.reset();
        this->mockApplication.reset();
        this->snapshot.reset();
    }

    std::shared_ptr<TwitchChannel> twitchdevChannel;
    std::unique_ptr<MockApplication> mockApplication;
    std::unique_ptr<testlib::Snapshot> snapshot;
    testing::StrictMock<mock::Helix> mockHelix;
};

/// This tests the process of parsing IRC messages and emitting `MessagePtr`s.
///
/// Even though it's in the message builder category, this uses
/// `IrcMesssageHandler` to ensure the correct (or: "real") arguments to build
/// messages.
///
/// Tests are contained in `tests/snapshots/IrcMessageHandler`. Fixtures
/// consist of an object with the keys `input`, `output`, `settings` (optional),
/// and `params` (optional).
///
/// `UPDATE_SNAPSHOTS` (top) controls whether the `output` will be generated or
/// checked.
///
/// `params` is an optional object with the following keys:
/// - `prevMessages`: An array of past messages (used for replies)
/// - `findAllUsernames`: A boolean controlling the equally named setting
///   (default: false)
/// - `nAdditional`: Include n additional built messages (from `prevMessages`)
/// - `helixExpectations`: An object with names of Helix API methods that will
///   be called during the test and the expected call count. Name of the method
///   is the key and the number of calls its value.
TEST_P(TestIrcMessageHandlerP, Run)
{
    auto channel = makeMockTwitchChannel(u"pajlada"_s, *snapshot);

    VectorMessageSink sink;

    const auto &userData = snapshot->param("userData").toObject();
    for (auto it = userData.begin(); it != userData.end(); ++it)
    {
        const auto &userID = it.key();
        const auto &data = it.value().toObject();
        if (auto color = data.value("color").toString(); !color.isEmpty())
        {
            this->mockApplication->getUserData()->setUserColor(userID, color);
        }
    }

    for (auto prevInput : snapshot->param("prevMessages").toArray())
    {
        auto *ircMessage = Communi::IrcMessage::fromData(
            prevInput.toString().toUtf8(), nullptr);
        ASSERT_NE(ircMessage, nullptr);
        IrcMessageHandler::parseMessageInto(ircMessage, sink, channel.get());
        delete ircMessage;
    }

    auto *ircMessage =
        Communi::IrcMessage::fromData(snapshot->inputUtf8(), nullptr);
    ASSERT_NE(ircMessage, nullptr);

    auto nAdditionalMessages = snapshot->param("nAdditional").toInt(0);
    ASSERT_GE(sink.messages().size(), nAdditionalMessages);

    auto firstAddedMsg = sink.messages().size() - nAdditionalMessages;
    IrcMessageHandler::parseMessageInto(ircMessage, sink, channel.get());

    QJsonArray got;
    for (auto i = firstAddedMsg; i < sink.messages().size(); i++)
    {
        got.append(sink.messages()[i]->toJson());
    }

    delete ircMessage;

    ASSERT_TRUE(snapshot->run(got, UPDATE_SNAPSHOTS))
        << "Snapshot " << snapshot->name() << " failed. Expected JSON to be\n"
        << QJsonDocument(snapshot->output().toArray()).toJson() << "\nbut got\n"
        << QJsonDocument(got).toJson() << "\ninstead.";
}

INSTANTIATE_TEST_SUITE_P(
    IrcMessage, TestIrcMessageHandlerP,
    testing::ValuesIn(testlib::Snapshot::discover(IRC_CATEGORY)));

TEST(TestIrcMessageHandlerP, Integrity)
{
    ASSERT_FALSE(UPDATE_SNAPSHOTS);  // make sure fixtures are actually tested
}

TEST(IrcMessageHandler, RecentMessagesDoNotUpdateCurrentUserState)
{
    MockApplication app(QString::fromUtf8(SETTINGS_DEFAULT));
    initializeHelix(&app.helix);
    auto &accounts = app.getAccounts()->twitch;
    accounts.accounts.append(
        std::make_shared<TwitchAccount>("forsenwiki", "", "", "405330073"));
    accounts.load();
    accounts.currentUsername = "forsenwiki";
    ASSERT_EQ(accounts.getCurrent()->getUserId(), "405330073");

    auto channel = std::make_shared<TwitchChannel>("pajlada");
    ASSERT_FALSE(channel->isVip());
    const QByteArray data =
        "@tmi-sent-ts=1790452460376;id=863f60c5-3de6-4ecf-b2ff-20cdd85c12e9;"
        "room-id=11148817;user-id=405330073;display-name=forsenWiki;"
        "badges=vip/1;badge-info=;flags=;user-type=;emotes= "
        ":forsenwiki!forsenwiki@forsenwiki.tmi.twitch.tv "
        "PRIVMSG #pajlada :peepoZ";

    // A message that isn't from a live sink does not set vip status
    auto *historyMessage = Communi::IrcMessage::fromData(data, nullptr);
    ASSERT_NE(historyMessage, nullptr);
    VectorMessageSink historySink({}, MessageFlag::RecentMessage);
    IrcMessageHandler::parseMessageInto(historyMessage, historySink,
                                        channel.get());
    delete historyMessage;
    ASSERT_EQ(historySink.messages().size(), 1);
    EXPECT_FALSE(channel->isVip());

    // The same message parsed from a live sink sets vip status
    auto *liveMessage = Communi::IrcMessage::fromData(data, nullptr);
    ASSERT_NE(liveMessage, nullptr);
    VectorMessageSink liveSink(channel->sinkTraits());
    IrcMessageHandler::parseMessageInto(liveMessage, liveSink, channel.get());
    delete liveMessage;
    ASSERT_EQ(liveSink.messages().size(), 1);
    EXPECT_TRUE(channel->isVip());
}

#ifdef CHATTERINO_HAVE_PLUGINS
TEST(IrcMessageHandler, ModeratorAutoRepliesOnlyLiveVerifiedChannelAndAccount)
{
    MockApplication app(QString::fromUtf8(SETTINGS_DEFAULT));
    initializeHelix(&app.helix);
    app.settings.enablePinnedMessages = false;
    auto &accounts = app.accounts.twitch;
    accounts.accounts.append(
        std::make_shared<TwitchAccount>("fixturemod", "", "", "1000"));
    accounts.load();
    accounts.currentUsername = "fixturemod";
    ASSERT_EQ(accounts.getCurrent()->getUserId(), "1000");
    auto module = std::unique_ptr<ModeratorAutoReplies>(&ModeratorAutoReplies::instance());
    EXPECT_FALSE(module->hasGrant());
    EXPECT_TRUE(module->channels().empty());
    moderator::Policy policy;
    policy.revision = 1;
    policy.version = "1.0.0";
    policy.expires = QDateTime::currentDateTimeUtc().addDays(1);
    policy.allowed = {"1000"};
    policy.minimumCooldown = 0;
    policy.source = "return function(text,rules) if text==rules[1].triggers[1] then return 1 end end";
    ModeratorAutoRepliesTestAccess::configure(*module, policy, "1000",
        {"example", "another"}, "example", 1, "fixture answer", true, 0);
    ASSERT_TRUE(module->hasGrant());
    EXPECT_EQ(module->channels(), (QStringList{"another", "example"}));
    bool correctTabs = false;
    QTimer::singleShot(0, [&] {
        for (auto *widget : QApplication::topLevelWidgets())
        {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            auto *tabs = dialog->findChild<QTabBar *>("ModeratorChannelTabs");
            correctTabs = tabs && tabs->count() == 2 &&
                tabs->tabText(0) == "another" && tabs->tabText(1) == "example";
            dialog->reject();
        }
    });
    module->open(nullptr);
    EXPECT_TRUE(correctTabs);

    auto channel = std::make_shared<TwitchChannel>("example");
    TwitchChannelTestAccess::setMod(*channel, true);
    int sends = 0;
    pajlada::Signals::ScopedConnection connection(channel->sendReplySignal.connect(
        [&](const QString &text, const QString &id, bool &sent) {
            ++sends;
            EXPECT_EQ(text, "fixture answer");
            EXPECT_EQ(id, sends == 1 ? "live-event" : "second-event");
            sent = true;
        }));
    auto incoming = [&](const QByteArray &id, bool live, const QByteArray &badges = "",
                        const QByteArray &text = "question for the fixture") {
        const auto data = "@id=" + id + ";tmi-sent-ts=1790452460376;room-id=11148817;"
            "user-id=2000;display-name=viewer;badges=" + badges +
            ";badge-info=;flags=;user-type=;emotes= :viewer!viewer@viewer.tmi.twitch.tv "
            "PRIVMSG #example :" + text;
        std::unique_ptr<Communi::IrcMessage> message(Communi::IrcMessage::fromData(data, nullptr));
        auto *priv = dynamic_cast<Communi::IrcPrivateMessage *>(message.get());
        ASSERT_NE(priv, nullptr);
        VectorMessageSink sink(channel->sinkTraits());
        IrcMessageHandler::parsePrivMessageInto(priv, sink, channel.get(), live);
    };
    incoming("historical", false);
    EXPECT_EQ(sends, 0);
    incoming("moderator", true, "moderator/1");
    EXPECT_EQ(sends, 0);
    incoming("missing-word", true, "", "fixture questions");
    EXPECT_EQ(sends, 0);
    incoming("live-event", true);
    EXPECT_EQ(sends, 1);
    incoming("live-event", true);
    EXPECT_EQ(sends, 1);
    incoming("too-fast", true);
    EXPECT_EQ(sends, 1);
    QThread::msleep(1100);
    // The account-wide anti-flood remains; no deferred answer for a repeat.
    incoming("too-fast", true);
    EXPECT_EQ(sends, 1);
    incoming("live-event", true);
    EXPECT_EQ(sends, 1);
    incoming("second-event", true);
    EXPECT_EQ(sends, 2);
    TwitchChannelTestAccess::setMod(*channel, false);
    incoming("role-lost", true);
    EXPECT_EQ(sends, 2);
    policy.allowed.clear();
    ModeratorAutoRepliesTestAccess::configure(*module, policy, "1000",
        {"example"}, "example");
    EXPECT_FALSE(module->hasGrant());
    EXPECT_TRUE(module->channels().empty());
}

TEST(IrcMessageHandler, ModeratorAutoRepliesSelectedRoleAndCategoryReply)
{
    MockApplication app(QString::fromUtf8(SETTINGS_DEFAULT));
    initializeHelix(&app.helix);
    auto &accounts = app.accounts.twitch;
    accounts.accounts.append(std::make_shared<TwitchAccount>("fixturemod", "", "", "1000"));
    accounts.load();
    accounts.currentUsername = "fixturemod";
    ASSERT_EQ(accounts.getCurrent()->getUserId(), "1000");
    auto module = std::unique_ptr<ModeratorAutoReplies>(&ModeratorAutoReplies::instance());
    moderator::Policy policy;
    policy.revision = 1;
    policy.version = "1.0.0";
    policy.expires = QDateTime::currentDateTimeUtc().addDays(1);
    policy.allowed = {"1000"};
    policy.source = "return function(text,rules) if #rules>0 and text==rules[1].triggers[1] then return 1 end end";
    ModeratorAutoRepliesTestAccess::configure(*module, policy, "1000", {"example"}, "example",
        static_cast<unsigned>(moderator::SenderRole::Vip), "Playing {game}");
    auto channel = std::make_shared<TwitchChannel>("example");
    TwitchChannelTestAccess::setRoomId(*channel, "3000");
    TwitchChannelTestAccess::setMod(*channel, true);
    int sends = 0;
    pajlada::Signals::ScopedConnection connection(channel->sendReplySignal.connect(
        [&](const QString &text, const QString &id, bool &sent) {
            ++sends;
            EXPECT_EQ(text, "Playing Fixture Game");
            EXPECT_EQ(id, "vip-live");
            sent = true;
        }));
    Communi::TagsRef::MapType tags{{"id", "offline"}, {"user-id", "2000"}, {"room-id", "3000"}, {"badges", "vip/1"}};
    auto incoming = [&] { module->maybeReply(*channel, Communi::TagsRef(tags), "fixture question"); };
    incoming();
    EXPECT_EQ(sends, 0);
    HelixStream stream;
    stream.id = "4000";
    stream.gameId = "5000";
    stream.gameName = "Fixture Game";
    stream.startedAt = QDateTime::currentDateTimeUtc().addSecs(-30).toString(Qt::ISODate);
    TwitchChannelTestAccess::setLiveStream(*channel, stream);
    tags["id"] = "viewer-live"; tags["badges"] = "";
    incoming(); EXPECT_EQ(sends, 0);
    tags["id"] = "moderator-live"; tags["badges"] = "moderator/1";
    incoming(); EXPECT_EQ(sends, 0);
    tags["id"] = "own-live"; tags["user-id"] = "1000"; tags["badges"] = "vip/1";
    incoming(); EXPECT_EQ(sends, 0);
    tags["id"] = "vip-live"; tags["user-id"] = "2000";
    incoming(); EXPECT_EQ(sends, 1);
    incoming(); EXPECT_EQ(sends, 1);
    TwitchChannelTestAccess::setMod(*channel, false);
    tags["id"] = "after-role-loss";
    incoming(); EXPECT_EQ(sends, 1);
}
#endif

TEST_P(TestIrcMessageHandlerP, CloneElements)
{
    auto channel = makeMockTwitchChannel(u"pajlada"_s, *this->snapshot);

    VectorMessageSink sink;

    for (auto prevInput : this->snapshot->param("prevMessages").toArray())
    {
        auto *ircMessage = Communi::IrcMessage::fromData(
            prevInput.toString().toUtf8(), nullptr);
        ASSERT_NE(ircMessage, nullptr);
        IrcMessageHandler::parseMessageInto(ircMessage, sink, channel.get());
        delete ircMessage;
    }

    auto *ircMessage =
        Communi::IrcMessage::fromData(this->snapshot->inputUtf8(), nullptr);
    ASSERT_NE(ircMessage, nullptr);
    IrcMessageHandler::parseMessageInto(ircMessage, sink, channel.get());
    delete ircMessage;

    for (const auto &message : sink.messages())
    {
        for (const auto &original : message->elements)
        {
            auto originalObj = original->toJson();
            auto clonedObj = original->clone()->toJson();
            ASSERT_EQ(originalObj, clonedObj)
                << "\noriginal:\n"
                << QJsonDocument(originalObj).toJson() << "\ncloned:\n"
                << QJsonDocument(clonedObj).toJson();
        }
    }
}

TEST(IrcMessageHandler, DuplicateChannelPointRewardIrcMessageIgnored)
{
    MockApplication app(u"{}"_s);
    auto channel = std::make_shared<TwitchChannel>("pajlada");
    TwitchChannelTestAccess::setRoomId(*channel, u"11148817"_s);

    const auto rewardRedemption = QJsonObject{{
        {u"channel_id"_s, u"11148817"_s},
        {u"id"_s, u"redemption-1"_s},
        {u"user"_s,
         QJsonObject{
             {u"id"_s, u"129546453"_s},
             {u"login"_s, u"nerixyz"_s},
             {u"display_name"_s, u"nerixyz"_s},
         }},
        {u"reward"_s,
         {{
             {u"channel_id"_s, u"11148817"_s},
             {u"cost"_s, 1},
             {u"id"_s, u"31a2344e-0fce-4229-9453-fb2e8b6dd02c"_s},
             {u"is_user_input_required"_s, true},
             {u"title"_s, u"my reward"_s},
         }}},
    }};

    const QByteArray raw =
        "@tmi-sent-ts=1726662938032;subscriber=1;"
        "id=87af876f-5591-4a63-924f-46f465ecd3c4;"
        "room-id=11148817;user-id=129546453;display-name=nerixyz;"
        "badges=subscriber/24;badge-info=subscriber/27;color=#FF0000;"
        "flags=;user-type=;emotes=;"
        "custom-reward-id=31a2344e-0fce-4229-9453-fb2e8b6dd02c "
        ":nerixyz!nerixyz@nerixyz.tmi.twitch.tv PRIVMSG #pajlada "
        ":reward 1";

    auto parse = [&] {
        auto *ircMessage = Communi::IrcMessage::fromData(raw, nullptr);
        ASSERT_NE(ircMessage, nullptr);
        IrcMessageHandler::parseMessageInto(ircMessage, *channel,
                                            channel.get());
        delete ircMessage;
    };

    parse();
    EXPECT_EQ(channel->countMessages(), 1);
    ASSERT_NE(channel->getLastMessage(), nullptr);
    EXPECT_EQ(channel->getLastMessage()->reward, nullptr);

    parse();
    EXPECT_EQ(channel->countMessages(), 1);

    channel->addChannelPointReward(ChannelPointReward(rewardRedemption));
    EXPECT_EQ(channel->countMessages(), 1);
    ASSERT_NE(channel->getLastMessage(), nullptr);
    ASSERT_NE(channel->getLastMessage()->reward, nullptr);
    EXPECT_EQ(channel->getLastMessage()->reward->title, "my reward");
}

TEST(IrcMessageHandler, AutoModerationPilotUsesLiveEventsAndFiniteActions)
{
    using namespace automoderation;
    MockApplication app(QString::fromUtf8(SETTINGS_DEFAULT));
    initializeHelix(&app.helix);
    auto &accounts = app.accounts.twitch;
    accounts.accounts.append(std::make_shared<TwitchAccount>("fixturemoderator","","","730001"));
    // A secondary account saved in the same installation must remain eligible.
    accounts.accounts.append(std::make_shared<TwitchAccount>("alice","","","730003"));
    accounts.load();
    accounts.currentUsername = "fixturemoderator";
    ASSERT_EQ(accounts.getCurrent()->getUserId(),"730001");
    ASSERT_TRUE(TwitchChannel::isOwnAccountId("730003"));
    auto module = std::unique_ptr<ModeratorAutoReplies>(&ModeratorAutoReplies::instance());
    moderator::Policy policy;
    policy.revision=1;policy.version="1.0.0";policy.expires=QDateTime::currentDateTimeUtc().addDays(1);
    policy.allowed={"730001"};policy.source="return function(t,r) return nil end";
    ModeratorAutoRepliesTestAccess::configure(*module,policy,"730001",{"automodtest"},"automodtest");
    auto ch=std::make_shared<TwitchChannel>("automodtest");
    TwitchChannelTestAccess::setRoomId(*ch,"730002");
    TwitchChannelTestAccess::setMod(*ch,true);
    app.twitch.mockChannels["automodtest"]=ch;
    ASSERT_TRUE(permittedAutoModeration(*ch));

    const QString mute="313969fe-cc9f-4a0a-83c6-172acbd96957";
    const QString unmute="313969fe-cc9f-4a0a-83c6-172acbd96958";
    Config config{"730002",true,false,{},{
        {Kind::Words,"fixture phrase",{},600,"fixture word",true},
        {Kind::RewardTimeout,mute,"Reward mute",120,"fixture reward",true},
        {Kind::RewardUntimeout,unmute,"Reward unmute",1,"fixture unmute",true}
    }};
    const auto fileName=QDir(app.getPaths().settingsDirectory).filePath("noctis-auto-moderation-730001.json");
    {
        QFile file(fileName);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"schema",1},{"channels",QJsonObject{{"automodtest",encode(config)}}},{"journal",QJsonArray{}}}).toJson());
    }
    int actions=0;
    EXPECT_CALL(app.helix,banUser(QString("730002"),QString("730001"),QString("730003"),std::optional<int>(600),
                                 QString("fixture word"),testing::_,testing::_))
        .WillOnce([&](auto,auto,auto,auto,auto,auto done,auto){++actions;done();});
    EXPECT_CALL(app.helix,banUser(QString("730002"),QString("730001"),QString("730004"),std::optional<int>(120),
                                 QString("fixture reward"),testing::_,testing::_))
        .WillOnce([&](auto,auto,auto,auto,auto,auto done,auto){++actions;done();});
    EXPECT_CALL(app.helix,banUser(QString("730002"),QString("730001"),QString("730004"),std::optional<int>(1),
                                 QString("fixture unmute"),testing::_,testing::_))
        .WillOnce([&](auto,auto,auto,auto,auto,auto done,auto){++actions;done();});
    EXPECT_CALL(app.helix,getUserByName(QString("bob"),testing::_,testing::_))
        .Times(2).WillRepeatedly([](auto,auto success,auto){ success(HelixUser(QJsonObject{{"id","730004"},{"login","bob"}})); });
    auto incoming=[&](const QByteArray &id,const QByteArray &author,const QByteArray &user,const QByteArray &text,
                      const QByteArray &reward={},bool live=true,const QByteArray &badges={}) {
        const auto raw="@id="+id+";room-id=730002;user-id="+user+";badges="+badges+
            ";tmi-sent-ts="+QByteArray::number(QDateTime::currentMSecsSinceEpoch())+
            (reward.isEmpty()?QByteArray{}:";custom-reward-id="+reward)+
            " : "+author+"!"+author+"@"+author+".tmi.twitch.tv PRIVMSG #automodtest :"+text;
        // Use a normal IRC prefix without the extra space after ':'.
        QByteArray line=raw;line.replace(" : "," :");
        std::unique_ptr<Communi::IrcMessage> message(Communi::IrcMessage::fromData(line,nullptr));
        auto *priv=dynamic_cast<Communi::IrcPrivateMessage *>(message.get());
        ASSERT_NE(priv,nullptr);
        VectorMessageSink sink(ch->sinkTraits());
        IrcMessageHandler::parsePrivMessageInto(priv,sink,ch.get(),live);
    };
    incoming("old","alice","730003","fixture phrase",{},false);
    EXPECT_EQ(actions,0);
    incoming("seen","bob","730004","hello");
    incoming("live-word","alice","730003","fixture phrase");
    EXPECT_EQ(actions,1);
    QThread::msleep(1050);
    incoming("live-word","alice","730003","fixture phrase");
    EXPECT_EQ(actions,1);
    incoming("reward-mute","buyer","730005","@bob",mute.toUtf8());
    EXPECT_EQ(actions,2);
    QThread::msleep(1050);
    incoming("reward-unmute","buyer","730005","Bob",unmute.toUtf8());
    EXPECT_EQ(actions,3);
    // Persisted event reservation prevents reprocessing after the cooldown.
    QThread::msleep(1050);
    incoming("reward-unmute","buyer","730005","Bob",unmute.toUtf8());
    EXPECT_EQ(actions,3);
    incoming("wrong-target","buyer","730005","bob alice",mute.toUtf8());
    EXPECT_EQ(actions,3);
    // Actual dialog path: selecting Rewards fetches its eligible choices
    // automatically, then switching to test mode persists the configuration.
    int rewardFetchCalls=0;
    const QString additional="313969fe-cc9f-4a0a-83c6-172acbd96959";
    const QString textless="313969fe-cc9f-4a0a-83c6-172acbd96960";
    QTimer::singleShot(0,[&]{
        auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog,nullptr);
        auto *tabs=dialog->findChild<QTabWidget *>("AutoModerationTabs");
        auto *dry=dialog->findChild<QCheckBox *>("AutoModerationDryRun");
        auto *save=dialog->findChild<QPushButton *>("SaveRules");
        EXPECT_TRUE(tabs && tabs->count()==3);
        EXPECT_TRUE(dry && save);
        if(tabs)
        {
            EXPECT_EQ(rewardFetchCalls,0);
            tabs->setCurrentIndex(1);
            EXPECT_EQ(rewardFetchCalls,1);
            auto *table=dialog->findChild<QTableWidget *>("AutoModerationRewards");
            auto *refresh=dialog->findChild<QPushButton *>("AutoModerationRefreshRewards");
            EXPECT_TRUE(table && table->rowCount()==2 && refresh);
            if(table && table->rowCount()==2 && refresh)
            {
                auto *picker=qobject_cast<QComboBox *>(table->cellWidget(0,1));
                EXPECT_TRUE(picker);
                if(picker)
                {
                    EXPECT_EQ(picker->currentData().toString(),mute);
                    EXPECT_GT(picker->findData(additional),0);
                    EXPECT_EQ(picker->findData(textless),-1);
                    refresh->click();
                    EXPECT_EQ(rewardFetchCalls,2);
                    EXPECT_EQ(picker->currentData().toString(),mute);
                    EXPECT_GT(picker->findData(additional),0);
                }
            }
        }
        if(dry&&save){dry->setChecked(true);save->click();}
        if(const auto screenshot=qEnvironmentVariable("NOCTIS_AUTOMOD_SCREENSHOT");!screenshot.isEmpty())dialog->grab().save(screenshot);
        dialog->reject();
    });
    openAutoModeration(nullptr,[&](const QString &name,auto success,auto) {
        EXPECT_EQ(name,"automodtest");
        ++rewardFetchCalls;
        GqlChannelPointRewards result;
        result.channelId="730002";
        for(const auto &id:{mute,unmute,additional,textless})
        {
            GqlChannelPointReward reward;
            reward.id=id;reward.title="Fixture "+id.right(2);
            reward.isUserInputRequired=id!=textless;
            result.rewards.append(reward);
        }
        success(std::move(result));
    });
    QThread::msleep(1050);
    incoming("dry-word","alice","730003","fixture phrase");
    EXPECT_EQ(actions,3);
    QThread::msleep(1050);
    // IRC updates our current moderator role from the sender's badges.
    incoming("own-active","fixturemoderator","730001","fixture phrase",{},true,"moderator/1");
    QThread::msleep(1050);
    incoming("broadcaster","automodtest","730002","fixture phrase");
    QThread::msleep(1050);
    incoming("protected-vip","vipviewer","730006","fixture phrase",{},true,"vip/1");
    EXPECT_EQ(actions,3);
    QFile file(fileName);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto saved=QJsonDocument::fromJson(file.readAll()).object();
    EXPECT_TRUE(saved["channels"].toObject()["automodtest"].toObject()["dryRun"].toBool());
    QHash<QString,QString> journal;
    for(auto item:saved["journal"].toArray())
        journal.insert(item.toObject()["id"].toString(),item.toObject()["status"].toString());
    EXPECT_EQ(journal.size(),8);
    EXPECT_EQ(journal.value("730002/live-word"),"Action confirmed");
    EXPECT_EQ(journal.value("730002/reward-mute"),"Action confirmed");
    EXPECT_EQ(journal.value("730002/reward-unmute"),"Action confirmed");
    EXPECT_EQ(journal.value("730002/wrong-target"),"Invalid target login");
    EXPECT_EQ(journal.value("730002/dry-word"),"Test: would perform action");
    EXPECT_EQ(journal.value("730002/own-active"),"Protected or excluded target");
    EXPECT_EQ(journal.value("730002/broadcaster"),"Protected or excluded target");
    EXPECT_EQ(journal.value("730002/protected-vip"),"Protected or excluded target");
    TwitchChannelTestAccess::setMod(*ch,false);
    EXPECT_FALSE(permittedAutoModeration(*ch));
    // Finish queued UI/reward work while the mock application is still alive.
    for (int n = 0; n < 8; ++n)
    {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

