#include "common/UiLanguage.hpp"
#include "providers/twitch/TwitchReadConnectionPool.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/irc/IrcConnection2.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchReadJoinLimiter.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/QStringHash.hpp"  // IWYU pragma: keep

#include <boost/unordered/unordered_flat_map.hpp>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QTimer>

#include <array>
#include <span>

namespace {

using namespace chatterino;

// Ratelimits for joinBucket_
constexpr int JOIN_RATELIMIT_BUDGET = 18;
constexpr int BOT_JOIN_RATELIMIT_BUDGET = 2000;
constexpr int JOIN_RATELIMIT_COOLDOWN = 12500;

constexpr size_t CHANNELS_PER_CONNECTION = 18;

/// Connection trouble of one channel: one status line, updated in place,
/// for as long as drops keep coming (a quiet quarter of an hour ends it).
struct Trouble {
    MessagePtr line;
    QDateTime first;
    QDateTime last;
    QDateTime downSince;
    int drops = 0;
    bool down = false;
    QString reason;
};

constexpr qint64 TROUBLE_QUIET_SECS = 15 * 60;

QHash<QString, Trouble> &troubles()
{
    static QHash<QString, Trouble> value;
    return value;
}

QString hhmm(const QDateTime &time)
{
    return time.toLocalTime().toString(QStringLiteral("HH:mm"));
}

MessagePtr makeTroubleLine(const Trouble &t)
{
    QString text;
    if (t.down)
    {
        text = t.drops <= 1
                   ? QStringLiteral("Нет связи с чатом Twitch с %1")
                         .arg(hhmm(t.downSince))
                   : QStringLiteral("Связь с чатом нестабильна: %1 обрывов с "
                                    "%2, сейчас нет связи")
                         .arg(t.drops)
                         .arg(hhmm(t.first));
        if (!t.reason.isEmpty())
        {
            text += QStringLiteral(" (") + t.reason + QStringLiteral(")");
        }
        text += QStringLiteral(". Переподключаемся, /reconnect — сразу.");
    }
    else
    {
        text = t.drops <= 1
                   ? QStringLiteral("Связь с чатом пропадала: %1–%2")
                         .arg(hhmm(t.downSince), hhmm(t.last))
                   : QStringLiteral("Связь с чатом нестабильна: %1 обрывов с "
                                    "%2, восстановлена в %3")
                         .arg(t.drops)
                         .arg(hhmm(t.first), hhmm(t.last));
    }
    MessageBuilder b(systemMessage, text);
    b->flags.set(t.down ? MessageFlag::DisconnectedMessage
                        : MessageFlag::ConnectedMessage);
    return b.release();
}

bool recentlyContains(Channel &chan, const MessagePtr &message)
{
    const auto snapshot = chan.getMessageSnapshot();
    for (size_t i = snapshot.size(), seen = 0; i > 0 && seen < 2000;
         --i, ++seen)
    {
        if (snapshot[i - 1] == message)
        {
            return true;
        }
    }
    return false;
}

void showTrouble(Channel &chan, Trouble &t)
{
    auto next = makeTroubleLine(t);
    if (t.line && recentlyContains(chan, t.line))
    {
        chan.replaceMessage(t.line, next);
    }
    else
    {
        chan.addMessage(next, MessageContext::Original);
    }
    t.line = next;
}

void noteDisconnected(Channel &chan, const QString &reason = {})
{
    const auto now = QDateTime::currentDateTime();
    auto &t = troubles()[chan.getName()];
    if (!t.line || t.last.secsTo(now) > TROUBLE_QUIET_SECS)
    {
        t = {};
        t.first = now;
    }
    const bool changed = !t.down || (!reason.isEmpty() && reason != t.reason);
    if (!t.down)
    {
        t.down = true;
        t.downSince = now;
        ++t.drops;
    }
    if (!reason.isEmpty())
    {
        t.reason = reason;
    }
    t.last = now;
    if (changed)
    {
        showTrouble(chan, t);
    }
}

/// False when there was no trouble to close (a first connect).
bool noteConnected(Channel &chan)
{
    auto it = troubles().find(chan.getName());
    if (it == troubles().end() || !it->down)
    {
        return false;
    }
    it->down = false;
    it->last = QDateTime::currentDateTime();
    showTrouble(chan, *it);
    return true;
}

void appendConnectedMessage(std::span<const ChannelPtr> channels)
{
    // connected/disconnected message
    auto connectedMsg = makeSystemMessage("connected");
    connectedMsg->flags.set(MessageFlag::ConnectedMessage);
    auto reconnected = makeSystemMessage("reconnected");
    reconnected->flags.set(MessageFlag::ConnectedMessage);

    for (const auto &chan : channels)
    {
        if (noteConnected(*chan))
        {
            continue;  // the trouble line now says the connection is back
        }
        MessagePtr last = chan->getLastMessage();

        bool replaceMessage =
            last && last->flags.has(MessageFlag::DisconnectedMessage);

        if (replaceMessage)
        {
            chan->replaceMessage(last, reconnected);
        }
        else
        {
            chan->addMessage(connectedMsg, MessageContext::Original);
        }
    }
}

}  // namespace

