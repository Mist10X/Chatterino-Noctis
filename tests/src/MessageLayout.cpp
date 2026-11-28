// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/moderationactions/ModerationAction.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "mocks/BaseApplication.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/eventsub/MessageBuilder.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/ChannelHelpers.hpp"

#include <QDebug>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QSvgRenderer>
#include <QString>

#include <array>
#include <cmath>
#include <limits>
#include <memory>

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

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    AccountController accounts;
    WindowManager windowManager;
};

constexpr int WIDTH = 300;

class MessageLayoutTest
{
public:
    // "aaaaaaaa bbbbbbbb cccccccc"
    MessageLayoutTest(const QString &text)
    {
        MessageBuilder builder;
        builder.append(
            std::make_unique<TextElement>(text, MessageElementFlag::Text));
        this->layout = std::make_unique<MessageLayout>(builder.release());
        MessageColors colors;
        this->layout->layout(
            {
                .messageColors = colors,
                .flags = MessageElementFlag::Text,
                .width = WIDTH,
                .scale = 1,
                .imageScale = 1,
                .selectedChannel = nullptr,
                .message = *this->layout->getMessage(),
            },
            false);
    }

    MockApplication mockApplication;
    std::unique_ptr<MessageLayout> layout;
};

}  // namespace

TEST(TextElement, BasicCase)
{
    auto test = MessageLayoutTest("abc");

    // Simulate we are clicking on the first word
    auto point = QPoint(WIDTH / 20, test.layout->getHeight() / 2);

    const auto *hoveredElement = test.layout->getElementAt(point);
    ASSERT_NE(hoveredElement, nullptr);

    const auto [wordStart, wordEnd] =
        test.layout->getWordBounds(hoveredElement, point);

    EXPECT_EQ(wordStart, 0);
    EXPECT_EQ(wordEnd, 3);
}

TEST(ModerationChrome, InlineActionsKeepCommandsAndFitSingleLine)
{
    MockApplication app;
    const QStringList commands{
        "/timeout {user.name} 52", "/timeout {user.name} 10m",
        "/timeout {user.name} 1d", "/ban {user.name}", "/pin {msg.id}",
    };
    for (const auto &command : commands)
        app.settings.moderationActions.append(ModerationAction(command));
    app.settings.chatFontFamily.setValue("Noctis Montserrat");
    MessageBuilder builder;
    builder.emplace<TwitchModerationElement>(true, false, false);
    builder.emplace<TextElement>("viewer: Пример сообщения", MessageElementFlag::Text);
    auto message = builder.release();
    MessageLayout layout(message);
    MessageColors colors;
    colors.regularBg = QColor("#15151b");
    colors.alternateBg = colors.regularBg;
    colors.regularText = QColor("#e8e7ee");
    for (float scale : {1.F, 1.3F, 1.5F})
    {
        layout.layout({.messageColors = colors,
                       .flags = {MessageElementFlag::ModeratorTools, MessageElementFlag::Text},
                       .width = 500,
                       .scale = scale,
                       .imageScale = 1,
                       .selectedChannel = nullptr,
                       .message = *message}, false);
        QHash<QString, QRectF> hitBoxes;
        for (int y = 0; y < layout.getHeight(); ++y)
        {
            for (int x = 0; x < 500; ++x)
            {
                const auto *element = layout.getElementAt(QPointF(x, y));
                if (element && element->getLink().type == Link::UserAction)
                    hitBoxes.insert(element->getLink().value, element->getRect());
            }
        }
        EXPECT_EQ(hitBoxes.size(), commands.size());
        for (const auto &command : commands)
        {
            ASSERT_TRUE(hitBoxes.contains(command));
            EXPECT_NEAR(hitBoxes[command].height(), 16 * scale, 1.0);
            EXPECT_NEAR(hitBoxes[command].top(), hitBoxes[commands.front()].top(), 1.0);
        }
        EXPECT_GT(hitBoxes[commands.front()].width(), 16 * scale);
        if (scale == 1.3F)
        {
            QString copied;
            layout.addSelectionText(copied, 0, UINT32_MAX, CopyMode::OnlyTextAndEmotes);
            EXPECT_FALSE(copied.contains("/timeout"));
            EXPECT_TRUE(copied.contains("viewer:"));
            const auto screenshot = qEnvironmentVariable("NOCTIS_INLINE_MODERATION_PREVIEW");
            if (!screenshot.isEmpty())
            {
                QImage image(500, layout.getHeight(), QImage::Format_ARGB32_Premultiplied);
                image.fill(colors.regularBg);
                QPainter painter(&image);
                layout.paint({.painter = painter,
                              .selection = Selection{},
                              .colorProvider = ColorProvider::instance(),
                              .messageColors = colors,
                              .preferences = MessagePreferences{},
                              .canvasWidth = 500});
                painter.end();
                EXPECT_TRUE(image.save(screenshot));
            }
        }
    }
}

