#include "providers/twitch/ModeratorModule.hpp"
#include "providers/twitch/ModeratorReplyFeatures.hpp"
#include "Test.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <openssl/evp.h>

using namespace chatterino::moderator;

TEST(ModeratorReplyFeatures, SenderRolesRespectIrcFlagsAndLegacyDefault)
{
    Communi::TagsRef::MapType tags{{"user-id", "2000"}, {"room-id", "3000"}, {"badges", ""}};
    auto classify = [&] { return senderRole(Communi::TagsRef(tags)); };
    EXPECT_EQ(classify(), SenderRole::Viewer);
    tags["badges"] = "subscriber/12,vip/1";
    EXPECT_EQ(classify(), SenderRole::Vip);
    tags["mod"] = "1";
    EXPECT_EQ(classify(), SenderRole::Moderator);
    tags["badges"] = "broadcaster/1";
    EXPECT_EQ(classify(), SenderRole::Broadcaster);
    tags["badges"] = "lead_moderator/1";
    tags.remove("mod");
    EXPECT_EQ(classify(), SenderRole::Moderator);
    tags["user-id"] = "3000";
    EXPECT_EQ(classify(), SenderRole::Broadcaster);
    tags["user-id"] = "2000";
    tags["badges"] = "staff/1";
    EXPECT_EQ(classify(), SenderRole::Unknown);
    tags.remove("badges");
    EXPECT_EQ(classify(), SenderRole::Unknown);
    EXPECT_EQ(decodeRoles(QJsonValue(QJsonValue::Undefined)), DEFAULT_SENDER_ROLES);
    EXPECT_EQ(decodeRoles(QJsonArray{}), 0U);
    EXPECT_EQ(decodeRoles(encodeRoles(14)), 14U);
    EXPECT_FALSE(acceptsRole(14, SenderRole::Viewer));
    EXPECT_TRUE(acceptsRole(14, SenderRole::Moderator));
}

TEST(ModeratorReplyFeatures, TimerStartIsPersistedAndBounded)
{
    const auto now = qint64(200000);
    QHash<QString, qint64> timers{{"example", now - 1200}, {"another", now - 60}, {"*", now - 20}};
    auto stored = QJsonDocument(encodeTimers(timers)).toJson();
    auto restored = decodeTimers(QJsonDocument::fromJson(stored).object(), now + 30);
    EXPECT_EQ(restored.value("example"), now - 1200);
    EXPECT_EQ(restored.value("another"), now - 60);
    EXPECT_FALSE(restored.contains("*"));
    EXPECT_TRUE(decodeTimers(QJsonObject{{"example", now + 1}}, now).isEmpty());
    auto reply = expandReply("Watching {timer}", true, "100", "509658", "Just Chatting", {}, restored.value("example"), now + 30);
    ASSERT_TRUE(reply);
    EXPECT_EQ(*reply, "Watching " + duration(1230));
    EXPECT_FALSE(expandReply("{timer}", false, "", "", "", {}, restored.value("example"), now));
    EXPECT_FALSE(expandReply("{timer}", true, "100", "509658", "Just Chatting", {}, 0, now));
}

