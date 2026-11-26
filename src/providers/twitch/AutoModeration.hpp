#pragma once

#include <IrcTagsRef>
#include <QString>
#include <functional>

class QWidget;
namespace chatterino {
class TwitchChannel;
struct GqlChannelPointRewards;
using AutoModerationRewardsFetcher = std::function<void(
    const QString &, std::function<void(GqlChannelPointRewards)>,
    std::function<void(const QString &)>)>;
// Edition-specific permission provider. The public implementation requires
// the current signed grant AND verified moderator role.
bool permittedAutoModeration(TwitchChannel &channel);
void openAutoModeration(QWidget *parent,
                        AutoModerationRewardsFetcher fixtureFetcher = {});
void observeAutoModerationMessage(TwitchChannel &channel, Communi::TagsRef tags,
                                  const QString &author, const QString &text);
void observeAutoModerationTimeout(TwitchChannel &channel, Communi::TagsRef tags);
} // namespace chatterino
