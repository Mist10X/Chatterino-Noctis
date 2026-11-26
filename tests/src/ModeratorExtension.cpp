#include "providers/twitch/ModeratorExtension.hpp"
#include "providers/twitch/AutoModerationRules.hpp"
#include "Test.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <openssl/evp.h>

using namespace chatterino::moderatorextension;
namespace {
struct FixtureSigner {
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key{nullptr,EVP_PKEY_free};
    QByteArray pub;
    FixtureSigner()
    {
        std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> ctx(
            EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,nullptr),EVP_PKEY_CTX_free);
        EVP_PKEY *tmp=nullptr;
        if(!ctx || EVP_PKEY_keygen_init(ctx.get())!=1 || EVP_PKEY_keygen(ctx.get(),&tmp)!=1)return;
        key.reset(tmp);
        size_t len=32;pub.resize(32);
        if(EVP_PKEY_get_raw_public_key(key.get(),reinterpret_cast<unsigned char *>(pub.data()),&len)!=1)pub.clear();
    }
    QByteArray envelope(QJsonObject payload)
    {
        const auto raw=QJsonDocument(payload).toJson(QJsonDocument::Compact);
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> c(EVP_MD_CTX_new(),EVP_MD_CTX_free);
        QByteArray sig(64,0);size_t size=64;
        if(!c || EVP_DigestSignInit(c.get(),nullptr,nullptr,nullptr,key.get())!=1 ||
           EVP_DigestSign(c.get(),reinterpret_cast<unsigned char *>(sig.data()),&size,
             reinterpret_cast<const unsigned char *>(raw.constData()),raw.size())!=1)return {};
        return QJsonDocument(QJsonObject{{"payload",QString::fromLatin1(raw.toBase64())},
            {"signature",QString::fromLatin1(sig.toBase64())}}).toJson();
    }
};
QJsonObject fixture(int revision)
{
    return {{"type","noctis-moderator-extension"},{"schema",1},{"host_api",1},{"revision",revision},
        {"version",QString("1.%1.0").arg(revision-1)},
        {"labels",QJsonObject{{"rewards",QJsonObject{{"en","Reward rules"},{"ru","Правила наград"}}}}},
        {"pages",QJsonArray{QJsonObject{{"title",QJsonObject{{"en","Guide"},{"ru","Справка"}}},
            {"body",QJsonObject{{"en","Test"},{"ru","Тест"}}}}}},
        {"reward_actions",revision==1?QJsonArray{}:QJsonArray{QJsonObject{{"id","random-timeout"},
            {"title",QJsonObject{{"en","Random timeout"},{"ru","Случайный мут"}}},
            {"minimum_seconds",60},{"maximum_seconds",180}}}}};
}
}
TEST(ModeratorExtension, SignedAtomicUpdateDeferredAndPersistent)
{
    FixtureSigner signer;
    ASSERT_EQ(signer.pub.size(),32);
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    const auto path=profile.filePath("module.json");
    const auto v1=signer.envelope(fixture(1)),v2=signer.envelope(fixture(2));
    Store store(path,signer.pub);
    EXPECT_EQ(store.active().revision,0);
    EXPECT_EQ(store.offer(v1),Store::Result::Activated);
    EXPECT_EQ(store.active().revision,1);
    store.beginUi();store.beginUi();
    EXPECT_EQ(store.offer(v2),Store::Result::Deferred);
    EXPECT_EQ(store.active().revision,1);
    EXPECT_TRUE(store.pending());
    EXPECT_EQ(store.offer(v1),Store::Result::Rejected);
    store.endUi();EXPECT_EQ(store.active().revision,1);
    store.endUi();EXPECT_EQ(store.active().revision,2);
    EXPECT_FALSE(store.pending());
    const auto action=store.active().rewardAction("random-timeout");
    ASSERT_TRUE(action);
    EXPECT_EQ(action->minimumSeconds,60);
    EXPECT_EQ(action->maximumSeconds,180);
    EXPECT_EQ(store.offer(v2),Store::Result::Unchanged);
    Store restarted(path,signer.pub);
    EXPECT_EQ(restarted.active().revision,2);
    EXPECT_EQ(restarted.active().rewardActions.size(),1);
    EXPECT_EQ(restarted.offer(v1),Store::Result::Rejected);
}
TEST(ModeratorExtension, RejectsUntrustedAndIncompatiblePackages)
{
    FixtureSigner signer,other;
    Store store({},signer.pub);
    EXPECT_EQ(store.offer(other.envelope(fixture(1))),Store::Result::Rejected);
    auto bad=fixture(1);bad["host_api"]=3;
    EXPECT_EQ(store.offer(signer.envelope(bad)),Store::Result::Rejected);
    bad=fixture(2);bad["allowed_twitch_ids"]=QJsonArray{"999"};
    EXPECT_EQ(store.offer(signer.envelope(bad)),Store::Result::Rejected);
    bad=fixture(2);
    auto actions=bad["reward_actions"].toArray();
    auto action=actions[0].toObject();action["minimum_seconds"]=0;actions[0]=action;
    bad["reward_actions"]=actions;
    EXPECT_EQ(store.offer(signer.envelope(bad)),Store::Result::Rejected);
    EXPECT_EQ(store.active().revision,0);
}
TEST(ModeratorExtension, PreservesOldRulesAndDisablesPresetOnOlderHosts)
{
    using namespace chatterino::automoderation;
    Config c{"100",true,true,{},{Rule{Kind::RewardTimeout,
        "313969fe-cc9f-4a0a-83c6-172acbd96957","Mute",600,"reason",true}}};
    const auto original=encode(c);
    EXPECT_FALSE(original["rules"].toArray()[0].toObject().contains("moduleAction"));
    ASSERT_TRUE(decode(original));
    EXPECT_EQ(encode(*decode(original)),original);
    c.rules[0].moduleAction="random-timeout";
    const auto encoded=encode(c);
    EXPECT_FALSE(encoded["rules"].toArray()[0].toObject()["enabled"].toBool(true));
    ASSERT_TRUE(decode(encoded));
    EXPECT_TRUE(decode(encoded)->rules[0].enabled);
    EXPECT_EQ(decode(encoded)->rules[0].moduleAction,"random-timeout");
}
TEST(ModeratorExtension, V2LayoutAndTypedWorkflowCanAddActions)
{
    auto spec=fixture(3);
    spec["host_api"]=2;
    spec["layout"]=QJsonObject{{"reply_tabs",QJsonArray{"timer","rules","options"}},
        {"moderation_tabs",QJsonArray{"rewards","history","words"}}};
    const auto action=QJsonDocument::fromJson(R"({
      "id":"choose-action","title":{"en":"Choose","ru":"Выбор"},"sources":["message","reward"],
      "fields":[
        {"id":"mode","type":"choice","title":{"en":"Mode","ru":"Режим"},"default":"timeout",
         "choices":[{"id":"timeout","title":{"en":"Timeout","ru":"Мут"}},
                    {"id":"reply","title":{"en":"Reply","ru":"Ответ"}}]},
        {"id":"seconds","type":"integer","title":{"en":"Seconds","ru":"Секунды"},"default":90,"minimum":1,"maximum":600},
        {"id":"enabled","type":"boolean","title":{"en":"Active","ru":"Активно"},"default":true},
        {"id":"message","type":"text","title":{"en":"Reply","ru":"Ответ"},"default":"Welcome","maximum":100}],
      "program":{"operation":{"if":[{"param":"enabled"},{"param":"mode"},"skip"]},
        "seconds":{"param":"seconds"},"text":{"concat":[{"param":"message"}," ",{"event":"target"}]}}
    })").object();
    spec["reward_actions"]=QJsonArray{action};
    FixtureSigner signer;
    const auto bundle=verify(signer.envelope(spec),signer.pub);
    ASSERT_TRUE(bundle);
    EXPECT_EQ(bundle->replyOrder.front(),"timer");
    EXPECT_EQ(bundle->moderationOrder.front(),"rewards");
    const auto profile=bundle->rewardAction("choose-action");
    ASSERT_TRUE(profile);
    const QJsonObject event{{"source","reward"},{"target","alice"}};
    auto result=decide(*profile,{},event);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->operation,Operation::Timeout);
    EXPECT_EQ(result->seconds,90);
    result=decide(*profile,{{"mode","reply"}},event);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->operation,Operation::Reply);
    EXPECT_EQ(result->reply,"Welcome alice");
    result=decide(*profile,{{"enabled",false}},event);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->operation,Operation::Skip);
    EXPECT_FALSE(decide(*profile,{{"seconds",0}},event));
    EXPECT_FALSE(decide(*profile,{{"mode","reply"},{"message","/ban"}},event));
    EXPECT_FALSE(decide(*profile,{},{{"source","unsupported"}}));
    // Parameters and acknowledgement survive encoding without altering legacy rules.
    using namespace chatterino::automoderation;
    Config config{"100",true,true,{},{Rule{Kind::Words,"phrase",{},600,{},true,
        "choose-action",{{"seconds",120},{"mode","timeout"}}}}};
    ASSERT_TRUE(decode(encode(config)));
    EXPECT_EQ(decode(encode(config))->rules[0].moduleParameters,config.rules[0].moduleParameters);
}
TEST(ModeratorExtension, V2RejectsUnknownExpressionsAndDuplicateLayout)
{
    auto spec=fixture(3);spec["host_api"]=2;
    spec["layout"]=QJsonObject{{"reply_tabs",QJsonArray{"rules","rules","timer"}}};
    FixtureSigner signer;
    EXPECT_FALSE(verify(signer.envelope(spec),signer.pub));
    spec.remove("layout");
    spec["reward_actions"]=QJsonArray{QJsonObject{{"id","bad"},{"title",QJsonObject{{"en","Bad"},{"ru","Ошибка"}}},
        {"sources",QJsonArray{"message"}},{"fields",QJsonArray{}},
        {"program",QJsonObject{{"operation",QJsonObject{{"os",QJsonArray{"anything"}}}}}}}};
    EXPECT_FALSE(verify(signer.envelope(spec),signer.pub));
    EXPECT_TRUE(claimReplyEvent("fixture-room","fixture-id"));
    EXPECT_FALSE(claimReplyEvent("fixture-room","fixture-id"));
    EXPECT_TRUE(claimReplyEvent("another-room","fixture-id"));
}
