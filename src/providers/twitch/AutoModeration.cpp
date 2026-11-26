#include "providers/twitch/AutoModeration.hpp"
#include "providers/twitch/AutoModerationRules.hpp"
#include "providers/twitch/ModeratorExtension.hpp"
#include "providers/twitch/AutomationDialog.hpp"
#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/IvrApi.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QQueue>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>
#include <algorithm>
#include <boost/signals2/connection.hpp>

namespace chatterino {
namespace {
using namespace automoderation;
constexpr auto MAX_EVENTS = 4096;
constexpr qint64 DAY_MS = 86400000;
const qint64 sessionStart = QDateTime::currentMSecsSinceEpoch();

struct SeenTarget {
    QString login;
    bool protectedRole = true;
    qint64 seenAt = 0;
};
struct TimeoutEvidence {
    qint64 until = 0;
    qint64 observedAt = 0;
    std::weak_ptr<TwitchChannel> source;
};
struct State {
    QString uid;
    bool healthy = true;
    quint64 generation = 0;
    QHash<QString, Config> configs;
    QList<QJsonObject> journal;
    QSet<QString> keys;
    QSet<QString> skipped;
    QQueue<QString> skippedOrder;
    QHash<QString, SeenTarget> roles;
    QHash<QString, TimeoutEvidence> timeouts;
    QHash<QString, qint64> timeoutEvidenceFloor;
    QElapsedTimer lastAction;
};
QHash<QString, std::shared_ptr<State>> states;
boost::signals2::scoped_connection accountChanged;
AccountController *watchedAccounts = nullptr;
AutoModerationRoleFetcher fixtureRoleFetcher;

QString activeUid()
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    return account && !account->isAnon() ? account->getUserId() : QString{};
}
QString path(const QString &uid)
{
    return QDir(getApp()->getPaths().settingsDirectory)
        .filePath(QStringLiteral("noctis-auto-moderation-") + uid + ".json");
}
std::shared_ptr<State> stateFor(const QString &uid)
{
    if (!validId(uid)) return {};
    auto *accounts = getApp()->getAccounts();
    if (accounts != watchedAccounts || !accountChanged.connected())
    {
        watchedAccounts = accounts;
        accountChanged = accounts->twitch.currentUserChanged.connect([]{
            for (auto &state : states)
            {
                ++state->generation;
                state->roles.clear();
                state->timeouts.clear();
                state->timeoutEvidenceFloor.clear();
            }
        });
    }
    if (auto old = states.value(uid)) return old;
    auto state = std::make_shared<State>();
    state->uid = uid;
    states.insert(uid, state);
    QFile file(path(uid));
    if (!file.exists()) return state;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
    {
        state->healthy = false; return state;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto object = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        object.value("schema").toInt() != 1 || !object.value("channels").isObject() ||
        !object.value("journal").isArray())
    {
        state->healthy = false; return state;
    }
    const auto configs = object.value("channels").toObject();
    if (configs.size() > 200) { state->healthy = false; return state; }
    for (auto it = configs.begin(); it != configs.end(); ++it)
    {
        auto decoded = decode(it.value().toObject());
        if (!decoded || login(it.key()) != it.key()) { state->healthy = false; return state; }
        state->configs.insert(it.key(), std::move(*decoded));
    }
    for (const auto &value : object.value("journal").toArray())
    {
        const auto item = value.toObject();
        const auto key = item.value("id").toString();
        if (key.isEmpty() || key.size() > 160 || state->keys.contains(key) ||
            state->journal.size() >= MAX_EVENTS)
        { state->healthy = false; return state; }
        state->keys.insert(key);
        state->journal.append(item);
    }
    return state;
}
bool save(State &s)
{
    if (!s.healthy) return false;
    QJsonObject channels;
    for (auto it = s.configs.cbegin(); it != s.configs.cend(); ++it)
        channels.insert(it.key(), encode(it.value()));
    QJsonArray journal;
    for (const auto &entry : s.journal) journal.append(entry);
    const auto bytes = QJsonDocument(QJsonObject{{"schema",1},{"channels",channels},{"journal",journal}}).toJson();
    QSaveFile file(path(s.uid));
    if (bytes.size() > 4 * 1024 * 1024 || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit())
    {
        s.healthy = false;
        return false;
    }
    return true;
}
QString kindText(Kind kind)
{
    switch (kind)
    {
        case Kind::Words: return uiText("Word timeout");
        case Kind::RewardTimeout: return uiText("Reward timeout");
        case Kind::RewardUntimeout: return uiText("Reward untimeout");
    }
    return {};
}
QString targetKey(const QString &room, const QString &uid)
{
    return room + u'/' + uid;
}
bool reserve(State &s, const QString &key, const QString &channel,
             const Rule &rule, const QString &target, const QString &status)
{
    if (!s.healthy || s.keys.contains(key)) return false;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    while (!s.journal.isEmpty() &&
           s.journal.first().value("at").toInteger() < now - 7 * DAY_MS)
    {
        s.keys.remove(s.journal.takeFirst().value("id").toString());
    }
    if (s.journal.size() >= MAX_EVENTS) { s.healthy = false; return false; }
    s.journal.append(QJsonObject{{"id",key},{"channel",channel},{"at",now},
        {"kind",int(rule.kind)},{"target",target},{"trigger",rule.title.isEmpty() ? rule.trigger : rule.title},
        {"status",status}});
    s.keys.insert(key);
    return save(s);
}
void finish(const std::shared_ptr<State> &s, const QString &key, const QString &status)
{
    for (auto it = s->journal.rbegin(); it != s->journal.rend(); ++it)
    {
        if (it->value("id").toString() == key)
        {
            it->insert("status",status);
            save(*s);
            break;
        }
    }
}
struct Intent {
    std::shared_ptr<State> state;
    QString event, channel, room, sender, targetLogin;
    Rule rule;
    quint64 generation;
    qint64 at;
    int moduleRevision;
    QString message, buyer;
};
bool proceed(const Intent &i)
{
    if (!i.state->healthy || activeUid() != i.state->uid ||
        i.generation != i.state->generation ||
        !freshEvent(i.at, QDateTime::currentMSecsSinceEpoch()) ||
        (!i.rule.moduleAction.isEmpty() && i.moduleRevision!=moderatorextension::active().revision))
        return false;
    const auto c = i.state->configs.value(i.channel);
    if (!c.enabled || c.roomId != i.room) return false;
    const auto channel = getApp()->getTwitch()->getChannelOrEmpty(i.channel);
    auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
    return tc && tc->roomId() == i.room && permittedAutoModeration(*tc);
}
void execute(const Intent &i, const QString &targetId, const QString &targetLogin,
             const std::optional<QString> &accountType = std::nullopt,
             std::optional<moderatorextension::Decision> prepared = std::nullopt,
             bool roleSnapshotConfirmed = false)
{
    if (!proceed(i)) { finish(i.state,i.event,"Action canceled"); return; }
    const auto c = i.state->configs.value(i.channel);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto tk = targetKey(i.room,targetId);
    const auto known = i.state->roles.value(tk);
    const auto observedChannel=std::dynamic_pointer_cast<TwitchChannel>(
        getApp()->getTwitch()->getChannelOrEmpty(i.channel));
    // Only protect the account performing moderation, not other local accounts.
    if (!validId(targetId) || targetId == i.room || targetId == i.state->uid ||
        c.exceptions.contains(targetLogin) ||
        (known.protectedRole && known.seenAt > 0) ||
        (accountType && !accountType->isEmpty()))
    {
        finish(i.state,i.event,"Protected or excluded target"); return;
    }
    using moderatorextension::Operation;
    moderatorextension::Decision decision{
        i.rule.kind==Kind::RewardUntimeout?Operation::Untimeout:Operation::Timeout,
        i.rule.seconds,{}};
    if(prepared)
        decision=*prepared;
    else if(!i.rule.moduleAction.isEmpty())
    {
        const auto profile=moderatorextension::active().rewardAction(i.rule.moduleAction);
        const auto result=profile?moderatorextension::decide(*profile,i.rule.moduleParameters,
            QJsonObject{{"source",i.rule.kind==Kind::Words?"message":"reward"},
                         {"message",i.message.left(500)},{"target",i.targetLogin},
                         {"channel",i.channel},{"buyer",i.buyer}}):std::nullopt;
        if(!result){finish(i.state,i.event,"Module action unavailable");return;}
        decision=*result;
        if(decision.operation==Operation::Skip){finish(i.state,i.event,"Module condition did not match");return;}
        for(auto &entry:i.state->journal)
            if(entry.value("id").toString()==i.event)
            {
                if(decision.operation==Operation::Timeout)entry["duration"]=decision.seconds;
                if(decision.operation==Operation::Reply)entry["reply"]=decision.reply;
                break;
            }
    }
    const bool untimeout = decision.operation==Operation::Untimeout;
    const auto evidence=i.state->timeouts.value(tk);
    if (untimeout && (!observedChannel || evidence.source.lock()!=observedChannel ||
                      !canRemoveTimeout(evidence.until,now)))
    {
        finish(i.state,i.event,"No confirmed active timeout"); return;
    }
    // Recent IRC badge evidence remains the fast path. Reward targets may
    // never have sent a message in this session, so request the channel's
    // public role roster once instead of treating the missing evidence as a
    // permanent failure. Keep all known protected-role observations.
    if (decision.operation==Operation::Timeout && !roleSnapshotConfirmed &&
        (known.seenAt <= 0 || now - known.seenAt > 30 * 60 * 1000 ||
         known.login != targetLogin))
    {
        // A successful Twitch user lookup must explicitly report an ordinary
        // global account type before a channel roster can establish eligibility.
        if (i.rule.kind==Kind::Words || !accountType || !accountType->isEmpty())
        { finish(i.state,i.event,"Target role could not be confirmed"); return; }
        auto resolved=std::make_shared<bool>(false);
        const auto succeeded=[i,targetId,targetLogin,accountType,decision,resolved]
            (const QJsonArray &mods,const QJsonArray &vips) {
            if(std::exchange(*resolved,true)) return;
            if(!proceed(i)) {finish(i.state,i.event,"Action canceled");return;}
            const auto status=checkRoleSnapshot(mods,vips,i.state->uid,i.room,targetId,targetLogin);
            if(status==RoleSnapshot::Protected)
            {finish(i.state,i.event,"Protected or excluded target");return;}
            if(status!=RoleSnapshot::Ordinary)
            {finish(i.state,i.event,"Target role could not be confirmed");return;}
            // Revalidate current permission, generation, exclusions and any
            // newer live badge observation before dispatch. No delayed retry.
            execute(i,targetId,targetLogin,accountType,decision,true);
        };
        const auto failed=[i,resolved] {
            if(std::exchange(*resolved,true)) return;
            finish(i.state,i.event,"Target role list unavailable");
        };
        if(getApp()->isTest())
        {
            if(fixtureRoleFetcher) fixtureRoleFetcher(i.channel,succeeded,failed);
            else failed();
        }
        else
        {
            getIvr()->getModVip(i.channel,[succeeded](const IvrModVip &roles) {
                succeeded(roles.mods,roles.vips);
            },failed);
        }
        return;
    }
    if (c.dryRun)
    {
        finish(i.state,i.event,"Test: would perform action"); return;
    }
    // Twitch's DELETE removes both timeouts and bans. Require the latest
    // live evidence of an active finite timeout in this channel instance and
    // never retry an ambiguous result. A server-side concurrent change is
    // still possible because the endpoint is not conditional.
    if (!proceed(i) || i.state->configs.value(i.channel).dryRun)
    { finish(i.state,i.event,"Action canceled"); return; }
    if(decision.operation==Operation::Reply)
    {
        if(!observedChannel || i.sender==i.state->uid)
        {finish(i.state,i.event,"Action canceled");return;}
        if(!moderatorextension::claimReplyEvent(i.room,i.event.mid(i.room.size()+1)))
        {finish(i.state,i.event,"Reply already handled");return;}
        observedChannel->sendReply(decision.reply,i.event.mid(i.room.size()+1));
        finish(i.state,i.event,"Reply requested; not retried");
        return;
    }
    const auto requestedAt=QDateTime::currentMSecsSinceEpoch();
    const std::weak_ptr<TwitchChannel> source=observedChannel;
    if (untimeout)
    {
        const auto latest=i.state->timeouts.value(tk);
        if (latest.source.lock()!=observedChannel ||
            !canRemoveTimeout(latest.until,requestedAt))
        { finish(i.state,i.event,"No confirmed active timeout"); return; }
        // Consume evidence before dispatch; an ambiguous response must not
        // allow a later redemption to repeat the same removal.
        i.state->timeouts.insert(tk,{0,std::max(i.at,latest.observedAt),source});
        getHelix()->unbanUser(i.room,i.state->uid,targetId,
            [i] { finish(i.state,i.event,"Action confirmed"); },
            [i](auto,const QString &) { finish(i.state,i.event,"Action failed or unconfirmed; not retried"); });
        return;
    }
    const auto seconds = decision.seconds;
    getHelix()->banUser(i.room, i.state->uid, targetId, seconds, i.rule.reason,
        [i,tk,seconds,requestedAt,source,evidence] {
            // Do not overwrite a newer live clear-chat or permanent ban with
            // a delayed acknowledgement of an earlier request.
            const auto latest=i.state->timeouts.value(tk);
            if (i.state->generation==i.generation && source.lock() &&
                latest.observedAt==evidence.observedAt && latest.until==evidence.until)
                i.state->timeouts.insert(tk,{requestedAt+qint64(seconds)*1000,
                    std::max(i.at,latest.observedAt),source});
            finish(i.state,i.event,"Action confirmed");
        },
        [i](auto, const QString &) { finish(i.state,i.event,"Action failed or unconfirmed; not retried"); });
}
QString durationText(int seconds)
{
    return QString::number(seconds) + uiText(" sec");
}
}

