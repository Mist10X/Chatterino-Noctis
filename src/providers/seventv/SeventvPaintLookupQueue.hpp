#pragma once

#include <QString>

#include <array>
#include <iterator>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace chatterino::seventv {

enum class PaintLookupPriority : size_t { History, Live, Own };

// Access is serialized by the provider. Promote a pending historical lookup
// rather than adding a second request. In-flight users cannot be enqueued again.
class PaintLookupQueue
{
public:
    struct Entry {
        QString login;
        QString userID;
        PaintLookupPriority priority;
    };

    static constexpr size_t capacity = 2000;
    static constexpr size_t batchSize = 10;

    static constexpr qint64 refreshAfter(PaintLookupPriority priority,
                                         bool hasPaint)
    {
        if (priority == PaintLookupPriority::Own)
        {
            return 20;
        }
        if (priority == PaintLookupPriority::Live)
        {
            return hasPaint ? 30 : 120;
        }
        return 300;
    }

    bool enqueue(const QString &login, const QString &userID,
                 PaintLookupPriority priority)
    {
        if (this->inFlight_.contains(login))
        {
            return false;
        }
        const auto index = static_cast<size_t>(priority);
        if (auto it = this->pending_.find(login); it != this->pending_.end())
        {
            const auto previous = static_cast<size_t>(it->second->priority);
            if (previous >= index)
            {
                return false;
            }
            it->second->priority = priority;
            this->queues_[index].splice(this->queues_[index].end(),
                                       this->queues_[previous], it->second);
            return true;
        }
        if (this->pending_.size() >= capacity)
        {
            size_t lower = 0;
            while (lower < index && this->queues_[lower].empty())
            {
                ++lower;
            }
            if (lower >= index)
            {
                return false;
            }
            auto &queue = this->queues_[lower];
            this->pending_.erase(queue.back().login);
            queue.pop_back();
        }
        auto &queue = this->queues_[index];
        queue.push_back({login, userID, priority});
        this->pending_.emplace(login, std::prev(queue.end()));
        return true;
    }

    bool contains(const QString &login) const
    {
        return this->pending_.contains(login) || this->inFlight_.contains(login);
    }

    bool empty() const
    {
        return this->pending_.empty();
    }

    std::vector<Entry> takeBatch()
    {
        std::vector<Entry> batch;
        batch.reserve(batchSize);
        auto take = [&](size_t index, size_t limit) {
            auto &queue = this->queues_[index];
            while (!queue.empty() && batch.size() < limit)
            {
                auto &entry = queue.front();
                this->inFlight_.insert(entry.login);
                this->pending_.erase(entry.login);
                batch.push_back(std::move(entry));
                queue.pop_front();
            }
        };
        // Own, then live chat; leave two slots for history so it cannot starve.
        take(2, batchSize);
        take(1, this->queues_[0].empty() ? batchSize : batchSize - 2);
        take(0, batchSize);
        take(1, batchSize);
        return batch;
    }

    void finish(const std::vector<Entry> &batch)
    {
        for (const auto &entry : batch)
        {
            this->inFlight_.erase(entry.login);
        }
    }

private:
    std::array<std::list<Entry>, 3> queues_;
    std::unordered_map<QString, std::list<Entry>::iterator> pending_;
    std::unordered_set<QString> inFlight_;
};

// Track only the current batch. Accessed under the same lock as paint maps.
class PaintLookupEvents
{
public:
    void begin(const std::vector<PaintLookupQueue::Entry> &batch)
    {
        this->superseded_.clear();
        for (const auto &entry : batch)
        {
            this->superseded_.emplace(entry.login, false);
        }
    }

    bool accept(const QString &login, bool fromLookup)
    {
        const auto it = this->superseded_.find(login);
        if (it == this->superseded_.end())
        {
            return true;
        }
        if (fromLookup)
        {
            return !it->second;
        }
        it->second = true;
        return true;
    }

    void finish()
    {
        this->superseded_.clear();
    }

private:
    std::unordered_map<QString, bool> superseded_;
};

}  // namespace chatterino::seventv