namespace chatterino {

bool connectionTroubleRecent(const QString &channelName)
{
    const auto it = troubles().find(channelName);
    return it != troubles().end() &&
           it->last.secsTo(QDateTime::currentDateTime()) <= TROUBLE_QUIET_SECS;
}

TwitchReadConnectionPool::TwitchReadConnectionPool(TwitchIrcServer *parent)
    : QObject(parent)
{
}

// MARK: Single

/// Manages a single read connection.
///
/// When too many channels are joined, the IRC `JOIN`s are delayed.
class TwitchReadConnectionPoolSingle final : public TwitchReadConnectionPool
{
public:
    TwitchReadConnectionPoolSingle(TwitchIrcServer *parent);

    void onChannelCreated(TwitchChannel &channel) override;
    void reconnect() override;
    void close() override;
    void rejoin(const QString &channelName) override;

private:
    void onMessageReceived(Communi::IrcMessage *msg);
    void onConnected();
    void onDisconnected();

    void markChannelsConnected();
    QString currentReadIdentity() const;
    bool isCurrentReadIdentity() const;
    void drainJoins();
    void confirmJoin(const QString &name);

    QObjectPtr<IrcConnection> connection = nullptr;

    // Our rate limiting bucket for the Twitch join rate limits
    // https://dev.twitch.tv/docs/irc/guide#rate-limits
    TwitchReadJoinLimiter joinLimiter;
    QTimer joinTimer;
    bool authenticatedRead = false;
    QSet<QString> joinedChannels;

    pajlada::Signals::SignalHolder signalHolder;
    boost::unordered_flat_map<QString, std::weak_ptr<TwitchChannel>> channels;

    TwitchIrcServer *server;
};

TwitchReadConnectionPoolSingle::TwitchReadConnectionPoolSingle(
    TwitchIrcServer *parent)
    : TwitchReadConnectionPool(parent)
    , connection(new IrcConnection)
    , joinLimiter(getSettings()->useBotLimitsJoin ? BOT_JOIN_RATELIMIT_BUDGET
                                                : JOIN_RATELIMIT_BUDGET,
                  std::chrono::milliseconds(JOIN_RATELIMIT_COOLDOWN))
    , server(parent)
{
    assert(this->server);
    this->connection->setDiagnosticsLabel(QStringLiteral("read"));

    this->joinTimer.setSingleShot(true);
    this->joinTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&this->joinTimer, &QTimer::timeout, this,
                     &TwitchReadConnectionPoolSingle::drainJoins);

    // Listen to read connection message signals
    this->connection->moveToThread(QCoreApplication::instance()->thread());

