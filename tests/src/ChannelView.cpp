// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ChannelView.hpp"

#include "mocks/BaseApplication.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/Message.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "providers/repetitions/RepeatedMessageDetector.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/Scrollbar.hpp"

#include <QJsonArray>

#include <algorithm>

using namespace chatterino;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args, this->paths_, this->settings, this->theme,
                        this->fonts)
    {
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    WindowManager windowManager;
    mock::MockTwitchIrcServer twitch;
};

}  // namespace

TEST(ChannelViewTest, RepetitionDetectionRespectsOriginalRestrictions)
{
    MockApplication app;
    EXPECT_TRUE(app.settings.enableRepeatedMessageDetector);
    EXPECT_TRUE(app.settings.repeatedMessagesOnlyModChannels);
    EXPECT_TRUE(app.settings.repeatedMessagesShowOnlyModerationMode);
    RepeatedMessageDetector detector;
    RepeatedMessageCheck check{
        .channelID = "123", .userID = "456", .messageID = "one",
        .message = "repeated fixture message", .channelCanModerate = false,
    };
    EXPECT_FALSE(detector.check(check));
    app.settings.repeatedMessagesOnlyModChannels = false;
    EXPECT_FALSE(detector.check(check));
    check.messageID = "two";
    EXPECT_EQ(detector.check(check), 2);
    EXPECT_FALSE(detector.check(check)); // duplicate delivery does not count
    check.channelID = "789";
    check.messageID = "three";
    EXPECT_FALSE(detector.check(check)); // state is channel-specific
    app.settings.repeatedMessagesOnlyModChannels = true;
    check.messageID = "four";
    EXPECT_FALSE(detector.check(check));
    check.channelCanModerate = true;
    EXPECT_EQ(detector.check(check), 2);
    check.senderIsModerator = true;
    check.messageID = "five";
    EXPECT_FALSE(detector.check(check));
    check.senderIsModerator = false;
    check.senderIsBroadcaster = true;
    check.messageID = "six";
    EXPECT_FALSE(detector.check(check));
    check.senderIsBroadcaster = false;
    check.historical = true;
    EXPECT_FALSE(detector.check(check));
}

TEST(ChannelViewTest, TwitchUsercardCountsAreAuthoritativeOrUnavailable)
{
    const auto connection = [](int count, bool more) {
        return QJsonObject{{"__typename", "ModLogsTargetedActionsConnection"},
                           {"count", count},
                           {"pageInfo", QJsonObject{{"hasNextPage", more}}}};
    };
    const auto response = [](QJsonObject bans, QJsonObject timeouts) {
        return QJsonObject{{"data", QJsonObject{{"viewerCardModLogs",
            QJsonObject{{"bans", bans}, {"timeouts", timeouts}}}}}};
    };
    auto counts = TwitchGql::parseUsercardModerationCounts(
        response(connection(0, false), connection(99, true)));
    ASSERT_TRUE(counts);
    EXPECT_EQ(counts->bans, 0);
    EXPECT_FALSE(counts->moreBans);
    EXPECT_EQ(counts->timeouts, 99);
    EXPECT_TRUE(counts->moreTimeouts);
    auto zero = connection(0, false);
    zero.remove("pageInfo");
    counts = TwitchGql::parseUsercardModerationCounts(response(zero, zero));
    ASSERT_TRUE(counts);
    EXPECT_EQ(counts->bans, 0);
    EXPECT_EQ(counts->timeouts, 0);
    auto emptyPage = connection(0, false);
    emptyPage["count"] = QJsonValue::Null;
    EXPECT_FALSE(TwitchGql::parseUsercardModerationCounts(response(emptyPage, zero)));
    emptyPage["edges"] = QJsonArray{};
    counts = TwitchGql::parseUsercardModerationCounts(response(emptyPage, emptyPage));
    ASSERT_TRUE(counts);
    EXPECT_EQ(counts->bans, 0);
    EXPECT_EQ(counts->timeouts, 0);
    EXPECT_FALSE(TwitchGql::parseUsercardModerationCounts(
        response({{"__typename", "ModLogsTargetedActionsError"}}, connection(0, false))));
    EXPECT_FALSE(TwitchGql::parseUsercardModerationCounts(
        response(connection(-1, false), connection(0, false))));
    EXPECT_FALSE(TwitchGql::parseUsercardModerationCounts(QJsonObject{}));
}

