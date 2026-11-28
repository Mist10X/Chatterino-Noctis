// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>

namespace chatterino {

/// Tracks messages handed to the authenticated write socket but not yet
/// answered by Twitch. Never retains message bodies or retries those writes.
class IrcWriteRecovery
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto CONFIRMATION_TIMEOUT = std::chrono::seconds(12);
    // Keep timed-out slots briefly to correlate delayed USERSTATE/echoes
    // with the old send, instead of accidentally consuming a newer one.
    static constexpr auto LATE_CONFIRMATION_RETENTION = std::chrono::seconds(60);

    void bindAccount(const QString &id)
    {
        this->account_ = id;
        this->pending_.clear();
    }

    void unbindAccount()
    {
        this->account_.reset();
        this->pending_.clear();
    }

    bool belongsTo(const QString &id) const
    {
        return this->account_ && *this->account_ == id;
    }

    bool canSend(const QString &id, bool loggedIn) const
    {
        return loggedIn && !id.isEmpty() && this->belongsTo(id);
    }

    void noteWrite(const QString &channel, Clock::time_point now,
                   const QString &nonce = {})
    {
        this->pending_.push_back({channel, now, nonce, {}, {}, false});
    }

    // A nonce echo on the live socket is exact evidence. Keep its pending
    // slot until USERSTATE arrives so a later ACK cannot consume another send.
    void noteEcho(const QString &channel, const QString &sender,
                  const QString &nonce, const QString &id)
    {
        if (!this->belongsTo(sender) || nonce.isEmpty() || id.isEmpty())
        {
            return;
        }
        for (auto &pending : this->pending_)
        {
            if (pending.channel.compare(channel, Qt::CaseInsensitive) == 0 &&
                pending.nonce == nonce)
            {
                // USERSTATE has an ID but may not have a nonce. If it arrived
                // first, provisionally associated with another pending send,
                // move that acknowledgement to the actual nonce-correlated
                // send. Otherwise that other send could appear confirmed.
                for (auto &other : this->pending_)
                {
                    if (&other != &pending && other.ackId == id &&
                        other.channel.compare(channel, Qt::CaseInsensitive) == 0)
                    {
                        std::swap(other.ackId, pending.ackId);
                        break;
                    }
                }
                pending.echoId = id;
                return;
            }
        }
    }

    bool noteResponse(const QString &channel, const QString &id = {})
    {
        const auto inChannel = [&channel](const auto &item) {
            return item.channel.compare(channel, Qt::CaseInsensitive) == 0;
        };
        if (!id.isEmpty())
        {
            for (auto &pending : this->pending_)
            {
                if (inChannel(pending) && pending.ackId == id)
                {
                    return false;
                }
            }
            for (auto &pending : this->pending_)
            {
                if (inChannel(pending) && pending.echoId == id)
                {
                    pending.ackId = id;
                    return !pending.timeoutReported;
                }
            }
        }
        const auto item = std::find_if(
            this->pending_.begin(), this->pending_.end(),
            [&inChannel](const auto &pending) {
                return inChannel(pending) && pending.echoId.isEmpty() &&
                       pending.ackId.isEmpty();
            });
        if (item != this->pending_.end())
        {
            const bool firstReport = !item->timeoutReported;
            if (id.isEmpty())
            {
                // Explicit rejection from Twitch; no resend.
                this->pending_.erase(item);
            }
            else
            {
                // Keep the nonce until the receive-side echo can correlate it.
                item->ackId = id;
            }
            return firstReport;
        }
        return false;
    }

    void pruneConfirmed(Clock::time_point now)
    {
        std::erase_if(this->pending_, [&](const auto &pending) {
            const auto age = now - pending.writtenAt;
            if (pending.timeoutReported)
            {
                return age >= LATE_CONFIRMATION_RETENTION;
            }
            return (!pending.echoId.isEmpty() || !pending.ackId.isEmpty()) &&
                   age >= CONFIRMATION_TIMEOUT;
        });
    }

    bool hasPending() const
    {
        return !this->pending_.empty();
    }

    bool expired(Clock::time_point now) const
    {
        return std::ranges::any_of(this->pending_, [&](const auto &pending) {
            return !pending.timeoutReported && pending.echoId.isEmpty() && pending.ackId.isEmpty() &&
                   now - pending.writtenAt >= CONFIRMATION_TIMEOUT;
        });
    }

    // Delivery uncertainty is not a transport failure. Report each expired
    // dispatch once, without discarding younger pending sends or replaying.
    QStringList takeExpiredUnconfirmedChannels(Clock::time_point now)
    {
        QStringList channels;
        for (auto &pending : this->pending_)
        {
            if (pending.timeoutReported || !pending.echoId.isEmpty() ||
                !pending.ackId.isEmpty() ||
                now - pending.writtenAt < CONFIRMATION_TIMEOUT)
            {
                continue;
            }
            pending.timeoutReported = true;
            if (!channels.contains(pending.channel, Qt::CaseInsensitive))
            {
                channels.push_back(pending.channel);
            }
        }
        return channels;
    }

    QStringList takeUnconfirmedChannels()
    {
        QStringList channels;
        for (const auto &pending : this->pending_)
        {
            if (!pending.timeoutReported && pending.echoId.isEmpty() && pending.ackId.isEmpty() &&
                !channels.contains(pending.channel, Qt::CaseInsensitive))
            {
                channels.push_back(pending.channel);
            }
        }
        this->pending_.clear();
        return channels;
    }

private:
    struct Pending {
        QString channel;
        Clock::time_point writtenAt;
        QString nonce;
        QString echoId;
        QString ackId;
        bool timeoutReported;
    };
    std::optional<QString> account_;
    std::deque<Pending> pending_;
};

}  // namespace chatterino
