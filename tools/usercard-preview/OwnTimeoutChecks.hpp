// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QFontMetricsF>
#include <QApplication>
#include <QPainter>
#include <QPushButton>
#include <QFrame>
#include <QVariantAnimation>

// The own timeout in the input box (like Chatterino+): red with a countdown,
// green once it is over, normal after the first message. Synthetic channel,
// no IRC / PubSub connection; the state is driven through the same
// TwitchChannel entry points that IRC and PubSub use.

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "common/UiLanguage.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageParseArgs.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/links/ImagePreview.hpp"
#include "singletons/ImageUploader.hpp"
#include "widgets/helper/UploadPreview.hpp"
#include "widgets/TooltipWidget.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/Mist1XMark.hpp"
#include "widgets/splits/InputHighlighter.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "providers/seventv/paints/LinearGradientPaint.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/helper/CornerBanners.hpp"
#include "singletons/Theme.hpp"
#include <QTextEdit>
#include <QKeyEvent>
#include <QListWidget>
#include <QPushButton>
#include <QMouseEvent>
#include <QMenu>
#include <QTemporaryDir>
#include <QScreen>

#include <IrcMessage>
#include <QBuffer>
#include <QRegion>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QCoreApplication>
#include <cstdio>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace owntimeout {

inline void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

inline QString before(const QString &pattern)
{
    return pattern.section(QStringLiteral("%1"), 0, 0);
}

}  // namespace owntimeout

namespace chatterino {
/// Room modes are set by Twitch's ROOMSTATE; this fixture sets them directly.
class TwitchChannelTestAccess
{
public:
    static void loadStartupHistory(TwitchChannel &channel)
    {
        channel.loadRecentMessages();
    }

    static qint64 rejoinAfterHeartbeat(TwitchChannel &channel, qint64 heartbeatMs)
    {
        *channel.roomID_.access() = QStringLiteral("910000002");
        channel.lastConnectedAt_ = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(heartbeatMs));
        channel.markDisconnected();
        channel.joined.invoke();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   channel.lastConnectedAt_->time_since_epoch())
            .count();
    }

    static bool historyBadgeRefreshPending(const TwitchChannel &channel)
    {
        return channel.historyBadgeRefreshQueued_;
    }

    static void noteLoadedHistoryForBadges(
        TwitchChannel &channel, const std::vector<MessagePtr> &messages)
    {
        channel.noteLoadedHistoryForBadges(messages);
    }

    static void setSlowMode(TwitchChannel &channel, int seconds)
    {
        auto modes = *channel.accessRoomModes();
        modes.slowMode = seconds;
        channel.setRoomModes(modes);
    }

    static void setSlowAndFollowerMode(TwitchChannel &channel, int seconds, int minutes)
    {
        auto modes = *channel.accessRoomModes();
        modes.slowMode = seconds;
        modes.followerOnly = minutes;
        channel.setRoomModes(modes);
    }
};
}  // namespace chatterino

namespace owntimeout {

inline int runOwnTimeoutChecks(chatterino::Split &split, const QString &report,
                               const QString &screenshotBase)
{
    using namespace chatterino;
    using Kind = TwitchChannel::OwnRestriction::Kind;
    QJsonArray checks;
    int failed = 0;
    auto check = [&](const QString &name, bool ok) {
        failed += ok ? 0 : 1;
        checks.append(QJsonObject{{"name", name}, {"ok", ok}});
        std::fprintf(stderr, "own-timeout check: %s %s\n", ok ? "ok" : "FAIL",
                     name.toUtf8().constData());
    };
    auto shot = [&](const QString &suffix) {
        if (!screenshotBase.isEmpty())
        {
            split.getInput().grab().save(screenshotBase + suffix + ".png",
                                         "PNG");
        }
    };

    auto channel = std::make_shared<TwitchChannel>(QStringLiteral("archive_room"));
    split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(channel)));
    split.resize(560, 220);
    split.show();
    pump(200);
    ResizingTextEdit *edit = nullptr;
    for (auto *candidate : split.getInput().findChildren<QTextEdit *>())
    {
        if (auto *resizing = dynamic_cast<ResizingTextEdit *>(candidate))
        {
            edit = resizing;
            break;
        }
    }
    if (edit == nullptr)
    {
        check("input box found", false);
        return failed;
    }
    const auto timedOutPrefix = before(uiText("You are timed out · %1 left"));
    const auto endedText = uiText("Your timeout has ended — you can chat again");
    const auto unbannedText =
        uiText("You have been unbanned — you can chat again");

    {
        const bool wasEnabled = getSettings()->enableChannelPointsDisplay.getValue();
        getSettings()->enableChannelPointsDisplay = true;
        channel->setChannelPointBalance(150694);
        pump(50);
        auto *points = split.getInput().findChild<QLabel *>("ChannelPointsBalancePill");
        const auto expected = activeUiLanguage().startsWith(u"ru")
                                  ? QStringLiteral("150,6к")
                                  : QStringLiteral("150.6k");
        check("channel points show a shortened balance without exact-sum tooltip",
              points && points->isVisible() && points->text() == expected &&
                  !points->toolTip().contains("150") &&
                  channel->channelPointBalance() == 150694);
        shot(QStringLiteral("-points-compact"));
        if (points)
        {
            auto *animation = points->findChild<QVariantAnimation *>();
            const int height = split.getInput().height();
            const int width = points->width();
            channel->setChannelPointBalance(151694);
            pump(50);
            check("earned points animate while the actual balance is current",
                  animation && animation->state() == QAbstractAnimation::Running &&
                      animation->currentValue().toReal() > 0.0 &&
                      animation->currentValue().toReal() < 1.0 &&
                      points->width() == width &&
                      split.getInput().height() == height &&
                      channel->channelPointBalance() == 151694);
            shot(QStringLiteral("-points-animated"));
            channel->setChannelPointBalance(134694);
            pump(60);
            check("spending retargets an ongoing points animation without resizing",
                  animation && animation->state() == QAbstractAnimation::Running &&
                      points->width() == width && split.getInput().height() == height);
            pump(280);
            const auto spent = activeUiLanguage().startsWith(u"ru")
                                   ? QStringLiteral("134,6к")
                                   : QStringLiteral("134.6k");
            check("points animation settles at the real target",
                  animation && animation->state() == QAbstractAnimation::Stopped &&
                      animation->currentValue().toReal() == 1.0 && points->text() == spent &&
                      split.getInput().height() == height);
            channel->setChannelPointBalance(134699);
            check("small change still animates even when abbreviated value stays the same",
                  animation && animation->state() == QAbstractAnimation::Running &&
                      points->text() == spent);
        }
        getSettings()->enableChannelPointsDisplay = wasEnabled;
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    {
        auto &input = split.getInput();
        auto *send = input.findChild<Button *>("chatSendButton");
        auto *card = input.findChild<QWidget *>("channelPointRewardPromptWidget");
        auto *title = input.findChild<QLabel *>("channelPointRewardPromptTitle");
        auto *cancel = input.findChild<QPushButton *>("channelPointRewardPromptCancel");
        check("reward composer controls exist", send && card && title && cancel);
        if (send && card && title && cancel)
        {
            int submitted = 0;
            QString submittedText;
            auto open = [&] {
                input.showChannelPointRewardPrompt(
                    QStringLiteral("Замутить зрителя на 10 минут"), QColor("#e12828"),
                    uiText("Add a message"), true,
                    [&](const QString &text) {
                        ++submitted;
                        submittedText = text;
                    });
                pump(60);
            };
            input.setInputText("ordinary draft");
            split.resize(820, 250);
            open();
            check("reward is a separate localized card and Esc placeholder",
                  card->isVisible() && title->text() ==
                    uiText("Reward: %1").arg(QStringLiteral("Замутить зрителя на 10 минут")) &&
                  edit->placeholderText() == uiText("Enter a message · Esc to cancel") &&
                  edit->toPlainText().isEmpty());
            shot(QStringLiteral("-reward"));
            send->leftClicked();
            check("empty reward cannot submit or fall through to chat",
                  submitted == 0 && !card->isHidden());
            input.setInputText("reward draft");
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(edit, &escape);
            check("Esc cancels without submitting and restores the ordinary draft",
                  submitted == 0 && card->isHidden() &&
                  edit->toPlainText() == "ordinary draft" &&
                  edit->placeholderText() != uiText("Enter a message · Esc to cancel"));
            open();
            input.setInputText("fixture reward text");
            send->leftClicked();
            check("one explicit reward submission uses just the reward text",
                  submitted == 1 && submittedText == "fixture reward text" &&
                  card->isHidden() && edit->toPlainText() == "ordinary draft");
            open();
            cancel->click();
            check("close button cancels without submitting",
                  submitted == 1 && card->isHidden());
            // Simulate selecting a different channel. A stale reward must not
            // use the captured previous channel's token or callback.
            open();
            auto other = std::make_shared<TwitchChannel>("reward_other");
            split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(other)));
            pump(60);
            check("channel change cancels stale reward input",
                  submitted == 1 && card->isHidden() && edit->toPlainText().isEmpty());
            split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(channel)));
            input.setInputText({});
            split.resize(560, 220);
            pump(60);
        }
    }
