// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/ChannelStats.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "singletons/Paths.hpp"
#include "util/PostToThread.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QSet>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThreadPool>
#include <QTimer>
#include <QTimeZone>

#include <optional>
#include <unordered_map>
#include <vector>

namespace chatterino {

namespace {

/// Keeps the per-minute counts and the new chatters list bounded; a stream
/// longer than this is counted for its last day.
constexpr qint64 KEEP_MINUTES = 24 * 60;
constexpr int KEEP_NEW_CHATTERS = 500;
constexpr int MAX_EVENTS = 500000;
/// A shorter stretch before the first message in the chat is not worth
/// asking the logs for.
constexpr qint64 MIN_GAP_MS = 60 * 1000;
/// The logs are read in pieces of this length (a busy chat writes ~10 MB/h).
constexpr qint64 WINDOW_MS = 5 * 60 * 1000;
constexpr int LOG_TIMEOUT_MS = 60000;


/// rustlog mirrors, fastest first (the same ones the usercard reads)
const QStringList LOG_MIRRORS{
    QStringLiteral("https://logs.twitchmetrics.xyz"),
    QStringLiteral("https://logs.zonian.dev"),
    QStringLiteral("https://logs.spanix.team"),
};

/// The USERNOTICE kinds the chat marks as subscriptions (as MessageBuilder).
const QSet<QByteArray> SUB_NOTICES{"sub", "resub", "subgift", "anonsubgift"};

/// One counted thing, from the chat or from the logs.
struct Item {
    enum class Kind {
        Chat,
        Subscription,
        Moderation,
    } kind = Kind::Chat;
    QString id;
    QString login;
    QString name;
    QColor color;
    qint64 ms = 0;
    bool first = false;
    std::vector<EmotePtr> emotes;
    /// from the logs: Twitch emotes (id, name), made into emotes in the GUI
    /// thread
    std::vector<std::pair<QString, QString>> twitchEmotes;
    QStringList cachedEmotes;
};

struct Tracked {
    std::weak_ptr<Channel> channel;
    std::unique_ptr<pajlada::Signals::ScopedConnection> appended;
    std::unique_ptr<pajlada::Signals::ScopedConnection> prepended;
    std::unique_ptr<pajlada::Signals::ScopedConnection> filled;
    std::unique_ptr<pajlada::Signals::ScopedConnection> status;
    /// the stream the numbers are for (empty while offline)
    QString streamId;
    /// ids of the messages already counted: the logs repeat the chat
    QSet<QString> seen;
    /// bumped when the counting starts anew; older log answers are dropped
    int generation = 0;
    /// some mirror had this channel's messages
    bool logsAnswered = false;
    bool historyIncomplete = false;
    qint64 moderationLogUntil = 0;
    QString cachePath;
    QByteArray cachePending;
    bool cacheLoading = false;
    ChannelStatsData data;
};

std::unordered_map<QString, Tracked> &tracked()
{
    // Never destroyed: at exit the channels (and their signals) are gone
    // before static objects, so dropping the connections then would touch
    // freed memory.
    static auto *map = new std::unordered_map<QString, Tracked>;
    return *map;
}

QString statsKey(const QString &login, bool anonymous)
{
    return anonymous ? login.toLower() + u":anonymous" : login.toLower();
}

int nextGeneration()
{
    static int next = 0;
    return ++next;
}

Tracked *find(const QString &name, int generation)
{
    auto it = tracked().find(name);
    if (it == tracked().end() || it->second.generation != generation || it->second.channel.expired())
    {
        return nullptr;
    }
    return &it->second;
}

// Only deduplicable chat records go into the local stream journal. It contains
// statistics (not message bodies) and survives restarts independently of mirrors.
QByteArray encodeItem(const Item &item)
{
    QJsonArray emotes;
    for (const auto &emote : item.emotes)
        emotes.append(emote->name.string);
    for (const auto &name : item.cachedEmotes)
        emotes.append(name);
    return QJsonDocument(QJsonArray{item.id, item.login, item.name,
        item.color.isValid() ? item.color.name() : QString(), item.ms,
        item.first, emotes}).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<Item> decodeItem(const QByteArray &line)
{
    const auto values = QJsonDocument::fromJson(line).array();
    if (values.size() != 7 || values[0].toString().isEmpty() ||
        values[1].toString().isEmpty())
        return std::nullopt;
    Item item;
    item.id = values[0].toString();
    item.login = values[1].toString();
    item.name = values[2].toString();
    item.color = QColor(values[3].toString());
    item.ms = values[4].toInteger();
    item.first = values[5].toBool();
    for (const auto &value : values[6].toArray())
        item.cachedEmotes.append(value.toString());
    return item;
}

void flushCache(Tracked &entry)
{
    if (entry.cachePath.isEmpty() || entry.cacheLoading || entry.cachePending.isEmpty())
        return;
    QFile file(entry.cachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    const auto bytes = file.write(entry.cachePending);
    if (bytes > 0)
        entry.cachePending.remove(0, bytes);
    file.flush();
}

void add(Tracked &entry, const Item &item, bool persist = true)
{
    auto &data = entry.data;
    if (data.sinceStreamStart && item.ms < data.since.toMSecsSinceEpoch())
        return;
    if (entry.seen.size() >= MAX_EVENTS || data.messages >= MAX_EVENTS)
    {
        data.countLimitReached = true;
        return;
    }
    if (!item.id.isEmpty())
    {
        if (entry.seen.contains(item.id))
        {
            // Cached names can initially have no image. A matching live/log
            // copy may provide its image without recounting the message.
            for (const auto &emote : item.emotes)
            {
                if (auto it = data.emotes.find(emote->name.string);
                    it != data.emotes.end() && !it->emote)
                    it->emote = emote;
            }
            return;
        }
        entry.seen.insert(item.id);
    }
    if (item.kind == Item::Kind::Subscription)
    {
        ++data.subscriptions;
        return;
    }
    if (item.kind == Item::Kind::Moderation)
    {
        ++data.moderation;
        return;
    }

    ++data.messages;
    if (persist && !entry.cachePath.isEmpty() && !item.id.isEmpty())
        entry.cachePending.append(encodeItem(item));
    auto &chatter = data.chatters[item.login];
    if (chatter.name.isEmpty() || !item.name.isEmpty())
    {
        chatter.name = item.name.isEmpty() ? item.login : item.name;
    }
    if (item.color.isValid())
    {
        chatter.color = item.color;
    }
    ++chatter.messages;
    if (item.first && !data.newChatters.contains(item.login))
    {
        ++data.firstMessages;
        if (data.newChatters.size() < KEEP_NEW_CHATTERS)
        {
            data.newChatters.append(item.login);
        }
    }

    const auto minute = item.ms / 60000;
    ++data.perMinute[minute];
    data.incompleteMinutes.remove(minute);
    while (!data.perMinute.isEmpty() &&
           data.perMinute.lastKey() - data.perMinute.firstKey() > KEEP_MINUTES)
    {
        data.perMinute.erase(data.perMinute.begin());
    }

    for (const auto &emote : item.emotes)
    {
        auto &use = data.emotes[emote->name.string];
        if (!use.emote)
        {
            use.emote = emote;
        }
        ++use.uses;
    }
    for (const auto &name : item.cachedEmotes)
        ++data.emotes[name].uses;
}

std::optional<Item> itemFrom(const Message &message)
{
    Item item;
    item.id = message.id;
    item.ms = message.serverReceivedTime.isValid()
                  ? message.serverReceivedTime.toMSecsSinceEpoch()
                  : QDateTime::currentMSecsSinceEpoch();
    const auto flags = message.flags;
    if (flags.has(MessageFlag::Subscription))
    {
        item.kind = Item::Kind::Subscription;
        return item;
    }
    if (flags.hasAny({MessageFlag::Timeout, MessageFlag::ModerationAction,
                      MessageFlag::ClearChat}))
    {
        item.kind = Item::Kind::Moderation;
        return item;
    }
    if (flags.hasAny({MessageFlag::System, MessageFlag::Whisper}) ||
        message.loginName.isEmpty())
    {
        return std::nullopt;
    }

    item.login = message.loginName.toLower();
    item.name =
        message.displayName.isEmpty() ? message.loginName : message.displayName;
    item.color = message.usernameColor;
    item.first = flags.has(MessageFlag::FirstMessage);
    for (const auto &element : message.elements)
    {
        if (const auto *emote =
                dynamic_cast<const EmoteElement *>(element.get()))
        {
            if (auto ptr = emote->getEmote())
            {
                item.emotes.push_back(std::move(ptr));
            }
        }
        else if (const auto *layered =
                     dynamic_cast<const LayeredEmoteElement *>(element.get()))
        {
            if (!layered->getEmotes().empty() &&
                layered->getEmotes().front().ptr)
            {
                item.emotes.push_back(layered->getEmotes().front().ptr);
            }
        }
    }
    return item;
}

// ---- the public logs ------------------------------------------------------

/// The emote maps a log line's words are looked up in (snapshots: the maps
/// themselves are never changed, only replaced).
using EmoteMaps = std::vector<std::shared_ptr<const EmoteMap>>;

EmoteMaps emoteMapsOf(const std::shared_ptr<Channel> &channel)
{
    EmoteMaps maps;
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        maps.push_back(twitch->seventvEmotes());
        maps.push_back(twitch->bttvEmotes());
        maps.push_back(twitch->ffzEmotes());
    }
    auto *app = getApp();
    maps.push_back(app->getSeventvEmotes()->globalEmotes());
    maps.push_back(app->getBttvEmotes()->emotes());
    maps.push_back(app->getFfzEmotes()->emotes());
    std::erase(maps, nullptr);
    return maps;
}

void restoreCachedItems(const QString &name, int generation,
                        const std::shared_ptr<std::vector<Item>> &items,
                        size_t index)
{
    auto *entry = find(name, generation);
    if (entry == nullptr)
        return;
    const auto maps = emoteMapsOf(entry->channel.lock());
    const auto end = std::min(index + 1500, items->size());
    for (; index < end; ++index)
    {
        auto &item = (*items)[index];
        QStringList unresolved;
        for (const auto &emoteName : item.cachedEmotes)
        {
            EmotePtr emote;
            for (const auto &map : maps)
            {
                if (auto it = map->find(EmoteName{emoteName}); it != map->end())
                {
                    emote = it->second;
                    break;
                }
            }
            if (emote)
                item.emotes.push_back(std::move(emote));
            else
                unresolved.append(emoteName);
        }
        item.cachedEmotes = std::move(unresolved);
        add(*entry, item, false);
    }
    if (index < items->size())
    {
        QTimer::singleShot(0, [name, generation, items, index] {
            restoreCachedItems(name, generation, items, index);
        });
        return;
    }
    entry->cacheLoading = false;
    flushCache(*entry);
}

void startCache(const QString &name, Tracked &entry)
{
    // Twitch logins are safe path components; do not persist special channels.
    if (name.isEmpty() || entry.streamId.isEmpty() || name.section(u':', 0, 0).contains(QRegularExpression("[^a-z0-9_]")))
        return;
    const QDir dir(getApp()->getPaths().settingsDirectory + "/stream-stats");
    if (!QDir().mkpath(dir.absolutePath()))
        return;
    const auto path = dir.filePath(QString(name).replace(u':', u'-') + ".jsonl");
    const auto header = QJsonDocument(QJsonArray{QStringLiteral("noctis-stats-v1"), entry.streamId})
                            .toJson(QJsonDocument::Compact) + '\n';
    bool existing = false;
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            existing = file.readLine() == header;
    }
    if (!existing)
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(header) != header.size() || !file.commit())
            return;
    }
    entry.cachePath = path;
    if (!existing)
        return;
    entry.cacheLoading = true;
    const auto sinceMs = entry.data.since.toMSecsSinceEpoch();
    QThreadPool::globalInstance()->start([name, path, sinceMs, generation = entry.generation] {
        auto items = std::make_shared<std::vector<Item>>();
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
        {
            file.readLine();
            while (!file.atEnd() && items->size() <= MAX_EVENTS)
                if (auto item = decodeItem(file.readLine()); item && item->ms >= sinceMs)
                    items->push_back(std::move(*item));
        }
        postToThread([name, generation, items] { restoreCachedItems(name, generation, items, 0); });
    });
}

