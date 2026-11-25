// SPDX-License-Identifier: MIT
#include "providers/twitch/ReplyQuestionConflicts.hpp"
#include "Test.hpp"

using namespace chatterino;
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
