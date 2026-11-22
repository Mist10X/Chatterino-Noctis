// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvEventAPI.hpp"

#include "Application.hpp"
#include "providers/liveupdates/BasicPubSubManager.hpp"
#include "providers/seventv/eventapi/Client.hpp"

#include <QJsonArray>
#include <QRandomGenerator>

#include <set>
#include <unordered_set>
#include <utility>

namespace chatterino {
using namespace seventv;
using namespace seventv::eventapi;
using namespace Qt::Literals::StringLiterals;

class SeventvEventAPIPrivate
    : public BasicPubSubManager<SeventvEventAPIPrivate,
                                seventv::eventapi::Client>
{
public:
    SeventvEventAPIPrivate(SeventvEventAPI &parent, QString host,
                           std::chrono::milliseconds defaultHeartbeatInterval);
    ~SeventvEventAPIPrivate() override;
    SeventvEventAPIPrivate(const SeventvEventAPIPrivate &) = delete;
    SeventvEventAPIPrivate(SeventvEventAPIPrivate &&) = delete;
    SeventvEventAPIPrivate &operator=(const SeventvEventAPIPrivate &) = delete;
    SeventvEventAPIPrivate &operator=(SeventvEventAPIPrivate &&) = delete;

    std::shared_ptr<seventv::eventapi::Client> makeClient();
    void checkHeartbeats();

    std::unordered_set<QString> subscribedEmoteSets;

    std::unordered_set<QString> subscribedUsers;

    using PlatformChannel = std::pair<QString, QString>;  // platform, channel ID
    std::set<PlatformChannel> subscribedChannels;

    std::chrono::milliseconds heartbeatInterval;
    QTimer heartbeatTimer;

    /// Every 30 min all channels are subscribed anew, one every few seconds.
    QTimer resubscribeTimer;
    QTimer resubscribeStepTimer;
    std::vector<PlatformChannel> resubscribeQueue;
    void resubscribeNext();

    SeventvEventAPI &parent;

    friend BasicPubSubManager<SeventvEventAPI, seventv::eventapi::Client>;
    friend SeventvEventAPI;
};

SeventvEventAPIPrivate::SeventvEventAPIPrivate(
    SeventvEventAPI &parent, QString host,
    std::chrono::milliseconds defaultHeartbeatInterval)
    : BasicPubSubManager(std::move(host), u"7TV"_s)
    , heartbeatInterval(defaultHeartbeatInterval)
    , parent(parent)
{
    QObject::connect(&this->heartbeatTimer, &QTimer::timeout, this,
                     &SeventvEventAPIPrivate::checkHeartbeats);
    this->heartbeatTimer.setInterval(this->heartbeatInterval);
    this->heartbeatTimer.setSingleShot(false);
    this->heartbeatTimer.start();

    QObject::connect(&this->resubscribeTimer, &QTimer::timeout, this, [this] {
        this->resubscribeQueue.assign(this->subscribedChannels.begin(),
                                      this->subscribedChannels.end());
        this->resubscribeStepTimer.start();
    });
    this->resubscribeTimer.start(std::chrono::minutes(30));
    QObject::connect(&this->resubscribeStepTimer, &QTimer::timeout, this,
                     &SeventvEventAPIPrivate::resubscribeNext);
    this->resubscribeStepTimer.setInterval(std::chrono::seconds(3));
}

void SeventvEventAPIPrivate::resubscribeNext()
{
    if (this->resubscribeQueue.empty())
    {
        this->resubscribeStepTimer.stop();
        return;
    }
    const auto channel = this->resubscribeQueue.back();
    this->resubscribeQueue.pop_back();
    if (this->subscribedChannels.contains(channel))
    {
        const auto &[platform, id] = channel;
        this->parent.unsubscribePlatformChannel(id, platform);
        this->parent.subscribePlatformChannel(id, platform);
    }
}

SeventvEventAPIPrivate::~SeventvEventAPIPrivate()
{
    this->stop();
}

std::shared_ptr<Client> SeventvEventAPIPrivate::makeClient()
{
    return std::make_shared<Client>(this->parent, this->heartbeatInterval);
}

void SeventvEventAPIPrivate::checkHeartbeats()
{
    auto minInterval = std::chrono::milliseconds::max();
    for (const auto &[id, client] : this->clients())
    {
        client->checkHeartbeat();
        const auto interval = client->heartbeatInterval();
        if (interval > std::chrono::milliseconds::zero())
        {
            minInterval = std::min(minInterval, interval);
        }
    }
    if (minInterval != std::chrono::milliseconds::max())
    {
        this->heartbeatInterval = minInterval;
        this->heartbeatTimer.setInterval(this->heartbeatInterval);
    }
}

SeventvEventAPI::SeventvEventAPI(
    QString host, std::chrono::milliseconds defaultHeartbeatInterval)
    : private_(std::make_unique<SeventvEventAPIPrivate>(
          *this, std::move(host), defaultHeartbeatInterval))
{
}

SeventvEventAPI::~SeventvEventAPI() = default;

void SeventvEventAPI::subscribeUser(const QString &userID,
                                    const QString &emoteSetID)
{
    if (!userID.isEmpty() &&
        this->private_->subscribedUsers.insert(userID).second)
    {
        this->private_->subscribe(
            {ObjectIDCondition{userID}, SubscriptionType::UpdateUser});
    }
    if (!emoteSetID.isEmpty() &&
        this->private_->subscribedEmoteSets.insert(emoteSetID).second)
    {
        this->private_->subscribe(
            {ObjectIDCondition{emoteSetID}, SubscriptionType::UpdateEmoteSet});
    }
}

void SeventvEventAPI::subscribeKickChannel(const QString &id)
{
    this->subscribePlatformChannel(id, "KICK");
}

void SeventvEventAPI::subscribeTwitchChannel(const QString &id)
{
    this->subscribePlatformChannel(id, "TWITCH");
}

void SeventvEventAPI::subscribePlatformChannel(const QString &userID,
                                               const QString &platform)
{
    if (this->private_->subscribedChannels.emplace(platform, userID).second)
    {
        this->private_->subscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::CreateCosmetic,
        });
        this->private_->subscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::CreateEntitlement,
        });
        this->private_->subscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::DeleteEntitlement,
        });
        this->private_->subscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::AnyEmoteSet,
        });
    }
}