void ensureCacheTimer()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    auto *timer = new QTimer(QCoreApplication::instance());
    QObject::connect(timer, &QTimer::timeout, [] {
        for (auto it = tracked().begin(); it != tracked().end();)
        {
            auto &entry = it->second;
            const bool closed = entry.channel.expired();
            if (closed)
                entry.cacheLoading = false;
            flushCache(entry);
            if (closed)
                it = tracked().erase(it);
            else
                ++it;
        }
    });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] {
        for (auto &[name, entry] : tracked())
        {
            entry.cacheLoading = false;
            flushCache(entry);
        }
    });
    timer->start(2000);
}

std::vector<QByteArrayView> split(QByteArrayView text, char separator)
{
    std::vector<QByteArrayView> parts;
    qsizetype from = 0;
    while (from <= text.size())
    {
        auto end = text.indexOf(separator, from);
        if (end < 0)
        {
            end = text.size();
        }
        parts.push_back(text.sliced(from, end - from));
        from = end + 1;
    }
    return parts;
}

QString unescapeTag(const QByteArray &value)
{
    if (!value.contains('\\'))
    {
        return QString::fromUtf8(value);
    }
    QByteArray out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i)
    {
        if (value[i] != '\\' || i + 1 == value.size())
        {
            out.append(value[i]);
            continue;
        }
        switch (value[++i])
        {
            case 's':
                out.append(' ');
                break;
            case ':':
                out.append(';');
                break;
            case 'r':
                out.append('\r');
                break;
            case 'n':
                out.append('\n');
                break;
            default:
                out.append(value[i]);
        }
    }
    return QString::fromUtf8(out);
}