TEST(ModerationChrome, HeaderIconsRenderAtSmallSizes)
{
    const QStringList paths{
        ":/buttons/moderationDisabled-darkMode.svg",
        ":/buttons/moderationEnabled-darkMode.svg",
        ":/buttons/moderationDisabled-lightMode.svg",
        ":/buttons/moderationEnabled-lightMode.svg",
    };
    QImage preview(160, 80, QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor("#15151b"));
    QPainter previewPainter(&preview);
    previewPainter.fillRect(QRect(80, 0, 80, 80), QColor("#eeeeef"));
    for (int i = 0; i < paths.size(); ++i)
    {
        QSvgRenderer renderer(paths[i]);
        ASSERT_TRUE(renderer.isValid());
        for (const int size : {18, 27, 36})
        {
            QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            renderer.render(&painter);
            painter.end();
            int visible = 0;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    visible += qAlpha(image.pixel(x, y)) > 0;
            EXPECT_GT(visible, size * size / 5);
            EXPECT_LT(visible, size * size * 4 / 5);
        }
        renderer.render(&previewPainter, QRectF((i / 2) * 80 + 23, (i % 2) * 40 + 3, 34, 34));
    }
    previewPainter.end();
    const auto path = qEnvironmentVariable("NOCTIS_MODERATION_ICON_PREVIEW");
    if (!path.isEmpty())
        EXPECT_TRUE(preview.save(path));
}

TEST(ModerationChrome, InlinePinRequiresConfiguredActionAndModerationMode)
{
    MockApplication app;
    auto countPins = [&](bool canModerate, bool targetModerator, bool modMode) {
        MessageBuilder builder;
        builder.emplace<TwitchModerationElement>(canModerate, targetModerator, false);
        builder.emplace<TextElement>("viewer: text", MessageElementFlag::Text);
        const auto msg = builder.release();
        MessageLayout layout(msg);
        MessageColors colors;
        MessageElementFlags flags{MessageElementFlag::Text};
        if (modMode)
            flags.set(MessageElementFlag::ModeratorTools);
        layout.layout({.messageColors = colors, .flags = flags, .width = 500,
                       .scale = 1.F, .message = *msg}, false);
        QSet<const MessageLayoutElement *> pins;
        for (int y = 0; y < layout.getHeight(); ++y)
            for (int x = 0; x < 500; ++x)
                if (const auto *el = layout.getElementAt(QPointF(x, y));
                    el && el->getLink().type == Link::UserAction &&
                    el->getLink().value.startsWith("/pin"))
                    pins.insert(el);
        return pins.size();
    };
    EXPECT_EQ(countPins(true, false, true), 0);
    EXPECT_EQ(countPins(true, false, false), 0);
    EXPECT_EQ(countPins(false, false, true), 0);
    EXPECT_EQ(countPins(false, true, true), 0);
    EXPECT_EQ(countPins(false, true, false), 0);
    app.settings.moderationActions.append(ModerationAction("/pin {msg.id}"));
    EXPECT_EQ(countPins(true, false, true), 1);
    EXPECT_EQ(countPins(false, true, true), 1);
    EXPECT_EQ(countPins(true, false, false), 0);
    EXPECT_EQ(countPins(false, true, false), 0);
    EXPECT_EQ(countPins(false, false, true), 0);
    app.settings.moderationActions.removeAt(0);
    EXPECT_EQ(countPins(true, false, true), 0);
    EXPECT_EQ(countPins(false, true, true), 0);
}

namespace {

MessagePtrMut indicatorMessage(MessageFlags flags)
{
    MessageBuilder builder;
    builder->flags = flags;
    builder.emplace<TimestampElement>(QTime(12, 34, 56));
    builder.emplace<TextElement>(
        "Saved context has been banned. This is ordinary message text, not a "
        "type discriminator.",
        MessageElementFlag::Text);
    return builder.release();
}

struct PaintedIndicator {
    QImage image;
    int height;
    size_t lines;
    QString copyText;
};

class PunishmentIndicatorTest : public ::testing::Test
{
protected:
    MockApplication app;

    void SetUp() override
    {
        this->app.settings.clientDetectionIcon = false;
        // Keep animated username paints out of these moderation marker images.
        this->app.settings.displaySevenTVPaints = false;
        this->app.settings.hideModerated = false;
        this->app.settings.hideModerationActions = false;
    }