TEST(ModeratorReplyFeatures, CategorySegmentsSurviveRelaunchAndAccountIsolation)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir accountA(tmp.filePath("1000"));
    QDir accountB(tmp.filePath("2000"));
    const qint64 streamStart = 10000;
    auto first = observeCategoryFile(accountA, "3000", "100", "10", "Game A", streamStart, streamStart + 15);
    ASSERT_EQ(first.since, streamStart);
    // New calls read their state from disk, just as after a process restart.
    auto resumed = observeCategoryFile(accountA, "3000", "100", "10", "Game A", streamStart, streamStart + 55);
    EXPECT_EQ(resumed.since, streamStart);
    EXPECT_FALSE(resumed.uncertainSegment);
    auto changed = observeCategoryFile(accountA, "3000", "100", "20", "Game B", streamStart, streamStart + 85);
    EXPECT_EQ(changed.since, streamStart + 85);
    EXPECT_EQ(changed.completed.value("10"), 55);
    auto back = observeCategoryFile(accountA, "3000", "100", "10", "Game A", streamStart, streamStart + 115);
    EXPECT_EQ(back.completed.value("10"), 55);
    auto reply = expandReply("{game} | {game_time} | {game_total} | {game_since}", true, "100", "10", "Game A", back, 0, streamStart + 155);
    ASSERT_TRUE(reply);
    EXPECT_TRUE(reply->startsWith("Game A | " + duration(40) + " | " + duration(95)));
    auto otherAccount = observeCategoryFile(accountB, "3000", "100", "10", "Game A", streamStart, streamStart + 115);
    EXPECT_TRUE(otherAccount.uncertainSegment);
    EXPECT_TRUE(otherAccount.completed.isEmpty());
    auto gap = observeCategoryFile(accountA, "3000", "100", "10", "Game A", streamStart, streamStart + 300);
    EXPECT_TRUE(gap.uncertainSegment);
    EXPECT_TRUE(gap.uncertainTotal);
    auto unknown = expandReply("{game_time} {game_total}", true, "100", "10", "Game A", gap, 0, streamStart + 301);
    ASSERT_TRUE(unknown);
    EXPECT_EQ(*unknown, chatterino::uiText("unknown") + " " + chatterino::uiText("unknown"));
    auto newStream = observeCategoryFile(accountA, "3000", "101", "10", "Game A", streamStart + 400, streamStart + 405);
    EXPECT_EQ(newStream.since, streamStart + 400);
    EXPECT_TRUE(newStream.completed.isEmpty());
    EXPECT_FALSE(newStream.uncertainTotal);
    EXPECT_TRUE(observeCategoryFile(accountA, "../bad", "101", "10", "Game A", streamStart + 400, streamStart + 405).stream.isEmpty());
}

TEST(ModeratorReplyFeatures, VariablesNeverSendOfflineJustChattingOrUnsafeExpansion)
{
    CategoryState state;
    ASSERT_TRUE(state.observe("100", "10", "Game A", 1000, 1020));
    for (const auto &token : {"{game}", "{game_time}", "{game_since}", "{game_total}"})
    {
        EXPECT_FALSE(expandReply(token, false, "100", "10", "Game A", state, 0, 1030));
        EXPECT_FALSE(expandReply(token, true, "100", "509658", "Just Chatting", state, 0, 1030));
    }
    state.name = "/ban example";
    EXPECT_FALSE(expandReply("{game}", true, "100", "10", state.name, state, 0, 1030));
    state.name = QString(501, 'a');
    EXPECT_FALSE(expandReply("{game}", true, "100", "10", state.name, state, 0, 1030));
    state.name = "{timer}";
    const auto literal = expandReply("{game}", true, "100", "10", state.name, state, 1000, 1030);
    ASSERT_TRUE(literal);
    EXPECT_EQ(*literal, "{timer}");
    const auto staticReply = expandReply("ordinary reply", false, "", "", "", {}, 0, 0);
    ASSERT_TRUE(staticReply);
    EXPECT_EQ(*staticReply, "ordinary reply");
}

namespace {
struct FixtureKey {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    QByteArray publicKey;
    FixtureKey()
    {
        // Disposable key, never the owner's signing key.
        auto *p = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
        key.reset(p);
        if (!key) return;
        publicKey.resize(32);
        size_t size = 32;
        EVP_PKEY_get_raw_public_key(key.get(),
            reinterpret_cast<unsigned char *>(publicKey.data()), &size);
    }
    QByteArray envelope(const QJsonObject &payload) const
    {
        auto raw = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        QByteArray signature(64, '\0');
        size_t len = 64;
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!key || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
            EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char *>(signature.data()), &len,
                          reinterpret_cast<const unsigned char *>(raw.constData()), raw.size()) != 1)
            return {};
        return QJsonDocument(QJsonObject{{"payload", QString::fromLatin1(raw.toBase64())},
            {"signature", QString::fromLatin1(signature.toBase64())}}).toJson();
    }
};
QJsonObject payload(const QDateTime &now)
{
    return {{"schema", 1}, {"revision", 1}, {"module_version", "1.0.0"},
            {"issued_at", now.toString(Qt::ISODate)}, {"expires_at", now.addDays(1).toString(Qt::ISODate)},
            {"allowed_twitch_ids", QJsonArray{"1000"}}, {"engine", "return function(t,r) return 1 end"},
            {"min_cooldown_seconds", 5}, {"max_rules", 100}};
}
}