/// The chat messages, subscriptions and moderation in a mirror's raw answer
/// (IRC lines with tags), within [fromMs, toMs). Runs off the GUI thread.
std::vector<Item> parseLog(const QByteArray &body, qint64 fromMs, qint64 toMs,
                           const EmoteMaps &maps)
{
    std::vector<Item> items;
    for (const auto line : split(body, '\n'))
    {
        if (!line.startsWith('@'))
        {
            continue;
        }
        const auto tagsEnd = line.indexOf(' ');
        if (tagsEnd < 0)
        {
            continue;
        }
        QHash<QByteArray, QByteArray> tags;
        for (const auto tag : split(line.sliced(1, tagsEnd - 1), ';'))
        {
            const auto eq = tag.indexOf('=');
            if (eq > 0)
            {
                tags.insert(tag.first(eq).toByteArray(),
                            tag.sliced(eq + 1).toByteArray());
            }
        }
        auto rest = line.sliced(tagsEnd + 1);
        QByteArrayView prefix;
        if (rest.startsWith(':'))
        {
            const auto end = rest.indexOf(' ');
            if (end < 0)
            {
                continue;
            }
            prefix = rest.sliced(1, end - 1);
            rest = rest.sliced(end + 1);
        }
        const auto commandEnd = rest.indexOf(' ');
        const auto command = commandEnd < 0 ? rest : rest.first(commandEnd);
        const auto textStart = rest.indexOf(" :");
        const auto text = textStart < 0 ? QByteArrayView()
                                        : rest.sliced(textStart + 2);

        Item item;
        item.ms = tags.value("tmi-sent-ts").toLongLong();
        if (item.ms < fromMs || item.ms >= toMs)
        {
            continue;
        }
        item.id = QString::fromLatin1(tags.value("id"));

        if (command == "CLEARCHAT" || command == "CLEARMSG")
        {
            item.kind = Item::Kind::Moderation;
            item.id.clear();
            items.push_back(std::move(item));
            continue;
        }
        if (command == "USERNOTICE")
        {
            if (SUB_NOTICES.contains(tags.value("msg-id")))
            {
                item.kind = Item::Kind::Subscription;
                items.push_back(std::move(item));
            }
            continue;
        }
        if (command != "PRIVMSG")
        {
            continue;
        }

        const auto bang = prefix.indexOf('!');
        item.login =
            QString::fromUtf8(bang < 0 ? prefix : prefix.first(bang)).toLower();
        if (item.login.isEmpty())
        {
            continue;
        }
        item.name = unescapeTag(tags.value("display-name"));
        if (const auto color = tags.value("color"); !color.isEmpty())
        {
            item.color = QColor(QString::fromLatin1(color));
        }
        item.first = tags.value("first-msg") == "1";

        auto message = QString::fromUtf8(text);
        if (message.startsWith(u"\x01" "ACTION "))
        {
            message = message.mid(8);
            if (message.endsWith(u'\x01'))
            {
                message.chop(1);
            }
        }

        // Twitch emotes: "id:from-to,from-to/id:from-to" (code points)
        QSet<QString> twitchNames;
        if (const auto emotes = tags.value("emotes"); !emotes.isEmpty())
        {
            const auto codepoints = message.toUcs4();
            for (const auto emote : split(emotes, '/'))
            {
                const auto colon = emote.indexOf(':');
                if (colon <= 0)
                {
                    continue;
                }
                const auto id = QString::fromLatin1(emote.first(colon));
                for (const auto range : split(emote.sliced(colon + 1), ','))
                {
                    const auto dash = range.indexOf('-');
                    if (dash <= 0)
                    {
                        continue;
                    }
                    const auto from = range.first(dash).toInt();
                    const auto to = range.sliced(dash + 1).toInt();
                    if (from < 0 || to < from || to >= codepoints.size())
                    {
                        continue;
                    }
                    const auto name = QString::fromUcs4(
                        reinterpret_cast<const char32_t *>(
                            codepoints.constData()) +
                            from,
                        to - from + 1);
                    twitchNames.insert(name);
                    item.twitchEmotes.emplace_back(id, name);
                }
            }
        }
        // 7TV, BTTV and FFZ emotes: whole words found in the maps
        for (const auto word :
             QStringView(message).split(u' ', Qt::SkipEmptyParts))
        {
            const auto name = word.toString();
            if (twitchNames.contains(name))
            {
                continue;
            }
            for (const auto &map : maps)
            {
                auto it = map->find(EmoteName{name});
                if (it != map->end())
                {
                    item.emotes.push_back(it->second);
                    break;
                }
            }
        }
        items.push_back(std::move(item));
    }
    return items;
}

