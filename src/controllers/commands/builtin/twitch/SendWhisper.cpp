// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/SendWhisper.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/WhisperHistory.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "util/Twitch.hpp"

namespace {

using namespace chatterino;

QString formatWhisperError(HelixWhisperError error, const QString &message)
{
    using Error = HelixWhisperError;

    QString errorMessage = "Failed to send whisper - ";

    switch (error)
    {
        case Error::NoVerifiedPhone: {
            errorMessage += "Due to Twitch restrictions, you are now "
                            "required to have a verified phone number "
                            "to send whispers. You can add a phone "
                            "number in Twitch settings. "
                            "https://www.twitch.tv/settings/security";
        };
        break;

        case Error::RecipientBlockedUser: {
            errorMessage += "The recipient doesn't allow whispers "
                            "from strangers or you directly.";
        };
        break;

        case Error::WhisperSelf: {
            errorMessage += "You cannot whisper yourself.";
        };
        break;

        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::Ratelimited: {
            errorMessage += "You may only whisper a maximum of 40 "
                            "unique recipients per day. Within the "
                            "per day limit, you may whisper a "
                            "maximum of 3 whispers per second and "
                            "a maximum of 100 whispers per minute.";
        }
        break;

        case Error::UserMissingScope: {
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            errorMessage += "You don't have permission to "
                            "perform that action.";
        }
        break;

        case Error::Unknown: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }

    return errorMessage;
}

/// Your whisper as a card: «Whisper to <Name>» above your global badges,
/// your name (in its case and 7TV paint) and the text.
bool appendWhisperMessageWordsLocally(const QStringList &words,
                                      const QString &targetName,
                                      const QString &targetLogin,
                                      const std::shared_ptr<TwitchAccount> &self,
                                      const QString &confirmedName = {})
{
    auto *app = getApp();

    MessageBuilder b;

    b.emplace<TimestampElement>();
    b.appendGlobalBadges(self->getUserId());
    auto shown = self->getUserName();
    if (!confirmedName.isEmpty() &&
        confirmedName.compare(shown, Qt::CaseInsensitive) == 0)
    {
        shown = confirmedName;
    }
    if (auto *users = app->getTwitchUsers())
    {
        if (auto user = users->resolveID({self->getUserId()});
            user && !user->displayName.isEmpty() &&
            confirmedName.isEmpty() &&
            user->displayName.compare(shown, Qt::CaseInsensitive) == 0)
        {
            shown = user->displayName;
        }
    }
    const auto selfColor = self->color();
    b.emplace<TextElement>(shown + u':', MessageElementFlag::Username,
                           selfColor.isValid() ? MessageColor(selfColor)
                                               : MessageColor(MessageColor::Text),
                           FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, self->getUserName()});

    whisperhistory::appendBody(b, words.mid(2).join(u' '), {}, self.get(), true);

    b->flags.set(MessageFlag::DoNotTriggerNotification);
    b->flags.set(MessageFlag::Whisper);
    b->loginName = self->getUserName();
    b->displayName = shown;
    b->userID = self->getUserId();
    b->whisperPeer = targetLogin.toLower();
    b->usernameColor = selfColor;
    b->messageText = words.mid(2).join(' ');
    b->searchText = shown + " " + targetLogin + " " + b->messageText;
    b->serverReceivedTime = QDateTime::currentDateTimeUtc();
    auto messagexD = b.release();
    MessageBuilder::makeWhisperCard(
        *messagexD,
        uiText("Whisper to %1").arg(targetName.isEmpty() ? words[1] : targetName),
        {});

    whisperhistory::append(self->getUserId(), *messagexD, true, targetName);
    if (app->getAccounts()->twitch.getCurrent()->getUserId() != self->getUserId())
    {
        return true;
    }
    getApp()->getTwitch()->getWhispersChannel()->addMessage(
        messagexD, MessageContext::Original);

    if (getSettings()->inlineWhispers &&
        !(getSettings()->streamerModeSuppressInlineWhispers &&
          getApp()->getStreamerMode()->isEnabled()))
    {
        app->getTwitch()->forEachChannel([&messagexD](ChannelPtr _channel) {
            _channel->addMessage(messagexD, MessageContext::Repost);
        });
    }

    return true;
}

}  // namespace

namespace chatterino::commands {

QString sendWhisper(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 3)
    {
        ctx.channel->addSystemMessage("Usage: /w <username> <message>");
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to send a whisper!");
        return "";
    }
    auto target = ctx.words.at(1);
    stripChannelName(target);
    auto message = ctx.words.mid(2).join(' ');
    if (ctx.channel->isTwitchChannel())
    {
        getHelix()->getUserByName(
            target,
            [channel{ctx.channel}, currentUser, target, message,
             words{ctx.words}](const auto &targetUser) {
                getHelix()->sendWhisper(
                    currentUser->getUserId(), targetUser.id, message,
                    [words, targetName = targetUser.displayName, target,
                     currentUser] {
                        // resolveID may return an unresolved placeholder.
                        // Resolve the sender after successful delivery; never
                        // send the whisper again to obtain its display name.
                        auto *users = getApp()->getTwitchUsers();
                        const auto cached = users ? users->resolveID(
                            {currentUser->getUserId()}) : nullptr;
                        if (cached && !cached->displayName.isEmpty() &&
                            cached->displayName.compare(currentUser->getUserName(),
                                                        Qt::CaseInsensitive) == 0)
                        {
                            appendWhisperMessageWordsLocally(words, targetName,
                                target, currentUser, cached->displayName);
                            return;
                        }
                        // Save immediately, even if fetching the spelling
                        // fails or the application is closed meanwhile.
                        appendWhisperMessageWordsLocally(words, targetName,
                            target, currentUser);
                        const auto uid = currentUser->getUserId();
                        const auto login = currentUser->getUserName();
                        getHelix()->getUserById(uid,
                            [uid, login](const HelixUser &user) {
                                if (user.id != uid ||
                                    user.login.compare(login, Qt::CaseInsensitive) != 0)
                                    return;
                                whisperhistory::rememberDisplayName(uid, uid,
                                    login, user.displayName);
                                if (getApp()->getAccounts()->twitch.getCurrent()->getUserId() == uid)
                                {
                                    whisperhistory::applyDisplayName(
                                        *getApp()->getTwitch()->getWhispersChannel(),
                                        uid, login, user.displayName);
                                }
                            }, [] {});
                    },
                    [channel, target, targetUser](auto error, auto message) {
                        auto errorMessage = formatWhisperError(error, message);
                        channel->addSystemMessage(errorMessage);
                    });
            },
            [channel{ctx.channel}] {
                channel->addSystemMessage("No user matching that username.");
            });
        return "";
    }

    return "";
}

}  // namespace chatterino::commands