TEST(ModeratorModule, SignedGrantAndFailClosedRoleCheck)
{
    FixtureKey fixture;
    const auto now = QDateTime::currentDateTimeUtc();
    auto body = payload(now);
    auto policy = verifyPolicy(fixture.envelope(body), fixture.publicKey, now);
    ASSERT_TRUE(policy);
    const QSet<QString> channels{"example", "another"};
    EXPECT_TRUE(permitted(&*policy, "1000", "1000", channels, "example", true, true));
    EXPECT_FALSE(permitted(&*policy, "2000", "2000", channels, "example", true, true));
    EXPECT_FALSE(permitted(&*policy, "1000", "2000", channels, "example", true, true));
    EXPECT_FALSE(permitted(&*policy, "1000", "1000", channels, "own_channel", true, true));
    EXPECT_FALSE(permitted(&*policy, "1000", "1000", channels, "*", true, true));
    EXPECT_FALSE(permitted(&*policy, "1000", "1000", channels, "example", false, true));
    EXPECT_FALSE(permitted(&*policy, "1000", "1000", channels, "example", true, false));
    EXPECT_FALSE(permitted(nullptr, "1000", "1000", channels, "example", true, true));
    body["allowed_twitch_ids"] = QJsonArray{};
    body["revision"] = 2;
    auto revoked = verifyPolicy(fixture.envelope(body), fixture.publicKey, now);
    ASSERT_TRUE(revoked);
    EXPECT_FALSE(permitted(&*revoked, "1000", "1000", channels, "example", true, true));
}

TEST(ModeratorModule, SignatureExpirySchemaAndTampering)
{
    FixtureKey fixture, other;
    const auto now = QDateTime::currentDateTimeUtc();
    auto body = payload(now);
    auto signedBody = fixture.envelope(body);
    EXPECT_FALSE(verifyPolicy(signedBody, other.publicKey, now));
    auto outer = QJsonDocument::fromJson(signedBody).object();
    body["allowed_twitch_ids"] = QJsonArray{"2000"};
    outer["payload"] = QString::fromLatin1(QJsonDocument(body).toJson().toBase64());
    EXPECT_FALSE(verifyPolicy(QJsonDocument(outer).toJson(), fixture.publicKey, now));
    body["expires_at"] = now.addSecs(-1).toString(Qt::ISODate);
    EXPECT_FALSE(verifyPolicy(fixture.envelope(body), fixture.publicKey, now));
    body = payload(now);
    body["min_cooldown_seconds"] = 0;
    EXPECT_FALSE(verifyPolicy(fixture.envelope(body), fixture.publicKey, now));
    body = payload(now);
    body["schema"] = 2;
    EXPECT_FALSE(verifyPolicy(fixture.envelope(body), fixture.publicKey, now));
}

TEST(ModeratorModule, SignedChannelCooldownIsBackwardCompatible)
{
    FixtureKey fixture;
    const auto now = QDateTime::currentDateTimeUtc();
    auto body = payload(now);
    auto oldPolicy = verifyPolicy(fixture.envelope(body), fixture.publicKey, now);
    ASSERT_TRUE(oldPolicy);
    EXPECT_EQ(oldPolicy->minimumCooldown, 5);
    body["min_channel_cooldown_seconds"] = 0;
    auto policy = verifyPolicy(fixture.envelope(body), fixture.publicKey, now);
    ASSERT_TRUE(policy);
    EXPECT_EQ(policy->minimumCooldown, 0);
    EXPECT_EQ(policy->allowed, oldPolicy->allowed);
    EXPECT_EQ(policy->source, oldPolicy->source);
    for (const auto &bad : QList<QJsonValue>{-1, 301, 0.5, "0", QJsonValue::Null})
    {
        body["min_channel_cooldown_seconds"] = bad;
        EXPECT_FALSE(verifyPolicy(fixture.envelope(body), fixture.publicKey, now));
    }
    body["min_channel_cooldown_seconds"] = 0;
    body["min_cooldown_seconds"] = 0;
    EXPECT_FALSE(verifyPolicy(fixture.envelope(body), fixture.publicKey, now));
}