QString utcText(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)
        .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
}

struct WindowEvidence {
    QSet<qint64> minutes;
    bool answered = false;
};

// A gap of consecutive full minutes in a response is worth checking with
// another mirror. Empty responses never establish complete coverage.
bool needsFallback(const WindowEvidence &evidence, qint64 fromMs, qint64 toMs)
{
    if (evidence.minutes.isEmpty())
        return true;
    int gap = 0;
    for (auto minute = (fromMs + 59999) / 60000; minute < toMs / 60000; ++minute)
    {
        gap = evidence.minutes.contains(minute) ? 0 : gap + 1;
        if (gap >= 2)
            return true;
    }
    return false;
}

void readWindow(const QString &name, int generation, qint64 fromMs,
                qint64 untilMs, int mirror, int tried, bool failed,
                std::shared_ptr<WindowEvidence> evidence = {});

/// The next piece of the logs, or the end of reading.
void nextWindow(const QString &name, int generation, qint64 fromMs,
                qint64 untilMs, int mirror)
{
    auto *entry = find(name, generation);
    if (entry == nullptr)
    {
        return;
    }
    entry->data.historyRead = QDateTime::fromMSecsSinceEpoch(fromMs);
    if (fromMs >= untilMs)
    {
        // no mirror had any of it: the channel is not logged
        if (entry->data.history == ChannelStatsData::History::Loading)
        {
            entry->data.history = !entry->logsAnswered
                                      ? ChannelStatsData::History::Unavailable
                                      : entry->historyIncomplete ? ChannelStatsData::History::Partial
                                                                 : ChannelStatsData::History::Done;
        }
        return;
    }
    readWindow(name, generation, fromMs, untilMs, mirror, 0, false);
}

