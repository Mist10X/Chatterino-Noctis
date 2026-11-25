#include "providers/twitch/ModeratorModule.hpp"
#include "Test.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <openssl/evp.h>

using namespace chatterino::moderator;

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
#endif