#ifdef CHATTERINO_HAVE_PLUGINS
TEST(ModeratorModule, DownloadedMatcherIsBoundedAndCannotRunHostActions)
{
    Matcher matcher(R"(return function(text, rules)
        local best, n
        for i=1,#rules do
            local r=rules[i]
            for j=1,#r.triggers do
                local t=r.triggers[j]
                if r.exact and text==t then return i end
                if not r.exact and string.find(text,t,1,true) and (not n or #t<n) then best,n=i,#t end
            end
        end
        return best
    end)");
    ASSERT_TRUE(matcher.valid());
    const std::vector<MatchRule> rules{{{"что за игра"}, false},
                                       {{"игра"}, false},
                                       {{"ИГРА"}, true}};
    EXPECT_EQ(matcher.match("  ЧТО ЗА игра  ", rules), 1U);
    EXPECT_EQ(matcher.match("игра", rules), 2U);
    EXPECT_FALSE(matcher.match("unknown", rules));
    Matcher outOfBounds("return function() return 10000 end");
    ASSERT_TRUE(outOfBounds.valid());
    EXPECT_FALSE(outOfBounds.match("test", rules));
    Matcher scriptWithIO("return function() return io.open('anything') end");
    ASSERT_TRUE(scriptWithIO.valid());
    EXPECT_FALSE(scriptWithIO.match("test", rules));
    Matcher looping("return function() while true do end end");
    ASSERT_TRUE(looping.valid());
    EXPECT_FALSE(looping.match("test", rules));
    Matcher badInitialization("while true do end");
    EXPECT_FALSE(badInitialization.valid());
    EXPECT_FALSE(validReply("/ban viewer"));
    EXPECT_FALSE(validReply("  .timeout viewer 30"));
    EXPECT_FALSE(validReply(QStringLiteral("hello\nworld")));
    EXPECT_TRUE(validReply("обычный ответ"));
}

TEST(ModeratorModule, WordSetsUseSignedSelectionAndPreserveLegacyPriority)
{
    Matcher matcher(R"(return function(text, rules)
        local best, n
        for i=1,#rules do
            local r=rules[i]
            for j=1,#r.triggers do
                local t=r.triggers[j]
                if r.exact and text==t then return i end
                if not r.exact and string.find(text,t,1,true) and (not n or #t<n) then best,n=i,#t end
            end
        end
        return best
    end)");
    ASSERT_TRUE(matcher.valid());
    const std::vector<MatchRule> rules{
        {{QStringLiteral("давно пузатую смотрите"), QStringLiteral("когда начал фильм")}, true, true},
        {{QStringLiteral("давно пузатую")}, true, true},
        {{QStringLiteral("давно пузатую смотрите уже")}, true, true},
        {{QStringLiteral("команда")}, false},
        {{QStringLiteral("пузатую давно смотрите")}, true},
        {{"?!"}, true, true},
    };
    EXPECT_EQ(matcher.match(QStringLiteral("СМОТРИТЕ, давно вы пузатую?"), rules), 0U);
    EXPECT_EQ(matcher.match(QStringLiteral("Пузатую уже давно смотрите?"), rules), 2U);
    EXPECT_EQ(matcher.match(QStringLiteral("когда фильм начал"), rules), 0U);
    EXPECT_EQ(matcher.match(QStringLiteral("давно пузатую"), rules), 1U);
    EXPECT_EQ(matcher.match(QStringLiteral("пузатую давно смотрите"), rules), 4U);
    EXPECT_EQ(matcher.match(QStringLiteral("команда пузатую давно смотрите"), rules), 3U);
    EXPECT_FALSE(matcher.match(QStringLiteral("недавно пузатую смотрите"), rules));
    EXPECT_FALSE(matcher.match(QStringLiteral("давно пузатуюшка смотрите"), rules));
    EXPECT_FALSE(matcher.match(QStringLiteral("пузатую смотрите"), rules));
    EXPECT_FALSE(matcher.match("?!", rules));
    const std::vector<MatchRule> onlyWords{rules[0]};
    Matcher veto("return function() return nil end");
    Matcher invalid("return function() return 999 end");
    Matcher always("return function() return 1 end");
    EXPECT_FALSE(veto.match(QStringLiteral("давно пузатую смотрите"), onlyWords));
    EXPECT_FALSE(invalid.match(QStringLiteral("давно пузатую смотрите"), onlyWords));
    EXPECT_FALSE(always.match(QStringLiteral("давно смотрите"), onlyWords));
}
#endif