void readWindow(const QString &name, int generation, qint64 fromMs,
                qint64 untilMs, int mirror, int tried, bool failed,
                std::shared_ptr<WindowEvidence> evidence)
{
    const auto toMs = std::min(fromMs + WINDOW_MS, untilMs);
    const auto url = QStringLiteral("%1/channel/%2?raw&from=%3&to=%4")
                         .arg(LOG_MIRRORS[mirror], name.section(u':', 0, 0), utcText(fromMs),
                              utcText(toMs));
    auto *entry = find(name, generation);
    if (entry == nullptr)
    {
        return;
    }
    auto maps = emoteMapsOf(entry->channel.lock());
    if (!evidence)
        evidence = std::make_shared<WindowEvidence>();

    // a mirror that does not have this piece: the next one is asked; when
    // none has it, the piece is skipped (404 means "no messages" too)
    const auto tryNext = [name, generation, fromMs, toMs, untilMs, mirror,
                          tried, evidence](bool failure) {
        const auto count = int(LOG_MIRRORS.size());
        if (tried + 1 < count)
        {
            readWindow(name, generation, fromMs, untilMs,
                       (mirror + 1) % count, tried + 1, failure, evidence);
            return;
        }
        if (failure || needsFallback(*evidence, fromMs, toMs))
        {
            if (auto *entry = find(name, generation))
            {
                entry->historyIncomplete = true;
                for (auto minute = fromMs / 60000; minute <= (toMs - 1) / 60000; ++minute)
                    if (!entry->data.perMinute.contains(minute))
                        entry->data.incompleteMinutes.insert(minute);
            }
        }
        nextWindow(name, generation, toMs, untilMs, mirror);
    };

    NetworkRequest(QUrl(url))
        .timeout(LOG_TIMEOUT_MS)
        .concurrent()
        .onSuccess([name, generation, fromMs, toMs, untilMs, mirror, evidence, tryNext,
                    maps = std::move(maps)](const NetworkResult &result) {
            auto items = parseLog(result.getData(), fromMs, toMs, maps);
            postToThread([name, generation, fromMs, toMs, untilMs, mirror, evidence, tryNext,
                          items = std::move(items)]() mutable {
                auto *entry = find(name, generation);
                if (entry == nullptr)
                {
                    return;
                }
                if (!items.empty())
                    entry->logsAnswered = true;
                const bool alternate = evidence->answered;
                evidence->answered |= !items.empty();
                auto *twitchEmotes = getApp()->getEmotes()->getTwitchEmotes();
                for (auto &item : items)
                {
                    if (item.kind == Item::Kind::Chat)
                        evidence->minutes.insert(item.ms / 60000);
                    // Moderation notices lack stable message IDs; don't count
                    // the same event a second time on an overlapping mirror.
                    if (alternate && item.id.isEmpty())
                        continue;
                    // The extended range intentionally overlaps the local
                    // buffer. Preserve the former boundary for id-less
                    // moderation events, which can't be deduplicated by ID.
                    if (item.kind == Item::Kind::Moderation &&
                        item.id.isEmpty() && item.ms >= entry->moderationLogUntil)
                        continue;
                    for (const auto &[id, emoteName] : item.twitchEmotes)
                    {
                        item.emotes.push_back(twitchEmotes->getOrCreateEmote(
                            EmoteId{id}, EmoteName{emoteName}));
                    }
                    add(*entry, item);
                }
                if (needsFallback(*evidence, fromMs, toMs))
                    tryNext(false);
                else
                    nextWindow(name, generation, toMs, untilMs, mirror);
            });
        })
        .onError([name, generation, failed, tryNext](
                     const NetworkResult &result) {
            const auto status = result.status();
            postToThread([name, generation, failed, tryNext, status] {
                if (find(name, generation) == nullptr)
                {
                    return;
                }
                if (status == 403)
                {
                    // the channel opted out of being logged
                    auto *entry = find(name, generation);
                    entry->data.history = entry->logsAnswered ? ChannelStatsData::History::Partial
                                                              : ChannelStatsData::History::Unavailable;
                    for (auto minute = entry->data.historyRead.toMSecsSinceEpoch() / 60000;
                         minute <= entry->data.historyUntil.toMSecsSinceEpoch() / 60000; ++minute)
                        if (!entry->data.perMinute.contains(minute))
                            entry->data.incompleteMinutes.insert(minute);
                    return;
                }
                tryNext(failed || status != 404);
            });
        })
        .execute();
}

