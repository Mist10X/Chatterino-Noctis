// SPDX-License-Identifier: MIT
#include "providers/twitch/ReplyQuestionConflicts.hpp"
#include "providers/twitch/RepeatCommand.hpp"
#include "providers/twitch/RepeatCommandPage.hpp"
#include "providers/twitch/AutoModerationTable.hpp"
#include <QEventLoop>
#include <QElapsedTimer>
#include <QApplication>
#include <QScrollBar>
#include "providers/twitch/AutoReplyWords.hpp"
#include "Test.hpp"

using namespace chatterino;
TEST(AutoModerationTable, LocalizedHeadersFitAtWideAndNarrowWidths)
{
    AutoModerationTable table(6, nullptr);
    table.setAttribute(Qt::WA_DontShowOnScreen);
    auto font = table.font();
    font.setPointSize(13);
    table.setFont(font);
    table.setHorizontalHeaderLabels({QStringLiteral("Включено"), QStringLiteral("Часть сообщения"),
        QStringLiteral("Действие"), QStringLiteral("Длительность мута"),
        QStringLiteral("Причина"), QStringLiteral("Параметры")});
    for (const auto width : {900, 420, 1600})
    {
        table.resize(width, 250);
        table.show();
        QEventLoop settled;
        QTimer::singleShot(40, &settled, &QEventLoop::quit);
        settled.exec();
        const QFontMetrics metrics(table.horizontalHeader()->font());
        for (int column = 0; column < table.columnCount(); ++column)
            EXPECT_GE(table.columnWidth(column),
                      metrics.horizontalAdvance(table.horizontalHeaderItem(column)->text()) + 18);
        if (width == 420) EXPECT_GT(table.horizontalScrollBar()->maximum(), 0);
        if (width == 1600) EXPECT_EQ(table.horizontalScrollBar()->maximum(), 0)
            << "viewport " << table.viewport()->width()
            << " header length " << table.horizontalHeader()->length();
    }
}
namespace {
struct CommandEditorRule {
    QString channel;
    QStringList triggers;
    QString reply;
    QString title;
    bool enabled = true;
    bool repeatCommand = false;
    QString label() const { return title.isEmpty() ? triggers.value(0) : title; }
};
}

TEST(RepeatCommand, DedicatedEditorPreservesAndSeparatesRules)
{
    using namespace repeatcommand;
    const QList<CommandEditorRule> original{
        {"alpha", {"question"}, "ordinary answer", "", true, false},
        {"alpha", {"!tg", "!telegram"}, "old reply", "group", true, true},
        {"other", {"!other"}, "untouched", "", false, true},
    };
    QList<CommandEditorRule> persisted;
    std::unique_ptr<QWidget> page(makePage(nullptr, original, {"alpha", "beta"}, "alpha",
        [&](const QList<CommandEditorRule> &rules) {
            persisted = mergeRules(original, rules);
            return QString{};
        }));
    auto *table = page->findChild<QTableWidget *>("RepeatCommandRules");
    auto *trigger = page->findChild<QPlainTextEdit *>("RepeatCommandTrigger");
    auto *reply = page->findChild<QPlainTextEdit *>("RepeatCommandReply");
    auto *picker = page->findChild<QComboBox *>("RepeatCommandChannel");
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(trigger->toPlainText(), QStringLiteral("!tg\n!telegram"));
    EXPECT_EQ(page->findChild<QComboBox *>("RepeatCommandSender"), nullptr);
    reply->setPlainText("new reply");
    picker->setCurrentIndex(1);
    EXPECT_EQ(table->rowCount(), 0);
    page->findChild<QPushButton *>("RepeatCommandAdd")->click();
    ASSERT_EQ(table->rowCount(), 1);
    trigger->setPlainText("!new");
    reply->setPlainText("new channel");
    page->findChild<QPushButton *>("RepeatCommandSave")->click();
    ASSERT_EQ(persisted.size(), 4);
    EXPECT_EQ(persisted[0].reply, QStringLiteral("ordinary answer"));
    EXPECT_EQ(persisted[1].reply, QStringLiteral("new reply"));
    EXPECT_EQ(persisted[2].reply, QStringLiteral("untouched"));
    EXPECT_EQ(persisted[3].triggers, QStringList{"!new"});
    EXPECT_EQ(onlyRules(persisted, false).size(), 1);
    EXPECT_EQ(onlyRules(persisted, true).size(), 3);
}

TEST(RepeatCommand, StrictCountAndWholeCommand)
{
    using namespace repeatcommand;
    for (int count = 1; count <= MaxMessages; ++count)
    {
        const auto parsed = parse(QStringLiteral("!тг %1").arg(count));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->count, count);
        EXPECT_TRUE(matches({QStringLiteral("!ТГ")}, *parsed));
        EXPECT_FALSE(matches({QStringLiteral("!тгр")}, *parsed));
    }
    for (const auto *bad : {"!tg", "!tg0", "!tg 0", "!tg 11", "!tg 999999",
                            "!tg -1", "!tg +5", "!tg 05", "!tg 5x", "!tg 5 6", "x !tg 5"})
        EXPECT_FALSE(parse(QString::fromUtf8(bad)));
    EXPECT_FALSE(validOutput(QStringLiteral("!tg 5")));
    EXPECT_TRUE(validOutput(QStringLiteral("Telegram: fixture")));
    EXPECT_TRUE(ownsCommand("42", "42", {}, true));
    EXPECT_FALSE(ownsCommand("43", "42", {}, true));
    EXPECT_FALSE(ownsCommand("42", "42", "43", true));
    EXPECT_FALSE(ownsCommand("42", "42", {}, false));
    EXPECT_FALSE(ownsCommand({}, {}, {}, true));
}