    QObject::connect(this->connection.get(), &IrcConnection::messageReceived,
                     this, &TwitchReadConnectionPoolSingle::onMessageReceived);
    QObject::connect(this->connection.get(),
                     &Communi::IrcConnection::privateMessageReceived, this,
                     [this](auto *msg) {
                         if (this->isCurrentReadIdentity())
                         {
                             this->privateMessageReceived.invoke(msg);
                         }
                     });
    QObject::connect(this->connection.get(), &Communi::IrcConnection::connected,
                     this, &TwitchReadConnectionPoolSingle::onConnected);
    QObject::connect(this->connection.get(),
                     &Communi::IrcConnection::disconnected, this,
                     &TwitchReadConnectionPoolSingle::onDisconnected);
    this->signalHolder.managedConnect(
        this->connection->connectionLost, [this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Read connection reconnect requested. Timeout:" << timeout;
            const auto delay = this->connection->smartReconnect();
            const auto reason = timeout ? QStringLiteral("сервер не отвечает")
                                        : this->connection->lastError();
            (void)delay;
            // One status line per channel, updated in place (no stream of
            // "timed out" / "disconnected" lines on a flaky network).
            for (const auto &[name, weak] : this->channels)
            {
                if (auto chan = weak.lock())
                {
                    noteDisconnected(*chan, reason);
                }
            }
        });
    this->signalHolder.managedConnect(this->connection->heartbeat, [this] {
        this->markChannelsConnected();
    });
}

QString TwitchReadConnectionPoolSingle::currentReadIdentity() const
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!this->authenticatedRead || account->isAnon())
    {
        return QStringLiteral("anonymous");
    }
    const auto uid = account->getUserId();
    return uid.isEmpty() ? QStringLiteral("login:") + account->getUserName()
                         : QStringLiteral("uid:") + uid;
}

bool TwitchReadConnectionPoolSingle::isCurrentReadIdentity() const
{
    return this->joinLimiter.belongsTo(this->currentReadIdentity());
}

void TwitchReadConnectionPoolSingle::drainJoins()
{
    this->joinTimer.stop();
    if (!this->connection->isConnected() || !this->isCurrentReadIdentity())
    {
        return;
    }
    while (this->connection->isConnected() && this->isCurrentReadIdentity())
    {
        auto name = this->joinLimiter.takeNext(TwitchReadJoinLimiter::Clock::now());
        if (!name)
        {
            break;
        }
        if (this->channels.contains(*name))
        {
            qCDebug(chatterinoIrc) << "Joining" << *name;
            this->connection->sendRaw("JOIN #" + *name);
        }
    }
    if (!this->connection->isConnected() || !this->isCurrentReadIdentity())
    {
        return;
    }
    if (const auto delay = this->joinLimiter.nextDelay(TwitchReadJoinLimiter::Clock::now()))
    {
        this->connection->logEvent(QStringLiteral("channel join pending; account limit, wait %1 ms")
                                       .arg(delay->count()));
        this->joinTimer.start(*delay);
    }
}

void TwitchReadConnectionPoolSingle::onChannelCreated(TwitchChannel &channel)
{
    this->channels.emplace(channel.getName(), channel.weakFromThis());
    if (this->connection->isConnected())
    {
        this->joinLimiter.enqueue(channel.getName());
        this->drainJoins();
    }
    this->signalHolder.managedConnect(
        channel.destroyed, [this, name = channel.getName()] {
            this->channels.erase(name);
            this->joinLimiter.remove(name);
            this->joinedChannels.remove(name);
            qCDebug(chatterinoIrc) << "[read] parting" << name;
            if (this->connection->isConnected() && this->isCurrentReadIdentity())
            {
                this->connection->sendRaw("PART #" + name);
            }
        });
}

void TwitchReadConnectionPoolSingle::reconnect()
{
    this->close();
    this->authenticatedRead = getSettings()->twitchReadConnectionMode ==
                              TwitchReadConnectionMode::Authenticated;
    this->joinLimiter.bind(this->currentReadIdentity());
    TwitchIrcServer::initializeConnection(
        this->connection.get(), TwitchIrcServer::ConnectionType::Read);
}

