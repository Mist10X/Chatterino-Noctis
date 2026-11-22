#pragma once

#include <QString>

#include <algorithm>
#include <chrono>
#include <deque>

namespace chatterino {

// Passive, bounded evidence from the live read socket. Never stores text or
// issues a send. An echo can precede the write-side/HTTP acknowledgement.
class ChatDeliveryEvidence
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto RETENTION = std::chrono::seconds(60);

    void watch(const QString &sender, Clock::time_point now)
    {
        this->prune(now);
        std::erase_if(this->watched_, [&](const auto &item) {
            return item.sender == sender;
        });
        this->watched_.push_back({sender, now});
        if (this->watched_.size() > 32)
        {
            this->watched_.pop_front();
        }
    }

    void observe(const QString &sender, const QString &id, Clock::time_point now)
    {
        if (id.isEmpty() || this->watched_.empty())
        {
            return;
        }
        this->prune(now);
        if (!std::ranges::any_of(this->watched_, [&](const auto &item) {
                return item.sender == sender;
            }))
        {
            return;
        }
        this->echoes_.push_back({sender, id, now});
        if (this->echoes_.size() > 256)
        {
            this->echoes_.pop_front();
        }
    }

    bool seen(const QString &sender, const QString &id, Clock::time_point now)
    {
        this->prune(now);
        return !id.isEmpty() &&
               std::ranges::any_of(this->echoes_, [&](const auto &item) {
                   return item.sender == sender && item.id == id;
               });
    }

private:
    struct Watch {
        QString sender;
        Clock::time_point at;
    };
    struct Echo {
        QString sender;
        QString id;
        Clock::time_point at;
    };
    std::deque<Watch> watched_;
    std::deque<Echo> echoes_;

    void prune(Clock::time_point now)
    {
        while (!this->watched_.empty() &&
               now - this->watched_.front().at >= RETENTION)
        {
            this->watched_.pop_front();
        }
        while (!this->echoes_.empty() &&
               now - this->echoes_.front().at >= RETENTION)
        {
            this->echoes_.pop_front();
        }
    }
};
}  // namespace chatterino
