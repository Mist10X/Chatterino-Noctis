// SPDX-License-Identifier: MIT
#pragma once

#include <QDateTime>
#include <QString>

#include <algorithm>
#include <functional>
#include <iterator>

namespace chatterino::channelpoints {

inline constexpr int BackgroundPollTickMs = 5000;

inline bool backgroundRefreshDue(const QDateTime &now,
                                 const QDateTime &lastObservation,
                                 const QDateTime &lastAttempt, bool inFlight)
{
    const auto due = [&now](const QDateTime &previous, qint64 intervalMs) {
        if (!now.isValid() || !previous.isValid())
        {
            return true;
        }
        const auto elapsed = previous.msecsTo(now);
        return elapsed < 0 || elapsed >= intervalMs;
    };
    return !inFlight &&
           due(lastObservation, 5 * 60 * 1000) &&
           due(lastAttempt, 2 * 60 * 1000);
}

// Channels must be sorted in ascending key order for this tick. Keep only
// the last started key between ticks, even when that channel has disappeared.
// A false tryStart means the channel is not eligible; continue looking until
// exactly one refresh starts, or every current channel has been checked.
template <typename Range, typename Key, typename TryStart>
bool pollNextChannel(const Range &sortedChannels, QString &lastStartedKey,
                     Key key, TryStart tryStart)
{
    const auto first = std::begin(sortedChannels);
    const auto last = std::end(sortedChannels);
    const auto next = std::upper_bound(
        first, last, lastStartedKey,
        [&key](const QString &previous, const auto &channel) {
            return previous < std::invoke(key, channel);
        });
    const auto scan = [&](auto begin, auto end) {
        for (auto it = begin; it != end; ++it)
        {
            const QString candidateKey = std::invoke(key, *it);
            if (std::invoke(tryStart, *it))
            {
                lastStartedKey = candidateKey;
                return true;
            }
        }
        return false;
    };
    return scan(next, last) || scan(first, next);
}

}  // namespace chatterino::channelpoints