void TwitchReadConnectionPoolSingle::close()
{
    this->joinTimer.stop();
    this->joinLimiter.clearPending();
    this->connection->close();
}

void TwitchReadConnectionPoolSingle::rejoin(const QString &channelName)
{
    if (!this->channels.contains(channelName) ||
        !this->connection->isConnected() || !this->isCurrentReadIdentity())
    {
        return;
    }
    this->connection->sendRaw("PART #" + channelName);
    this->joinLimiter.enqueue(channelName);
    this->drainJoins();
}

void TwitchReadConnectionPoolSingle::onMessageReceived(Communi::IrcMessage *msg)
{
    if (!this->isCurrentReadIdentity())
    {
        return;
    }
    if (msg->type() == Communi::IrcMessage::Type::Private)
    {
        // We already have a handler for private messages
        return;
    }

    const QString &command = msg->command();

    if (command == "RECONNECT")
    {
        this->server->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->markChannelsConnected();
        this->reconnect();
        return;
    }

    // A connected IRC socket has not joined the channel until Twitch
    // acknowledges that particular JOIN. Do not clear its outage early.
    if (command == "JOIN" &&
        msg->nick().compare(this->connection->nickName(), Qt::CaseInsensitive) == 0)
    {
        this->confirmJoin(msg->parameter(0).mid(1).toLower());
    }
    this->messageReceived.invoke(msg);
}

void TwitchReadConnectionPoolSingle::confirmJoin(const QString &name)
{
    if (this->joinedChannels.contains(name))
    {
        return;
    }
    const auto it = this->channels.find(name);
    if (it != this->channels.end())
    {
        if (auto channel = it->second.lock())
        {
            this->joinedChannels.insert(name);
            this->connection->logEvent(QStringLiteral("channel joined: ") + name);
            const std::array<ChannelPtr, 1> confirmed{channel};
            appendConnectedMessage(confirmed);
        }
    }
}

void TwitchReadConnectionPoolSingle::onConnected()
{
    if (!this->isCurrentReadIdentity())
    {
        return;
    }
    std::vector<ChannelPtr> activeChannels;
    activeChannels.reserve(this->channels.size());
    for (const auto &[name, weak] : this->channels)
    {
        if (auto channel = weak.lock())
        {
            activeChannels.push_back(channel);
        }
    }

    // put the visible channels first
    auto visible = getApp()->getWindows()->getVisibleChannelNames();

    std::ranges::stable_partition(activeChannels, [&](const auto &chan) {
        return visible.contains(chan->getName());
    });

    // join channels
    for (const auto &channel : activeChannels)
    {
        this->joinLimiter.enqueue(channel->getName());
    }
    this->drainJoins();
}

void TwitchReadConnectionPoolSingle::onDisconnected()
{
    this->joinTimer.stop();
    this->joinLimiter.clearPending();
    this->joinedChannels.clear();
    for (const auto &[name, weak] : this->channels)
    {
        auto chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        noteDisconnected(*chan);
        chan->markDisconnected();
    }
}

void TwitchReadConnectionPoolSingle::markChannelsConnected()
{
    for (const auto &[name, weak] : this->channels)
    {
        if (auto c = weak.lock(); c && this->joinedChannels.contains(name))
        {
            c->markConnected();
        }
    }
}

// MARK: Multi

/// Join channels on multiple connections at once.
///
/// Twitch imposes a limit of 20 joins per 10s per authenticated user or
/// connection if anonymous. By using multiple connections, we can join channels
/// faster. This significantly speeds up the launch.
///
/// The join rate limits on Twitch are based on the user. Because of that,
/// usins multiple connections requires them to use the anonymous user.
class TwitchReadConnectionPoolMulti final : public TwitchReadConnectionPool
{
public:
    TwitchReadConnectionPoolMulti(TwitchIrcServer *parent);