void SeventvEventAPI::unsubscribeEmoteSet(const QString &id)
{
    if (this->private_->subscribedEmoteSets.erase(id) > 0)
    {
        this->private_->unsubscribe(
            {ObjectIDCondition{id}, SubscriptionType::UpdateEmoteSet});
    }
}

void SeventvEventAPI::unsubscribeUser(const QString &id)
{
    if (this->private_->subscribedUsers.erase(id) > 0)
    {
        this->private_->unsubscribe(
            {ObjectIDCondition{id}, SubscriptionType::UpdateUser});
    }
}

void SeventvEventAPI::unsubscribeTwitchChannel(const QString &id)
{
    this->unsubscribePlatformChannel(id, "TWITCH");
}

void SeventvEventAPI::unsubscribeKickChannel(const QString &id)
{
    this->unsubscribePlatformChannel(id, "KICK");
}

void SeventvEventAPI::unsubscribePlatformChannel(const QString &userID,
                                                 const QString &platform)
{
    if (this->private_->subscribedChannels.erase({platform, userID}) > 0)
    {
        this->private_->unsubscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::CreateCosmetic,
        });
        this->private_->unsubscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::CreateEntitlement,
        });
        this->private_->unsubscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::DeleteEntitlement,
        });
        this->private_->unsubscribe({
            ChannelCondition{userID, platform},
            SubscriptionType::AnyEmoteSet,
        });
    }
}

void SeventvEventAPI::resubscribeTwitchChannel(const QString &id)
{
    if (this->private_->subscribedChannels.contains({u"TWITCH"_s, id}))
    {
        this->unsubscribePlatformChannel(id, "TWITCH");
    }
    this->subscribePlatformChannel(id, "TWITCH");
}

void SeventvEventAPI::stop()
{
    this->private_->stop();
}

void SeventvEventAPI::reconnect()
{
    for (const auto &[id, c] : this->private_->clients())
    {
        c->close();
    }
}

void SeventvEventAPI::reconnectRandom()
{
    size_t i = QRandomGenerator::global()->bounded(
        static_cast<quint32>(this->private_->clients().size()));
    for (const auto &[id, c] : this->private_->clients())
    {
        if (i == 0)
        {
            c->close();
            break;
        }
        --i;
    }
}

const liveupdates::Diag &SeventvEventAPI::diag() const
{
    return this->private_->diag;
}

}  // namespace chatterino
