#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>
#include <utility>

namespace chatterino::repeatcommand {

inline constexpr int MaxMessages = 10;
// The normal moderator send limiter accepts messages at least 100 ms apart.
// A precise single-shot timer respects the normal sender's minimum.
inline constexpr int IntervalMs = 100;

inline bool validTrigger(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("\\A![\\p{L}\\p{N}_-]{1,64}\\z"));
    return pattern.match(text.trimmed()).hasMatch();
}

struct Invocation {
    QString command;
    int count;
};

inline std::optional<Invocation> parse(const QString &text)
{
    if (!text.startsWith(u'!'))
        return {};
    const auto fields = text.simplified().split(u' ');
    if (fields.size() != 2 || !validTrigger(fields.front()))
        return {};
    static const QRegularExpression countPattern(QStringLiteral("\\A(?:[1-9]|10)\\z"));
    if (!countPattern.match(fields.back()).hasMatch())
        return {};
    return Invocation{fields.front().toCaseFolded(), fields.back().toInt()};
}

inline bool matches(const QStringList &triggers, const Invocation &invocation)
{
    for (const auto &trigger : triggers)
        if (trigger.trimmed().toCaseFolded() == invocation.command)
            return true;
    return false;
}

inline bool validOutput(const QString &text)
{
    // A repeat's own output must never be a new invocation.
    return !text.trimmed().startsWith(u'!');
}

inline bool ownsCommand(const QString &sender, const QString &selected,
                        const QString &configured, bool moderator)
{
    return moderator && !sender.isEmpty() && sender == selected &&
           (configured.isEmpty() || configured == sender);
}

// One active batch per local client. Each step goes through the ordinary
// sender's authorization and rate limiter; failure cancels the rest.
class Batch
{
public:
    Batch()
    {
        timer_.setInterval(IntervalMs);
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { tick(); });
    }

    bool active() const { return remaining_ > 0 || running_; }

    bool start(int count, std::function<bool()> attempt)
    {
        if (active() || count < 1 || count > MaxMessages || !attempt)
            return false;
        remaining_ = count;
        attempt_ = std::move(attempt);
        // Start after the triggering chat message has consumed its normal
        // send slot; never bypass the standard channel/account limiter.
        arm();
        return true;
    }

    void cancel()
    {
        timer_.stop();
        remaining_ = 0;
        attempt_ = {};
    }

private:
    void arm()
    {
        notBefore_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(IntervalMs);
        timer_.start(IntervalMs);
    }

    void tick()
    {
        if (running_ || remaining_ <= 0)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now < notBefore_)
        {
            timer_.start(static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(notBefore_ - now).count()));
            return;
        }
        running_ = true;
        --remaining_;
        const auto callback = attempt_;
        const bool accepted = callback && callback();
        running_ = false;
        if (!accepted || remaining_ == 0)
            cancel();
        else
            arm();
    }

    QTimer timer_;
    std::chrono::steady_clock::time_point notBefore_;
    int remaining_ = 0;
    bool running_ = false;
    std::function<bool()> attempt_;
};

}  // namespace chatterino::repeatcommand