    PaintedIndicator paint(MessagePtr message, float scale = 1.F,
                           qreal dpr = 1., bool overlays = false)
    {
        MessageColors colors;
        colors.regularBg = QColor(24, 25, 32);
        colors.alternateBg = colors.regularBg;
        colors.regularText = Qt::white;
        colors.systemText = Qt::white;
        colors.linkText = Qt::white;
        colors.disabled = QColor(0, 0, 0, 180);
        colors.selection = QColor(30, 60, 150);
        colors.messageSeperator = QColor(80, 80, 90);
        MessagePreferences preferences;
        preferences.fadeMessageHistory = overlays;
        preferences.separateMessages = overlays;
        // Deliberately do not enable AutoMod or other highlight preferences.

        MessageLayout layout(message);
        layout.layout(
            {.messageColors = colors,
             .flags = {MessageElementFlag::Timestamp, MessageElementFlag::Text},
             .width = WIDTH,
             .scale = scale,
             .message = *message},
            false);
        constexpr int OFFSET = 7;
        QImage image(
            int(std::ceil(WIDTH * dpr)),
            int(std::ceil((layout.getHeight() + 2 * OFFSET + 1) * dpr)),
            QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(colors.regularBg);
        QPainter painter(&image);
        Selection selection;
        if (overlays)
        {
            selection = {{0, 0}, {0, layout.getLastCharacterIndex()}};
        }
        layout.paint({.painter = painter,
                      .selection = selection,
                      .colorProvider = ColorProvider::instance(),
                      .messageColors = colors,
                      .preferences = preferences,
                      .canvasWidth = WIDTH,
                      .y = OFFSET});
        painter.end();
        QString copied;
        layout.addSelectionText(copied, 0);
        return {std::move(image), layout.getHeight(), layout.getLineCount(),
                std::move(copied)};
    }

    static QRect indicatorBounds(const QImage &image, float scale = 1.F)
    {
        // The box starts inside the eight-unit text margin. Compare with the
        // same row outside it so whole-row moderation and selection overlays
        // cannot masquerade as an indicator, including on high-DPI displays.
        const int margin = int(8 * scale * image.devicePixelRatio());
        QRect bounds;
        for (int y = 0; y < image.height(); ++y)
        {
            const QColor bg = image.pixelColor(0, y);
            for (int x = 1; x < std::min(margin, image.width()); ++x)
            {
                const auto color = image.pixelColor(x, y);
                if (std::abs(color.red() - bg.red()) +
                        std::abs(color.green() - bg.green()) +
                        std::abs(color.blue() - bg.blue()) >
                    4)
                {
                    const QRect pixel(x, y, 1, 1);
                    bounds = bounds.isNull() ? pixel : bounds.united(pixel);
                }
            }
        }
        return bounds;
    }
};

TEST_F(PunishmentIndicatorTest, UsesTypedFlagsRatherThanSystemOrMessageText)
{
    struct Case {
        const char *name;
        MessageFlags flags;
        bool stripe;
    };
    const std::array cases{
        Case{"ordinary chat mentioning a ban", {}, false},
        Case{"ordinary system", {MessageFlag::System}, false},
        Case{"generic moderation action",
             {MessageFlag::System, MessageFlag::ModerationAction},
             false},
        Case{"IRC ban or timeout",
             {MessageFlag::System, MessageFlag::Timeout},
             true},
        Case{"EventSub timeout",
             {MessageFlag::EventSub, MessageFlag::PubSub, MessageFlag::Timeout,
              MessageFlag::ModerationAction},
             true},
        Case{"deletion notice",
             {MessageFlag::System, MessageFlag::ModerationAction,
              MessageFlag::Deletion},
             true},
        Case{"disabled original",
             {MessageFlag::Disabled, MessageFlag::InvalidReplyTarget},
             false},
        Case{"AutoMod status",
             {MessageFlag::AutoMod, MessageFlag::ModerationAction},
             false},
        Case{"AutoMod header only",
             {MessageFlag::AutoModOffendingMessageHeader},
             false},
        Case{"held AutoMod text",
             {MessageFlag::AutoMod, MessageFlag::AutoModOffendingMessage},
             true},
        Case{"blocked-term hold",
             {MessageFlag::AutoMod, MessageFlag::AutoModBlockedTerm,
              MessageFlag::AutoModOffendingMessage},
             true},
        Case{"unban or untimeout",
             {MessageFlag::ModerationAction, MessageFlag::Untimeout},
             false},
        Case{"defensive untimeout wins",
             {MessageFlag::Timeout, MessageFlag::Untimeout},
             false},
        Case{"channel-wide clear",
             {MessageFlag::System, MessageFlag::ClearChat,
              MessageFlag::ModerationAction},
             false},
    };
    for (const auto &test : cases)
    {
        SCOPED_TRACE(test.name);
        EXPECT_EQ(!indicatorBounds(this->paint(indicatorMessage(test.flags)).image)
                       .isNull(),
                  test.stripe);
    }
}

TEST_F(PunishmentIndicatorTest, BoxDoesNotChangeLayoutOrCopyText)
{
    for (const auto scale : {1.F, 1.5F})
    {
        for (const auto dpr : {1., 1.5})
        {
            SCOPED_TRACE(::testing::Message()
                         << "scale=" << scale << " dpr=" << dpr);
            const auto plain = this->paint(indicatorMessage({}), scale, dpr);
            const auto marked = this->paint(
                indicatorMessage({MessageFlag::Timeout}), scale, dpr);
            ASSERT_EQ(marked.height, plain.height);
            ASSERT_EQ(marked.lines, plain.lines);
            ASSERT_EQ(marked.copyText, plain.copyText);
            EXPECT_FALSE(indicatorBounds(marked.image, scale).isNull());
        }
    }
}

TEST_F(PunishmentIndicatorTest, SurvivesSelectionHistoryFadeAndDisabledOverlay)
{
    const auto marked = this->paint(
        indicatorMessage({MessageFlag::Timeout, MessageFlag::Disabled,
                          MessageFlag::RecentMessage}),
        1.F, 1., true);
    EXPECT_FALSE(indicatorBounds(marked.image).isNull());
}

TEST_F(PunishmentIndicatorTest, HiddenActionsDoNotLeaveAnIndicator)
{
    this->app.settings.hideModerationActions = true;
    const auto hidden =
        this->paint(indicatorMessage({MessageFlag::System, MessageFlag::Timeout,
                                      MessageFlag::ModerationAction}));
    EXPECT_EQ(hidden.lines, 0);
    EXPECT_TRUE(indicatorBounds(hidden.image).isNull());
}

TEST_F(PunishmentIndicatorTest,
       ActualIrcBuildersAndStackedReplacementRemainMarked)
{
    const auto time =
        QDateTime::fromString("2026-10-20T12:00:00Z", Qt::ISODate);
    this->app.settings.timeoutStackStyle =
        static_cast<int>(TimeoutStackStyle::StackHard);
    std::vector<MessagePtr> messages;
    for (const auto duration : {QStringLiteral("60"), QString{}})
    {
        SCOPED_TRACE(duration.isEmpty() ? "permanent ban" : "timeout");
        auto message = [&] {
            SCOPED_TRACE("build IRC punishment");
            MessageBuilder builder(timeoutMessage, "user", duration, false,
                                   time);
            return builder.release();
        }();
        ASSERT_TRUE(message->flags.has(MessageFlag::Timeout));
        [&] {
            SCOPED_TRACE("render IRC punishment");
            EXPECT_FALSE(indicatorBounds(this->paint(message).image).isNull());
        }();
        addOrReplaceChannelTimeout(
            messages, message, time,
            [&](auto index, const auto &, const auto &replacement) {
                messages.at(index) = replacement;
            },
            [&](const auto &added) {
                messages.push_back(added);
            },
            false);
    }
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages.front()->count, 2U);
    EXPECT_TRUE(messages.front()->flags.has(MessageFlag::Timeout));
    EXPECT_FALSE(indicatorBounds(this->paint(messages.front()).image).isNull());

