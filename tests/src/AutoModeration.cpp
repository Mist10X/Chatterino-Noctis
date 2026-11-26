#include "providers/twitch/AutoModerationRules.hpp"
#include "Test.hpp"

using namespace chatterino::automoderation;

TEST(AutoModeration, LiteralWordsAndRewardRouting)
{
    const QString reward = "313969fe-cc9f-4a0a-83c6-172acbd96957";
    const QString other = "313969fe-cc9f-4a0a-83c6-172acbd96958";
    Config c{"111",true,true,{},{
        {Kind::Words,QStringLiteral("плохая фраза"),{},600,"reason",true},
        {Kind::RewardTimeout,reward,"Mute",600,"reward",true},
        {Kind::RewardUntimeout,other,"Unmute",1,"reward",true}
    }};
    ASSERT_TRUE(validate(c));
    EXPECT_EQ(match(c,QStringLiteral("Это ПЛОХАЯ ФРАЗА!"),{})->kind,Kind::Words);
    EXPECT_FALSE(match(c,QStringLiteral("фраза плохая"),{}));
    EXPECT_EQ(match(c,QStringLiteral("плохая фраза"),reward)->kind,Kind::RewardTimeout);
    EXPECT_EQ(match(c,"@example",other)->kind,Kind::RewardUntimeout);
    EXPECT_FALSE(match(c,QStringLiteral("плохая фраза"),"unrelated-reward"));
    c.enabled=false;
    EXPECT_FALSE(match(c,"@example",other));
}
TEST(AutoModeration, ValidatesNicknameRolesAndFiniteTimeout)
{
    EXPECT_EQ(login("  @ExAmPlE_1 "), "example_1");
    EXPECT_TRUE(login("example, second").isEmpty());
    EXPECT_TRUE(login("@example 10m").isEmpty());
    EXPECT_TRUE(login("/timeout example").isEmpty());
    EXPECT_TRUE(protectedBadges("subscriber/1,moderator/1"));
    EXPECT_TRUE(protectedBadges("vip/1"));
    EXPECT_TRUE(protectedBadges("broadcaster/1"));
    EXPECT_FALSE(protectedBadges("subscriber/1"));
    EXPECT_FALSE(canShortenTimeout(0,1000));
    EXPECT_FALSE(canShortenTimeout(1500,1000));
    EXPECT_TRUE(canShortenTimeout(3000,1000));
    EXPECT_FALSE(freshEvent(1000,32000));
    EXPECT_TRUE(freshEvent(1000,1001));
    EXPECT_FALSE(freshEvent(8000,1000));
}
TEST(AutoModeration, ProfileRoundTripAndDuplicateRules)
{
    Config c{"111",true,true,{"protected"},{
        {Kind::Words,"phrase",{},60,"test",true},
        {Kind::RewardTimeout,"313969fe-cc9f-4a0a-83c6-172acbd96957","Reward",600,"",true}
    }};
    const auto loaded=decode(encode(c));
    ASSERT_TRUE(loaded);
    EXPECT_EQ(encode(*loaded),encode(c));
    EXPECT_TRUE(loaded->dryRun);
    c.rules.append({Kind::Words,"PHRASE",{},60,"",true});
    EXPECT_FALSE(validate(c));
    c.rules.removeLast();
    c.rules.append({Kind::RewardUntimeout,c.rules.last().trigger,"Duplicate",1,"",true});
    EXPECT_FALSE(validate(c));
    c.rules.removeLast();
    c.rules[0].seconds=0;
    EXPECT_FALSE(validate(c));
}