TEST(RepeatCommand, BatchIsBoundedAndStopsOnRejectedDispatch)
{
    using namespace repeatcommand;
    Batch batch;
    int sent = 0;
    EXPECT_FALSE(batch.start(0, [] { return true; }));
    EXPECT_FALSE(batch.start(11, [] { return true; }));
    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    QElapsedTimer spacing;
    spacing.start();
    ASSERT_TRUE(batch.start(10, [&] {
        EXPECT_GE(spacing.restart(), 100);
        if (++sent == 10) loop.quit();
        return true;
    }));
    EXPECT_FALSE(batch.start(1, [] { return true; }));
    loop.exec();
    EXPECT_EQ(sent, 10);
    EXPECT_FALSE(batch.active());
    sent = 0;
    QEventLoop stopped;
    ASSERT_TRUE(batch.start(5, [&] { ++sent; return false; }));
    QTimer::singleShot(600, &stopped, &QEventLoop::quit);
    stopped.exec();
    EXPECT_EQ(sent, 1);
    EXPECT_FALSE(batch.active());
}
namespace {
struct Rule {
    QString channel;
    QStringList triggers;
    QString title;
};
}

TEST(ReplyQuestionConflicts, ExactNormalizedLinesIncludingBlankLines)
{
    const QList<Rule> rules{{"demo", {"Hello", "", "  HELLO  "}, "First group"}};
    const auto conflicts = replyquestions::findConflicts(rules, 0, [](const Rule &) { return true; });
    ASSERT_TRUE(conflicts.first);
    EXPECT_EQ(conflicts.count, 1);
    EXPECT_EQ(conflicts.first->first.rule, 0);
    EXPECT_EQ(conflicts.first->first.line, 0);
    EXPECT_EQ(conflicts.first->second.rule, 0);
    EXPECT_EQ(conflicts.first->second.line, 2);
    const auto message = replyquestions::describe(conflicts, rules);
    EXPECT_TRUE(message.contains("HELLO"));
    EXPECT_TRUE(message.contains("First group"));
    QPlainTextEdit editor;
    editor.setPlainText(rules[0].triggers.join(u'\n'));
    replyquestions::selectLine(editor, conflicts.first->second.line);
    EXPECT_EQ(editor.textCursor().selectedText(), "  HELLO  ");
    EXPECT_EQ(editor.textCursor().blockNumber(), 2);
}

TEST(ReplyQuestionConflicts, AcrossGroupsOnlyWithinSameChannelAndPreferEditedRule)
{
    const QList<Rule> rules{
        {"demo", {"old", "", "Question   One"}, "Edited"},
        {"other", {"question one"}, "Other channel"},
        {"#DEMO", {"different", "question one"}, "Original"},
        {"demo", {"question"}, "Partial match"},
    };
    const auto result = replyquestions::findConflicts(rules, 0, [](const Rule &) { return true; });
    ASSERT_TRUE(result.first);
    EXPECT_EQ(result.count, 1);
    EXPECT_EQ(result.first->first.rule, 2);
    EXPECT_EQ(result.first->first.channelOrdinal, 2);
    EXPECT_EQ(result.first->first.line, 1);
    EXPECT_EQ(result.first->second.rule, 0);
    EXPECT_EQ(result.first->second.channelOrdinal, 1);
    EXPECT_EQ(result.first->second.line, 2);
    const auto text = replyquestions::describe(result, rules);
    EXPECT_TRUE(text.contains("Original"));
    EXPECT_TRUE(text.contains("Edited"));
    const auto excluded = replyquestions::findConflicts(rules, 0, [](const Rule &r) { return r.title != "Original"; });
    EXPECT_FALSE(excluded.first);
    EXPECT_EQ(excluded.count, 0);
}

TEST(ReplyQuestionConflicts, WordSetsIgnoreOrderCasePunctuationAndRepeatedWords)
{
    struct WordRule {
        QString channel;
        QStringList triggers;
        QString title;
        bool allWords;
    };
    const QList<WordRule> rules{
        {"demo", {QStringLiteral("давно пузатую смотрите")}, "One", true},
        {"demo", {QStringLiteral("давно пузатую смотрите")}, "Literal", false},
        {"demo", {QStringLiteral("СМОТРИТЕ, пузатую   давно давно?")}, "Two", true},
        {"other", {QStringLiteral("пузатую давно смотрите")}, "Different channel", true},
    };
    const auto result = replyquestions::findConflicts(rules, 2,
        [](const auto &) { return true; }, [](const auto &rule, const auto &phrase) {
            return autoreplywords::conflictKey(phrase, rule.allWords);
        });
    ASSERT_TRUE(result.first);
    EXPECT_EQ(result.count, 1);
    EXPECT_EQ(result.first->first.rule, 0);
    EXPECT_EQ(result.first->second.rule, 2);
    EXPECT_FALSE(autoreplywords::conflictKey("?!", true).size());
}
