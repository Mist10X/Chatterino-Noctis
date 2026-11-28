// SPDX-License-Identifier: MIT
#pragma once

#include <QString>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <optional>

namespace chatterino {

// Twitch's authenticated JOIN limit belongs to the account, not the socket.
// Keep spent capacity across a reconnect or a quick switch back, but never
// retain pending JOINs from a retired login/connection.
class TwitchReadJoinLimiter
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TwitchReadJoinLimiter(size_t capacity, std::chrono::milliseconds cooldown)
        : capacity_(capacity)
        , cooldown_(cooldown)
    {
    }

    void bind(QString identity)
    {
        this->pending_.clear();
        this->identity_ = std::move(identity);
    }

    bool belongsTo(const QString &identity) const
    {
        return this->identity_ && *this->identity_ == identity;
    }

    void clearPending()
    {
        this->pending_.clear();
    }

    void enqueue(QString channel)
    {
        if (std::ranges::find(this->pending_, channel) == this->pending_.end())
        {
            this->pending_.push_back(std::move(channel));
        }
    }

    void remove(const QString &channel)
    {
        std::erase(this->pending_, channel);
    }

    std::optional<QString> takeNext(TimePoint now)
    {
        const auto delay = this->nextDelay(now);
        if (!delay || delay->count() != 0)
        {
            return std::nullopt;
        }
        auto channel = std::move(this->pending_.front());
        this->pending_.pop_front();
        this->spent_[*this->identity_].push_back(now);
        return channel;
    }

    std::optional<std::chrono::milliseconds> nextDelay(TimePoint now)
    {
        if (!this->identity_ || this->pending_.empty() || this->capacity_ == 0)
        {
            return std::nullopt;
        }
        // Also retire expired windows of previously used accounts.
        for (auto it = this->spent_.begin(); it != this->spent_.end();)
        {
            auto &times = it->second;
            while (!times.empty() && times.front() + this->cooldown_ <= now)
            {
                times.pop_front();
            }
            if (times.empty())
            {
                it = this->spent_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        const auto it = this->spent_.find(*this->identity_);
        if (it == this->spent_.end() || it->second.size() < this->capacity_)
        {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::ceil<std::chrono::milliseconds>(
            it->second.front() + this->cooldown_ - now);
    }

private:
    size_t capacity_;
    std::chrono::milliseconds cooldown_;
    std::optional<QString> identity_;
    std::map<QString, std::deque<TimePoint>> spent_;
    std::deque<QString> pending_;
};

}  // namespace chatterino
