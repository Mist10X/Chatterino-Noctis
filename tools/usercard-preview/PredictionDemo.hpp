#pragma once

// Renders the prediction banner and the betting dialog with synthetic
// predictions, for looking at them (no network, no Twitch).

#include "OwnTimeoutChecks.hpp"
#include "common/UiLanguage.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "widgets/dialogs/PredictionDialog.hpp"
#include "widgets/dialogs/PollDialog.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/Split.hpp"

#include <QDateTime>
#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QLocale>
#include <QScrollArea>

namespace chatterino {

class PredictionDialogTestAccess
{
public:
    static void showBetError(PredictionDialog &dialog, const QString &error)
    {
        dialog.showBetError(error);
    }
};

}  // namespace chatterino

namespace predictiondemo {

inline chatterino::TwitchChannel::PredictionEvent samplePrediction(
    int outcomes, const QString &status, bool ownBet)
{
    using chatterino::TwitchChannel;
    TwitchChannel::PredictionEvent p;
    p.id = QStringLiteral("demo-%1-%2").arg(outcomes).arg(status);
    p.title = outcomes == 2 ? QStringLiteral("Пройдёт босса с первой попытки?")
                            : QStringLiteral("Какое место займёт в турнире?");
    p.status = status;
    p.predictionWindowSeconds = 300;
    p.createdAt = QDateTime::currentDateTimeUtc().addSecs(-75);
    p.createdByName = QStringLiteral("archive_room");
    const QStringList titles2{QStringLiteral("Да"), QStringLiteral("Нет")};
    const QStringList titlesN{QStringLiteral("Первое"), QStringLiteral("Топ-3"),
                              QStringLiteral("Топ-10"),
                              QStringLiteral("Ниже десятого")};
    const qlonglong points[] = {184'250, 61'700, 12'400, 3'050};
    const int users[] = {231, 97, 40, 12};
    const char *names[] = {"big_better", "lucky_one", "gambler", "small"};
    for (int i = 0; i < outcomes; ++i)
    {
        TwitchChannel::PredictionOutcome o;
        o.id = QStringLiteral("o%1").arg(i);
        o.title = outcomes == 2 ? titles2[i] : titlesN[i];
        o.color = outcomes == 2 && i == 1 ? QStringLiteral("PINK")
                                          : QStringLiteral("BLUE");
        o.totalPoints = points[i];
        o.totalUsers = users[i];
        o.topPoints = points[i] / 6;
        o.topPredictorName = QString::fromLatin1(names[i]);
        p.outcomes.push_back(o);
    }
    if (status == QStringLiteral("LOCKED") ||
        status == QStringLiteral("RESOLVED"))
    {
        p.lockedAt = p.createdAt.addSecs(300);
        p.lockedByName = QStringLiteral("archive_room");
    }
    if (status == QStringLiteral("RESOLVED"))
    {
        p.winningOutcomeId = QStringLiteral("o1");
        p.endedByName = QStringLiteral("archive_room");
    }
    if (ownBet)
    {
        p.selfOutcomeId = QStringLiteral("o0");
        p.selfPoints = 2500;
    }
    return p;
}

inline int runPredictionDemo(chatterino::Split &split,
                             const QString &screenshotBase)
{
    using namespace chatterino;
    using owntimeout::pump;
    auto channel =
        std::make_shared<TwitchChannel>(QStringLiteral("archive_room"));
    split.setChannel(
        IndirectChannel(std::static_pointer_cast<Channel>(channel)));
    split.resize(520, 420);
    split.show();
    pump(200);

    struct Case {
        const char *name;
        int outcomes;
        const char *status;
        bool ownBet;
    };
    const Case cases[] = {
        {"active2", 2, "ACTIVE", false},
        {"active2-bet", 2, "ACTIVE", true},
        {"active4", 4, "ACTIVE", false},
        {"locked", 2, "LOCKED", true},
        {"locked-viewer", 2, "LOCKED", false},
        {"resolved", 2, "RESOLVED", true},
        {"resolved-won", 2, "RESOLVED-WON", true},
        {"resolved4", 4, "RESOLVED", true},
    };
    for (const auto &c : cases)
    {
        auto status = QString::fromLatin1(c.status);
        const bool wonCase = status == QStringLiteral("RESOLVED-WON");
        if (wonCase)
        {
            status = QStringLiteral("RESOLVED");
        }
        auto prediction = samplePrediction(c.outcomes, status, c.ownBet);
        if (wonCase)
        {
            prediction.winningOutcomeId = QStringLiteral("o0");
        }
        channel->setActivePrediction(prediction);
        pump(300);
        split.grab().save(
            screenshotBase + "-banner-" + QString::fromLatin1(c.name) + ".png",
            "PNG");

        auto *dialog = new PredictionDialog(channel.get(), &split);
        dialog->setPrediction(prediction);
        dialog->show();
        pump(400);
        dialog->grab().save(
            screenshotBase + "-dialog-" + QString::fromLatin1(c.name) + ".png",
            "PNG");
        std::fprintf(stderr, "prediction demo %s: dialog %dx%d\n", c.name,
                     dialog->width(), dialog->height());
        dialog->close();
        delete dialog;
        pump(50);
    }
    // the own result as Twitch sends it (predictions-user-v1)
    int failed = 0;
    auto check = [&](const char *name, bool ok) {
        failed += ok ? 0 : 1;
        std::fprintf(stderr, "prediction check: %s %s\n", ok ? "ok" : "FAIL",
                     name);
    };
    TwitchChannel::PollEvent poll;
    poll.id = QStringLiteral("poll-style-fixture");
    poll.title = QStringLiteral("Что смотрим?");
    poll.status = QStringLiteral("ACTIVE");
    poll.endsAt = QDateTime::currentDateTimeUtc().addSecs(90);
    poll.totalVotes = 100;
    poll.choices = {{.id = "first", .title = "Первый", .totalVotes = 20},
                    {.id = "second", .title = "Второй", .totalVotes = 65},
                    {.id = "third", .title = "Третий", .totalVotes = 15}};
    poll.selfVotes = {{.choiceId = "first", .freeVotes = 1}};
    auto *pollDialog = new PollDialog(channel.get(), &split);
    pollDialog->setPoll(poll);
    pollDialog->show();
    pump(150);
    const auto labelText = [pollDialog](const char *name) {
        auto *label = pollDialog->findChild<QLabel *>(QString::fromLatin1(name));
        return label ? label->text() : QString();
    };
    check("active poll countdown uses untranslated protocol status",
          labelText("PollCountdown").startsWith(uiText("Ending in %1").section("%1", 0, 0)) &&
          labelText("PollHeaderSubtitle").contains(uiText("Active")) &&
          labelText("PollSectionTitle") == uiText("Vote Submitted"));
    auto ownMarkIs = [pollDialog](const QString &id) {
        auto marks = pollDialog->findChildren<QLabel *>("PollOwnVoteMark");
        return marks.size() == 1 && marks.first()->parentWidget()->property("choiceId").toString() == id;
    };
    check("confirmed non-leading vote marked separately from leading choice", ownMarkIs("first"));
    const auto allChoicesFit = [pollDialog] {
        auto *area = pollDialog->findChild<QScrollArea *>("PollOutcomesScrollArea");
        auto *contents = pollDialog->findChild<QWidget *>("PollOutcomesContent");
        return area && contents && area->viewport()->height() >= contents->minimumSizeHint().height() - 2 &&
               area->viewport()->height() >= 70;
    };
    check("active poll shows every option without unnecessary scrolling", allChoicesFit());
    pollDialog->grab().save(screenshotBase + "-poll-active.png");
    poll.status = QStringLiteral("active");
    poll.endsAt.reset();
    pollDialog->setPoll(poll);
    pump(100);
    check("active snapshot without duration never claims voting ended",
          labelText("PollCountdown") == uiText("Voting in progress") && ownMarkIs("first"));
    poll.status = QStringLiteral("COMPLETED");
    pollDialog->setPoll(poll);
    pump(100);
    check("completed poll displays ended and retains own vote",
          labelText("PollCountdown") == uiText("Voting ended") && ownMarkIs("first"));
    check("finished poll still fits outcomes and has only one visible footer",
          allChoicesFit() && std::ranges::count_if(pollDialog->findChildren<QWidget *>("PollBottomBar"), [](const auto *footer) { return footer->isVisible(); }) == 1);
    pollDialog->grab().save(screenshotBase + "-poll-ended.png");
    poll.currentUserId = QStringLiteral("other-fixture-user");
    pollDialog->setPoll(poll);
    pump(60);
    check("a different account's vote is not marked as mine",
          pollDialog->findChildren<QLabel *>("PollOwnVoteMark").isEmpty());
    poll.currentUserId.clear();
    poll.id = QStringLiteral("another-poll");
    poll.status = QStringLiteral("ACTIVE");
    poll.selfVotes.clear();
    pollDialog->setPoll(poll);
    pump(100);
    check("new poll does not retain prior vote marker",
          pollDialog->findChildren<QLabel *>("PollOwnVoteMark").isEmpty());
    pollDialog->close();
    delete pollDialog;
    pump(50);
    poll.currentUserId = QStringLiteral("fixture-account-one");
    poll.selfVotes = {{.choiceId = "first", .freeVotes = 1}};
    channel->setActivePoll(poll);
    auto eventOnly = poll;
    eventOnly.currentUserId.clear();
    eventOnly.selfVotes.clear();
    channel->setActivePoll(eventOnly);
    {
        const auto snapshot = channel->accessPoll();
        check("event-only poll updates preserve confirmed voter identity",
              *snapshot && snapshot->value().currentUserId == poll.currentUserId &&
              snapshot->value().selfVotes.size() == 1);
    }
    eventOnly.currentUserId = QStringLiteral("fixture-account-two");
    channel->setActivePoll(eventOnly);
    {
        const auto snapshot = channel->accessPoll();
        check("poll update for a new account does not inherit others' vote",
              *snapshot && snapshot->value().selfVotes.empty());
    }
    channel->setActivePoll(std::nullopt);

    channel->addRecentChatter(QStringLiteral("fixtureuser"));
    channel->addRecentChatter(QStringLiteral("fixtureother"));
    auto *completion = new InputCompletionPopup(&split);
    QString accepted;
    completion->setInputAction([&](const QString &text) { accepted = text; });
    completion->updateCompletion("fixture", CompletionKind::User, channel);
    completion->show();
    pump(70);
    const int twoHeight = completion->height();
    completion->updateCompletion("fixtureuser", CompletionKind::User, channel);
    pump(70);
    auto *list = completion->findChild<GenericListView *>();
    check("completion shrinks to one result and uses readable font",
          completion->hasResults() && completion->height() < twoHeight &&
          list && list->font().weight() >= QFont::Medium && list->sizeHintForRow(0) >= list->fontMetrics().height() + 8);
    completion->grab().save(screenshotBase + "-completion.png");
    QKeyEvent tabEvent(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    completion->eventFilter(&split, &tabEvent);
    check("completion accepts the selected user with Tab", accepted.contains("fixtureuser"));
    delete completion;

    // Deliver the actual failed-bet callback into the production dialog,
    // without attempting a bet on Twitch or altering the dialog's sizing.
    auto unavailable = samplePrediction(2, QStringLiteral("ACTIVE"), false);
    channel->setActivePrediction(unavailable);
    auto *unavailableDialog = new PredictionDialog(channel.get(), &split);
    unavailableDialog->show();
    pump(200);
    auto *notice = unavailableDialog->findChild<QLabel *>("PredictionBetError");
    auto *subtitle = unavailableDialog->findChild<QLabel *>("PredictionHeaderSubtitle");
    const auto beforeErrorMessages = channel->getMessageSnapshot().size();
    const auto heightWithoutNotice = unavailableDialog->height();
    check("no prediction error panel before a failed bet", notice && notice->isHidden());
    PredictionDialogTestAccess::showBetError(*unavailableDialog,
                                              QStringLiteral("REGION_LOCKED"));
    pump(200);
    check("region denial is a localized notice in the dialog, never a chat row",
          notice && notice->isVisible() &&
              notice->text() == uiText("Predictions are unavailable in your region.") &&
              channel->getMessageSnapshot().size() == beforeErrorMessages);
    check("prediction notice follows UI font and Noctis card styling",
          notice && subtitle && notice->font() == subtitle->font() &&
              notice->styleSheet().contains("border-radius:") &&
              notice->styleSheet().contains("padding:"));
    check("prediction notice spans the card and has room for wrapped text",
          notice && notice->width() >= unavailableDialog->width() * 0.8 &&
              notice->height() >= notice->heightForWidth(notice->width()) - 1 &&
              unavailableDialog->height() > heightWithoutNotice);
    unavailableDialog->grab().save(screenshotBase + "-dialog-region-locked.png", "PNG");
    const auto heightWithNotice = unavailableDialog->height();
    PredictionDialogTestAccess::showBetError(*unavailableDialog,
                                              QStringLiteral("REGION_LOCKED"));
    pump(100);
    check("repeat prediction error does not create another notice or grow the dialog",
          unavailableDialog->height() == heightWithNotice &&
              unavailableDialog->findChildren<QLabel *>("PredictionBetError").size() == 1);
    unavailable.status = QStringLiteral("LOCKED");
    unavailableDialog->setPrediction(unavailable);
    pump(150);
    check("prediction notice is cleared after betting closes", notice && notice->isHidden());
    unavailableDialog->close();
    delete unavailableDialog;
    pump(50);

    const auto oldSystemMessages =
        getSettings()->showPredictionSystemMessages.getValue();
    getSettings()->showPredictionSystemMessages.setValue(true);
    auto lastResultMatches = [&](Message::ChannelNotice kind,
                                 const QString &text) {
        const auto snapshot = channel->getMessageSnapshot();
        return snapshot.size() > 0 && snapshot.back()->channelNotice == kind &&
               snapshot.back()->clone()->channelNotice == kind &&
               snapshot.back()->messageText == text &&
               snapshot.back()->searchText == text &&
               snapshot.back()->flags.has(MessageFlag::System);
    };
    auto userEvent = [&](const QString &type, const QString &eventId,
                         const QJsonObject &result, int stake = 2500) {
        QJsonObject bet{{"event_id", eventId},
                        {"outcome_id", "o0"},
                        {"channel_id", "1"},
                        {"points", stake}};
        if (!result.isEmpty())
        {
            bet.insert("result", result);
        }
        channel->handlePredictionUserEvent(QJsonObject{
            {"type", type},
            {"data", QJsonObject{{"prediction", bet}}}});
    };

    auto open = samplePrediction(2, QStringLiteral("LOCKED"), false);
    channel->setActivePrediction(open);
    userEvent(QStringLiteral("prediction-made"), open.id, {});
    check("a bet made elsewhere shows as the own bet",
          channel->accessPrediction()->has_value() &&
              (*channel->accessPrediction())->selfPoints == 2500 &&
              (*channel->accessPrediction())->selfOutcomeId == "o0");
    userEvent(QStringLiteral("prediction-result"), open.id,
              QJsonObject{{"type", "WIN"}, {"points_won", 3337}});
    const QLocale locale(activeUiLanguage());
    check("a won bet shows its own marker, localized payout and profit",
          lastResultMatches(Message::ChannelNotice::PredictionWon,
                            uiText("Prediction won · Payout: %1 (+%2)")
                                .arg(locale.toString(3337), locale.toString(837))));
    const auto countAfterWin = channel->getMessageSnapshot().size();
    userEvent(QStringLiteral("prediction-result"), open.id,
              QJsonObject{{"type", "WIN"}, {"points_won", 3337}});
    check("the result is said once",
          channel->getMessageSnapshot().size() == countAfterWin);

    auto second = samplePrediction(2, QStringLiteral("LOCKED"), false);
    second.id = QStringLiteral("second");
    channel->setActivePrediction(second);
    userEvent(QStringLiteral("prediction-result"), second.id,
              QJsonObject{{"type", "LOSE"}, {"points_won", QJsonValue()}});
    check("a lost bet shows its own marker and localized stake",
          lastResultMatches(Message::ChannelNotice::PredictionLost,
                            uiText("Prediction lost · Channel points: −%1")
                                .arg(locale.toString(2500))));

    second.id = QStringLiteral("refund-before-cancel");
    channel->setActivePrediction(second);
    userEvent("prediction-result", second.id, QJsonObject{{"type", "REFUND"}});
    const QString refundMessage =
        uiText("Prediction refunded · Channel points: +%1").arg(locale.toString(2500));
    check("confirmed refund while the prediction is open",
          lastResultMatches(Message::ChannelNotice::PredictionRefund, refundMessage));
    channel->setActivePrediction(std::nullopt);
    const auto countAfterRefund = channel->getMessageSnapshot().size();
    userEvent("prediction-result", second.id, QJsonObject{{"type", "REFUND"}});
    userEvent("prediction-result", "unknown-stake", QJsonObject{{"type", "REFUND"}}, 0);
    check("late duplicate and unknown stake do not add another refund",
          channel->getMessageSnapshot().size() == countAfterRefund);
    userEvent("prediction-result", "refund-after-cancel", QJsonObject{{"type", "REFUND"}});
    check("refund delivered after the public prediction is gone",
          channel->getMessageSnapshot().size() == countAfterRefund + 1 &&
              lastResultMatches(Message::ChannelNotice::PredictionRefund, refundMessage));

    // computed from the pool when Twitch's own result has not come
    auto pool = samplePrediction(2, QStringLiteral("RESOLVED"), true);
    pool.winningOutcomeId = QStringLiteral("o0");
    const auto payout = TwitchChannel::predictionPayout(pool);
    check("payout from the pool share",
          payout && *payout == qlonglong(2500.0 * 245950 / 184250));
    pool.winningOutcomeId = QStringLiteral("o1");
    check("a losing side pays nothing",
          TwitchChannel::predictionPayout(pool) == std::optional<qlonglong>(0));
    check("no bet, no payout",
          !TwitchChannel::predictionPayout(
               samplePrediction(2, QStringLiteral("RESOLVED"), false))
               .has_value());

    // Public event rows, independent of the betting dialog's current state.
    // Verify the marker is attached at the source of actual event messages
    // and survives copies without changing the existing system message text.
    const QJsonObject moderator{{"display_name", "moderator_demo"}};
    const QJsonObject predictionEvent{
        {"id", "voting-notice-prediction"},
        {"title", "Will the demo pass?"},
        {"created_by", moderator},
        {"locked_by", moderator},
        {"ended_by", moderator},
        {"winning_outcome_id", "yes"},
        {"outcomes", QJsonArray{QJsonObject{{"id", "yes"}, {"title", "Yes"}},
                                 QJsonObject{{"id", "no"}, {"title", "No"}}}},
    };
    const QJsonObject pollEvent{
        {"id", "voting-notice-poll"},
        {"title", "Choose a demo"},
        {"created_by", moderator},
        {"ended_by", moderator},
        {"choices", QJsonArray{
            QJsonObject{{"id", "alpha"}, {"title", "Alpha"},
                        {"votes", QJsonObject{{"total", 10}}}},
            QJsonObject{{"id", "beta"}, {"title", "Beta"},
                        {"votes", QJsonObject{{"total", 2}}}}}},
    };
    auto predictionNotice = [&](const QString &type, const QString &status,
                                const char *name) {
        auto event = predictionEvent;
        event.insert("status", status);
        auto before = channel->getMessageSnapshot().size();
        const QJsonObject payload{{"type", type},
                                  {"data", QJsonObject{{"event", event}}}};
        channel->handlePredictionUpdate(payload);
        auto messages = channel->getMessageSnapshot();
        check(name, messages.size() == before + 1 &&
                        messages.back()->channelNotice ==
                            Message::ChannelNotice::Prediction &&
                        messages.back()->messageText.contains("moderator_demo") &&
                        messages.back()->clone()->channelNotice ==
                            Message::ChannelNotice::Prediction &&
                        messages.back()->flags.has(MessageFlag::System));
        channel->handlePredictionUpdate(payload);
        check("duplicate prediction event produces no extra notice",
              channel->getMessageSnapshot().size() == messages.size());
    };
    auto pollNotice = [&](const QString &type, const QString &status,
                          const char *name) {
        auto event = pollEvent;
        event.insert("status", status);
        auto before = channel->getMessageSnapshot().size();
        const QJsonObject payload{{"type", type},
                                  {"data", QJsonObject{{"poll", event}}}};
        channel->handlePollUpdate(payload);
        auto messages = channel->getMessageSnapshot();
        check(name, messages.size() == before + 1 &&
                        messages.back()->channelNotice ==
                            Message::ChannelNotice::Poll &&
                        messages.back()->messageText.contains("moderator_demo") &&
                        messages.back()->clone()->channelNotice ==
                            Message::ChannelNotice::Poll &&
                        messages.back()->flags.has(MessageFlag::System));
        channel->handlePollUpdate(payload);
        check("duplicate poll event produces no extra notice",
              channel->getMessageSnapshot().size() == messages.size());
    };
    predictionNotice("event-created", "ACTIVE", "styled prediction creation");
    predictionNotice("event-locked", "LOCKED", "styled prediction locking");
    predictionNotice("event-resolved", "RESOLVED", "styled prediction resolution");
    predictionNotice("event-canceled", "CANCELED", "styled prediction cancellation");
    pollNotice("POLL_CREATE", "ACTIVE", "styled poll creation");
    pollNotice("POLL_END", "COMPLETED", "styled poll completion");
    pollNotice("POLL_END", "TERMINATED", "styled poll termination");
    pollNotice("POLL_END", "ARCHIVED", "styled poll archival");
    getSettings()->showPredictionSystemMessages.setValue(false);
    const auto countWhenDisabled = channel->getMessageSnapshot().size();
    userEvent("prediction-result", "disabled", QJsonObject{{"type", "REFUND"}});
    channel->handlePredictionUpdate(QJsonObject{
        {"type", "event-created"},
        {"data", QJsonObject{{"event", predictionEvent}}}});
    channel->handlePollUpdate(QJsonObject{
        {"type", "POLL_CREATE"}, {"data", QJsonObject{{"poll", pollEvent}}}});
    check("voting notices obey existing system messages setting",
          channel->getMessageSnapshot().size() == countWhenDisabled);
    getSettings()->showPredictionSystemMessages.setValue(oldSystemMessages);

    channel->setActivePrediction(std::nullopt);
    channel->setActivePoll(std::nullopt);
    pump(100);
    split.grab().save(screenshotBase + "-voting-notices.png", "PNG");

    // Exercise the actual create-mode constructors at their natural size.
    TwitchChannelTestAccess::setFixtureModRole(*channel);
    auto *createPoll = new PollDialog(channel.get(), &split);
    createPoll->show();
    pump(150);
    auto *additional = createPoll->findChild<QCheckBox *>("PollCheckBox");
    auto *pollDuration = createPoll->findChild<QComboBox *>("PollCreateDurationCombo");
    check("poll creation checkbox is not clipped",
          additional && additional->width() >=
                            additional->fontMetrics().horizontalAdvance(additional->text()) + 20);
    check("poll creation duration is localized",
          pollDuration && pollDuration->findText(uiText("2 minutes")) >= 0);
    createPoll->grab().save(screenshotBase + "-create-poll.png");
    createPoll->close();
    delete createPoll;
    auto *createPrediction = new PredictionDialog(channel.get(), &split);
    createPrediction->show();
    pump(150);
    auto *predictionDuration = createPrediction->findChild<QComboBox *>(
        "PredictionCreateDurationCombo");
    check("prediction creation duration is localized",
          predictionDuration && predictionDuration->findText(uiText("2 minutes")) >= 0);
    check("prediction creation uses an antialiased translucent surface",
          createPrediction->testAttribute(Qt::WA_TranslucentBackground));
    createPrediction->grab().save(screenshotBase + "-create-prediction.png");
    createPrediction->close();
    delete createPrediction;
    return failed;
}

}  // namespace predictiondemo