void setAutoModerationRoleFetcherForTesting(AutoModerationRoleFetcher fetcher)
{
    if(getApp()->isTest()) fixtureRoleFetcher=std::move(fetcher);
}

void observeAutoModerationTimeout(TwitchChannel &channel, Communi::TagsRef tags)
{
    if (tags.has("historical") || !permittedAutoModeration(channel)) return;
    const auto s = stateFor(activeUid());
    if (!s || !s->healthy || !s->configs.contains(channel.getName())) return;
    const auto room = tags.getOrEmpty("room-id");
    const auto target = tags.getOrEmpty("target-user-id");
    if (!validId(target) || room != channel.roomId()) return;
    bool valid = false;
    const auto timestamp = tags.getOrEmpty("tmi-sent-ts").toLongLong(&valid);
    if (!valid || timestamp < sessionStart ||
        timestamp < s->timeoutEvidenceFloor.value(room) ||
        !freshEvent(timestamp,QDateTime::currentMSecsSinceEpoch())) return;
    const auto key = targetKey(room,target);
    const auto seconds = tags.getOrEmpty("ban-duration").toInt(&valid);
    const auto previous=s->timeouts.value(key);
    if (previous.observedAt > timestamp ||
        (previous.observedAt==timestamp && previous.until==0)) return;
    // Keep a tombstone for an indefinite ban; delayed older finite events
    // must not turn it back into a timeout.
    const auto until=valid && seconds>0 && seconds<=1209600 ?
        timestamp+qint64(seconds)*1000 : 0;
    s->timeouts.insert(key,{until,timestamp,channel.weakFromThis()});
    if (s->timeouts.size() > 4096) s->timeouts.clear();
}

