// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/recentmessages/Api.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/network/ServiceHealth.hpp"
#include "common/QLogging.hpp"
#include "common/UiLanguage.hpp"
#include "messages/Message.hpp"
#include "providers/recentmessages/Impl.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <set>

namespace chatterino::recentmessages {

using namespace recentmessages::detail;

namespace {

using Time = std::chrono::system_clock::time_point;
constexpr qint64 CheckGapMs = 5 * 60 * 1000;
constexpr qint64 MaxLookbackMs = 24 * 60 * 60 * 1000;
constexpr qsizetype MaxRawBytes = 8 * 1024 * 1024;

struct Window {
    qint64 from;
    qint64 to;
};

struct RequestState {
    QString name;
    std::weak_ptr<Channel> channel;
    ResultCallback loaded;
    ErrorCallback failed;
    QUrl primary;
    QString backup;
    QString raw;
    std::optional<Time> after;
    qint64 cutoff;
    int limit;
    bool partial = false;
    bool rmAnswered = false;
    std::set<qint64> timestamps;
    QSet<QString> ids;
    std::optional<Window> search;
};

using State = std::shared_ptr<RequestState>;

qint64 milliseconds(Time time)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

Time timePoint(qint64 ms)
{
    return Time(std::chrono::milliseconds(ms));
}

qint64 serverTime(const Communi::IrcMessage &row)
{
    const auto tags = row.tags();
    const auto sent = tags.getOrEmpty("tmi-sent-ts").toLongLong();
    return sent > 0 ? sent : tags.getOrEmpty("rm-received-ts").toLongLong();
}

bool isChat(const Communi::IrcMessage &row, const QString &name)
{
    return row.command() == QStringLiteral("PRIVMSG") &&
           row.parameter(0).compare(QStringLiteral("#") + name, Qt::CaseInsensitive) == 0 &&
           !row.tags().getOrEmpty("id").isEmpty();
}

Window allowedWindow(const RequestState &state)
{
    const auto earliest = state.cutoff - MaxLookbackMs;
    return {state.after ? std::max(milliseconds(*state.after), earliest) : earliest,
            state.cutoff};
}

// Fast channels can lose hundreds of lines in a minute. Estimate their usual
// cadence only from real messages, without including startup/reconnect bounds.
qint64 verificationGap(const std::set<qint64> &times, Window bounds)
{
    auto it = times.upper_bound(bounds.from);
    if (it == times.end() || *it >= bounds.to)
        return CheckGapMs;
    auto previous = *it++;
    std::vector<qint64> intervals;
    for (; it != times.end() && *it < bounds.to; ++it)
    {
        intervals.push_back(*it - previous);
        previous = *it;
    }
    if (intervals.size() < 20)
        return CheckGapMs;
    const auto median = intervals.begin() + intervals.size() / 2;
    std::nth_element(intervals.begin(), median, intervals.end());
    return std::clamp<qint64>(*median * 10, 60000, CheckGapMs);
}

// A silence warrants verification, never a warning by itself. Check internal
// gaps and the tail; a page's truncated startup head is not a gap.
std::optional<Window> suspectWindow(const RequestState &state)
{
    const auto bounds = allowedWindow(state);
    const auto minGap = verificationGap(state.timestamps, bounds);
    std::optional<Window> missing;
    std::optional<qint64> previous;
    if (state.after || (!state.timestamps.empty() && *state.timestamps.begin() <= bounds.from))
        previous = bounds.from;
    const auto observe = [&](qint64 at) {
        if (previous && at - *previous >= minGap)
        {
            if (missing)
                missing->to = at;
            else
                missing = Window{*previous, at};
        }
        previous = at;
    };
    for (auto at : state.timestamps)
        if (at > bounds.from && at < bounds.to)
            observe(at);
    observe(bounds.to);
    return missing;
}

void warnPartial(const ChannelPtr &channel)
{
    channel->addSystemMessage(uiText("Message history service recovering, there may be gaps in the message history."));
}

ChannelPtr active(const State &state)
{
    if (isAppAboutToQuit())
        return {};
    auto channel = state->channel.lock();
    if (channel && !getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        state->failed();
        return {};
    }
    return channel;
}

void deliverRaw(const State &state, const std::shared_ptr<QJsonArray> &lines, qsizetype offset)
{
    auto channel = active(state);
    if (!channel)
        return;
    QJsonArray batch;
    const auto end = std::min(lines->size(), offset + 48);
    for (; offset < end; ++offset)
        batch.append(lines->at(offset));
    auto rows = parseRecentMessages({{"messages", batch}});
    auto messages = buildRecentMessages(rows, channel.get(), false);
    const bool last = end == lines->size();
    state->loaded(messages, true, last);
    if (!last)
        QTimer::singleShot(0, [state, lines, end] { deliverRaw(state, lines, end); });
}

// Parse on a network worker. Mirrors may ignore order, bounds or row limit.
// Carry moderation information across delivery slices.
QJsonArray rawRows(const QByteArray &body, const QString &name, Window range,
                   int limit, QSet<QString> seen)
{
    struct Row {
        qint64 at;
        QString id;
        QString uid;
        QByteArray data;
    };
    std::vector<Row> valid;
    QSet<QString> deleted;
    QHash<QString, qint64> clearedUsers;
    qint64 clearedAll = 0;
    if (body.size() > MaxRawBytes)
        return {};
    qsizetype start = 0;
    while (start < body.size())
    {
        auto newline = body.indexOf('\n', start);
        if (newline < 0)
            newline = body.size();
        auto line = QByteArrayView(body).sliced(start, newline - start);
        if (line.endsWith('\r'))
            line.chop(1);
        start = newline + 1;
        if (!line.startsWith('@') || line.size() > 16384)
            continue;
        auto data = line.toByteArray();
        std::unique_ptr<Communi::IrcMessage> row(Communi::IrcMessage::fromData(data, nullptr));
        if (!row || row->parameter(0).compare(QStringLiteral("#") + name, Qt::CaseInsensitive) != 0)
            continue;
        const auto tags = row->tags();
        const auto at = serverTime(*row);
        if (row->command() == QStringLiteral("CLEARMSG"))
        {
            deleted.insert(tags.getOrEmpty("target-msg-id"));
            continue;
        }
        if (row->command() == QStringLiteral("CLEARCHAT"))
        {
            const auto uid = tags.getOrEmpty("target-user-id");
            if (!uid.isEmpty())
                clearedUsers[uid] = std::max(clearedUsers.value(uid), at);
            else if (row->parameters().size() == 1)
                clearedAll = std::max(clearedAll, at);
            continue;
        }
        if (!isChat(*row, name) || at <= range.from || at >= range.to)
            continue;
        const auto id = tags.getOrEmpty("id");
        if (seen.contains(id))
            continue;
        seen.insert(id);
        // Append canonical tags last. Raw chat must never trigger live IRC logic.
        const auto tagEnd = data.indexOf(' ');
        data.insert(tagEnd, ";historical=1;rm-received-ts=" + QByteArray::number(at));
        valid.push_back({at, id, tags.getOrEmpty("user-id"), std::move(data)});
    }
    std::stable_sort(valid.begin(), valid.end(), [](const Row &a, const Row &b) {
        return a.at < b.at;
    });
    QJsonArray lines;
    const auto first = valid.size() > size_t(limit) ? valid.size() - size_t(limit) : 0;
    for (size_t i = first; i < valid.size(); ++i)
    {
        auto &row = valid[i];
        if (deleted.contains(row.id) || row.at <= clearedAll || row.at <= clearedUsers.value(row.uid))
            row.data.insert(1, "rm-deleted=1;");
        lines.append(QString::fromUtf8(row.data));
    }
    return lines;
}

void requestRaw(const State &state, Window range)
{
    if (!active(state))
        return;
    QUrl url(state->raw.arg(state->name));
    QUrlQuery query(url);
    query.addQueryItem("raw", {});
    query.addQueryItem("reverse", "true");
    query.addQueryItem("limit", QString::number(state->limit));
    const auto utc = [](qint64 ms) {
        return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc()).toString(Qt::ISODateWithMs);
    };
    query.addQueryItem("from", utc(range.from));
    query.addQueryItem("to", utc(range.to));
    url.setQuery(query);
    const auto seen = state->ids;
    NetworkRequest(url).timeout(8000).followRedirects(false)
        .header("Accept", "text/plain").concurrent()
        .onSuccess([state, range, seen](const NetworkResult &result) {
            auto lines = std::make_shared<QJsonArray>(
                rawRows(result.getData(), state->name, range, state->limit, seen));
            postToThread([state, lines] {
                auto channel = active(state);
                if (!channel)
                    return;
                if (lines->empty() && state->partial)
                    warnPartial(channel);
                deliverRaw(state, lines, 0);
            });
        })
        .onError([state](const NetworkResult &result) {
            postToThread([state, result] {
                if (auto channel = active(state))
                {
                    if (!state->rmAnswered)
                        servicehealth::failed(servicehealth::History,
                            constructRecentMessagesUrl(state->name, 1, {}, {}), result);
                    if (state->partial)
                        warnPartial(channel);
                    state->failed();
                }
            });
        }).execute();
}

void requestHistory(const State &state, bool backup);

void handleResponse(const State &state, bool backup, bool success, const NetworkResult &result)
{
    auto channel = active(state);
    if (!channel)
        return;
    const auto status = result.status().value_or(0);
    const auto root = result.parseJson();
    const auto error = root.value("error_code").toString();
    const bool partial = error == QStringLiteral("channel_not_joined");
    const bool refused = status == 401 || status == 403 || status == 429 ||
                         (!error.isEmpty() && !partial);
    // A backup 404 without an opt-out only says that mirror lacks the
    // channel. It can still be present on the independent interval source.
    const bool absentOnBackup = backup && status == 404 && error.isEmpty();
    if (refused || (!success && !partial && !absentOnBackup && !servicehealth::relevant(result)))
    {
        if (!backup)
            channel->addSystemMessage(uiText("Message history service unavailable (Error: %1)")
                                          .arg(error.isEmpty() ? result.formatError() : error));
        else if (state->partial)
            warnPartial(channel);
        state->failed();
        return;
    }
    const bool valid = (success || partial) && root.value("messages").isArray();
    if (valid)
    {
        state->rmAnswered = true;
        servicehealth::success(servicehealth::History);
    }
    auto parsed = valid ? parseRecentMessages(root) : std::vector<Communi::IrcMessage *>{};
    const auto range = state->search.value_or(allowedWindow(*state));
    QSet<QString> batchIds;
    std::erase_if(parsed, [&](auto *row) {
        if (!row)
            return true;
        const auto at = serverTime(*row);
        const bool chat = isChat(*row, state->name);
        const bool wrongChannel = row->parameter(0).startsWith(u'#') &&
            row->parameter(0).mid(1).compare(state->name, Qt::CaseInsensitive) != 0;
        const bool outsideReconnect = state->after && at > 0 &&
            (at <= milliseconds(*state->after) || at >= state->cutoff);
        bool discard = wrongChannel || outsideReconnect ||
            (backup && (!chat || at <= range.from || at >= range.to));
        if (chat)
        {
            const auto id = row->tags().getOrEmpty("id");
            discard |= at <= 0 || at >= state->cutoff || batchIds.contains(id) || state->ids.contains(id);
            if (!discard)
            {
                batchIds.insert(id);
                row->setTag(QStringLiteral("historical"), QStringLiteral("1"));
                row->setTag(QStringLiteral("rm-received-ts"), QString::number(at));
            }
        }
        if (discard)
            row->deleteLater();
        return discard;
    });
    if (backup)
    {
        // Some mirrors disregard the query limit and the direction. Keep
        // the most recent matching rows before constructing rich messages.
        std::stable_sort(parsed.begin(), parsed.end(), [](const auto *a, const auto *b) {
            return serverTime(*a) < serverTime(*b);
        });
        if (parsed.size() > size_t(state->limit))
        {
            const auto keep = parsed.end() - state->limit;
            for (auto it = parsed.begin(); it != keep; ++it)
                (*it)->deleteLater();
            parsed.erase(parsed.begin(), keep);
        }
    }
    const auto initialCount = state->ids.size();
    for (const auto *row : parsed)
    {
        if (isChat(*row, state->name))
        {
            state->ids.insert(row->tags().getOrEmpty("id"));
            state->timestamps.insert(serverTime(*row));
        }
    }
    // Live IRC can win the race with either HTTP response. Preserve coverage
    // evidence, but skip second copies before parsing and highlighting.
    QSet<QString> onScreen;
    for (const auto &message : channel->getMessageSnapshot())
        if (!message->id.isEmpty())
            onScreen.insert(message->id);
    std::erase_if(parsed, [&](auto *row) {
        if (!onScreen.contains(row->tags().getOrEmpty("id")))
            return false;
        row->deleteLater();
        return true;
    });
    auto messages = buildRecentMessages(parsed, channel.get(), !backup);
    if (backup)
        std::erase_if(messages, [](const auto &m) {
            return m->id.isEmpty() || !m->serverReceivedTime.isValid();
        });
    auto missing = suspectWindow(*state);
    if (backup && missing && state->search)
    {
        missing->from = std::max(missing->from, state->search->from);
        missing->to = std::min(missing->to, state->search->to);
        if (missing->from >= missing->to)
            missing.reset();
    }
    // JOIN/ROOMSTATE/NOTICE may occupy an entire apparently successful page.
    // Those events say nothing about coverage of actual chat messages.
    const bool noChat = state->timestamps.empty();
    const bool added = state->ids.size() > initialCount;
    // An optional source failing to confirm a lull cannot turn a successful
    // primary response into a known outage. Keep that separate from deciding
    // whether the final available source is worth asking.
    state->partial = backup ? state->partial && (partial || !valid || !added)
                            : partial || !valid;
    const bool verify = partial || !valid || state->partial || missing || noChat;
    const bool tryBackup = !backup && verify && !state->backup.isEmpty();
    const bool tryRaw = verify && !tryBackup && !state->raw.isEmpty();
    qCDebug(chatterinoRecentMessages) << "Recent history" << state->name
        << (backup ? "backup" : "primary") << "rows" << messages.size()
        << "partial" << state->partial << "checking another source" << (tryBackup || tryRaw);
    if (tryBackup || tryRaw)
    {
        state->loaded(messages, backup, false);
        if (!backup)
            state->search = missing;
        if (tryBackup)
            QTimer::singleShot(0, [state] { requestHistory(state, true); });
        else
        {
            const auto target = missing.value_or(state->search.value_or(allowedWindow(*state)));
            QTimer::singleShot(0, [state, target] { requestRaw(state, target); });
        }
        return;
    }
    if (!state->rmAnswered && !success)
        servicehealth::failed(servicehealth::History, backup
            ? constructRecentMessagesUrl(state->name, 1, {}, {}, state->backup)
            : constructRecentMessagesUrl(state->name, 1, {}, {}), result);
    if (state->partial)
        warnPartial(channel);
    state->loaded(messages, backup, true);
}

void requestHistory(const State &state, bool backup)
{
    if (!active(state))
        return;
    const auto after = backup && state->search
        ? std::optional<Time>(timePoint(state->search->from)) : state->after;
    const auto before = timePoint(state->search ? state->search->to : state->cutoff);
    const auto url = backup
        ? constructRecentMessagesUrl(state->name, state->limit, after, before, state->backup)
        : state->primary;
    NetworkRequest(url).timeout(8000)
        .onSuccess([state, backup](const NetworkResult &result) {
            postToThread([state, backup, result] {
                handleResponse(state, backup, true, result);
            });
        })
        .onError([state, backup](const NetworkResult &result) {
            postToThread([state, backup, result] {
                handleResponse(state, backup, false, result);
            });
        }).execute();
}

}  // namespace

