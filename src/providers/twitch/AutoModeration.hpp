#pragma once

#include <IrcTagsRef>
#include <QString>
#include <functional>

class QWidget;
class QJsonArray;
namespace chatterino {
class TwitchChannel;
struct GqlChannelPointRewards;
using AutoModerationRewardsFetcher = std::function<void(
    const QString &, std::function<void(GqlChannelPointRewards)>,
    std::function<void(const QString &)>)>;
using AutoModerationRoleFetcher = std::function<void(
    const QString &, std::function<void(const QJsonArray &, const QJsonArray &)>,
    std::function<void()>)>;
void setAutoModerationRoleFetcherForTesting(AutoModerationRoleFetcher fetcher);
// Edition-specific permission provider. The public implementation requires
// the current signed grant AND verified moderator role.
bool permittedAutoModeration(TwitchChannel &channel);
void openAutoModeration(QWidget *parent,
                        AutoModerationRewardsFetcher fixtureFetcher = {});
void observeAutoModerationMessage(TwitchChannel &channel, Communi::TagsRef tags,
                                  const QString &author, const QString &text);
void observeAutoModerationTimeout(TwitchChannel &channel, Communi::TagsRef tags);
void invalidateAutoModerationTimeouts(TwitchChannel &channel);
} // namespace chatterino