void invalidateAutoModerationTimeouts(TwitchChannel &channel)
{
    const auto room=channel.roomId();
    if (room.isEmpty()) return;
    const auto prefix=room+u'/';
    for (auto &s : states)
    {
        if (s->configs.value(channel.getName()).roomId!=room) continue;
        ++s->generation; // cancel pending lookups/old acknowledgements
        s->timeoutEvidenceFloor[room]=QDateTime::currentMSecsSinceEpoch();
        for (auto it=s->timeouts.begin();it!=s->timeouts.end();)
            if (it.key().startsWith(prefix)) it=s->timeouts.erase(it);
            else ++it;
    }
}

void observeAutoModerationMessage(TwitchChannel &channel, Communi::TagsRef tags,
                                  const QString &author, const QString &text)
{
    if (tags.has("historical") || !permittedAutoModeration(channel)) return;
    const auto uid = activeUid();
    const auto s = stateFor(uid);
    if (!s || !s->healthy || !s->configs.contains(channel.getName())) return;
    const auto c = s->configs.value(channel.getName());
    const auto room = tags.getOrEmpty("room-id");
    const auto source = tags.getOrEmpty("source-room-id");
    const auto sender = tags.getOrEmpty("user-id");
    const auto event = tags.getOrEmpty("id");
    const auto name = login(author);
    bool ok = false;
    const auto timestamp = tags.getOrEmpty("tmi-sent-ts").toLongLong(&ok);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (room.isEmpty() || room != channel.roomId() || room != c.roomId ||
        (!source.isEmpty() && source != room) || tags.getOrEmpty("msg-id") == "sharedchatnotice" ||
        !validId(sender) || name.isEmpty() || event.isEmpty() || event.size() > 128 ||
        !ok || timestamp < sessionStart || !freshEvent(timestamp,now) || !tags.has("badges")) return;
    const auto tk = targetKey(room,sender);
    if (s->roles.size() > 4096) s->roles.clear();
    if (s->roles.value(tk).seenAt <= timestamp)
        s->roles.insert(tk,{name,protectedBadges(tags.getOrEmpty("badges")) ||
                            tags.getOrEmpty("mod")=="1" || tags.getOrEmpty("vip")=="1",timestamp});
    if (const auto timeout=s->timeouts.value(tk); timeout.observedAt>0 && timestamp>timeout.observedAt)
        s->timeouts.insert(tk,{0,timestamp,channel.weakFromThis()}); // later live message disproves it
    const auto key = room + u'/' + event;
    if (!c.enabled || s->keys.contains(key) || s->skipped.contains(key)) return;
    const auto reward = tags.getOrEmpty("custom-reward-id");
    const auto rule = match(c,text,reward);
    if (!rule) return;
    if (s->lastAction.isValid() && !s->lastAction.hasExpired(1000))
    {
        s->skipped.insert(key);
        s->skippedOrder.enqueue(key);
        if (s->skippedOrder.size() > 8192)
            s->skipped.remove(s->skippedOrder.dequeue());
        return;
    }
    s->lastAction.start();
    const bool words = rule->kind == Kind::Words;
    const auto target = words ? name : login(text);
    if (!reserve(*s,key,channel.getName(),*rule,target,"Pending action")) return;
    if (target.isEmpty())
    { finish(s,key,"Invalid target login"); return; }
    const int moduleRevision=rule->moduleAction.isEmpty()?0:moderatorextension::active().revision;
    const Intent action{s,key,channel.getName(),room,sender,target,*rule,s->generation,timestamp,moduleRevision,text.left(500),name};
    if (words)
    {
        execute(action,sender,name);
        return;
    }
    if (!proceed(action)) { finish(s,key,"Action canceled"); return; }
    getHelix()->getUserByName(target,[action,target](HelixUser user) {
        if (user.login.toLower() != target || !validId(user.id))
        { finish(action.state,action.event,"Invalid target login"); return; }
        execute(action,user.id,target,user.accountType);
    },[action] { finish(action.state,action.event,"Target lookup failed"); });
}