TEST(ChannelViewTest, RepetitionCounterVisibilityAndSimilarityFilter)
{
    MockApplication app;
    const auto flag = MessageElementFlag::RepeatedMessageCounter;
    ChannelView chat(nullptr, ChannelView::Context::None);
    EXPECT_FALSE(chat.getFlags().has(flag));
    app.settings.repeatedMessagesShowOnlyModerationMode = true;
    EXPECT_FALSE(chat.getFlags().has(flag));
    app.settings.repeatedMessagesShowOnlyModerationMode = false;
    ChannelView card(nullptr, ChannelView::Context::UserCard);
    EXPECT_TRUE(card.getFlags().has(flag));
    app.settings.repeatedMessagesShowInUsercards = false;
    EXPECT_FALSE(card.getFlags().has(flag));

    app.settings.hideSimilar = true;
    app.settings.hideModerated = true;
    MessageBuilder builder;
    builder->flags.set(MessageFlag::Similar, MessageFlag::RepeatedMessage);
    builder.emplace<TextElement>(QStringLiteral("repeat"), MessageElementFlag::Text);
    builder.emplace<TextElement>(QStringLiteral("x2"), flag);
    const auto message = builder.release();
    MessageColors colors{};
    MessageLayoutContext ctx{.messageColors = colors,
                             .flags = {MessageElementFlag::Text, flag},
                             .width = 500, .message = *message};
    const auto hasCounter = [&] {
        MessageLayout layout(message);
        layout.layout(ctx, false);
        for (int y = 0; y < layout.getHeight(); ++y)
            for (int x = 0; x < layout.getWidth(); ++x)
                if (const auto *element = layout.getElementAt({double(x), double(y)});
                    element && element->getCreator().getFlags().has(flag))
                    return true;
        return false;
    };
    EXPECT_TRUE(hasCounter());
    app.settings.hideSimilar = false;
    EXPECT_TRUE(hasCounter());
    ctx.flags.unset(flag);
    EXPECT_FALSE(hasCounter());
    app.settings.enableRepeatedMessageDetector = false;
    EXPECT_FALSE(chat.getFlags().has(flag));
}

TEST(ChannelViewTest, GoToBottomGeometryUpdatesWhenScaleChanges)
{
    MockApplication mockApplication;

    ChannelView view(nullptr);
    view.resize(320, 180);

    const auto widgets = view.findChildren<QWidget *>();
    const auto it =
        std::find_if(widgets.begin(), widgets.end(), [](QWidget *w) {
            return dynamic_cast<Button *>(w) != nullptr &&
                   !w->accessibleName().isEmpty();
        });
    auto *button =
        it == widgets.end() ? nullptr : dynamic_cast<Button *>(*it);
    ASSERT_NE(button, nullptr);

    auto *scrollbar = view.scrollbar();
    ASSERT_NE(scrollbar, nullptr);

    const auto initialHeight = button->height();
    const auto initialScrollbarWidth = scrollbar->width();
    const auto initialLabelFontHeight = button->fontMetrics().height();

    view.setOverrideScale(1.5F);

    EXPECT_GT(button->height(), initialHeight);
    EXPECT_GT(scrollbar->width(), initialScrollbarWidth);
    EXPECT_GT(button->fontMetrics().height(), initialLabelFontHeight);
    EXPECT_GE(button->font().weight(), QFont::DemiBold);
    EXPECT_GE(button->height(), qRound(view.scale() * 26));
    EXPECT_GE(button->height(), button->fontMetrics().height() + qRound(view.scale() * 8));
    EXPECT_EQ(button->geometry().bottom(), view.rect().bottom() - qRound(view.scale() * 5));
    EXPECT_LT(button->geometry().right(), scrollbar->geometry().left());
    EXPECT_LT(button->width(), view.width());
    EXPECT_EQ(scrollbar->width(), int(view.scale() * 16));
}