// ---- counting ---------------------------------------------------------------

/// Counts anew: the current stream (the chat's messages since it went live,
/// the rest from the logs) or, while offline, what the chat shows.
void restart(const QString &name, Tracked &entry,
             const std::shared_ptr<Channel> &channel)
{
    entry.cacheLoading = false;
    flushCache(entry);
    entry.cachePath.clear();
    entry.cachePending.clear();
    entry.generation = nextGeneration();
    entry.seen.clear();
    entry.logsAnswered = false;
    entry.historyIncomplete = false;
    entry.data = {};
    entry.streamId.clear();

    const auto now = QDateTime::currentDateTime();
    QDateTime streamStarted;
    auto *twitch = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitch != nullptr && twitch->isLive())
    {
        const auto status = twitch->accessStreamStatus();
        if (status->startedAt.isValid())
        {
            streamStarted = status->startedAt.toLocalTime();
            entry.streamId = status->streamId;
        }
    }
    // stand only: "<from>/<to>" (ISO) — a past stream of this channel, read
    // from the logs as if it were the current one
    std::optional<qint64> testUntil;
    if (const auto test = qEnvironmentVariable("MIST1X_STATS_TEST_STREAM");
        !test.isEmpty())
    {
        const auto parts = test.split(u'/');
        streamStarted = QDateTime::fromString(parts.value(0), Qt::ISODate);
        testUntil = QDateTime::fromString(parts.value(1), Qt::ISODate)
                        .toMSecsSinceEpoch();
        entry.streamId = QStringLiteral("test");
    }
    const auto windowEnd = testUntil ? QDateTime::fromMSecsSinceEpoch(*testUntil) : now;
    const auto start = streamStarted.isValid()
                           ? std::max(streamStarted, windowEnd.addSecs(-KEEP_MINUTES * 60))
                           : QDateTime{};
    entry.data.streamStarted = streamStarted;
    entry.data.live = start.isValid();
    entry.data.sinceStreamStart = start.isValid();
    entry.data.since = start;
    const auto startMs = start.isValid() ? start.toMSecsSinceEpoch() : 0;
    entry.moderationLogUntil = now.toMSecsSinceEpoch();
    if (start.isValid() && !testUntil)
        startCache(name, entry);

    std::optional<qint64> earliest = testUntil;
    for (const auto &message : channel->getMessageSnapshot())
    {
        if (testUntil)
        {
            break;
        }
        auto item = itemFrom(*message);
        if (!item || item->ms < startMs)
        {
            continue;
        }
        if (!earliest || item->ms < *earliest)
        {
            earliest = item->ms;
        }
        add(entry, *item);
    }

    if (!start.isValid())
    {
        entry.data.since = earliest
                               ? QDateTime::fromMSecsSinceEpoch(std::min(
                                     *earliest, now.toMSecsSinceEpoch()))
                               : now;
        return;
    }
    entry.data.since = start;
    // The earliest buffered message is NOT evidence that the intervening
    // history is complete: a restored buffer can contain a hole after exit.
    const auto untilMs = testUntil.value_or(now.toMSecsSinceEpoch());
    entry.moderationLogUntil = earliest.value_or(untilMs);
    if (untilMs - startMs > MIN_GAP_MS)
    {
        entry.data.history = ChannelStatsData::History::Available;
        entry.data.historyUntil = QDateTime::fromMSecsSinceEpoch(untilMs);
        entry.data.historyRead = start;
    }
}

/// A stream that went live (or a new one) starts the numbers anew; one that
/// ended keeps them.
void onStatus(const QString &name)
{
    auto it = tracked().find(name);
    if (it == tracked().end())
    {
        return;
    }
    auto &entry = it->second;
    auto channel = entry.channel.lock();
    auto *twitch = dynamic_cast<TwitchChannel *>(channel.get());
    if (twitch == nullptr || entry.streamId == u"test")
    {
        return;
    }
    if (twitch->isLive())
    {
        const bool newStream = [&] {
            const auto status = twitch->accessStreamStatus();
            return status->startedAt.isValid() && status->streamId != entry.streamId;
        }();
        if (newStream)
        {
            restart(name, entry, channel);
        }
    }
    else
    {
        entry.data.live = false;
    }
}

}  // namespace