#endif

    // Real SplitInput layout, not a text-edit sized by the test itself:
    // an ordinary message must keep its original one-line height even though
    // it would wrap at the editor's hypothetical minimum width. The optional
    // character count must not create a second row of controls either.
    {
        auto &input = split.getInput();
        const bool oldLengthSetting = getSettings()->showMessageLength;
        const auto message = QStringLiteral("ейфазыафаыфаыфджлыаф");
        auto *length = input.findChild<QLabel *>(QStringLiteral("chatMessageLength"));
        split.resize(880, 280);
        for (const bool showLength : {false, true})
        {
            getSettings()->showMessageLength = showLength;
            input.setInputText({});
            pump(80);
            const int emptyHeight = input.height();
            const int emptyEditorHeight = edit->height();
            const int emptyMinimum = edit->minimumSizeHint().height();
            edit->insertPlainText(message.left(1));
            pump(50);
            bool stable = input.height() == emptyHeight &&
                          edit->height() == emptyEditorHeight;
            edit->insertPlainText(message.mid(1));
            pump(80);
            stable &= input.height() == emptyHeight &&
                      edit->height() == emptyEditorHeight &&
                      edit->minimumSizeHint().height() == emptyMinimum &&
                      edit->document()->firstBlock().layout()->lineCount() == 1;
            std::fprintf(stderr, "plain input length=%d: empty=%d typed=%d (editor %d -> %d)\n",
                         showLength, emptyHeight, input.height(),
                         emptyEditorHeight, edit->height());
            check(showLength ? "one-line input stays the same height with character count"
                             : "one-line input stays the same height without character count",
                  stable && length && length->isVisible() == showLength &&
                      (!showLength || length->text() == QString::number(message.size())));
        }
        shot(QStringLiteral("-typed-one-line"));
        input.setInputText({});
        split.resize(360, 280);
        pump(80);
        const int narrowEmptyHeight = input.height();
        input.setInputText(QStringLiteral("проверка переноса сообщения ").repeated(3));
        pump(100);
        check("real input grows when its text actually wraps",
              edit->document()->firstBlock().layout()->lineCount() > 1 &&
                  input.height() > narrowEmptyHeight &&
                  edit->document()->size().height() <= edit->viewport()->height() + 1);
        input.setInputText({});
        pump(80);
        check("clearing wrapped input restores one-line height", input.height() == narrowEmptyHeight);
        getSettings()->showMessageLength = oldLengthSetting;
        split.resize(560, 220);
        pump(80);
    }

    // 1. timed out (CLEARCHAT with ban-duration / msg_timedout notice)
    // (Typing into the real input needs app services this fixture lacks,
    // so the draft hand-back is not exercised here.)
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(300));
    pump(150);
    check("timed out: the countdown shows, the input stays usable",
          !edit->isReadOnly() && edit->toPlainText().isEmpty() &&
              edit->placeholderText().startsWith(timedOutPrefix) &&
              (edit->placeholderText().contains(QStringLiteral("5:00")) ||
               edit->placeholderText().contains(QStringLiteral("4:59"))));
    shot(QStringLiteral("-timeout"));
    pump(1200);
    check("the countdown ticks every second",
          edit->placeholderText().contains(QStringLiteral("4:59")) ||
              edit->placeholderText().contains(QStringLiteral("4:58")));
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(299));
    check("a repeated notice about the same timeout changes nothing",
          channel->ownRestriction().kind == Kind::TimedOut);

    // 2. removed early (PubSub chatrooms-user-v1 untimeout)
    channel->handleOwnModerationPubSub(
        QJsonObject{{"action", "untimeout"}, {"channel_id", "1"}});
    pump(150);
    check("untimeout: green and editable again",
          channel->ownRestriction().kind == Kind::Lifted &&
              !edit->isReadOnly() && edit->placeholderText() == endedText);
    pump(100);
    shot(QStringLiteral("-ended"));

    // 3. the first message afterwards
    channel->acknowledgeOwnRestrictionLifted();
    pump(100);
    check("after the first message the input is normal again",
          channel->ownRestriction().kind == Kind::None &&
              !edit->isReadOnly() && edit->placeholderText() != endedText);
    shot(QStringLiteral("-normal"));

    // 4. runs out by itself
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(2));
    pump(150);
    const bool wasRed = channel->ownRestriction().kind == Kind::TimedOut &&
                        edit->placeholderText().startsWith(timedOutPrefix);
    pump(3200);
    check("a timeout that runs out turns the input green",
          wasRed && channel->ownRestriction().kind == Kind::Lifted &&
              edit->placeholderText() == endedText && !edit->isReadOnly());
    channel->acknowledgeOwnRestrictionLifted();

    // 5. PubSub timeout with only expires_in_ms, then ban and unban
    channel->handleOwnModerationPubSub(QJsonObject{
        {"action", "timeout"}, {"channel_id", "1"}, {"expires_in_ms", 600000}});
    pump(100);
    check("PubSub timeout starts the countdown",
          channel->ownRestriction().kind == Kind::TimedOut &&
              edit->placeholderText().startsWith(timedOutPrefix) &&
              (edit->placeholderText().contains(QStringLiteral("10:00")) ||
               edit->placeholderText().contains(QStringLiteral("9:59"))));
    channel->handleOwnModerationPubSub(
        QJsonObject{{"action", "ban"}, {"channel_id", "1"}});
    pump(100);
    check("ban: the ban text",
          channel->ownRestriction().kind == Kind::Banned &&
              edit->placeholderText() ==
                  uiText("You are banned in this channel"));
    shot(QStringLiteral("-banned"));
    channel->handleOwnModerationPubSub(
        QJsonObject{{"action", "unban"}, {"channel_id", "1"}});
    pump(100);
    check("unban: green with the unban text",
          channel->ownRestriction().liftedBan &&
              edit->placeholderText() == unbannedText && !edit->isReadOnly());
    channel->acknowledgeOwnRestrictionLifted();

    // 6. each account has its own state
    const QString other = QStringLiteral("4242");
    channel->setOwnBanned(other);
    pump(100);
    check("a ban of another own account leaves this account's input alone",
          channel->ownRestriction().kind == Kind::None &&
              channel->ownRestrictionOf(other).kind == Kind::Banned &&
              !edit->placeholderText().startsWith(
                  uiText("You are banned in this channel")));

    // 7. "X unbanned Y" in chat lifts Y, even when Y is not signed in
    {
        MessageBuilder builder;
        builder->flags.set(MessageFlag::ModerationAction,
                           MessageFlag::Untimeout);
        builder->timeoutUser = QStringLiteral("other_account");
        builder->timeoutUserID = other;
        builder.emplace<TextElement>(QStringLiteral("someone unbanned other"),
                                     MessageElementFlag::Text);
        // Channel::addMessage needs app services this fixture lacks; the
        // hook listens to the signal addMessage raises for each live message.
        MessagePtr message = builder.release();
        channel->messageAppended.invoke(message, std::nullopt);
    }
    pump(100);
    check("an unban message in chat lifts that account",
          channel->ownRestrictionOf(other).kind == Kind::Lifted &&
              channel->ownRestrictionOf(other).liftedBan);

    // 8. a send that Twitch accepts (no "timed out" notice) ends it
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(300));
    pump(100);
    channel->noteSendWhileRestricted();
    pump(5600);
    check("a message accepted by Twitch ends a missed timeout",
          channel->ownRestriction().kind == Kind::Lifted);
    channel->acknowledgeOwnRestrictionLifted();
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(300));
    pump(100);
    channel->noteSendWhileRestricted();
    pump(1000);
    // Twitch answers "You are timed out for N more seconds."
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(298));
    pump(4600);
    check("a rejected message keeps the timeout",
          channel->ownRestriction().kind == Kind::TimedOut);
    channel->liftOwnRestriction();
    channel->acknowledgeOwnRestrictionLifted();
    pump(100);

    // 9. setting off: the input stays as usual
    getSettings()->showOwnTimeoutInInput = false;
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(60));
    pump(100);
    check("with the setting off the input stays usable",
          !edit->isReadOnly() &&
              !edit->placeholderText().startsWith(timedOutPrefix));
    getSettings()->showOwnTimeoutInInput = true;
    channel->liftOwnRestriction();
    channel->acknowledgeOwnRestrictionLifted();
    pump(100);

    // 10. slow mode: a countdown at the end of the input, a queue for early
    // messages.
    auto badge = [&]() -> QString {
        for (auto *label : edit->findChildren<QLabel *>())
        {
            if (!label->isHidden())
            {
                return label->text();
            }
        }
        return {};
    };
    QStringList sent;
    // A sent message "shows up in the chat" like Twitch's echo would, which
    // starts the next wait; echo=false: it never shows up (not delivered).
    bool echo = true;
    channel->slowQueueSenderForTesting = [&sent, &echo, &channel](
                                             const QString &text,
                                             const QString &) {
        sent.append(text);
        if (echo)
        {
            channel->noteOwnMessageSent();
        }
    };
    auto sends = [&] {
        return int(sent.size());
    };
    const auto slowPrefix = before(uiText("Slow mode · %1"));
    TwitchChannelTestAccess::setSlowMode(*channel, 3);
    channel->setSendWait(0);
    pump(50);
    check("no wait: no countdown and nothing to queue",
          badge().isEmpty() && !channel->queueWhileSlow(QStringLiteral("hi")));
    channel->noteOwnMessageSent();
    pump(100);
    check("after a message: the countdown is in the input, which stays usable",
          badge().startsWith(slowPrefix) &&
              badge().contains(uiText("%1 s").arg(3)) && !edit->isReadOnly());
    shot(QStringLiteral("-slow"));
    // Long text wraps earlier next to the badge: the box grows, nothing is cut.
    // (A standalone box of the same kind: typing into the split's own input
    // needs app services this fixture lacks.)
    {
        ResizingTextEdit box;
        box.setFont(edit->font());
        box.setVerticalPadding(0.15);
        box.setSideBadge(uiText("Slow mode · %1").arg(uiText("%1 s").arg(1)));
        box.setPlainText(QString(QStringLiteral("ХЪЗАЫФХ ")).repeated(30));
        const int width = 560;
        box.resize(width, static_cast<QWidget &>(box).heightForWidth(width));
        box.show();
        pump(150);
        check("a long message next to the countdown is not cut off",
              box.document()->size().height() <= box.viewport()->height() + 1);
        box.grab().save(screenshotBase + "-slow-long.png", "PNG");
    }
    const auto before0 = sends();
    check("a message during the wait is queued",
          channel->queueWhileSlow(QStringLiteral("first")) &&
              channel->queueWhileSlow(QStringLiteral("second")) &&
              channel->slowQueueSize() == 2 && sends() == before0);
    pump(100);
    check("the countdown shows the queue",
          badge().contains(QStringLiteral("2")) &&
              badge() != uiText("Slow mode · %1").arg(uiText("%1 s").arg(3)));
    shot(QStringLiteral("-slow-queued"));
    {
        QLabel *badgeLabel = nullptr;
        for (auto *label : edit->findChildren<QLabel *>())
        {
            if (!label->isHidden())
            {
                badgeLabel = label;
            }
        }
        check("with a queue the countdown can be clicked to cancel",
              badgeLabel != nullptr && !badgeLabel->toolTip().isEmpty() &&
                  !badgeLabel->testAttribute(Qt::WA_TransparentForMouseEvents));
        check("cancelling gives back the newest queued message",
              channel->takeLastQueued() == QStringLiteral("second") &&
                  channel->slowQueueSize() == 1);
        channel->queueWhileSlow(QStringLiteral("second"));
    }
    QElapsedTimer clock;
    clock.start();
    pump(3600);
    const auto firstAt = clock.elapsed();
    check("the first queued message goes by itself when the wait ends",
          sends() == before0 + 1 && channel->slowQueueSize() == 1);
    check("…and starts the next wait", channel->sendWaitSeconds() >= 2);
    pump(3400);
    check("the second goes one wait later, in order",
          sends() == before0 + 2 && channel->slowQueueSize() == 0 &&
              clock.elapsed() - firstAt >= 2900 &&
              sent == QStringList{QStringLiteral("first"),
                                  QStringLiteral("second")});
    pump(3300);
    check("then the countdown disappears", badge().isEmpty());

    // A message that does not go through starts no countdown, and a queued
    // one behind it is not stuck.
    echo = false;
    channel->noteOwnMessageSent();
    pump(50);
    channel->queueWhileSlow(QStringLiteral("third"));
    channel->queueWhileSlow(QStringLiteral("fourth"));
    pump(3400);
    check("a message that never shows up in the chat starts no countdown",
          sent.size() >= 3 && sent[2] == QStringLiteral("third") &&
              channel->sendWaitSeconds() == 0);
    pump(2700);
    check("…and the next queued one still goes",
          sent.size() == 4 && sent[3] == QStringLiteral("fourth") &&
              channel->slowQueueSize() == 0);
    pump(300);
    check("nothing left on the countdown", badge().isEmpty());
    echo = true;
    channel->setOwnTimedOut(QDateTime::currentDateTimeUtc().addSecs(60));
    channel->setSendWait(60);
    pump(50);
    check("a timeout is not a slow mode wait: nothing queued, no slow badge",
          !channel->queueWhileSlow(QStringLiteral("x")) &&
              !badge().startsWith(slowPrefix));
    channel->liftOwnRestriction();
    channel->acknowledgeOwnRestrictionLifted();
    channel->setSendWait(0);
    TwitchChannelTestAccess::setSlowMode(*channel, 0);
    channel->slowQueueSenderForTesting = nullptr;
    pump(100);

    // 11. a live channel's name in a split header: dot, name, muted details
    // (no live stream in this fixture, so the label is shown on its own).
    {
        Label live(QStringLiteral("kopsteep"), FontStyle::UiMediumBold);
        live.setCentered(true);
        QPalette palette;
        palette.setColor(QPalette::Window, QColor("#20202a"));
        palette.setColor(QPalette::WindowText, QColor("#e8e8ee"));
        live.setPalette(palette);
        live.setAutoFillBackground(true);
        live.setLeadingDot(QColor(0xff, 0x4f, 0x5e));
        live.setDetails(QString::fromUtf8(
            "2 ч 14 мин · 22 326 зрителей · IRL"));
        live.resize(620, 30);
        live.show();
        pump(50);
        check("a live header shows the details next to the name",
              static_cast<QWidget &>(live).sizeHint().width() > 200);
        if (!screenshotBase.isEmpty())
        {
            live.grab().save(screenshotBase + "-live-header.png", "PNG");
            live.resize(260, 30);
            pump(30);
            live.grab().save(screenshotBase + "-live-header-narrow.png", "PNG");
        }
    }

    // 13. an outgoing raid above the input box, ended by Twitch's own word
    {
        TwitchChannel::RaidEvent raid;
        raid.id = QStringLiteral("raid-1");
        raid.targetId = QStringLiteral("123");
        raid.targetLogin = QStringLiteral("dangerlyoha");
        raid.targetDisplayName = QStringLiteral("dangerlyoha");
        raid.viewerCount = 13748;
        raid.forceRaidNowSeconds = 90;
        raid.receivedAt = QDateTime::currentDateTimeUtc();
        channel->setActiveRaid(raid);
        pump(300);
        QLabel *raidLabel = nullptr;
        QLabel *raidTime = nullptr;
        for (auto *label : split.findChildren<QLabel *>())
        {
            if (label->objectName() == u"raidStatusLabel")
            {
                raidLabel = label;
            }
            if (label->objectName() == u"raidStatusTime")
            {
                raidTime = label;
            }
        }
        check("the raid bar shows the target and viewers",
              raidLabel != nullptr && raidLabel->isVisible() &&
                  raidLabel->text().contains(u"dangerlyoha") &&
                  raidLabel->text().contains(u"748") &&
                  raidTime != nullptr && raidTime->text().contains(u':'));
        std::fprintf(stderr, "raid text: %s | %s\n",
                     raidLabel ? raidLabel->text().toUtf8().constData() : "",
                     raidTime ? raidTime->text().toUtf8().constData() : "");
        if (!screenshotBase.isEmpty())
        {
            split.getInput().grab().save(screenshotBase + "-raid.png", "PNG");
        }
        QString wentTo;
        pajlada::Signals::ScopedConnection went =
            channel->raidWent.connect([&](const QString &target) {
                wentTo = target;
            });
        channel->handleRaidUpdate(QJsonObject{
            {"type", "raid_go_v2"},
            {"topic", QStringLiteral("raid.%1").arg(channel->roomId())},
            {"raid", QJsonObject{{"id", "raid-1"}}},
        });
        pump(300);
        check("the raid bar goes when Twitch says the raid went",
              !channel->accessRaid()->has_value() &&
                  (raidLabel == nullptr || !raidLabel->isVisible()));
        check("the chat follows the raid to its target", wentTo == u"dangerlyoha");
    }

    // 14. a 7TV paint with a glow: no box where the glow used to be cut off
    {
        QGradientStops stops{{0.0, QColor("#ffcce0")}, {0.5, QColor("#bfe0ff")},
                             {1.0, QColor("#ffe6b0")}};
        LinearGradientPaint paint(QStringLiteral("Pastel"),
                                  QStringLiteral("test"), std::nullopt, stops,
                                  false, 90,
                                  {PaintDropShadow(0, 0, 5, QColor(191, 185,
                                                                   255, 191))});
        QFont font = QApplication::font();
        font.setPixelSize(15);
        font.setBold(true);
        const QFontMetricsF metrics(font);
        const QSizeF size(metrics.horizontalAdvance(QStringLiteral("@dapta4ke")),
                          metrics.height());
        qreal margin = 0;
        const auto pixmap = paint.getPixmap(QStringLiteral("@dapta4ke"), font,
                                            QColor("#c0c0ff"), size, 1, 1,
                                            false, &margin);
        const auto image = pixmap.toImage();
        // the outermost ring of the picture is clear: nothing is cut off
        int edge = 0;
        for (int x = 0; x < image.width(); ++x)
        {
            edge = std::max({edge, qAlpha(image.pixel(x, 0)),
                             qAlpha(image.pixel(x, image.height() - 1))});
        }
        for (int y = 0; y < image.height(); ++y)
        {
            edge = std::max({edge, qAlpha(image.pixel(0, y)),
                             qAlpha(image.pixel(image.width() - 1, y))});
        }
        check("a glowing 7TV name fades out inside its picture (no box)",
              margin > 0 && edge < 40);
        if (!screenshotBase.isEmpty())
        {
            QImage shot(image.size() + QSize(20, 20), QImage::Format_ARGB32);
            shot.fill(QColor("#18181b"));
            QPainter painter(&shot);
            painter.drawPixmap(10, 10, pixmap);
            painter.end();
            shot.save(screenshotBase + "-paint-glow.png", "PNG");
        }
    }

    // 15. punishments: a soft rounded box around the text — red ban, yellow
    //     timeout, grey deletion
    {
        auto punished = std::make_shared<TwitchChannel>(QStringLiteral("box_room"));
        split.setChannel(
            IndirectChannel(std::static_pointer_cast<Channel>(punished)));
        split.resize(560, 220);
        const auto now = QDateTime::currentDateTime();
        punished->addMessage(
            MessageBuilder(timeoutMessage, QStringLiteral("mainkrafter2000"),
                           QString(), false, now)
                .release(),
            MessageContext::Repost);
        punished->addMessage(
            MessageBuilder(timeoutMessage, QStringLiteral("furay_hd"),
                           QStringLiteral("600"), false, now)
                .release(),
            MessageContext::Repost);
        auto original = std::make_shared<Message>();
        original->loginName = QStringLiteral("vihkiins");
        original->displayName = QStringLiteral("vihkiins");
        original->messageText = QStringLiteral("DinoDance DinoDance PogChamp");
        original->id = QStringLiteral("deleted-id");
        punished->addMessage(MessageBuilder::makeDeletionMessageFromIRC(original),
                             MessageContext::Repost);
        pump(300);
        const auto image = split.getChannelView().grab().toImage();
        int red = 0;
        int yellow = 0;
        int grey = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                const QColor c = image.pixelColor(x, y);
                red += c.red() > c.green() + 30 && c.red() > c.blue() + 20 &&
                       c.green() < c.blue() + 25;
                yellow += c.red() > c.blue() + 30 && c.green() > c.blue() + 25;
                grey += std::abs(c.red() - c.green()) < 6 &&
                        c.blue() - c.red() >= 4 && c.blue() - c.red() < 14 &&
                        c.red() > 55 && c.red() < 90;
            }
        }
        std::fprintf(stderr, "punishment boxes: red=%d yellow=%d grey=%d\n", red,
                     yellow, grey);
        check("a ban gets a red box, a timeout a yellow one",
              red > 40 && yellow > 40);
        if (!screenshotBase.isEmpty())
        {
            image.save(screenshotBase + "-punishments.png", "PNG");
        }

        // a jump with room kept on top (the "while you were away" card)
        // leaves the message below that room, not at the very top
        std::vector<MessagePtr> filler;
        for (int i = 0; i < 120; ++i)
        {
            filler.push_back(makeSystemMessage(QStringLiteral("line %1").arg(i)));
            punished->addMessage(filler.back(), MessageContext::Repost);
        }
        pump(300);
        auto &view = split.getChannelView();
        const bool found = view.scrollToMessage(filler[60], 100);
        pump(400);
        const auto value =
            view.getScrollBar().getDesiredValue() - view.getScrollBar().getMinimum();
        const auto index = qreal(3 + 60);  // three punishments come first
        std::fprintf(stderr, "jump with room: value=%.2f target=%.0f\n", value,
                     index);
        check("a jump can keep room above the message",
              found && value < index - 1 && value > index - 12);
    }

    // 16. pictures: under image links in the chat, and the card above the
    //     input after a paste, with its delete button
    {
        namespace ip = imagepreview;
        const auto sites =
            ip::sites(QStringLiteral("https://www.kappa.lol/, segs.lol"));
        check("image sites are read from the setting",
              sites == QStringList{QStringLiteral("kappa.lol"),
                                   QStringLiteral("segs.lol")});
        check("a kappa.lol screenshot link is an image link",
              ip::isImageLink(QStringLiteral("https://kappa.lol/vCECFg"),
                              sites) &&
                  ip::isImageLink(QStringLiteral("kappa.lol/vCECFg"), sites) &&
                  ip::isImageLink(QStringLiteral("https://segs.lol/AbC12"),
                                  sites));
        check("kappa.lol's own pages are not",
              !ip::isImageLink(QStringLiteral("https://kappa.lol/"), sites) &&
                  !ip::isImageLink(QStringLiteral("https://kappa.lol/delete?ab"),
                                   sites) &&
                  !ip::isImageLink(QStringLiteral("https://kappa.lol/uploaders"),
                                   sites));
        const auto more =
            ip::sites(QStringLiteral("kappa.lol segs.lol eblo.id kappalol.fun"));
        check("eblo.id and kappalol.fun posts and albums are image links",
              ip::isImageLink(QStringLiteral("https://eblo.id/hH0WwpT"), more) &&
                  ip::isImageLink(QStringLiteral("https://eblo.id/AqUEzee"),
                                  more) &&
                  ip::isImageLink(QStringLiteral("https://kappalol.fun/Y8zpTA"),
                                  more) &&
                  ip::isImageLink(
                      QStringLiteral("https://kappalol.fun/group/Ab12Cd"), more));
        check("their own pages are not",
              !ip::isImageLink(QStringLiteral("https://eblo.id/videos"), more) &&
                  !ip::isImageLink(QStringLiteral("https://eblo.id/new"), more) &&
                  !ip::isImageLink(QStringLiteral("https://kappalol.fun/board"),
                                   more) &&
                  !ip::isImageLink(
                      QStringLiteral("https://kappalol.fun/profile/twenixz"),
                      more) &&
                  !ip::isImageLink(QStringLiteral("https://kappalol.fun/text/Ab12"),
                                   more));
        check("an image file on any site is one",
              ip::isImageLink(QStringLiteral("https://example.com/a/b.PNG"),
                              sites) &&
                  ip::isImageLink(
                      QStringLiteral("https://cdn.example.org/x.webp?v=2"),
                      sites));
        check("other pages and local addresses are not",
              !ip::isImageLink(QStringLiteral("https://example.com/page"),
                               sites) &&
                  !ip::isImageLink(QStringLiteral("http://192.168.1.5/a.png"),
                                   sites) &&
                  !ip::isImageLink(QStringLiteral("http://localhost/a.png"),
                                   sites));
        check("the same link written differently is one key",
              ip::key(QStringLiteral("http://www.kappa.lol/abc/")) ==
                  ip::key(QStringLiteral("https://kappa.lol/abc")));

        // a picture under the message
        QPixmap shot(1920, 1080);
        shot.fill(QColor(47, 168, 79));
        {
            QPainter painter(&shot);
            painter.fillRect(QRect(0, 0, 960, 1080), QColor(40, 90, 200));
        }
        const auto link = QStringLiteral("https://kappa.lol/demo01");
        ip::rememberUpload(link, Image::fromResourcePixmap(shot));
        auto pictures =
            std::make_shared<TwitchChannel>(QStringLiteral("pictures_room"));
        split.setChannel(
            IndirectChannel(std::static_pointer_cast<Channel>(pictures)));
        split.resize(560, 420);
        MessageBuilder builder;
        builder.emplace<TextElement>(QStringLiteral("mist1x: смотри"),
                                     MessageElementFlag::Text);
        builder.addLink(*linkparser::parse(link), link);
        auto message = builder.release();
        const bool hasPreview =
            std::ranges::any_of(message->elements, [](const auto &element) {
                return dynamic_cast<ImagePreviewElement *>(element.get()) !=
                       nullptr;
            });
        QTemporaryDir uploadLog;
        const auto originalLogPath = getSettings()->logPath.getValue();
        const bool originallyEnabled = getSettings()->imageUploaderEnabled;
        getSettings()->imageUploaderEnabled = true;
        getSettings()->logPath = uploadLog.path();
        QFile file(uploadLog.filePath("ImageUploader.json"));
        const QJsonArray entries{QJsonObject{{"imageLink", link},
                                             {"deletionLink", "https://example.invalid/delete?fixture"}}};
        if (uploadLog.isValid() && file.open(QIODevice::WriteOnly))
        {
            file.write(QJsonDocument(entries).toJson());
            file.close();
        }
        auto &accounts = getApp()->getAccounts()->twitch.accounts;
        const auto accountIndex = accounts.append(std::make_shared<TwitchAccount>(
            QStringLiteral("image_owner"), QString{}, QString{}, QStringLiteral("314")));
        auto *images = getApp()->getImageUploader();
        message->loginName = QStringLiteral("another_viewer");
        message->userID = QStringLiteral("314");
        check("reposting another account's uploaded picture has no delete action",
              images->deletableImages(*message).isEmpty());
        message->loginName = QStringLiteral("Image_Owner");
        message->userID = QStringLiteral("900");
        check("a mismatched Twitch UID has no delete action",
              images->deletableImages(*message).isEmpty());
        message->userID = QStringLiteral("314");
        check("a saved author's posted link matches the local upload",
              images->deletableImages(*message) == QStringList{link});
        pictures->addMessage(message, MessageContext::Repost);
        pump(400);
        const auto image = split.getChannelView().grab().toImage();
        int green = 0;
        int blue = 0;
        QPoint picturePixel;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                const QColor c = image.pixelColor(x, y);
                if (c == QColor(47, 168, 79) && picturePixel.isNull())
                {
                    picturePixel = QPoint(x, y);
                }
                green += std::abs(c.red() - 47) < 6 &&
                         std::abs(c.green() - 168) < 6 &&
                         std::abs(c.blue() - 79) < 6;
                blue += std::abs(c.red() - 40) < 6 &&
                        std::abs(c.green() - 90) < 6 &&
                        std::abs(c.blue() - 200) < 6;
            }
        }
        std::fprintf(stderr, "picture under the link: green=%d blue=%d\n",
                     green, blue);
        check("a screenshot link gets its picture under the message",
              hasPreview && green > 3000 && blue > 3000);
        {
            auto &view = split.getChannelView();
            const bool previous = getSettings()->linkInfoTooltip;
            getSettings()->linkInfoTooltip = true;
            const auto at = picturePixel / image.devicePixelRatio() + QPoint(0, 3);
            QMouseEvent move(QEvent::MouseMove, at, view.mapToGlobal(at),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&view, &move);
            pump(30);
            const auto dpr = view.devicePixelRatioF();
            const auto wanted = shot.size().scaled(
                QSize(640, 420).boundedTo(view.screen()->availableSize() * 0.8) * dpr,
                Qt::KeepAspectRatio);
            bool sharp = false;
            std::fprintf(stderr, "screenshot hover at %d,%d on DPR %.2f: expected %dx%d pixels\n",
                         at.x(), at.y(), dpr, wanted.width(), wanted.height());
            for (auto *window : QApplication::topLevelWidgets())
            {
                auto *tooltip = qobject_cast<TooltipWidget *>(window);
                if (!tooltip || !tooltip->isVisible())
                    continue;
                for (auto *label : tooltip->findChildren<QLabel *>())
                {
                    const auto displayed = label->pixmap();
                    if (!label->isVisible() || displayed.isNull())
                        continue;
                    const auto actual = label->grab().toImage();
                    std::fprintf(stderr, "hover label pixmap %dx%d at %.2f, rendered %dx%d\n",
                                 displayed.width(), displayed.height(), displayed.devicePixelRatio(),
                                 actual.width(), actual.height());
                    const auto quarter = actual.pixelColor(actual.width() / 4, actual.height() / 2);
                    const auto threeQuarters = actual.pixelColor(actual.width() * 3 / 4, actual.height() / 2);
                    // At fractional DPR the integer QLabel geometry and its QIcon
                    // can each round a physical edge by one pixel.
                    sharp = std::abs(actual.width() - wanted.width()) <= 2 &&
                            std::abs(actual.height() - wanted.height()) <= 2 &&
                            std::abs(displayed.width() - wanted.width()) <= 2 &&
                            std::abs(displayed.height() - wanted.height()) <= 2 &&
                            qFuzzyCompare(actual.devicePixelRatio(), dpr) &&
                            quarter == QColor(40, 90, 200) && threeQuarters == QColor(47, 168, 79);
                    if (sharp)
                        break;
                }
                if (sharp)
                {
                    if (!screenshotBase.isEmpty())
                        tooltip->grab().save(screenshotBase + "-expanded-picture.png", "PNG");
                    break;
                }
            }
            check("hover on the rendered screenshot expands it at native display density", sharp);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&view, &leave);
            getSettings()->linkInfoTooltip = previous;
        }
        if (!screenshotBase.isEmpty())
        {
            image.save(screenshotBase + "-picture.png", "PNG");
        }

        const auto &layouts = split.getChannelView().getMessagesSnapshot();
        QPoint deleteCenter;
        bool gapIsClear = false;
        if (!layouts.empty())
        {
            const auto &layout = layouts.back();
            for (int y = 0; y < layout->getHeight() && deleteCenter.isNull(); y += 2)
            {
                for (int x = layout->getWidth() / 2; x < layout->getWidth(); x += 2)
                {
                    auto *element = layout->getElementAt(QPointF(x, y));
                    if (element && element->getLink().type == Link::DeleteUploadedImage)
                    {
                        deleteCenter = element->getRect().center().toPoint();
                        auto *beside = layout->getElementAt(deleteCenter - QPoint(25, 0));
                        gapIsClear = !beside || beside->getLink().type != Link::DeleteUploadedImage;
                        break;
                    }
                }
            }
        }
        check("the screenshot delete button has its own small hit target on the right",
              !deleteCenter.isNull() && gapIsClear);
        if (!deleteCenter.isNull())
        {
            auto &view = split.getChannelView();
            const QPointF global = view.mapToGlobal(deleteCenter);
            QMouseEvent press(QEvent::MouseButtonPress, deleteCenter, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, deleteCenter, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&view, &press);
            QApplication::sendEvent(&view, &release);
            auto *menu = view.findChild<QMenu *>("deleteUploadedPictureMenu");
            check("click opens the deletion action for the public picture host",
                  menu && menu->actions().size() == 1 &&
                      menu->actions().front()->text().contains("kappa.lol") &&
                      !menu->actions().front()->text().contains("fixture"));
            if (menu)
            {
                menu->close();
            }
        }
        accounts.removeAt(accountIndex);
        getSettings()->logPath = originalLogPath;
        getSettings()->imageUploaderEnabled = originallyEnabled;

        // the card above the input
        auto *uploader = getApp()->getImageUploader();
        auto *card = dynamic_cast<UploadPreview *>(
            split.getChannelView().findChild<QFrame *>(
                QStringLiteral("uploadPreview")));
        check("the chat has a card for pasted pictures",
              uploader != nullptr && card != nullptr && edit != nullptr);
        if (uploader != nullptr && card != nullptr && edit != nullptr)
        {
            QByteArray png;
            {
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                shot.save(&buffer, "PNG");
            }
            // (typing into this fixture's input is not possible: its text
            // handlers need services the fixture has not got)
            uploader->updated.invoke({
                .kind = ImageUploadUpdate::Kind::Started,
                .textEdit = edit,
                .imageData = png,
            });
            pump(100);
            const bool uploading =
                card->isVisible() && !card->deleteButton()->isVisible();
            const auto uploaded = QStringLiteral("https://kappa.lol/up42");
            uploader->updated.invoke({
                .kind = ImageUploadUpdate::Kind::Finished,
                .textEdit = edit,
                .link = uploaded,
                .deletionLink = QStringLiteral("https://kappa.lol/delete?K"),
            });
            pump(150);
            check("while uploading the card shows the picture, no delete yet",
                  uploading);
            check("after the upload it has the delete button",
                  card->isVisible() && card->deleteButton()->isVisible() &&
                      card->link() == uploaded);
            const auto cached = ip::cachedUpload(uploaded);
            check("an uploaded 1080p screenshot retains a 1280x720 source for chat and hover",
                  cached && cached->size() == QSizeF(1280, 720));
            const auto geometry = card->geometry();
            const auto viewHeight = split.getChannelView().height();
            std::fprintf(stderr, "upload card: %d,%d %dx%d in a view %d high\n",
                         geometry.x(), geometry.y(), geometry.width(),
                         geometry.height(), viewHeight);
            check("the card sits at the bottom of the chat, over the input",
                  geometry.bottom() <= viewHeight &&
                      geometry.bottom() > viewHeight - 24 &&
                      geometry.height() >= 60);
            if (!screenshotBase.isEmpty())
            {
                split.grab().save(screenshotBase + "-upload-card.png", "PNG");
            }
            card->hide();

            // the same card on a bare input: it goes once the link is gone
            QWidget view;
            view.resize(500, 300);
            ResizingTextEdit box;
            UploadPreview bare(&view, &box, [](const QString &) {});
            view.show();
            box.setPlainText(QStringLiteral("look ") + uploaded +
                             QStringLiteral(" "));
            bare.apply({
                .kind = ImageUploadUpdate::Kind::Finished,
                .textEdit = &box,
                .link = uploaded,
            });
            pump(50);
            const bool stays = bare.isVisible();
            box.setPlainText(QStringLiteral("look again"));
            pump(50);
            check("the card goes away once the link leaves the input",
                  stays && !bare.isVisible());
            view.hide();
        }
    }

    {
        auto status = std::make_shared<TwitchChannel>("connection_fixture");
        split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(status)));
        split.resize(800, 330);
        const auto add = [&](const QString &text, MessageFlags flags) {
            MessageBuilder builder(systemMessage, text);
            builder->flags.set(flags);
            status->addMessage(builder.release(), MessageContext::Repost);
        };
        add("connected", MessageFlag::ConnectedMessage);
        add("joined channel", MessageFlag::None);
        add(QStringLiteral("Связь с чатом нестабильна: 3 обрывов с 02:35, сейчас нет связи. Переподключаемся, /reconnect — сразу."),
            MessageFlag::DisconnectedMessage);
        add(QStringLiteral("Связь с чатом нестабильна: 3 обрывов с 02:35, восстановлена в 02:45"),
            MessageFlag::ConnectedMessage);
        add("Twitch Servers requested us to reconnect, reconnecting",
            MessageFlag::None);
        MessageBuilder ordinary;
        ordinary->loginName = "viewer";
        ordinary->displayName = "Viewer";
        ordinary->messageText = "joined channel";
        ordinary.emplace<TimestampElement>();
        ordinary.emplace<TextElement>("Viewer:", MessageElementFlag::Username);
        ordinary.emplace<TextElement>("joined channel", MessageElementFlag::Text);
        status->addMessage(ordinary.release(), MessageContext::Repost);
        pump(260);
        const auto image = split.getChannelView().grab().toImage();
        int blue = 0, amber = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                const auto c = image.pixelColor(x, y);
                blue += c.blue() > c.red() + 14 && c.blue() > c.green() + 10 &&
                        c.red() > 55 && c.green() > 55;
                amber += c.red() > c.blue() + 25 && c.green() > c.blue() + 10 &&
                         c.red() > 65;
            }
        }
        check("connection and interruption notices use distinct subdued accents",
              blue > 30 && amber > 30);
        const auto rows = status->getMessageSnapshot();
        check("connection notice styling preserves original text and normal chat",
              rows.size() == 6 && rows[0]->messageText == "connected" &&
              rows[1]->messageText == "joined channel" &&
              rows[5]->messageText == "joined channel" &&
              !rows[5]->flags.has(MessageFlag::System));
        if (!screenshotBase.isEmpty())
            image.save(screenshotBase + "-connection.png", "PNG");
    }

    // 17. whispers as cards: header, the sender's name as written, global
    //     badges only, a reply button; your own ones «to <Name>»
    {
        auto whispers = std::make_shared<Channel>(
            QStringLiteral("/whispers"), Channel::Type::TwitchWhispers);
        MessageBuilder theirs;
        theirs.emplace<TimestampElement>();
        theirs.emplace<TextElement>(QStringLiteral("OneShkof:"),
                                    MessageElementFlag::Username,
                                    QColor(QStringLiteral("#FF4D4D")),
                                    FontStyle::ChatMediumBold)
            ->setLink({Link::UserWhisper, QStringLiteral("OneShkof")});
        theirs.emplace<TextElement>(QStringLiteral("о, да, работает"),
                                    MessageElementFlag::Text);
        auto incoming = theirs.release();
        incoming->loginName = QStringLiteral("oneshkof");
        MessageBuilder::makeWhisperCard(*incoming, uiText("Whisper"),
                                        incoming->loginName);
        const auto &els = incoming->elements;
        const bool cardOk =
            !els.empty() &&
            dynamic_cast<NoticeIconElement *>(els.front().get()) &&
            dynamic_cast<WhisperReplyElement *>(els.back().get()) &&
            incoming->flags.has(MessageFlag::Whisper);
        whispers->addMessage(incoming, MessageContext::Repost);
        check("an incoming whisper is a card with a reply button", cardOk);

        MessageBuilder mine;
        mine.emplace<TimestampElement>();
        mine.emplace<TextElement>(QStringLiteral("Mist1X_x:"),
                                  MessageElementFlag::Username,
                                  MessageColor::Text, FontStyle::ChatMediumBold)
            ->setLink({Link::UserInfo, QStringLiteral("mist1x_x")});
        mine.emplace<TextElement>(QStringLiteral("ну шо работает?"),
                                  MessageElementFlag::Text);
        auto own = mine.release();
        MessageBuilder::makeWhisperCard(
            *own, uiText("Whisper to %1").arg(QStringLiteral("OneShkof")), {});
        whispers->addMessage(own, MessageContext::Repost);

        split.setChannel(IndirectChannel(whispers));
        split.resize(560, 260);
        pump(300);
        if (!screenshotBase.isEmpty())
        {
            split.getChannelView().grab().save(screenshotBase + "-whispers.png",
                                               "PNG");
        }
        check("quick whisper opens a distinct local conversation",
              split.selectWhisperPeer("OneShkof") &&
                  split.whisperPeer() == "oneshkof");
        split.getInput().setInputText("unsent local draft");
        check("quick whisper never changes recipient of an unsent draft",
              !split.selectWhisperPeer("AnotherUser") &&
                  split.whisperPeer() == "oneshkof" &&
                  split.getInput().getInputText() == "unsent local draft");
        split.getInput().setInputText({});
        check("quick whisper can select a new conversation with empty input",
              split.selectWhisperPeer("AnotherUser") &&
                  split.whisperPeer() == "anotheruser");
        auto *picker = split.findChild<QPushButton *>("WhisperConversations");
        auto *panel = split.findChild<QFrame *>("WhisperConversationPanel");
        auto *list = split.findChild<QListWidget *>("WhisperConversationList");
        check("whisper conversation picker is present", picker && panel && list);
        if (picker && panel && list)
        {
            const QRect chatBefore = split.getChannelView().geometry();
            picker->click();
            const int firstHeight = panel->height();
            pump(200);
            check("conversation list reveals below its anchor without moving chat",
                  !panel->isHidden() && panel->height() > firstHeight &&
                      panel->y() >= picker->geometry().bottom() + 1 &&
                      split.getChannelView().geometry() == chatBefore &&
                      list->currentItem() && list->currentItem()->text() == "AnotherUser");
            if (!screenshotBase.isEmpty())
                split.grab().save(screenshotBase + "-whisper-picker.png", "PNG");
            const auto panelBefore = panel->geometry();
            const auto rowsBefore = list->count();
            MessageBuilder arrived;
            arrived->whisperPeer = "newpeer";
            arrived->displayName = "NewPeer";
            arrived->flags.set(MessageFlag::Whisper);
            arrived.emplace<TextElement>("test", MessageElementFlag::Text);
            const auto anotherPeer = arrived.release();
            whispers->addMessage(anotherPeer, MessageContext::Repost);
            pump(25);
            check("arriving conversation keeps the open list and current row stationary",
                  panel->isVisible() && panel->geometry() == panelBefore &&
                      list->count() == rowsBefore + 1 &&
                      list->item(rowsBefore)->text() == "NewPeer" &&
                      list->currentItem() && list->currentItem()->text() == "AnotherUser");
            const auto initialSize = split.size();
            split.resize(245, 175);
            pump(40);
            check("conversation list fits a resized split and never opens upwards",
                  panel->isVisible() && panel->y() == picker->y() + picker->height() &&
                      split.rect().contains(panel->geometry()));
            split.resize(initialSize);
            pump(40);
            QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
            QApplication::sendEvent(list, &home);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(list, &enter);
            pump(200);
            check("conversation keyboard selection returns to all messages",
                  panel->isHidden() && split.whisperPeer().isEmpty() &&
                      picker->text() == uiText("All conversations") &&
                      split.getChannelView().geometry() == chatBefore);
            picker->click();
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(list, &escape);
            pump(150);
            check("Escape reverses an opening conversation list",
                  panel->isHidden() && split.whisperPeer().isEmpty());
            picker->click();
            whispers->clearMessages();
            pump(30);
            check("resetting whispers dismisses the list and clears old peers",
                  panel->isHidden() && list->count() == 1 && split.whisperPeer().isEmpty());
        }
    }

    // 18. emotes in the input are set taller than the text: the input grows
    //     with them instead of cutting them off
    {
        struct Tall : QSyntaxHighlighter {
            using QSyntaxHighlighter::QSyntaxHighlighter;
            void highlightBlock(const QString &text) override
            {
                auto font = this->document()->defaultFont();
                if (font.pixelSize() > 0)
                {
                    font.setPixelSize(font.pixelSize() * 2);
                }
                else
                {
                    font.setPointSizeF(font.pointSizeF() * 2);
                }
                QTextCharFormat format;
                format.setFont(font);
                this->setFormat(0, int(text.size()), format);
            }
        };
        ResizingTextEdit box;
        box.resize(300, 40);
        box.setPlainText(QStringLiteral("peepoHappy peepoHappy"));
        const int plain = box.sizeHint().height();
        Tall tall(box.document());
        tall.rehighlight();
        const int grown = box.sizeHint().height();
        std::fprintf(stderr, "input height: plain=%d with emotes=%d\n", plain,
                     grown);
        check("taller emotes make the input taller", grown > plain + 4);
    }

    // Selecting an inline emote must not expose the hidden name or repaint
    // an adjacent line. A very narrow input forces that emote to wrap.
    {
        const QString emoteName = QStringLiteral("SelectionWWWWWWWWWWWWWWWW");
        static const QPixmap emotePixmap = [] {
            QPixmap picture(56, 28);
            picture.fill(QColor(0, 255, 0));
            return picture;
        }();
        auto room = std::make_shared<TwitchChannel>(
            QStringLiteral("input_selection_fixture"));
        auto emotes = std::make_shared<EmoteMap>();
        emotes->emplace(EmoteName(emoteName),
                        std::make_shared<Emote>(Emote{
                            .name = EmoteName(emoteName),
                            .images = ImageSet{
                                Image::fromResourcePixmap(emotePixmap)},
                        }));
        room->setBttvEmotes(std::move(emotes));

        ResizingTextEdit box;
        box.setFont(edit != nullptr ? edit->font() : box.font());
        auto colors = box.palette();
        colors.setColor(QPalette::Base, QColor(16, 16, 22));
        colors.setColor(QPalette::Text, Qt::white);
        colors.setColor(QPalette::Highlight, QColor(156, 138, 204));
        colors.setColor(QPalette::HighlightedText, Qt::red);
        box.setPalette(colors);

        InputHighlighter highlighter(*getApp()->getSpellChecker(), nullptr);
        highlighter.setEmotePreviewEnabled(true);
        highlighter.setDocument(box.document());
        highlighter.setChannel(room);
        box.paintOverlay = [&](QPainter &painter) {
            highlighter.paintEmotes(painter, box);
        };
        const QString text = emoteName + QStringLiteral(" following text");
        box.setPlainText(text);
        box.show();
        box.setFocus();
        highlighter.rehighlight();
        pump(50);
        const auto spanFormats = box.document()->firstBlock().layout()->formats();
        const auto reserved = std::ranges::find_if(spanFormats, [&](const auto &range) {
            return range.start == 0 && range.length == emoteName.size() &&
                   range.format.foreground().color().alpha() == 0;
        });
        check("inline emote reserves width without negative letter spacing",
              highlighter.showsEmotes() && reserved != spanFormats.end() &&
                  reserved->format.fontLetterSpacing() >= 0);

        for (const int width : {260, 36})
        {
            box.resize(width, 150);
            QTextCursor cursor(box.document());
            cursor.setPosition(0);
            cursor.setPosition(int(emoteName.size()), QTextCursor::KeepAnchor);
            box.setTextCursor(cursor);
            pump(60);

            const auto block = box.document()->firstBlock();
            const auto *layout = block.layout();
            QRegion allowed;
            for (int i = 0; i < layout->lineCount(); ++i)
            {
                const auto line = layout->lineAt(i);
                const int start = line.textStart();
                const int end = std::min(start + line.textLength(),
                                         int(emoteName.size()));
                if (start >= end)
                {
                    break;
                }
                QTextCursor startCursor(block);
                startCursor.setPosition(block.position() + start);
                const auto caret = box.cursorRect(startCursor);
                const auto span = line.cursorToX(end) - line.cursorToX(start);
                const qreal dpr = box.viewport()->devicePixelRatioF();
                allowed += QRectF(caret.left() * dpr, caret.top() * dpr,
                                  span * dpr, caret.height() * dpr)
                               .toAlignedRect();
            }
            const auto image = box.viewport()->grab().toImage();
            bool hasEmote = false;
            bool outside = false;
            bool hiddenTextVisible = false;
            for (int y = 0; y < image.height(); ++y)
            {
                for (int x = 0; x < image.width(); ++x)
                {
                    const auto color = image.pixelColor(x, y);
                    if (color.green() > 220 && color.red() < 40 &&
                        color.blue() < 40)
                    {
                        hasEmote = true;
                        outside |= !allowed.contains(QPoint(x, y));
                    }
                    hiddenTextVisible |= color.red() > 220 &&
                                         color.green() < 40 && color.blue() < 40;
                }
            }
            check(QStringLiteral("selected inline emote stays inside its visual "
                                 "line at width %1").arg(width),
                  hasEmote && !outside && !hiddenTextVisible &&
                      box.toPlainText() == text &&
                      box.textCursor().selectedText() == emoteName &&
                      (width != 36 ||
                       layout->lineAt(0).textLength() < emoteName.size()));
        }
        box.hide();
    }

    // 19. "/w <name> " at the start of the input is a «DM to <name>» chip
    {
        ResizingTextEdit box;
        box.setFont(edit != nullptr ? edit->font() : box.font());
        box.resize(520, 36);
        InputHighlighter highlighter(*getApp()->getSpellChecker(), nullptr);
        highlighter.setDocument(box.document());
        box.paintOverlay = [&](QPainter &painter) {
            highlighter.paintWhisperChip(painter, box);
        };
        box.setPlainText(QStringLiteral("/w oneshkof ну шо работает?"));
        box.show();
        pump(100);
        const int prefix = highlighter.whisperPrefix();
        if (!screenshotBase.isEmpty())
        {
            box.grab().save(screenshotBase + "-whisper-chip.png", "PNG");
        }
        box.setPlainText(QStringLiteral("/w oneshk"));
        pump(20);
        const int typing = highlighter.whisperPrefix();
        box.setPlainText(QStringLiteral("обычное сообщение"));
        pump(20);
        check("a typed-out /w name becomes a chip, not while typing the name",
              prefix == int(QStringLiteral("/w oneshkof ").size()) &&
                  typing == 0 && highlighter.whisperPrefix() == 0);
        box.hide();
    }

    // 20. the Chatterino Noctis badge: a marked client-nonce, nothing else
    {
        mist1xmark::resetForTesting();
        bool nonceOk = true;
        for (int i = 0; i < 50; ++i)
        {
            const auto nonce = mist1xmark::makeNonce();
            nonceOk = nonceOk && nonce.size() == 32 &&
                      mist1xmark::isMarked(nonce) &&
                      nonce == nonce.toLower();
        }
        check("own nonces look like the web chat's and carry the mark",
              nonceOk &&
                  !mist1xmark::isMarked(
                      QStringLiteral("5d2627b0cbe56fa05faf5420def4807d")));

        mist1xmark::remember(QStringLiteral("880001"));
        const bool marked = mist1xmark::badgeFor(QStringLiteral("880001")).has_value();
        const bool web = mist1xmark::badgeFor(QStringLiteral("880002")).has_value();
        const bool later = mist1xmark::isKnown(QStringLiteral("880001"));
        check("a marked message's sender gets the badge, others do not",
              marked && !web && later);
    }

    // 21. "More messages below" on a paused chat (pause while hovering, as
    // the mouse is over the chat to click it): the end of the chat, no blank
    // space under the last message - with a message limit and without
    // while paused: nothing more / the missed messages filled in after a
    // reconnect / older history added at the start
    for (const auto &[limit, extra] :
         std::vector<std::pair<size_t, int>>{{300, 0}, {0, 0}, {0, 1}, {0, 2}})
    {
        getSettings()->pauseOnHoverDuration = -1.F;
        getSettings()->enableSmoothScrolling = false;
        getSettings()->enableSmoothScrollingNewMessages = false;
        auto room = std::make_shared<TwitchChannel>(
            QStringLiteral("scroll_room_%1_%2").arg(limit).arg(extra));
        room->setMessageLimit(limit);
        split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(room)));
        split.resize(560, 420);
        split.show();
        int n = 0;
        auto add = [&] {
            room->addMessage(
                makeSystemMessage(QStringLiteral("line %1 with some words").arg(n++)),
                MessageContext::Repost);
        };
        for (int i = 0; i < 400; ++i)
        {
            add();
        }
        pump(200);
        auto &view = split.getChannelView();
        auto &bar = view.getScrollBar();
        bar.setDesiredValue(bar.getMinimum() + 40);  // read the history
        pump(100);
        for (int i = 0; i < 30; ++i)
        {
            add();
            pump(10);
        }
        view.pause(PauseReason::Mouse);  // the mouse goes to the button
        for (int i = 0; i < 30; ++i)
        {
            add();
            pump(10);
        }
        if (extra != 0)
        {
            std::vector<MessagePtr> more;
            for (int i = 0; i < 12; ++i)
            {
                MessageBuilder builder;
                builder.emplace<TextElement>(
                    QStringLiteral("filled %1 with some words").arg(i),
                    MessageElementFlag::Text);
                builder->id = QStringLiteral("filled-%1-%2").arg(extra).arg(i);
                builder->messageText = builder->id;
                more.push_back(builder.release());
            }
            if (extra == 1)
            {
                room->fillInMissingMessages(more);
            }
            else
            {
                room->addMessagesAtStart(more);
            }
            pump(50);
        }
        Button *jump = nullptr;
        for (auto *button : view.findChildren<Button *>())
        {
            if (button->isVisible())
            {
                jump = button;
            }
        }
        if (jump != nullptr)
        {
            Q_EMIT jump->leftClicked();
        }
        for (int i = 0; i < 40; ++i)
        {
            add();
            pump(15);
        }
        pump(300);
        auto gap = [&] {
            // the lowest row with text-bright pixels, from the bottom up
            const auto image = view.grab().toImage();
            for (int y = image.height() - 1; y >= 0; --y)
            {
                for (int x = 0; x < image.width() - 20; ++x)
                {
                    if (QColor(image.pixel(x, y)).lightness() > 90)
                    {
                        return image.height() - 1 - y;
                    }
                }
            }
            return image.height();
        };
        const int paused = gap();
        std::fprintf(stderr,
                     "jump limit=%zu extra=%d button=%d paused gap=%d cur=%.2f bottom=%.2f "
                     "max=%.2f min=%.2f page=%.2f\n",
                     limit, extra, jump != nullptr, paused, bar.getCurrentValue(),
                     bar.getBottom(), bar.getMaximum(), bar.getMinimum(),
                     bar.getPageSize());
        if (!screenshotBase.isEmpty())
        {
            view.grab().save(screenshotBase + QStringLiteral("-jump-%1-%2.png").arg(limit).arg(extra), "PNG");
        }
        view.unpause(PauseReason::Mouse);
        pump(300);
        add();
        pump(300);
        const int after = gap();
        std::fprintf(stderr, "jump limit=%zu extra=%d unpaused gap=%d\n", limit, extra, after);
        check(QStringLiteral("the jump to new messages leaves no blank space "
                             "(limit %1, case %2)").arg(limit).arg(extra),
              jump != nullptr && paused < 40 && after < 40);
    }

    {
        auto room = std::make_shared<Channel>("short-history", Channel::Type::None);
        split.setChannel(IndirectChannel(room));
        split.resize(560, 180);
        for (int i = 0; i < 7; ++i)
            room->addMessage(makeSystemMessage(QStringLiteral("short history row %1").arg(i)), MessageContext::Repost);
        pump(150);
        auto &bar = split.getChannelView().getScrollBar();
        bar.scrollToBottom(false);
        pump(50);
        split.resize(560, 650);
        pump(150);
        check("short history resized to fit resets stale scroll position and page size",
              !bar.isVisible() && qAbs(bar.getCurrentValue() - bar.getMinimum()) < 0.001 && bar.getPageSize() >= 7);
    }

    // 22. Tab after typing in the Russian layout by mistake
    {
        auto room =
            std::make_shared<TwitchChannel>(QStringLiteral("layout_room"));
        room->addRecentChatter(QStringLiteral("mist1x_x"));
        room->addRecentChatter(QStringLiteral("someone"));
        TabCompletionModel model(*room, nullptr);
        auto first = [&](const QString &query) {
            model.updateResults(query, query, int(query.size()), false);
            return model.rowCount() > 0
                       ? model.index(0).data().toString()
                       : QString();
        };
        // only "@" queries here: the emote sources need the whole program
        const auto at = first(QStringLiteral("@ьшые"));
        const auto plain = first(QStringLiteral("@mist"));
        const auto none = first(QStringLiteral("@рфк"));
        const auto keys = TabCompletionModel::fromRussianLayout(
            QStringLiteral("ЗщпСрфьз \"ьшые1ч_ч ЛУЛ Ж)"));
        std::fprintf(stderr, "layout at=%s plain=%s none=%s keys=%s\n",
                     qPrintable(at), qPrintable(plain), qPrintable(none),
                     qPrintable(keys));
        check(QStringLiteral("Tab completes a name typed in the Russian "
                             "layout"),
              at.startsWith(QStringLiteral("@mist1x_x")) &&
                  plain.startsWith(QStringLiteral("@mist1x_x")) &&
                  none.isEmpty() &&
                  keys == QStringLiteral("PogChamp @mist1x_x KEK :)"));
    }

    // 23. Clicking a raid banner away must not start a selection in the chat
    {
        struct PressCounter : QWidget {
            int presses = 0;
            void mousePressEvent(QMouseEvent *event) override
            {
                ++this->presses;
                event->accept();
            }
        } view;
        view.resize(400, 300);
        auto *stack = new CornerBannerStack(&view);
        stack->show({.kind = CornerBannerData::Kind::Raid,
                     .login = QStringLiteral("raider"),
                     .amount = 12});
        CornerBanner *banner = nullptr;
        for (auto *child : view.findChildren<QWidget *>())
        {
            banner = banner ? banner : dynamic_cast<CornerBanner *>(child);
        }
        if (banner)
        {
            const QPointF at(banner->width() / 2.0, banner->height() / 2.0);
            QMouseEvent press(QEvent::MouseButtonPress, at,
                              banner->mapToGlobal(at), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(banner, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at,
                                banner->mapToGlobal(at), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(banner, &release);
        }
        std::fprintf(stderr, "raid banner presses under it=%d\n", view.presses);
        check(QStringLiteral("closing a raid banner with the mouse does not "
                             "press the chat below"),
              banner != nullptr && view.presses == 0 && banner->isLeaving());
    }

    // 24. An opaque selection (Mist-Dark) keeps the text readable: lavender
    //     fill with the text dark on top, as in a text field
    {
        auto room =
            std::make_shared<TwitchChannel>(QStringLiteral("select_room"));
        split.setChannel(
            IndirectChannel(std::static_pointer_cast<Channel>(room)));
        split.resize(560, 160);
        for (const auto *text : {"first line WWWWWWWWWWWW",
                                 "selected text stays readable WWWWWWWWWWWW",
                                 "last line WWWWWWWWWWWW"})
        {
            room->addMessage(makeSystemMessage(QString::fromUtf8(text)),
                             MessageContext::Repost);
        }
        auto &theme = *getTheme();
        const auto previous = theme.messages.selection;
        theme.messages.selection = QColor(184, 167, 240);
        pump(300);
        auto &view = split.getChannelView();
        auto drag = [&](QEvent::Type type, QPointF at, Qt::MouseButtons held) {
            QMouseEvent event(type, at, view.mapToGlobal(at), Qt::LeftButton,
                              held, Qt::NoModifier);
            QApplication::sendEvent(&view, &event);
        };
        // from the second line to the end of the view
        drag(QEvent::MouseButtonPress, QPointF(4, 36), Qt::LeftButton);
        drag(QEvent::MouseMove, QPointF(view.width() - 30, 60),
             Qt::LeftButton);
        drag(QEvent::MouseButtonRelease,
             QPointF(view.width() - 30, 60), Qt::NoButton);
        pump(200);
        const auto image = view.grab().toImage();

        // the input box selects with the same lavender and dark text
        // the input palette is rebuilt from the theme (as on a theme change)
        split.getInput().setProperty(
            "backgroundColor", split.getInput().property("backgroundColor"));
        // (typing here needs the whole program, so the palette is checked)
        auto *edit = split.getInput().findChild<QTextEdit *>();
        const auto highlight =
            edit ? edit->palette().color(QPalette::Highlight) : QColor();
        const auto highlighted =
            edit ? edit->palette().color(QPalette::HighlightedText) : QColor();
        std::fprintf(stderr, "input selection %s on %s\n",
                     qPrintable(highlighted.name()), qPrintable(highlight.name()));
        check(QStringLiteral("the input box selects with the same lavender"),
              highlight == QColor(184, 167, 240) && highlighted.value() < 60);
        theme.messages.selection = previous;
        split.getInput().setProperty(
            "backgroundColor", split.getInput().property("backgroundColor"));
        int lavender = 0;
        int dark = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            // dark text counts only on rows that are mostly lavender
            int rowLavender = 0;
            int rowDark = 0;
            for (int x = 0; x < image.width(); ++x)
            {
                const QColor c = image.pixelColor(x, y);
                rowLavender +=
                    c.red() == 184 && c.green() == 167 && c.blue() == 240;
                rowDark += c.red() < 60 && c.green() < 60 && c.blue() < 70;
            }
            lavender += rowLavender;
            dark += rowLavender > image.width() / 2 ? rowDark : 0;
        }
        std::fprintf(stderr, "opaque selection lavender=%d dark=%d\n", lavender,
                     dark);
        if (!screenshotBase.isEmpty())
        {
            image.save(screenshotBase + "-selection.png", "PNG");
        }
        check(QStringLiteral("an opaque selection shows dark text over "
                             "lavender"),
              lavender > 2000 && dark > 200);
        view.clearSelection();
    }

    QFile out(report);
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(QJsonObject{{"mode", "own-timeout"},
                                            {"checks", checks}})
                      .toJson());
    }
    split.hide();
    split.setChannel(IndirectChannel(Channel::getEmpty()));
    return failed;
}

}  // namespace owntimeout