void load(const QString &channelName, std::weak_ptr<Channel> channelPtr,
          ResultCallback onLoaded, ErrorCallback onError, int limit,
          std::optional<Time> after, std::optional<Time> before, bool jitter)
{
    auto state = std::make_shared<RequestState>();
    state->name = channelName;
    state->channel = std::move(channelPtr);
    state->loaded = std::move(onLoaded);
    state->failed = std::move(onError);
    state->limit = std::clamp(limit, 1, 10000);
    state->primary = constructRecentMessagesUrl(channelName, state->limit, after, before);
    state->after = after;
    state->cutoff = milliseconds(before.value_or(std::chrono::system_clock::now()));
    const auto alternative = fallbackRecentMessagesApiUrlTemplate();
    if (alternative != recentMessagesApiUrlTemplate())
        state->backup = alternative;
    // Custom primaries never contact unspecified production mirrors.
    state->raw = qEnvironmentVariableIsEmpty("CHATTERINO2_RECENT_MESSAGES_URL")
        ? QStringLiteral("https://logs.zonian.dev/channel/%1")
        : qEnvironmentVariable("CHATTERINO2_RECENT_MESSAGES_RAW_URL");
    const int delayMs = jitter ? std::rand() % 100 : 0;
    QTimer::singleShot(delayMs, [state] { requestHistory(state, false); });
}

}  // namespace chatterino::recentmessages