void ChannelStats::attach(const std::shared_ptr<Channel> &channel)
{
    if (!channel || channel->getName().isEmpty())
    {
        return;
    }
    auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
    if (!twitchChannel && (!getApp()->isTest() || channel->getType() != Channel::Type::None))
        return;
    const auto name = statsKey(channel->getName(), twitchChannel && twitchChannel->isAnonymous());
    ensureCacheTimer();
    auto &entry = tracked()[name];
    if (entry.channel.lock() == channel)
    {
        return;  // already counted
    }
    entry.channel = channel;
    entry.status.reset();
    restart(name, entry, channel);

    std::weak_ptr<Channel> weak = channel;
    entry.appended = std::make_unique<pajlada::Signals::ScopedConnection>(
        channel->messageAppended.connect(
            [name, weak](MessagePtr &message, auto) {
                auto it = tracked().find(name);
                if (it == tracked().end() || weak.lock() == nullptr ||
                    !message)
                {
                    return;
                }
                if (auto item = itemFrom(*message))
                {
                    add(it->second, *item);
                }
            }));
    // the history loaded on joining comes in front of the chat
    entry.prepended = std::make_unique<pajlada::Signals::ScopedConnection>(
        channel->messagesAddedAtStart.connect(
            [name, weak](std::vector<MessagePtr> &messages) {
                auto it = tracked().find(name);
                if (it == tracked().end() || weak.lock() == nullptr)
                {
                    return;
                }
                auto &data = it->second.data;
                const auto startMs =
                    data.sinceStreamStart ? data.since.toMSecsSinceEpoch() : 0;
                const auto logsUntil =
                    data.history == ChannelStatsData::History::NotNeeded ||
                            data.history == ChannelStatsData::History::Available
                        ? 0
                        : it->second.moderationLogUntil;
                for (const auto &message : messages)
                {
                    auto item = message ? itemFrom(*message) : std::nullopt;
                    // the logs count this stretch's subscriptions and bans
                    if (!item || item->ms < startMs ||
                        (item->kind != Item::Kind::Chat && item->id.isEmpty() &&
                         item->ms < logsUntil))
                    {
                        continue;
                    }
                    add(it->second, *item);
                    if (!data.sinceStreamStart &&
                        item->ms < data.since.toMSecsSinceEpoch())
                    {
                        data.since = QDateTime::fromMSecsSinceEpoch(item->ms);
                    }
                }
            }));
    entry.filled = std::make_unique<pajlada::Signals::ScopedConnection>(
        channel->filledInMessages.connect([name, weak](const std::vector<MessagePtr> &messages) {
            auto it = tracked().find(name);
            if (it == tracked().end() || weak.lock() == nullptr)
                return;
            for (const auto &message : messages)
                if (message)
                    if (auto item = itemFrom(*message); item && item->kind == Item::Kind::Chat)
                    {
                        add(it->second, *item);
                        auto &data = it->second.data;
                        if (!data.sinceStreamStart && item->ms < data.since.toMSecsSinceEpoch())
                            data.since = QDateTime::fromMSecsSinceEpoch(item->ms);
                    }
        }));
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        entry.status = std::make_unique<pajlada::Signals::ScopedConnection>(
            twitch->streamStatusChanged.connect([name] {
                onStatus(name);
            }));
    }
}

void ChannelStats::requestHistory(const QString &channelName, bool anonymous)
{
    const auto name = statsKey(channelName, anonymous);
    auto it = tracked().find(name);
    if (it == tracked().end() || it->second.channel.expired() ||
        it->second.data.history != ChannelStatsData::History::Available)
        return;
    auto &entry = it->second;
    entry.data.history = ChannelStatsData::History::Loading;
    readWindow(name, entry.generation, entry.data.since.toMSecsSinceEpoch(),
               entry.data.historyUntil.toMSecsSinceEpoch(), 0, 0, false);
}

ChannelStatsData ChannelStats::get(const QString &channelName, bool anonymous)
{
    auto it = tracked().find(statsKey(channelName, anonymous));
    if (it == tracked().end())
    {
        return {};
    }
    return it->second.data;
}