    void onChannelCreated(TwitchChannel &channel) override;
    void reconnect() override;
    void close() override;
    void rejoin(const QString & /*channelName*/) override
    {
        // Anonymous connections: a ban of the account does not hide anything.
    }

private:
    struct Connection {
        Connection(size_t id, TwitchReadConnectionPoolMulti *parent);

        void reconnect();
        void close();

        void onMessageReceived(Communi::IrcMessage *msg);
        void onConnected();
        void onDisconnected();
        void markChannelsConnected();

        size_t id = 0;
        IrcConnection connection;
        /// Channel name -> Channel
        boost::unordered_flat_map<QString, std::weak_ptr<TwitchChannel>>
            channels;
        pajlada::Signals::ScopedConnection heartbeatConnection;
        pajlada::Signals::ScopedConnection connectionLostConnection;
        TwitchReadConnectionPoolMulti *parent = nullptr;
    };

    /// Connection ID -> Connection
    boost::unordered_flat_map<size_t, std::unique_ptr<Connection>> connections;
    size_t nextID = 1;
    std::pair<Connection *, bool> getOrCreate();

    /// Channel name -> Connection ID
    boost::unordered_flat_map<QString, size_t> channels;

    pajlada::Signals::SignalHolder signalHolder;

    /// Have we ever been asked to connect? If not, we shouldn't open
    /// connections on our own.
    bool everConnected = false;

    friend Connection;
};

TwitchReadConnectionPoolMulti::TwitchReadConnectionPoolMulti(
    TwitchIrcServer *parent)
    : TwitchReadConnectionPool(parent)
{
}

void TwitchReadConnectionPoolMulti::onChannelCreated(TwitchChannel &channel)
{
    auto [conn, created] = this->getOrCreate();
    this->channels.emplace(channel.getName(), conn->id);
    conn->channels.emplace(channel.getName(), channel.weakFromThis());
    if (conn->connection.isConnected())
    {
        qCDebug(chatterinoIrc) << "[read] joining" << channel.getName();
        conn->connection.sendRaw("JOIN #" + channel.getName());
    }
    this->signalHolder.managedConnect(
        channel.destroyed, [this, id = conn->id, name = channel.getName()] {
            this->channels.erase(name);
            auto it = this->connections.find(id);
            if (it == this->connections.end())
            {
                return;
            }
            qCDebug(chatterinoIrc) << "[read] parting" << name;
            it->second->connection.sendRaw("PART #" + name);
            it->second->channels.erase(name);
            if (it->second->channels.empty())
            {
                qCDebug(chatterinoIrc)
                    << "Connection" << id << "has no more channels, removing";
                this->connections.erase(it);
            }
        });

    if (created && this->everConnected)
    {
        conn->reconnect();
    }
}

void TwitchReadConnectionPoolMulti::reconnect()
{
    for (const auto &[id, conn] : this->connections)
    {
        conn->reconnect();
    }
    this->everConnected = true;
}

void TwitchReadConnectionPoolMulti::close()
{
    for (const auto &[id, conn] : this->connections)
    {
        conn->close();
    }
}

std::pair<TwitchReadConnectionPoolMulti::Connection *, bool>
    TwitchReadConnectionPoolMulti::getOrCreate()
{
    // This is slow if we have many connections, but realistically, users have <5.
    for (const auto &[id, conn] : this->connections)
    {
        if (conn->channels.size() < CHANNELS_PER_CONNECTION)
        {
            return {conn.get(), false};
        }
    }

    size_t id = this->nextID++;
    qCDebug(chatterinoIrc) << "Creating new connection" << id;
    auto [it, inserted] =
        this->connections.emplace(id, new Connection(id, this));
    assert(inserted);
    return {it->second.get(), true};
}

