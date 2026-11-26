#include "providers/twitch/AutoModeration.hpp"
#include "providers/twitch/AutoModerationRules.hpp"
#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
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
    QElapsedTimer lastAction;
};
QHash<QString, std::shared_ptr<State>> states;
boost::signals2::scoped_connection accountChanged;
AccountController *watchedAccounts = nullptr;

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
};
bool proceed(const Intent &i)
{
    if (!i.state->healthy || activeUid() != i.state->uid ||
        i.generation != i.state->generation ||
        !freshEvent(i.at, QDateTime::currentMSecsSinceEpoch()))
        return false;
    const auto c = i.state->configs.value(i.channel);
    if (!c.enabled || c.roomId != i.room) return false;
    const auto channel = getApp()->getTwitch()->getChannelOrEmpty(i.channel);
    auto *tc = dynamic_cast<TwitchChannel *>(channel.get());
    return tc && tc->roomId() == i.room && permittedAutoModeration(*tc);
}
void execute(const Intent &i, const QString &targetId, const QString &targetLogin)
{
    if (!proceed(i)) { finish(i.state,i.event,"Action canceled"); return; }
    const auto c = i.state->configs.value(i.channel);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto tk = targetKey(i.room,targetId);
    const auto known = i.state->roles.value(tk);
    // Only protect the account performing moderation, not other local accounts.
    if (!validId(targetId) || targetId == i.room || targetId == i.state->uid ||
        c.exceptions.contains(targetLogin) ||
        (known.protectedRole && known.seenAt > 0))
    {
        finish(i.state,i.event,"Protected or excluded target"); return;
    }
    const bool untimeout = i.rule.kind == Kind::RewardUntimeout;
    if (untimeout && !canShortenTimeout(i.state->timeouts.value(tk).until, now))
    {
        finish(i.state,i.event,"No confirmed active timeout"); return;
    }
    // For muting, require a recent live role observation; do not guess whether
    // an unseen account is a moderator or VIP.
    if (!untimeout && (known.seenAt <= 0 || now - known.seenAt > 30 * 60 * 1000 ||
                       known.login != targetLogin))
    {
        finish(i.state,i.event,"Target role could not be confirmed"); return;
    }
    if (c.dryRun)
    {
        finish(i.state,i.event,"Test: would perform action"); return;
    }
    // No permanent bans or DELETE /bans. A finite one-second timeout can
    // shorten an existing timeout but Twitch rejects changing a permanent ban
    // to a timeout. Never retry an ambiguous result.
    if (!proceed(i) || i.state->configs.value(i.channel).dryRun)
    { finish(i.state,i.event,"Action canceled"); return; }
    const auto seconds = untimeout ? 1 : i.rule.seconds;
    getHelix()->banUser(i.room, i.state->uid, targetId, seconds, i.rule.reason,
        [i,tk,seconds,untimeout] {
            if (untimeout) i.state->timeouts.remove(tk);
            else
            {
                const auto at=QDateTime::currentMSecsSinceEpoch();
                i.state->timeouts.insert(tk,{at+qint64(seconds)*1000,at});
            }
            finish(i.state,i.event,"Action confirmed");
        },
        [i](auto, const QString &) { finish(i.state,i.event,"Action failed or unconfirmed; not retried"); });
}
QString durationText(int seconds)
{
    return QString::number(seconds) + uiText(" sec");
}
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
    if (!valid || timestamp < sessionStart || !freshEvent(timestamp,QDateTime::currentMSecsSinceEpoch())) return;
    const auto key = targetKey(room,target);
    const auto seconds = tags.getOrEmpty("ban-duration").toInt(&valid);
    if (valid && seconds > 0 && seconds <= 1209600)
        s->timeouts.insert(key,{timestamp+qint64(seconds)*1000,timestamp});
    else
        s->timeouts.remove(key); // permanent ban, never shorten it
    if (s->timeouts.size() > 4096) s->timeouts.clear();
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
        s->roles.insert(tk,{name,protectedBadges(tags.getOrEmpty("badges")),timestamp});
    if (const auto timeout=s->timeouts.value(tk); timeout.observedAt>0 && timestamp>timeout.observedAt)
        s->timeouts.remove(tk); // only a later live message disproves the timeout
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
    const Intent action{s,key,channel.getName(),room,sender,target,*rule,s->generation,timestamp};
    if (words)
    {
        execute(action,sender,name);
        return;
    }
    if (!proceed(action)) { finish(s,key,"Action canceled"); return; }
    getHelix()->getUserByName(target,[action,target](HelixUser user) {
        if (user.login.toLower() != target || !validId(user.id))
        { finish(action.state,action.event,"Invalid target login"); return; }
        execute(action,user.id,target);
    },[action] { finish(action.state,action.event,"Target lookup failed"); });
}