    auto original = indicatorMessage({});
    original->loginName = "user";
    original->displayName = "User";
    original->messageText = "Original saved message";
    original->id = "original-message-id";
    auto deletion = MessageBuilder::makeDeletionMessageFromIRC(original);
    EXPECT_TRUE(deletion->flags.has(MessageFlag::Deletion));
    EXPECT_FALSE(original->flags.has(MessageFlag::Deletion));
    EXPECT_FALSE(indicatorBounds(this->paint(deletion).image).isNull());
}

TEST_F(PunishmentIndicatorTest,
       EventSubDeletionIsTypedAndSerializesWithoutTruncation)
{
    static_assert(sizeof(MessageFlags::Int) == sizeof(std::int64_t));
    static_assert(std::numeric_limits<MessageFlags::Int>::digits > 51);
    static_assert(static_cast<std::int64_t>(MessageFlag::GigantifiedEmote) ==
                  (1LL << 50));
    static_assert(static_cast<std::int64_t>(MessageFlag::Deletion) ==
                  (1LL << 51));

    eventsub::EventSubMessageBuilder builder(nullptr,
                                             QDateTime::currentDateTimeUtc());
    eventsub::lib::payload::channel_moderate::v2::Event event{};
    eventsub::lib::payload::channel_moderate::v2::Delete action{
        .userID = {"42"},
        .userLogin = {"user"},
        .userName = {"User"},
        .messageID = {"saved-id"},
        .messageBody = {"saved deleted text"}};
    eventsub::makeModerateMessage(builder, event, action);
    auto deletion = builder.release();
    ASSERT_TRUE(deletion->flags.has(MessageFlag::Deletion));
    EXPECT_TRUE(deletion->toJson()["flags"].toString().endsWith("|Deletion"));
    EXPECT_FALSE(indicatorBounds(this->paint(deletion).image).isNull());
}

}  // namespace
