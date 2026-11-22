#pragma once

#include "messages/Message.hpp"

#include <QSet>

#include <cstddef>
#include <vector>

namespace chatterino {

struct UsercardActivity {
    size_t messageCount = 0;
    QDateTime latestMessage;
};

/// Summarize the already loaded history of one user in one channel. This is
/// not a lifetime total from Twitch; callers must label it as such.
inline UsercardActivity summarizeUsercardActivity(
    const std::vector<MessagePtr> &messages, const QString &userId,
    const QString &channelName)
{
    UsercardActivity result;
    if (userId.isEmpty())
    {
        // A login alone cannot distinguish owners of a reused username.
        return result;
    }

    QSet<QString> seenIds;
    QSet<QString> seenLegacyKeys;
    QSet<QString> seenIdentifiedKeys;
    QSet<const Message *> seenUndated;
    for (const auto &row : messages)
    {
        if (!row)
        {
            continue;
        }
        const auto &message = row->translatedFrom ? *row->translatedFrom : *row;
        if (message.userID != userId || message.loginName.isEmpty() ||
            message.flags.hasAny(
                MessageFlag::System, MessageFlag::Whisper,
                MessageFlag::Timeout, MessageFlag::Untimeout,
                MessageFlag::Deletion, MessageFlag::ClearChat,
                MessageFlag::ModerationAction, MessageFlag::AutoMod,
                MessageFlag::AutoModOffendingMessageHeader,
                MessageFlag::AutoModOffendingMessage, MessageFlag::LowTrustUsers,
                MessageFlag::Subscription, MessageFlag::WatchStreak,
                MessageFlag::Follow, MessageFlag::ChatWarning,
                MessageFlag::UncategorizedNotification,
                MessageFlag::SharedMessage) ||
            (!channelName.isEmpty() && !message.channelName.isEmpty() &&
             message.channelName.compare(channelName, Qt::CaseInsensitive) !=
                 0))
        {
            continue;
        }

        // Ordinary messages from subscribers carry a badge, not the
        // Subscription notice flag. Deleted original chat messages still
        // count; their deletion notice does not.
        QString key;
        if (message.serverReceivedTime.isValid())
        {
            key = QString::number(
                      message.serverReceivedTime.toMSecsSinceEpoch()) +
                  u'|' + message.messageText;
        }
        bool deduplicated = false;
        if (!message.id.isEmpty())
        {
            deduplicated = seenIds.contains(message.id);
            seenIds.insert(message.id);
            if (!key.isEmpty())
            {
                seenIdentifiedKeys.insert(key);
                // Some sources retain the body and timestamp but lose the
                // original ID. Merge that legacy copy with the known ID in
                // either arrival order; distinct IDs remain distinct.
                if (seenLegacyKeys.remove(key))
                {
                    --result.messageCount;
                }
            }
        }
        else if (!key.isEmpty())
        {
            deduplicated = seenIdentifiedKeys.contains(key) ||
                           seenLegacyKeys.contains(key);
            if (!deduplicated)
            {
                seenLegacyKeys.insert(key);
            }
        }
        else
        {
            // No timestamp means matching by text would merge unrelated
            // messages. Count the same in-memory row only once.
            deduplicated = seenUndated.contains(&message);
            seenUndated.insert(&message);
        }

        if (message.serverReceivedTime.isValid() &&
            (!result.latestMessage.isValid() ||
             message.serverReceivedTime > result.latestMessage))
        {
            result.latestMessage = message.serverReceivedTime;
        }
        if (!deduplicated)
        {
            ++result.messageCount;
        }
    }
    return result;
}

}  // namespace chatterino