void openAutoModeration(QWidget *parent,
                        AutoModerationRewardsFetcher fixtureFetcher)
{
    moderatorextension::UiLease moduleUi;
    const auto &bundle=moderatorextension::active();
    const auto uid = activeUid();
    auto s = stateFor(uid);
    if (!s) return;
    QHash<QString,QString> rooms;
    getApp()->getTwitch()->forEachChannel([&](const ChannelPtr &channel) {
        if (auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
            tc && permittedAutoModeration(*tc) && validId(tc->roomId()))
            rooms.insert(tc->getName(),tc->roomId());
    });
    auto channels = rooms.keys();
    channels.sort(Qt::CaseInsensitive);
    QDialog dialog(parent);
    dialog.setObjectName("NoctisAutoModeration");
    dialog.setWindowTitle(uiText("Automatic moderation"));
    dialog.resize(900,640);
    auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium,
        std::clamp(getSettings()->getClampedUiScale(),1.F,1.4F));
    font.setWeight(std::max(font.weight(),QFont::Medium));
    dialog.setFont(font);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14,12,14,12);
    auto *top = new QHBoxLayout;
    auto *channel = new QComboBox(&dialog);
    channel->setObjectName("AutoModerationChannel");
    channel->addItems(channels);
    auto *enabled = new QCheckBox(uiText("Enable automatic moderation"),&dialog);
    enabled->setObjectName("AutoModerationEnabled");
    auto *dryRun = new QCheckBox(uiText("Test mode (no real timeouts)"),&dialog);
    dryRun->setObjectName("AutoModerationDryRun");
    top->addWidget(new QLabel(uiText("Channel"),&dialog));
    top->addWidget(channel,1);
    top->addWidget(enabled);
    top->addWidget(dryRun);
    layout->addLayout(top);
    auto *hint = new QLabel(uiText("Only live events in a verified moderation channel are processed. Protected roles and excluded users are never punished."),&dialog);
    hint->setWordWrap(true);
    hint->setObjectName("MutedText");
    layout->addWidget(hint);
    auto *exceptions = new QLineEdit(&dialog);
    exceptions->setObjectName("AutoModerationExceptions");
    exceptions->setPlaceholderText(uiText("Excluded logins, separated by spaces"));
    layout->addWidget(exceptions);
    auto *tabs = new QTabWidget(&dialog);
    tabs->setObjectName("AutoModerationTabs");
    layout->addWidget(tabs,1);
    auto setupPage = [&](const QString &title) {
        auto *page = new QWidget(tabs);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(8,8,8,8);
        tabs->addTab(page,title);
        return pageLayout;
    };
    auto *wordPage = setupPage(bundle.label("words","Words"));
    auto *wordHint = new QLabel(uiText("A contiguous fragment, ignoring letter case. The author of a matching message receives a timeout."),&dialog);
    wordHint->setWordWrap(true);
    wordPage->addWidget(wordHint);
    auto *words = new QTableWidget(0,6,&dialog);
    words->setObjectName("AutoModerationWords");
    words->setHorizontalHeaderLabels({uiText("Enabled"),uiText("Contains phrase"),uiText("Action"),uiText("Timeout duration"),uiText("Reason"),uiText("Parameters")});
    wordPage->addWidget(words,1);
    auto *wordButtons = new QHBoxLayout;
    auto *addWord = new QPushButton(uiText("Add rule"),&dialog);
    addWord->setObjectName("AutoModerationAddWord");
    auto *removeWord = new QPushButton(uiText("Remove selected"),&dialog);
    wordButtons->addWidget(addWord);wordButtons->addWidget(removeWord);wordButtons->addStretch();
    wordPage->addLayout(wordButtons);
    auto *rewardPage = setupPage(bundle.label("rewards","Rewards"));
    auto *rewardHint = new QLabel(uiText("Choose a reward that requires a nickname as input. Enter only one exact Twitch login. Untimeout requires a confirmed active timeout; known permanent bans are excluded."),&dialog);
    rewardHint->setWordWrap(true);
    rewardPage->addWidget(rewardHint);
    auto *rewards = new QTableWidget(0,6,&dialog);
    rewards->setObjectName("AutoModerationRewards");
    rewards->setHorizontalHeaderLabels({uiText("Enabled"),uiText("Reward"),uiText("Action"),uiText("Timeout duration"),uiText("Reason"),uiText("Parameters")});
    rewardPage->addWidget(rewards,1);
    auto *rewardButtons = new QHBoxLayout;
    auto *refreshRewards = new QPushButton(uiText("Refresh rewards"),&dialog);
    refreshRewards->setObjectName("AutoModerationRefreshRewards");
    auto *addReward = new QPushButton(uiText("Add rule"),&dialog);
    addReward->setObjectName("AutoModerationAddReward");
    auto *removeReward = new QPushButton(uiText("Remove selected"),&dialog);
    rewardButtons->addWidget(refreshRewards);rewardButtons->addWidget(addReward);rewardButtons->addWidget(removeReward);rewardButtons->addStretch();
    rewardPage->addLayout(rewardButtons);
    auto *historyPage = setupPage(bundle.label("history","History"));
    const QHash<QString,QWidget *> pageById{{"words",wordPage->parentWidget()},
        {"rewards",rewardPage->parentWidget()},{"history",historyPage->parentWidget()}};
    for(int index=0;index<bundle.moderationOrder.size();++index)
        tabs->tabBar()->moveTab(tabs->indexOf(pageById.value(bundle.moderationOrder[index])),index);
    tabs->setCurrentIndex(0);
    auto *history = new QTableWidget(0,4,&dialog);
    history->setObjectName("AutoModerationHistory");
    history->setHorizontalHeaderLabels({uiText("Time"),uiText("Action"),uiText("User"),uiText("Status")});
    history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyPage->addWidget(history,1);
    auto *status = new QLabel(&dialog);
    status->setObjectName("AutoModerationStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *buttons = new QHBoxLayout;
    auto *saveButton = new QPushButton(uiText("Save rules"),&dialog);
    saveButton->setObjectName("SaveRules");
    auto *close = new QPushButton(uiText("Close"),&dialog);
    buttons->addStretch();buttons->addWidget(saveButton);buttons->addWidget(close);
    layout->addLayout(buttons);
    for (auto *table : {words,rewards,history})
    {
        table->verticalHeader()->hide();
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    }
    words->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    rewards->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    words->horizontalHeader()->setSectionResizeMode(5,QHeaderView::ResizeToContents);
    rewards->horizontalHeader()->setSectionResizeMode(5,QHeaderView::ResizeToContents);
    QHash<QString,Config> draft = s->configs;
    QHash<QString,QList<std::pair<QString,QString>>> rewardChoices;
    QSet<QString> loadingRewards;
    QString selected;
    auto populateRewardCombo = [&](QComboBox *combo,const QString &name,const QString &id,const QString &title) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(uiText("Choose a reward"),QString{});
        for(const auto &[itemId,itemTitle] : rewardChoices.value(name))
            combo->addItem(itemTitle,itemId);
        if(!id.isEmpty() && combo->findData(id)<0)
            combo->addItem(title.isEmpty() ? id : title,id);
        combo->setCurrentIndex(std::max(0,combo->findData(id)));
    };
    auto duration = [&](QTableWidget *table,int row,int col,int seconds) {
        auto *spin = new QSpinBox(table);
        spin->setRange(1,86400); spin->setSuffix(uiText(" sec"));spin->setValue(seconds);
        spin->setKeyboardTracking(false);table->setCellWidget(row,col,spin);
        return spin;
    };
    auto checkCell = [](QTableWidget *table,int row,bool value) {
        auto *item = new QTableWidgetItem;
        item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable);
        item->setCheckState(value ? Qt::Checked : Qt::Unchecked);
        table->setItem(row,0,item);
    };
    auto appendAction = [&](QTableWidget *table,int row,const Rule &r,bool word) {
        auto *action = new QComboBox(table);
        action->addItem(uiText("Timeout"),int(Kind::RewardTimeout));
        if(!word)action->addItem(uiText("Remove timeout"),int(Kind::RewardUntimeout));
        for(const auto &preset:bundle.rewardActions)
        {
            if(!preset.sources.contains(word?"message":"reward"))continue;
            action->addItem(preset.title.localized(),int(Kind::RewardTimeout));
            action->setItemData(action->count()-1,preset.id,Qt::UserRole+1);
        }
        int index=r.kind==Kind::RewardUntimeout?1:0;
        if(!r.moduleAction.isEmpty())
        {
            index=action->findData(r.moduleAction,Qt::UserRole+1);
            if(index<0)
            {
                action->addItem(uiText("Unavailable module action (%1)").arg(r.moduleAction),int(Kind::RewardTimeout));
                index=action->count()-1;
                action->setItemData(index,r.moduleAction,Qt::UserRole+1);
            }
        }
        action->setCurrentIndex(index);
        table->setCellWidget(row,2,action);
        auto *spin = duration(table,row,3,r.seconds);
        auto *settings=new QPushButton(bundle.label("action_settings","Configure"),table);
        settings->setObjectName("ModuleActionSettings");
        settings->setProperty("moduleParams",r.moduleParameters);
        settings->setProperty("moduleAction",r.moduleAction);
        table->setCellWidget(row,5,settings);
        const auto sync=[&bundle,action,spin,settings]{
            const auto id=action->currentData(Qt::UserRole+1).toString();
            if(settings->property("moduleAction").toString()!=id)
            {
                settings->setProperty("moduleAction",id);
                settings->setProperty("moduleParams",QJsonObject{});
            }
            spin->setEnabled(action->currentIndex()==0 && id.isEmpty());
            const auto profile=bundle.rewardAction(id);
            settings->setEnabled(profile && !profile->fields.isEmpty());
        };
        QObject::connect(action,&QComboBox::currentIndexChanged,settings,[sync](int){sync();});
        QObject::connect(settings,&QPushButton::clicked,&dialog,[&bundle,action,settings,&dialog]{
            const auto profile=bundle.rewardAction(action->currentData(Qt::UserRole+1).toString());
            if(!profile)return;
            const auto params=moderatorextension::editParameters(&dialog,*profile,
                settings->property("moduleParams").value<QJsonObject>());
            if(params)settings->setProperty("moduleParams",*params);
        });
        sync();
        table->setItem(row,4,new QTableWidgetItem(r.reason));
    };
    auto appendWord = [&](const Rule &r) {
        const int row=words->rowCount();words->insertRow(row);checkCell(words,row,r.enabled);
        words->setItem(row,1,new QTableWidgetItem(r.trigger));
        appendAction(words,row,r,true);
    };
    auto appendReward = [&](const Rule &r) {
        const int row=rewards->rowCount();rewards->insertRow(row);checkCell(rewards,row,r.enabled);
        auto *picker=new QComboBox(rewards);
        populateRewardCombo(picker,selected,r.trigger,r.title);
        rewards->setCellWidget(row,1,picker);
        appendAction(rewards,row,r,false);
    };
    auto collect = [&] {
        if (selected.isEmpty()) return;
        auto &config = draft[selected];
        config.roomId = rooms.value(selected);
        config.enabled = enabled->isChecked();
        config.dryRun = dryRun->isChecked();
        config.exceptions = exceptions->text().simplified().toLower().split(u' ',Qt::SkipEmptyParts);
        config.rules.clear();
        for(int row=0; row<words->rowCount();++row)
        {
            auto *action=qobject_cast<QComboBox *>(words->cellWidget(row,2));
            config.rules.append(Rule{Kind::Words,words->item(row,1)->text().trimmed(),{},
                qobject_cast<QSpinBox *>(words->cellWidget(row,3))->value(), words->item(row,4)->text().trimmed(),
                words->item(row,0)->checkState()==Qt::Checked, action->currentData(Qt::UserRole+1).toString(),
                words->cellWidget(row,5)->property("moduleParams").value<QJsonObject>()});
        }
        for(int row=0; row<rewards->rowCount();++row)
        {
            auto *picker = qobject_cast<QComboBox *>(rewards->cellWidget(row,1));
            auto *action = qobject_cast<QComboBox *>(rewards->cellWidget(row,2));
            config.rules.append(Rule{Kind(action->currentData().toInt()),picker->currentData().toString(),
                picker->currentText(),qobject_cast<QSpinBox *>(rewards->cellWidget(row,3))->value(),
                rewards->item(row,4)->text().trimmed(),rewards->item(row,0)->checkState()==Qt::Checked,
                action->currentData(Qt::UserRole+1).toString(),
                rewards->cellWidget(row,5)->property("moduleParams").value<QJsonObject>()});
        }
    };
    auto showHistory = [&] {
        history->setRowCount(0);
        for(auto it=s->journal.crbegin();it!=s->journal.crend() && history->rowCount()<200;++it)
        {
            if(it->value("channel").toString()!=selected) continue;
            const int row=history->rowCount();history->insertRow(row);
            const auto at=QDateTime::fromMSecsSinceEpoch(it->value("at").toInteger()).toString("dd.MM HH:mm:ss");
            history->setItem(row,0,new QTableWidgetItem(at));
            history->setItem(row,1,new QTableWidgetItem(kindText(Kind(it->value("kind").toInt()))));
            history->setItem(row,2,new QTableWidgetItem(it->value("target").toString()));
            auto result=uiText(it->value("status").toString());
            if(const auto seconds=it->value("duration").toInt();seconds>0)
                result+=QStringLiteral(" · ")+QString::number(seconds)+uiText(" sec");
            if(!it->value("reply").toString().isEmpty())
                result+=QStringLiteral(" · ")+it->value("reply").toString();
            history->setItem(row,3,new QTableWidgetItem(result));
        }
    };
    auto requestRewards = [&](bool force) {
#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
        if(selected.isEmpty() || activeUid()!=uid || (getApp()->isTest() && !fixtureFetcher)) return;
        const auto name=selected;
        auto tc=std::dynamic_pointer_cast<TwitchChannel>(getApp()->getTwitch()->getChannelOrEmpty(name));
        if(!tc || !permittedAutoModeration(*tc) || tc->roomId()!=rooms.value(name)) return;
        if(loadingRewards.contains(name))
        {
            refreshRewards->setEnabled(false);
            status->setText(uiText("Loading rewards..."));
            return;
        }
        const auto showCount = [&] {
            const auto count=rewardChoices.value(selected).size();
            status->setText(count ? uiText("Available text-input rewards: %1").arg(count)
                                  : uiText("No rewards requiring text input were found."));
        };
        if(!force && rewardChoices.contains(name))
        {
            refreshRewards->setEnabled(true);
            showCount();
            return;
        }
        loadingRewards.insert(name);
        refreshRewards->setEnabled(false);
        status->setText(uiText("Loading rewards..."));
        QPointer<QDialog> guard(&dialog);
        auto success=[&,guard,name](GqlChannelPointRewards result) {
            if(!guard)return;
            loadingRewards.remove(name);
            auto currentChannel=std::dynamic_pointer_cast<TwitchChannel>(getApp()->getTwitch()->getChannelOrEmpty(name));
            const bool current=selected==name;
            if(current) refreshRewards->setEnabled(true);
            if(activeUid()!=uid || !currentChannel || !permittedAutoModeration(*currentChannel) ||
               result.channelId!=rooms.value(name) || currentChannel->roomId()!=rooms.value(name))
            {
                if(current) status->setText(uiText("Unable to load rewards."));
                return;
            }
            QList<std::pair<QString,QString>> choices;
            for(const auto &r:result.rewards)
                if(!r.isAutomatic && r.isUserInputRequired && rewardId(r.id)) choices.append({r.id,r.title});
            rewardChoices[name]=choices;
            if(!current)return;
            for(int row=0;row<rewards->rowCount();++row)
            {
                auto *combo=qobject_cast<QComboBox *>(rewards->cellWidget(row,1));
                const auto id=combo->currentData().toString(),title=combo->currentText();
                populateRewardCombo(combo,name,id,title);
            }
            const auto count=choices.size();
            status->setText(count ? uiText("Available text-input rewards: %1").arg(count)
                                  : uiText("No rewards requiring text input were found."));
        };
        auto failure=[&,guard,name](const QString &) {
            if(!guard)return;
            loadingRewards.remove(name);
            if(selected==name)
            {
                refreshRewards->setEnabled(true);
                status->setText(uiText("Unable to load rewards. Check the connection and sign-in for extra features."));
            }
        };
        if(getApp()->isTest())
        {
            fixtureFetcher(name,std::move(success),std::move(failure));
            return;
        }
        // The regular channel-points menu uses the extra-feature TV login.
        // The token is only used to read the reward catalog; moderation still
        // uses the current Twitch ID and the independently verified role.
        const auto auth=MoltorinoAuth::resolveCurrentUserToken();
        const auto token=auth.hasToken() ? auth.token : getApp()->getAccounts()->twitch.getCurrent()->getOAuthToken();
        if(token.isEmpty()) {failure({});return;}
        TwitchGql::getChannelPointRewards(name,token,std::move(success),std::move(failure));
#else
        Q_UNUSED(force);
#endif
    };
    auto load = [&](const QString &name) {
        collect();
        selected=name;
        words->setRowCount(0);rewards->setRowCount(0);
        Config config=draft.value(name);config.roomId=rooms.value(name);
        enabled->setChecked(config.enabled);dryRun->setChecked(config.dryRun);
        exceptions->setText(config.exceptions.join(u' '));
        for(const auto &r:config.rules)
            if(r.kind==Kind::Words) appendWord(r);else appendReward(r);
        showHistory();
        if(tabs->currentWidget()==rewardPage->parentWidget()) requestRewards(false);
    };
    QObject::connect(channel,&QComboBox::currentTextChanged,&dialog,load);
    QObject::connect(addWord,&QPushButton::clicked,&dialog,[&] {
        if(words->rowCount()+rewards->rowCount()<100) {appendWord(Rule{});words->setCurrentCell(words->rowCount()-1,1);words->editItem(words->item(words->rowCount()-1,1));}
    });
    QObject::connect(removeWord,&QPushButton::clicked,&dialog,[&] {if(words->currentRow()>=0)words->removeRow(words->currentRow());});
    QObject::connect(addReward,&QPushButton::clicked,&dialog,[&] {
        if(words->rowCount()+rewards->rowCount()<100) appendReward(Rule{Kind::RewardTimeout});
        requestRewards(false);
    });
    QObject::connect(removeReward,&QPushButton::clicked,&dialog,[&] {if(rewards->currentRow()>=0)rewards->removeRow(rewards->currentRow());});
    QObject::connect(refreshRewards,&QPushButton::clicked,&dialog,[&] {requestRewards(true);});
    QObject::connect(tabs,&QTabWidget::currentChanged,&dialog,[&](int index) {
        if(tabs->widget(index)==rewardPage->parentWidget()) requestRewards(false);
    });
    QObject::connect(saveButton,&QPushButton::clicked,&dialog,[&] {
        collect();
        if(activeUid()!=uid || !s->healthy) {status->setText(uiText("Unable to save rules."));return;}
        bool confirm=false;
        for(auto it=draft.cbegin();it!=draft.cend();++it)
        {
            if(!rooms.contains(it.key())) continue;
            const auto &c=it.value();
            auto tc=std::dynamic_pointer_cast<TwitchChannel>(getApp()->getTwitch()->getChannelOrEmpty(it.key()));
            if(!tc || !permittedAutoModeration(*tc) || tc->roomId()!=c.roomId || !validate(c))
            {status->setText(uiText("Check moderation rules for %1: incomplete or duplicate rules.").arg(it.key()));return;}
            const auto old=s->configs.value(it.key());
            if(c.enabled && !c.dryRun && encode(c)!=encode(old)) confirm=true;
        }
        if(confirm)
        {
            QMessageBox confirmation(QMessageBox::Question,uiText("Enable real moderation?"),
                uiText("Rules will execute real timeouts on Twitch. Verify the rules in test mode first."),
                QMessageBox::Yes|QMessageBox::No,&dialog);
            confirmation.setDefaultButton(QMessageBox::No);
            if(automationdialog::run(confirmation)!=QMessageBox::Yes) return;
        }
        // Recheck after the modal confirmation: accounts/roles may change.
        if(activeUid()!=uid) {dialog.reject();return;}
        for(auto it=draft.cbegin();it!=draft.cend();++it)
        {
            if(!rooms.contains(it.key()))continue;
            auto tc=std::dynamic_pointer_cast<TwitchChannel>(getApp()->getTwitch()->getChannelOrEmpty(it.key()));
            if(!tc || !permittedAutoModeration(*tc) || tc->roomId()!=it->roomId)
            {status->setText(uiText("Unable to save rules."));return;}
        }
        for(auto it=draft.cbegin();it!=draft.cend();++it)
            if(rooms.contains(it.key())) s->configs[it.key()]=it.value();
        ++s->generation;
        status->setText(save(*s)?uiText("Auto-reply rules saved."):uiText("Unable to save rules."));
    });
    QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::reject);
    QTimer tick(&dialog);tick.setInterval(1000);
    QObject::connect(&tick,&QTimer::timeout,&dialog,[&] {
        if(activeUid()!=uid){dialog.reject();return;}
        if(!selected.isEmpty())
        {
            auto tc=std::dynamic_pointer_cast<TwitchChannel>(getApp()->getTwitch()->getChannelOrEmpty(selected));
            if(!tc || !permittedAutoModeration(*tc)){dialog.reject();return;}
        }
        if(tabs->currentWidget()==historyPage->parentWidget())showHistory();
    });tick.start();
    if(!channels.isEmpty())load(channels.first());
    else status->setText(uiText("No verified moderator channels are currently available."));
    saveButton->setEnabled(!channels.isEmpty() && s->healthy);
    tabs->setEnabled(!channels.isEmpty() && s->healthy);
    enabled->setEnabled(!channels.isEmpty() && s->healthy);dryRun->setEnabled(enabled->isEnabled());
    if(!s->healthy)status->setText(uiText("Moderation data could not be read or saved. Actions are disabled."));
    installMoltorinoDialogTheme(&dialog,[&] {
        auto *t=getTheme();
        dialog.setStyleSheet(QStringLiteral(R"(
QDialog, QTabWidget::pane, QTableWidget { background:%1; color:%2; }
QLabel, QCheckBox { background:transparent; color:%2; }
QLabel#MutedText, QLabel#AutoModerationStatus { color:%5; }
QTableWidget { border:1px solid %4; border-radius:6px; alternate-background-color:%3; }
QHeaderView::section { background:%3; color:%2; padding:6px; border:0; border-bottom:1px solid %4; }
QTableWidget::item { padding:4px; }
QTableWidget::item:selected { background:%3; color:%2; }
QTabBar::tab { background:transparent;color:%5;padding:7px 14px; }
QTabBar::tab:selected { color:%2; border-bottom:2px solid %6; }
QPushButton, QLineEdit, QComboBox, QSpinBox { background:%3;color:%2;border:1px solid %4;border-radius:5px;padding:5px 8px; }
QPushButton:hover, QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border-color:%6; }
QPushButton#SaveRules { background:%6;color:%1; }
)").arg(t->messages.backgrounds.regular.name(),t->messages.textColors.regular.name(),
            t->messages.backgrounds.alternate.name(),t->tabs.dividerLine.name(),
            t->messages.textColors.system.name(),t->tabs.selected.line.regular.name()));
        for(auto *child:dialog.findChildren<QWidget *>())child->setFont(font);
        for(auto *table:{words,rewards,history})table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height()+18);
    });
    automationdialog::run(dialog);
    tick.stop();
    for(auto *child:dialog.findChildren<QWidget *>())QObject::disconnect(child,nullptr,&dialog,nullptr);
}
} // namespace chatterino



