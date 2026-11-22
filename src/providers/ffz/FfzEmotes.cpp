// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/network/ServiceHealth.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/ffz/FfzRoomBatch.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/network/RetryingGet.hpp"
#include "Application.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/ffz/FfzUtil.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"

#include <limits>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QThread>
#include <QStringList>

namespace {

using namespace chatterino;

const auto &LOG = chatterinoFfzemotes;

const QString CHANNEL_HAS_NO_EMOTES(
    "This channel has no FrankerFaceZ channel emotes.");

constexpr QSize BASE_BADGE_SIZE(18, 18);

struct RoomRequest {
    QString id;
    QString login;
    std::function<void(QJsonObject)> success;
    std::function<void(QString)> error;
};

using RoomBatch = std::shared_ptr<std::vector<RoomRequest>>;

void fetchRoomBatch(const RoomBatch &batch, bool byLogin)
{
    if (isAppAboutToQuit()) return;
    QStringList keys;
    for (const auto &request : *batch)
    {
        const auto key = byLogin ? request.login : request.id;
        if (!keys.contains(key)) keys.push_back(key);
    }
    const auto prefix = byLogin ? QStringLiteral("https://api.frankerfacez.com/v1/multi_room/")
                                : QStringLiteral("https://api.frankerfacez.com/v1/multi_room/id/");
    network::fetchWithBackoff(QUrl(prefix + keys.join(',')),
        [batch](const NetworkResult &result) {
            servicehealth::success(servicehealth::FFZ);
            const auto root = result.parseJson();
            for (auto &request : *batch)
            {
                if (auto room = ffz::detail::roomFromBatch(root, request.id))
                    request.success(std::move(*room));
                else request.error(QStringLiteral("FFZ room missing from response"));
            }
        }, [batch, byLogin, prefix, keys](const NetworkResult &result) {
            // Two documented paths. A stalled login batch may still work by
            // numeric ID. Do not retry repeatedly or bypass rate limits.
            if (byLogin && result.status() != 429)
            {
                fetchRoomBatch(batch, false);
                return;
            }
            servicehealth::failed(servicehealth::FFZ, QUrl(prefix + keys.join(',')), result);
            for (auto &request : *batch) request.error(result.formatError());
        }, 15000);
}

// Individual room requests can stall even when FFZ's official multi-room
// endpoint works. Coalesce startup requests and keep each room's set isolated.
void requestRoom(RoomRequest request)
{
    auto *app = QCoreApplication::instance();
    if (!app || isAppAboutToQuit()) return;
    if (QThread::currentThread() != app->thread())
    {
        QMetaObject::invokeMethod(app, [request = std::move(request)]() mutable {
            requestRoom(std::move(request));
        }, Qt::QueuedConnection);
        return;
    }
    static std::vector<RoomRequest> pending;
    const bool schedule = pending.empty();
    pending.push_back(std::move(request));
    if (!schedule) return;
    QTimer::singleShot(100, QCoreApplication::instance(), [] {
        auto requests = std::exchange(pending, {});
        if (isAppAboutToQuit()) return;
        while (!requests.empty())
        {
            auto batch = std::make_shared<std::vector<RoomRequest>>();
            QStringList ids;
            while (!requests.empty() && ids.size() < 8)
            {
                auto next = std::move(requests.back());
                requests.pop_back();
                if (!ids.contains(next.id)) ids.push_back(next.id);
                batch->push_back(std::move(next));
            }
            const bool hasNames = std::all_of(batch->begin(), batch->end(), [](const auto &r) { return !r.login.isEmpty(); });
            fetchRoomBatch(batch, hasNames);
        }
    });
}

Url getEmoteLink(const QJsonObject &urls, const QString &emoteScale)
{
    auto emote = urls[emoteScale];
    if (emote.isUndefined() || emote.isNull())
    {
        return {""};
    }

    assert(emote.isString());

    return parseFfzUrl(emote.toString());
}

void fillInEmoteData(const QJsonObject &emote, const QJsonObject &urls,
                     const EmoteName &name, const QString &tooltip,
                     Emote &emoteData)
{
    auto url1x = getEmoteLink(urls, "1");
    auto url2x = getEmoteLink(urls, "2");
    auto url3x = getEmoteLink(urls, "4");
    QSize baseSize(emote["width"].toInt(28), emote["height"].toInt(28));

    emoteData.name = name;
    emoteData.images = ImageSet{
        Image::fromUrl(url1x, 1, baseSize),
        url2x.string.isEmpty() ? Image::getEmpty()
                               : Image::fromUrl(url2x, 0.5, baseSize * 2),
        url3x.string.isEmpty() ? Image::getEmpty()
                               : Image::fromUrl(url3x, 0.25, baseSize * 4)};
    emoteData.tooltip = {tooltip};
}

uint32_t modifierFlags(const QJsonObject &jsonEmote)
{
    const auto value = jsonEmote.value("modifier_flags").toInteger(-1);
    if (value < 0 || value > std::numeric_limits<uint32_t>::max())
    {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

void applyModifierMetadata(const QJsonObject &jsonEmote, Emote &emote)
{
    if (!jsonEmote.value("modifier").toBool())
    {
        return;
    }

    const auto providerFlags = modifierFlags(jsonEmote);
    if ((providerFlags & emote_modifiers::EFFECTS) == 0)
    {
        return;
    }

    const bool isPrefix = jsonEmote.value("modifier_prefix").toBool(false);
    emote.modifierFlags = providerFlags;
    emote.modifierPlacement = isPrefix ? EmoteModifierPlacement::Prefix
                                       : EmoteModifierPlacement::Suffix;
    emote.modifierSource = EmoteModifierSource::FrankerFaceZ;
    emote.tooltip =
        Tooltip{emote.name.string + "<br>FrankerFaceZ emote effect"};
}

EmotePtr cachedOrMake(Emote &&emote, const EmoteId &id)
{
    static std::unordered_map<EmoteId, std::weak_ptr<const Emote>> cache;
    static std::mutex mutex;

    return cachedOrMakeEmotePtr(std::move(emote), cache, mutex, id);
}

void parseEmoteSetInto(const QJsonObject &emoteSet, const QString &kind,
                       EmoteMap &map, bool modifiersOnly = false)
{
    for (const auto emoteRef : emoteSet["emoticons"].toArray())
    {
        const auto emoteJson = emoteRef.toObject();

        if (modifiersOnly)
        {
            const auto providerFlags = modifierFlags(emoteJson);
            if (!emoteJson.value("modifier").toBool() ||
                (providerFlags & emote_modifiers::EFFECTS) == 0)
            {
                continue;
            }
        }

        auto id = EmoteId{QString::number(emoteJson["id"].toInt())};
        auto name = EmoteName{emoteJson["name"].toString()};
        auto author =
            EmoteAuthor{emoteJson["owner"]["display_name"].toString()};
        auto urls = emoteJson["urls"].toObject();
        if (emoteJson["animated"].isObject())
        {
            urls = emoteJson["animated"].toObject();
        }

        Emote emote;
        fillInEmoteData(emoteJson, urls, name,
                        QString("%1<br>%2 FFZ Emote<br>By: %3")
                            .arg(name.string, kind, author.string),
                        emote);
        emote.homePage =
            Url{QString("https://www.frankerfacez.com/emoticon/%1-%2")
                    .arg(id.string)
                    .arg(name.string)};
        applyModifierMetadata(emoteJson, emote);

        if (!modifiersOnly || !map.contains(name))
        {
            map[name] = cachedOrMake(std::move(emote), id);
        }
    }
}

EmoteMap parseGlobalEmotes(const QJsonObject &jsonRoot)
{
    std::unordered_set<int> defaultSets{};
    auto jsonDefaultSets = jsonRoot["default_sets"].toArray();
    for (auto jsonDefaultSet : jsonDefaultSets)
    {
        defaultSets.insert(jsonDefaultSet.toInt());
    }

    auto emotes = EmoteMap();

    for (const auto emoteSetRef : jsonRoot["sets"].toObject())
    {
        const auto emoteSet = emoteSetRef.toObject();
        auto emoteSetID = emoteSet["id"].toInt();
        if (!defaultSets.contains(emoteSetID))
        {
            qCDebug(LOG) << "Skipping global emote set" << emoteSetID
                         << "as it's not part of the default sets";
            continue;
        }

        parseEmoteSetInto(emoteSet, "Global", emotes);
    }

    for (const auto emoteSetRef : jsonRoot["sets"].toObject())
    {
        const auto emoteSet = emoteSetRef.toObject();
        if (defaultSets.contains(emoteSet["id"].toInt()))
        {
            continue;
        }

        parseEmoteSetInto(emoteSet, "Global", emotes, true);
    }

    return emotes;
}

std::optional<EmotePtr> parseAuthorityBadge(const QJsonObject &badgeUrls,
                                            const QString &tooltip)
{
    std::optional<EmotePtr> authorityBadge;

    if (!badgeUrls.isEmpty())
    {
        auto authorityBadge1x = getEmoteLink(badgeUrls, "1");
        auto authorityBadge2x = getEmoteLink(badgeUrls, "2");
        auto authorityBadge3x = getEmoteLink(badgeUrls, "4");

        auto authorityBadgeImageSet = ImageSet{
            Image::fromUrl(authorityBadge1x, 1, BASE_BADGE_SIZE),
            authorityBadge2x.string.isEmpty()
                ? Image::getEmpty()
                : Image::fromUrl(authorityBadge2x, 0.5, BASE_BADGE_SIZE * 2),
            authorityBadge3x.string.isEmpty()
                ? Image::getEmpty()
                : Image::fromUrl(authorityBadge3x, 0.25, BASE_BADGE_SIZE * 4),
        };

        authorityBadge = std::make_shared<Emote>(Emote{
            .name = {""},
            .images = authorityBadgeImageSet,
            .tooltip = Tooltip{tooltip},
            .homePage = authorityBadge1x,
        });
    }
    return authorityBadge;
}

}  // namespace

namespace chatterino {

using namespace ffz::detail;

EmoteMap ffz::detail::parseChannelEmotes(const QJsonObject &jsonRoot)
{
    auto emotes = EmoteMap();

    for (const auto emoteSetRef : jsonRoot["sets"].toObject())
    {
        parseEmoteSetInto(emoteSetRef.toObject(), "Channel", emotes);
    }

    return emotes;
}

FfzChannelBadgeMap ffz::detail::parseChannelBadges(const QJsonObject &badgeRoot)
{
    FfzChannelBadgeMap channelBadges;

    for (auto it = badgeRoot.begin(); it != badgeRoot.end(); ++it)
    {
        const auto badgeID = it.key().toInt();
        const auto &jsonUserIDs = it.value().toArray();
        for (const auto &jsonUserID : jsonUserIDs)
        {
            if (jsonUserID.isString())
            {
                channelBadges[jsonUserID.toString()].emplace_back(badgeID);
            }
            else
            {
                channelBadges[QString::number(jsonUserID.toInt())].emplace_back(
                    badgeID);
            }
        }
    }

    return channelBadges;
}

FfzEmotes::FfzEmotes()
    : global_(std::make_shared<EmoteMap>())
{
    getSettings()->enableFFZGlobalEmotes.connect(
        [this] {
            this->loadEmotes();
        },
        this->managedConnections, false);
}

std::shared_ptr<const EmoteMap> FfzEmotes::emotes() const
{
    return this->global_.get();
}

std::optional<EmotePtr> FfzEmotes::emote(EmoteNameView name) const
{
    auto emotes = this->global_.get();
    auto it = emotes->find(name);
    if (it != emotes->end())
    {
        return it->second;
    }
    return std::nullopt;
}

void FfzEmotes::loadEmotes()
{
    if (!Settings::instance().enableFFZGlobalEmotes)
    {
        this->setEmotes(EMPTY_EMOTE_MAP);
        return;
    }

    readProviderEmotesCache("global", "frankerfacez", [this](auto jsonDoc) {
        auto parsedSet = parseGlobalEmotes(jsonDoc.object());
        this->setEmotes(std::make_shared<EmoteMap>(std::move(parsedSet)));
    });

    QString url("https://api.frankerfacez.com/v1/set/global");

    NetworkRequest(url)
        .timeout(30000)
        .onSuccess([this](auto result) {
            writeProviderEmotesCache("global", "frankerfacez",
                                     result.getData());
            auto parsedSet = parseGlobalEmotes(result.parseJson());
            this->setEmotes(std::make_shared<EmoteMap>(std::move(parsedSet)));
        })
        .onError([](auto result) {
            qCWarning(chatterinoFfzemotes)
                << "Failed to fetch global FFZ emotes. "
                << result.formatError();
        })
        .execute();
}

void FfzEmotes::setEmotes(std::shared_ptr<const EmoteMap> emotes)
{
    this->global_.set(std::move(emotes));
}

void FfzEmotes::loadChannel(
    std::weak_ptr<Channel> channel, const QString &channelID,
    std::function<void(EmoteMap &&)> emoteCallback,
    std::function<void(std::optional<EmotePtr>)> modBadgeCallback,
    std::function<void(std::optional<EmotePtr>)> vipBadgeCallback,
    std::function<void(FfzChannelBadgeMap &&)> channelBadgesCallback,
    bool manualRefresh, bool cacheHit)
{
    qCDebug(LOG) << "Reload FFZ Channel Emotes for channel" << channelID;

    const auto sharedChannel = channel.lock();
    requestRoom({channelID, sharedChannel ? sharedChannel->getName() : QString{},
        [emoteCallback = std::move(emoteCallback),
         modBadgeCallback = std::move(modBadgeCallback),
         vipBadgeCallback = std::move(vipBadgeCallback),
         channelBadgesCallback = std::move(channelBadgesCallback), channel,
         channelID, manualRefresh](const QJsonObject json) {
            if (channel.expired() || isAppAboutToQuit()) return;
            writeProviderEmotesCache(channelID, "frankerfacez",
                                     QJsonDocument(json).toJson(QJsonDocument::Compact));

            auto emoteMap = parseChannelEmotes(json);
            auto modBadge = parseAuthorityBadge(
                json["room"]["mod_urls"].toObject(), "Moderator");
            auto vipBadge = parseAuthorityBadge(
                json["room"]["vip_badge"].toObject(), "VIP");
            auto channelBadges =
                parseChannelBadges(json["room"]["user_badge_ids"].toObject());

            bool hasEmotes = !emoteMap.empty();

            emoteCallback(std::move(emoteMap));
            modBadgeCallback(std::move(modBadge));
            vipBadgeCallback(std::move(vipBadge));
            channelBadgesCallback(std::move(channelBadges));
            if (auto shared = channel.lock(); manualRefresh && shared)
            {
                if (hasEmotes)
                {
                    shared->addSystemMessage(
                        "FrankerFaceZ channel emotes reloaded.");
                }
                else
                {
                    shared->addSystemMessage(CHANNEL_HAS_NO_EMOTES);
                }
            }
        },
        [channelID, channel, manualRefresh, cacheHit](const QString &errorString) {
            auto shared = channel.lock();
            if (!shared)
            {
                return;
            }

            qCWarning(LOG) << "Error fetching FFZ emotes for channel"
                           << channelID << ", error" << errorString << ", cached:" << cacheHit;
            if (manualRefresh)
            {
                shared->addSystemMessage(
                    QStringLiteral("Failed to fetch FrankerFaceZ channel "
                                   "emotes. (Error: %1)")
                        .arg(errorString));
                if (cacheHit)
                {
                    shared->addSystemMessage(
                        "Using cached FrankerFaceZ emotes as fallback.");
                }
            }
        }});
}

}  // namespace chatterino