void openAutoModeration(QWidget *parent,
                        AutoModerationRewardsFetcher fixtureFetcher)
{
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
    auto *wordPage = setupPage(uiText("Words"));
    auto *wordHint = new QLabel(uiText("A contiguous fragment, ignoring letter case. The author of a matching message receives a timeout."),&dialog);
    wordHint->setWordWrap(true);
    wordPage->addWidget(wordHint);
    auto *words = new QTableWidget(0,4,&dialog);
    words->setObjectName("AutoModerationWords");
    words->setHorizontalHeaderLabels({uiText("Enabled"),uiText("Contains phrase"),uiText("Timeout duration"),uiText("Reason")});
    wordPage->addWidget(words,1);
    auto *wordButtons = new QHBoxLayout;
    auto *addWord = new QPushButton(uiText("Add rule"),&dialog);
    addWord->setObjectName("AutoModerationAddWord");
    auto *removeWord = new QPushButton(uiText("Remove selected"),&dialog);
    wordButtons->addWidget(addWord);wordButtons->addWidget(removeWord);wordButtons->addStretch();
    wordPage->addLayout(wordButtons);
    auto *rewardPage = setupPage(uiText("Rewards"));
    auto *rewardHint = new QLabel(uiText("Choose a reward that requires a nickname as input. Enter only one exact Twitch login. Untimeout requires an observed active timeout; a permanent ban is never removed."),&dialog);
    rewardHint->setWordWrap(true);
    rewardPage->addWidget(rewardHint);
    auto *rewards = new QTableWidget(0,5,&dialog);
    rewards->setObjectName("AutoModerationRewards");
    rewards->setHorizontalHeaderLabels({uiText("Enabled"),uiText("Reward"),uiText("Action"),uiText("Timeout duration"),uiText("Reason")});
    rewardPage->addWidget(rewards,1);
    auto *rewardButtons = new QHBoxLayout;
    auto *refreshRewards = new QPushButton(uiText("Refresh rewards"),&dialog);
    refreshRewards->setObjectName("AutoModerationRefreshRewards");
    auto *addReward = new QPushButton(uiText("Add rule"),&dialog);
    addReward->setObjectName("AutoModerationAddReward");
    auto *removeReward = new QPushButton(uiText("Remove selected"),&dialog);
    rewardButtons->addWidget(refreshRewards);rewardButtons->addWidget(addReward);rewardButtons->addWidget(removeReward);rewardButtons->addStretch();
    rewardPage->addLayout(rewardButtons);
    auto *historyPage = setupPage(uiText("History"));
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
    auto appendWord = [&](const Rule &r) {
        const int row = words->rowCount();
        words->insertRow(row);
        checkCell(words,row,r.enabled);
        words->setItem(row,1,new QTableWidgetItem(r.trigger));
        duration(words,row,2,r.seconds);
        words->setItem(row,3,new QTableWidgetItem(r.reason));
    };
    auto appendReward = [&](const Rule &r) {
        const int row = rewards->rowCount();
        rewards->insertRow(row);
        checkCell(rewards,row,r.enabled);
        auto *picker = new QComboBox(rewards);
        populateRewardCombo(picker,selected,r.trigger,r.title);
        rewards->setCellWidget(row,1,picker);
        auto *action = new QComboBox(rewards);
        action->addItem(uiText("Timeout"),int(Kind::RewardTimeout));
        action->addItem(uiText("Remove timeout"),int(Kind::RewardUntimeout));
        action->setCurrentIndex(r.kind==Kind::RewardUntimeout ? 1 : 0);
        rewards->setCellWidget(row,2,action);
        auto *spin = duration(rewards,row,3,r.seconds);
        spin->setEnabled(action->currentIndex()==0);
        QObject::connect(action,&QComboBox::currentIndexChanged,spin,[spin](int index){spin->setEnabled(index==0);});
        rewards->setItem(row,4,new QTableWidgetItem(r.reason));
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
            config.rules.append(Rule{Kind::Words,words->item(row,1)->text().trimmed(),{},
                qobject_cast<QSpinBox *>(words->cellWidget(row,2))->value(), words->item(row,3)->text().trimmed(),
                words->item(row,0)->checkState()==Qt::Checked});
        for(int row=0; row<rewards->rowCount();++row)
        {
            auto *picker = qobject_cast<QComboBox *>(rewards->cellWidget(row,1));
            auto *action = qobject_cast<QComboBox *>(rewards->cellWidget(row,2));
            config.rules.append(Rule{Kind(action->currentData().toInt()),picker->currentData().toString(),
                picker->currentText(),qobject_cast<QSpinBox *>(rewards->cellWidget(row,3))->value(),
                rewards->item(row,4)->text().trimmed(),rewards->item(row,0)->checkState()==Qt::Checked});
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
            history->setItem(row,3,new QTableWidgetItem(uiText(it->value("status").toString())));
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
        if(tabs->currentIndex()==1) requestRewards(false);
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
        if(index==1) requestRewards(false);
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
        if(confirm && QMessageBox::question(&dialog,uiText("Enable real moderation?"),
            uiText("Rules will execute real timeouts on Twitch. Verify the rules in test mode first."),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
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
        if(tabs->currentIndex()==2)showHistory();
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
    dialog.exec();
    tick.stop();
    for(auto *child:dialog.findChildren<QWidget *>())QObject::disconnect(child,nullptr,&dialog,nullptr);
}
} // namespace chatterino