QJsonArray ChannelStats::fixtureChecks()
{
    QJsonArray checks;
    if (!getApp()->isTest())
        return checks;
    auto check = [&checks](QString name, bool ok) {
        checks.append(QJsonObject{{"name", name}, {"ok", ok}});
    };
    const auto oldInterval = qgetenv("MIST1X_STATS_TEST_STREAM");
    const auto streamStart = QDateTime::fromString("2026-01-01T00:00:00Z", Qt::ISODate);
    const auto windowEnd = streamStart.addDays(3);
    qputenv("MIST1X_STATS_TEST_STREAM",
            (streamStart.toString(Qt::ISODate) + u'/' + windowEnd.toString(Qt::ISODate)).toUtf8());
    auto group = std::make_shared<Channel>("stats_fixture_platform", Channel::Type::None);
    Tracked longStream;
    restart(group->getName(), longStream, group);
    if (oldInterval.isNull())
        qunsetenv("MIST1X_STATS_TEST_STREAM");
    else
        qputenv("MIST1X_STATS_TEST_STREAM", oldInterval);
    check("three-day stream retains real start and counts the newest day",
          longStream.data.streamStarted == streamStart && longStream.data.since == windowEnd.addDays(-1));
    check("opening statistics leaves public history idle until requested",
          longStream.data.history == ChannelStatsData::History::Available);
    for (const auto type : {Channel::Type::Kick, Channel::Type::YouTube})
        attach(std::make_shared<Channel>(group->getName(), type));
    check("other providers cannot replace the Twitch statistics entry", !tracked().contains(group->getName()));
    const auto start = QDateTime::currentDateTime().addSecs(-600);
    const auto ms = start.toMSecsSinceEpoch();
    const auto minute = ms / 60000;
    auto item = Item{.id = "a", .login = "synthetic", .name = "Synthetic",
                     .color = QColor("#b8a7f0"), .ms = ms, .first = true};
    item.cachedEmotes = {"TestSmile"};
    Tracked capped;
    capped.data.messages = MAX_EVENTS;
    add(capped, item);
    check("count limit is visible and stops retaining more identities",
          capped.data.countLimitReached && capped.seen.empty() && capped.data.messages == MAX_EVENTS);
    const auto restored = decodeItem(encodeItem(item));
    check("journal roundtrip retains id, login, timestamp and name", restored &&
          restored->id == item.id && restored->ms == item.ms && restored->name == item.name && restored->login == item.login && restored->cachedEmotes == item.cachedEmotes);
    check("invalid journal line is ignored", !decodeItem("broken"));
    Tracked entry;
    entry.data.since = start;
    entry.data.sinceStreamStart = true;
    add(entry, item);
    add(entry, item);
    auto second = item;
    second.id = "b";
    second.ms += 120000;
    add(entry, second);
    auto earlier = item;
    earlier.id = "prestream";
    earlier.ms -= 1;
    add(entry, earlier);
    check("identity merge and stream boundary", entry.data.messages == 2 && entry.data.chatters["synthetic"].messages == 2);
    check("messages retain original minutes", entry.data.perMinute.value(minute) == 1 && entry.data.perMinute.value(minute + 2) == 1);
    check("first-message count is not duplicated", entry.data.firstMessages == 1);
    WindowEvidence evidence;
    evidence.minutes = {minute, minute + 3};
    check("gap inside a successful mirror response triggers fallback", needsFallback(evidence, minute * 60000, (minute + 4) * 60000));
    evidence.minutes.insert(minute + 1);
    evidence.minutes.insert(minute + 2);
    check("merged coverage stops further mirror requests", !needsFallback(evidence, minute * 60000, (minute + 4) * 60000));
    check("empty success is not complete", needsFallback({}, minute * 60000, (minute + 1) * 60000));
    auto name = QStringLiteral("stats_fixture_journal");
    auto journalOwner = std::make_shared<Channel>(name, Channel::Type::None);
    auto &journal = tracked()[name];
    journal.channel = journalOwner;
    check("separate normal and anonymous statistics", statsKey(name, false) != statsKey(name, true));
    journal.streamId = QString::number(ms);
    journal.data.since = start;
    journal.data.sinceStreamStart = true;
    startCache(name, journal);
    add(journal, item);
    add(journal, second);
    flushCache(journal);
    const auto path = journal.cachePath;
    check("journal records written", !path.isEmpty() && QFileInfo(path).size() > 0 && journal.cachePending.isEmpty());
    ++journal.generation;
    journal.seen.clear();
    journal.data = {};
    journal.data.since = start;
    journal.data.sinceStreamStart = true;
    startCache(name, journal);
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!journal.cacheLoading)
            loop.quit();
    });
    poll.start(10);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    check("local cache reload restores exact count and minute buckets", !journal.cacheLoading && journal.data.messages == 2 && journal.data.perMinute.value(minute + 2) == 1);
    add(journal, second);
    check("live/log overlap after restart is deduplicated", journal.data.messages == 2);
    auto nextStream = QString::number(ms + 1);
    journal.streamId = nextStream;
    ++journal.generation;
    journal.seen.clear();
    journal.data = {};
    startCache(name, journal);
    check("new stream does not reuse previous stream stats", !journal.cacheLoading && journal.data.messages == 0);
    journalOwner.reset();
    check("closed channel rejects old log callbacks", find(name, journal.generation) == nullptr);
    return checks;
}

}  // namespace chatterino
