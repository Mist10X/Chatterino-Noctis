#pragma once

#include "common/Args.hpp"
#include "mocks/DisabledStreamerMode.hpp"
#include "mocks/EmptyApplication.hpp"
#include "mocks/TwitchUsers.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/badgehq/BadgeHQBadges.hpp"
#include "providers/bluzyrino/BluzyrinoBadges.hpp"
#include "providers/chatsen/ChatsenBadges.hpp"
#include "providers/dankchat/DankChatBadges.hpp"
#include "providers/ffzap/FfzApBadges.hpp"
#include "providers/jilchat/JilChatBadges.hpp"
#include "providers/limerino/LimerinoBadges.hpp"
#include "providers/moltorino/MoltorinoSupporterBadges.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <QString>

namespace chatterino::mock {

/**
 * BaseApplication intends to be a mock application with a few more sane defaults, but with less configurability
 */
class BaseApplication : public EmptyApplication
{
public:
    BaseApplication(bool runMigrations = false)
        : settings(this->modes_, this->args_, this->settingsDir.path(),
                   {
                       .isTest = true,
                       .runMigrations = runMigrations,
                   })
        , updates(this->modes_, this->paths_, this->settings)
        , theme(this->paths_)
        , fonts(this->settings)
    {
    }

    BaseApplication(const QString &settingsData, bool runMigrations = false,
                    const QString &customThemeFile = {})
        : EmptyApplication(settingsData, customThemeFile)
        , settings(this->modes_, this->args_, this->settingsDir.path(),
                   {
                       .isTest = true,
                       .runMigrations = runMigrations,
                   })
        , updates(this->modes_, this->paths_, this->settings)
        , theme(this->paths_)
        , fonts(this->settings)
    {
    }

    Updates &getUpdates() override
    {
        return this->updates;
    }

    IStreamerMode *getStreamerMode() override
    {
        return &this->streamerMode;
    }

    Theme *getThemes() override
    {
        return &this->theme;
    }

    Fonts *getFonts() override
    {
        return &this->fonts;
    }

    ITwitchUsers *getTwitchUsers() override
    {
        return &this->twitchUsers;
    }

    BttvLiveUpdates *getBttvLiveUpdates() override
    {
        return nullptr;
    }

    SeventvEventAPI *getSeventvEventAPI() override
    {
        return nullptr;
    }

    FfzApBadges *getFfzApBadges() override
    {
        return &this->ffzApBadges_;
    }

    DankChatBadges *getDankChatBadges() override
    {
        return &this->dankChatBadges_;
    }

    ChatsenBadges *getChatsenBadges() override
    {
        return &this->chatsenBadges_;
    }

    BluzyrinoBadges *getBluzyrinoBadges() override
    {
        return &this->bluzyrinoBadges_;
    }

    JilChatBadges *getJilChatBadges() override
    {
        return &this->jilChatBadges_;
    }

    BadgeHQBadges *getBadgeHQBadges() override
    {
        return &this->badgeHQBadges_;
    }

    LimerinoBadges *getLimerinoBadges() override
    {
        return &this->limerinoBadges_;
    }

    MoltorinoSupporterBadges *getMoltorinoSupporterBadges() override
    {
        return &this->moltorinoSupporterBadges_;
    }

    SeventvPaints *getSeventvPaints() override
    {
        return &this->seventvPaints_;
    }

    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &this->seventvPersonalEmotes_;
    }

    PubSub *getTwitchPubSub() override
    {
        if (!this->pubSub_)
        {
            this->pubSub_ = std::make_unique<PubSub>(QStringLiteral("wss://127.0.0.1:9050"));
        }
        return this->pubSub_.get();
    }

    Args args;
    Settings settings;
    Updates updates;
    DisabledStreamerMode streamerMode;
    Theme theme;
    Fonts fonts;
    TwitchUsers twitchUsers;

private:
    FfzApBadges ffzApBadges_;
    DankChatBadges dankChatBadges_;
    ChatsenBadges chatsenBadges_;
    BluzyrinoBadges bluzyrinoBadges_;
    JilChatBadges jilChatBadges_;
    BadgeHQBadges badgeHQBadges_;
    LimerinoBadges limerinoBadges_;
    MoltorinoSupporterBadges moltorinoSupporterBadges_;
    SeventvPaints seventvPaints_;
    SeventvPersonalEmotes seventvPersonalEmotes_;
    std::unique_ptr<PubSub> pubSub_;
};

}  // namespace chatterino::mock