TwitchReadConnectionPoolMulti::Connection::Connection(
    size_t id, TwitchReadConnectionPoolMulti *parent)
    : id(id)
    , parent(parent)
{
    this->connectionLostConnection =
        this->connection.connectionLost.connect([this](bool timeout) {
            qCDebug(chatterinoIrc)
                << "Read connection" << this->id
                << "reconnect requested. Timeout:" << timeout;
            if (timeout)
            {
                for (const auto &[name, weak] : this->channels)
                {
                    if (auto chan = weak.lock())
                    {
                        noteDisconnected(*chan,
                                         QStringLiteral("сервер не отвечает"));
                    }
                }
            }
            this->connection.smartReconnect();
        });
    this->heartbeatConnection = this->connection.heartbeat.connect([this] {
        this->markChannelsConnected();
    });

    // Listen to read connection message signals
    this->connection.moveToThread(QCoreApplication::instance()->thread());

    QObject::connect(&this->connection, &IrcConnection::messageReceived,
                     &this->connection, [this](auto *msg) {
                         this->onMessageReceived(msg);
                     });
    QObject::connect(&this->connection,
                     &Communi::IrcConnection::privateMessageReceived,
                     &this->connection, [this](auto *msg) {
                         this->parent->privateMessageReceived.invoke(msg);
                     });
    QObject::connect(&this->connection, &Communi::IrcConnection::connected,
                     &this->connection, [this] {
                         this->onConnected();
                     });
    QObject::connect(&this->connection, &Communi::IrcConnection::disconnected,
                     &this->connection, [this] {
                         this->onDisconnected();
                     });
}

void TwitchReadConnectionPoolMulti::Connection::onMessageReceived(
    Communi::IrcMessage *msg)
{
    if (msg->type() == Communi::IrcMessage::Type::Private)
    {
        // We already have a handler for private messages
        return;
    }

    const QString &command = msg->command();

    if (command == "RECONNECT")
    {
        for (const auto &[name, weak] : this->channels)
        {
            if (auto chan = weak.lock())
            {
                chan->addSystemMessage(
                    "Twitch Servers requested us to reconnect, reconnecting");
            }
        }
        this->markChannelsConnected();
        this->reconnect();
        return;
    }

    this->parent->messageReceived.invoke(msg);
}

void TwitchReadConnectionPoolMulti::Connection::reconnect()
{
    this->connection.close();
    TwitchIrcServer::initializeConnection(
        &this->connection, TwitchIrcServer::ConnectionType::Read);
}

void TwitchReadConnectionPoolMulti::Connection::close()
{
    this->connection.close();
}

void TwitchReadConnectionPoolMulti::Connection::onConnected()
{
    std::vector<ChannelPtr> activeChannels;
    activeChannels.reserve(this->channels.size());
    for (const auto &[name, weak] : this->channels)
    {
        if (auto channel = weak.lock())
        {
            activeChannels.push_back(channel);
        }
    }

    // join channels
    for (const auto &channel : activeChannels)
    {
        qCDebug(chatterinoIrc) << "[read] joining" << channel->getName();
        this->connection.sendRaw("JOIN #" + channel->getName());
    }

    appendConnectedMessage(activeChannels);
}

void TwitchReadConnectionPoolMulti::Connection::onDisconnected()
{
    for (const auto &[name, weak] : this->channels)
    {
        auto chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        noteDisconnected(*chan);
        chan->markDisconnected();
    }
}

void TwitchReadConnectionPoolMulti::Connection::markChannelsConnected()
{
    for (const auto &[name, weak] : this->channels)
    {
        if (auto chan = weak.lock())
        {
            chan->markConnected();
        }
    }
}

// MARK: Selection

TwitchReadConnectionPool *createTwitchConnectionPool(TwitchIrcServer *parent,
                                                     bool multi)
{
    if (multi)
    {
        return new TwitchReadConnectionPoolMulti(parent);
    }
    return new TwitchReadConnectionPoolSingle(parent);
}

}  // namespace chatterino
