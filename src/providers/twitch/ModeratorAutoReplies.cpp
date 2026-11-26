#include "providers/twitch/ModeratorAutoReplies.hpp"
#include "providers/twitch/AutoModeration.hpp"
#include "providers/twitch/ModeratorExtension.hpp"
#include "providers/twitch/AutomationDialog.hpp"
#include "providers/twitch/ModeratorModule.hpp"
#include "providers/twitch/ModeratorReplyFeatures.hpp"
#include "providers/twitch/ReplyQuestionConflicts.hpp"
#include "providers/twitch/AutoReplyWords.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/UiLanguage.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/CancellationToken.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/Window.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFormLayout>
#include <QFrame>
#include <QLineEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QQueue>
#include <QSignalBlocker>
#include <QUrlQuery>
#include <boost/signals2/connection.hpp>
#include <algorithm>

namespace chatterino {
namespace {
constexpr auto MODULE_URL =
    "https://raw.githubusercontent.com/Mist10X/Chatterino-Noctis/main/moderator/module.json";
constexpr int MODULE_REFRESH_MS = 5 * 60 * 1000;
constexpr int MODULE_FRESH_MS = 6 * 60 * 1000;
constexpr int ROLE_REFRESH_MS = 60 * 1000;
constexpr int ROLE_FRESH_MS = 90 * 1000;
constexpr int MAX_DEDUP = 8192;
constexpr int MAX_LOCAL_RULES = 200;
QPointer<ModeratorAutoReplies> service;

struct Rule {
    QString channel;
    QStringList triggers;
    QString reply;
    bool exact = true;
    bool enabled = true;
    QString title;
    bool allWords = false;
    QList<QStringList> wordSets;

    QString label() const { return title.isEmpty() ? triggers.value(0) : title; }
};

QString currentId()
{
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    return account && !account->isAnon() ? account->getUserId() : QString{};
}

bool fresh(const QElapsedTimer &timer, int limit)
{
    return timer.isValid() && timer.elapsed() < limit;
}

bool validRule(const Rule &r)
{
    return moderator::validChannel(r.channel) &&
           !r.triggers.empty() && r.triggers.size() <= 30 &&
           std::all_of(r.triggers.begin(), r.triggers.end(), [](const auto &trigger) {
               return !trigger.simplified().isEmpty() && trigger.size() <= 500;
           }) && r.title.size() <= 80 && moderator::validReply(r.reply);
}

bool prepareRule(Rule &r)
{
    if (!validRule(r)) return false;
    r.wordSets.clear();
    if (r.allWords)
        for (const auto &trigger : r.triggers)
        {
            auto words = autoreplywords::tokens(trigger);
            if (words.isEmpty()) return false;
            r.wordSets.append(std::move(words));
        }
    return true;
}

class RuleDialog final : public QDialog {
public:
    explicit RuleDialog(QWidget *parent) : QDialog(parent) {}
};

void openModeratorHelp(QDialog &parent, const QFont &font)
{
    QDialog help(&parent);
    help.setObjectName("ModeratorHelpDialog");
    help.setWindowTitle(uiText("Auto-reply guide"));
    help.setFont(font);
    const auto screen = parent.screen()->availableGeometry();
    help.resize(std::min(640, screen.width() - 36), std::min(630, screen.height() - 48));
    auto *layout = new QVBoxLayout(&help);
    layout->setContentsMargins(14, 14, 14, 12);
    auto *scroll = new QScrollArea(&help);
    scroll->setObjectName("ModeratorHelpScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    content->setObjectName("ModeratorHelpContent");
    auto *sections = new QVBoxLayout(content);
    sections->setContentsMargins(0, 0, 8, 0);
    sections->setSpacing(10);
    const auto add = [&](const QString &title, const QString &body) {
        auto *card = new QFrame(content);
        card->setObjectName("ModeratorHelpSection");
        auto *box = new QVBoxLayout(card);
        box->setContentsMargins(12, 10, 12, 10);
        box->setSpacing(5);
        auto *heading = new QLabel(title, card);
        heading->setObjectName("ModeratorHelpHeading");
        heading->setWordWrap(true);
        auto headingFont = font;
        headingFont.setWeight(QFont::DemiBold);
        heading->setFont(headingFont);
        auto *text = new QLabel(body, card);
        text->setFont(font);
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        box->addWidget(heading);
        box->addWidget(text);
        sections->addWidget(card);
    };
    add(uiText("Quick start"), uiText("Select a channel where you are a moderator, add a rule or group, enter the questions and reply text, then save. Enable auto-replies and the rule. In Options select the roles of people to answer."));
    add(uiText("Full match"), uiText("The whole message must equal the question. Letter case and repeated spaces are ignored, punctuation is not. Example: !playlist matches !playlist, but not please !playlist."));
    add(uiText("Contains phrase"), uiText("The question must occur as one continuous phrase inside the message. Example: what game matches hey what game is this. A fragment may also occur inside another word, so avoid very short triggers."));
    add(uiText("All words"), uiText("All the words on one line must be present as separate, whole words, in any order. Case and punctuation are ignored. Example: long watching movie matches How long have you been watching this movie? but not How long have you been watching?"));
    add(uiText("Question variants"), uiText("Write each alternative on a separate line. Any one matching line is enough; words from different lines are not mixed. Only one reply is selected: full match first, then the shortest matching phrase, then the most specific word set. Identical variants or reordered identical word sets are marked as conflicts."));
    add(uiText("Reply variables"), uiText("{game} — current category name.\n{game_time} — time in the current uninterrupted category segment.\n{game_since} — local time when that segment began.\n{game_total} — total time in this category during the current stream, including earlier visits.\n{timer} — elapsed time of the channel's manual timer."));
    add(uiText("Manual timer"), uiText("Choose the channel and start the timer now, a specified number of minutes ago, or from a specific date and time. It survives restarting Noctis. Use it for activities such as watching a movie without changing category. Stop it when the activity ends."));
    add(uiText("When replies are sent"), uiText("Category variables do not trigger replies while offline or in Just Chatting. The manual timer requires a live stream and a running timer. If category history has a gap, unknown times are displayed as unknown. Replies are sent only on verified moderator channels; the configured channel pause, account-wide anti-flood and Twitch limits still apply. Duplicate events are never sent again."));
    sections->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto *close = new QPushButton(uiText("Close"), &help);
    close->setFont(font);
    close->setObjectName("ModeratorHelpClose");
    layout->addWidget(close, 0, Qt::AlignRight);
    QObject::connect(close, &QPushButton::clicked, &help, &QDialog::accept);
    QObject::connect(&parent, &QDialog::finished, &help, &QDialog::reject);
    installMoltorinoDialogTheme(&help, [&help] {
        const auto *t = getTheme();
        help.setStyleSheet(QStringLiteral(R"(
QDialog#ModeratorHelpDialog, QScrollArea, QWidget#ModeratorHelpContent {
 background:%1; color:%2; border:0;
}
QFrame#ModeratorHelpSection { background:%3; border:1px solid %4; border-radius:8px; }
QLabel { background:transparent; color:%2; }
QLabel#ModeratorHelpHeading { color:%5; }
QPushButton#ModeratorHelpClose { background:%3; color:%2; border:1px solid %4; border-radius:5px; padding:5px 14px; }
QPushButton#ModeratorHelpClose:hover { border-color:%5; }
QScrollBar:vertical { width:10px; background:transparent; }
QScrollBar::handle:vertical { background:%4; min-height:26px; border-radius:4px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
)").arg(t->messages.backgrounds.regular.name(), t->messages.textColors.regular.name(),
        t->messages.backgrounds.alternate.name(), t->tabs.dividerLine.name(),
        t->tabs.selected.line.regular.name()));
    });
    automationdialog::run(help);
}
}

struct ModeratorAutoReplies::Impl {
    ModeratorAutoReplies &owner;
    QTimer timer;
    boost::signals2::scoped_connection accountChanged;
    std::optional<moderator::Policy> policy;
    std::unique_ptr<moderator::Matcher> matcher;
    qint64 highestRevision = 0;
    QByteArray highestDigest;
    QElapsedTimer policyRequest, policyFresh, roleRequest, roleFresh;
    bool fetchingPolicy = false, fetchingRoles = false;
    QString account, roleAccount;
    QSet<QString> roles;
    CancellationToken roleToken;
    quint64 roleGeneration = 0;
    QList<Rule> rules;
    bool enabled = false;
    int cooldown = 10;
    unsigned senderRoles = moderator::DEFAULT_SENDER_ROLES;
    QHash<QString, qint64> manualTimers;
    QHash<QString, moderator::CategoryState> categoryStates;
    QHash<QString, QElapsedTimer> lastReplies;
    QHash<QString, QElapsedTimer> lastAccountReply;
    QSet<QString> seenEvents;
    QQueue<QString> eventOrder;

    explicit Impl(ModeratorAutoReplies &o) : owner(o)
    {
    }
    ~Impl()
    {
        roleToken.cancel();
    }
    QString fileName() const
    {
        if (!moderator::validUserId(account))
            return {};
        return QDir(getApp()->getPaths().settingsDirectory).filePath(
            QStringLiteral("moderator-auto-replies-") + account + ".json");
    }
    bool granted() const
    {
        return policy && matcher && matcher->valid() &&
            fresh(policyFresh, MODULE_FRESH_MS) &&
            policy->expires > QDateTime::currentDateTimeUtc() &&
            moderator::validUserId(account) && account == currentId() &&
            policy->allowed.contains(account);
    }
    bool rolesValid() const
    {
        return granted() && roleAccount == account && fresh(roleFresh, ROLE_FRESH_MS);
    }
    QSet<QString> includeOwnChannel(QSet<QString> confirmed) const
    {
        const auto user = getApp()->getAccounts()->twitch.getCurrent();
        if (user && !user->isAnon() && user->getUserId() == account)
        {
            const auto login = user->getUserName().toLower();
            if (moderator::validChannel(login)) confirmed.insert(login);
        }
        return confirmed;
    }
    void loadRules()
    {
        rules.clear();
        enabled = false;
        cooldown = 10;
        senderRoles = moderator::DEFAULT_SENDER_ROLES;
        manualTimers.clear();
        categoryStates.clear();
        QFile file(fileName());
        if (fileName().isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            return;
        const auto data = QJsonDocument::fromJson(file.readAll()).object();
        enabled = data.value("enabled").toBool(false);
        cooldown = std::clamp(data.value("cooldown").toInt(10), 0, 300);
        senderRoles = moderator::decodeRoles(data.value("senderRoles"));
        manualTimers = moderator::decodeTimers(data.value("manualTimers").toObject(), QDateTime::currentSecsSinceEpoch());
        for (const auto &entry : data.value("rules").toArray())
        {
            const auto object = entry.toObject();
            Rule r{object.value("channel").toString(), {}, object.value("reply").toString(),
                   object.value("exact").toBool(true), object.value("enabled").toBool(true), object.value("title").toString(),
                   object.value("allWords").toBool(false)};
            for (const auto &value : object.value("triggers").toArray())
                r.triggers.append(value.toString());
            if (prepareRule(r) && rules.size() < MAX_LOCAL_RULES)
                rules.append(std::move(r));
        }
    }
    bool save(bool newEnabled, int newCooldown, const QList<Rule> &newRules,
              unsigned newSenderRoles, const QHash<QString, qint64> &newTimers)
    {
        if (!granted() || !rolesValid() || newCooldown < 0 || newCooldown > 300 ||
            newRules.size() > MAX_LOCAL_RULES || newSenderRoles > 15 || newTimers.size() > 1000)
            return false;
        QJsonArray array;
        auto preparedRules = newRules;
        for (auto &r : preparedRules)
        {
            if (!prepareRule(r))
                return false;
            QJsonArray triggers;
            for (const auto &trigger : r.triggers)
                triggers.append(trigger);
            array.append(QJsonObject{{"channel", r.channel}, {"triggers", triggers},
                                     {"reply", r.reply}, {"exact", r.allWords || r.exact}, {"allWords", r.allWords},
                                     {"enabled", r.enabled}, {"title", r.title}});
        }
        QSaveFile file(fileName());
        const auto raw = QJsonDocument(QJsonObject{{"enabled", newEnabled},
            {"cooldown", newCooldown}, {"rules", array}, {"senderRoles", moderator::encodeRoles(newSenderRoles)},
            {"manualTimers", moderator::encodeTimers(newTimers)}}).toJson();
        if (raw.size() > 1024 * 1024 || !file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size() || !file.commit())
            return false;
        rules = std::move(preparedRules);
        enabled = newEnabled;
        cooldown = newCooldown;
        senderRoles = newSenderRoles;
        manualTimers = newTimers;
        return true;
    }
    bool setTimer(const QString &channel, qint64 start)
    {
        if (!rolesValid() || !roles.contains(channel) || !moderator::validChannel(channel) ||
            start < 0 || start > QDateTime::currentSecsSinceEpoch()) return false;
        auto next = manualTimers;
        if (start) next.insert(channel, start);
        else next.remove(channel);
        return save(enabled, cooldown, rules, senderRoles, next);
    }
    bool permittedChannel(TwitchChannel &channel) const
    {
        const bool currentBroadcaster =
            channel.isBroadcaster() && channel.roomId() == account;
        const bool currentModerator =
            channel.isBroadcaster() ? currentBroadcaster : channel.isMod();
        return rolesValid() && !channel.isAnonymous() &&
            moderator::permitted(policy ? &*policy : nullptr, currentId(), roleAccount,
                roles, channel.getName(), rolesValid(), currentModerator);
    }
    void invalidateRoles()
    {
        roleToken.cancel();
        ++roleGeneration;
        fetchingRoles = false;
        roleAccount.clear();
        roles.clear();
        roleFresh.invalidate();
        roleRequest.invalidate();
    }
    void accountSelected()
    {
        const auto id = currentId();
        if (id == account)
            return;
        invalidateRoles();
        account = id;
        loadRules();
        owner.changed.invoke();
        // Helix updates its credentials on the same account-change signal.
        QTimer::singleShot(0, &owner, [this] { tick(); });
    }
    void fetchModule()
    {
        if (fetchingPolicy)
            return;
        fetchingPolicy = true;
        policyRequest.start();
        const auto self = QPointer<ModeratorAutoReplies>(&owner);
        QUrl url(QString::fromLatin1(MODULE_URL));
        QUrlQuery query;
        query.addQueryItem("r", QString::number(QDateTime::currentSecsSinceEpoch() / 60));
        url.setQuery(query);
        NetworkRequest(url).caller(&owner).timeout(15000)
            .header("Cache-Control", "no-cache")
            .followRedirects(false)
            .onSuccess([self](const NetworkResult &result) {
                if (!self) return;
                auto &s = *self->impl_;
                s.fetchingPolicy = false;
                const auto p = moderator::verifyPolicy(result.getData(), moderator::ownerPublicKey(),
                                                        QDateTime::currentDateTimeUtc());
                if (!p || p->revision < s.highestRevision ||
                    (p->revision == s.highestRevision && p->digest != s.highestDigest))
                {
                    s.policy.reset(); s.matcher.reset(); s.policyFresh.invalidate();
                    s.invalidateRoles(); self->changed.invoke(); return;
                }
                if (!s.policy || s.policy->digest != p->digest || !s.matcher)
                {
                    auto next = std::make_unique<moderator::Matcher>(p->source);
                    if (!next->valid())
                    {
                        s.policy.reset(); s.matcher.reset(); s.policyFresh.invalidate();
                        s.invalidateRoles(); self->changed.invoke(); return;
                    }
                    s.matcher = std::move(next);
                }
                s.highestRevision = p->revision;
                s.highestDigest = p->digest;
                s.policy = p;
                s.policyFresh.start();
                if (!s.granted()) s.invalidateRoles();
                self->changed.invoke();
                s.tick();
            })
            .onError([self](const NetworkResult &) {
                if (!self) return;
                auto &s = *self->impl_;
                s.fetchingPolicy = false; s.policy.reset(); s.matcher.reset();
                s.policyFresh.invalidate(); s.invalidateRoles(); self->changed.invoke();
            }).execute();
    }
    void fetchRoles()
    {
        if (!granted() || fetchingRoles)
            return;
        const auto uid = account;
        const auto generation = ++roleGeneration;
        roleRequest.start();
        fetchingRoles = true;
        roleToken.cancel();
        roleToken = CancellationToken(false);
        const auto self = QPointer<ModeratorAutoReplies>(&owner);
        getHelix()->getModeratedChannels(uid,
            [self, uid, generation](QSet<QString> result) {
                if (!self) return;
                auto &s = *self->impl_;
                if (s.roleGeneration != generation || s.account != uid || currentId() != uid || !s.granted())
                    return;
                s.fetchingRoles = false;
                QSet<QString> confirmed;
                for (const auto &name : result)
                {
                    const auto login = name.toLower();
                    if (moderator::validChannel(login))
                        confirmed.insert(login);
                }
                confirmed = s.includeOwnChannel(std::move(confirmed));
                const bool diff = s.roles != confirmed || !s.rolesValid();
                s.roles = std::move(confirmed);
                s.roleAccount = uid;
                s.roleFresh.start();
                if (diff) self->changed.invoke();
            },
            [self, uid, generation](const QString &) {
                if (!self) return;
                auto &s = *self->impl_;
                if (s.roleGeneration != generation || s.account != uid)
                    return;
                s.fetchingRoles = false;
                s.roleFresh.invalidate(); s.roles.clear(); s.roleAccount.clear();
                self->changed.invoke();
            }, CancellationToken(roleToken));
    }
    void tick()
    {
        if (account != currentId())
        {
            accountSelected();
            return;
        }
        if (!moderator::validUserId(account))
            return;
        if (!fresh(policyRequest, MODULE_REFRESH_MS))
            fetchModule();
        if (granted() && !fresh(roleRequest, ROLE_REFRESH_MS))
            fetchRoles();
        if (granted()) moderatorextension::start();
        // Expiry must also remove UI while a request is stalled.
        if (policy && (!fresh(policyFresh, MODULE_FRESH_MS) ||
                       policy->expires <= QDateTime::currentDateTimeUtc()))
        {
            policy.reset(); matcher.reset(); invalidateRoles();
            owner.changed.invoke();
        }
        if (roleFresh.isValid() && !fresh(roleFresh, ROLE_FRESH_MS))
        {
            invalidateRoles();
            owner.changed.invoke();
        }
        if (fetchingRoles && !fresh(roleRequest, ROLE_FRESH_MS))
        {
            invalidateRoles();
            owner.changed.invoke();
        }
    }
    bool reserve(const QString &id)
    {
        if (seenEvents.contains(id)) return false;
        seenEvents.insert(id); eventOrder.enqueue(id);
        while (eventOrder.size() > MAX_DEDUP)
            seenEvents.remove(eventOrder.dequeue());
        return true;
    }
};

ModeratorAutoReplies &ModeratorAutoReplies::instance()
{
    if (!service)
        service = new ModeratorAutoReplies(qApp);
    return *service;
}
ModeratorAutoReplies *ModeratorAutoReplies::existing()
{
    return service;
}
ModeratorAutoReplies::ModeratorAutoReplies(QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this))
{
    if (getApp()->isTest() || getApp()->getArgs().safeMode ||
        getApp()->getArgs().uiSnapshot || getApp()->getArgs().dontSaveSettings)
        return;
    impl_->accountChanged = getApp()->getAccounts()->twitch.currentUserChanged.connect(
        [this] { impl_->accountSelected(); });
    QObject::connect(&impl_->timer, &QTimer::timeout, this, [this] { impl_->tick(); });
    impl_->timer.start(15000);
    impl_->accountSelected();
}
ModeratorAutoReplies::~ModeratorAutoReplies() = default;
#ifdef CHATTERINO_WITH_TESTS
void ModeratorAutoReplies::configureFixture(const moderator::Policy &policy,
    const QString &roleAccount, const QStringList &roles, const QString &ruleChannel,
    unsigned senderMask, const QString &reply, bool allWords, int cooldownSeconds)
{
    if (!getApp()->isTest())
        return;
    auto &s = *impl_;
    s.account = currentId();
    s.policy = policy;
    s.matcher = std::make_unique<moderator::Matcher>(policy.source);
    s.roleAccount = roleAccount;
    s.roles = s.includeOwnChannel(QSet<QString>(roles.begin(), roles.end()));
    s.policyFresh.start();
    s.roleFresh.start();
    s.enabled = true;
    s.cooldown = cooldownSeconds;
    s.senderRoles = senderMask;
    s.rules = {{ruleChannel, {"fixture question"}, reply, true, true}};
    s.rules[0].allWords = allWords;
    prepareRule(s.rules[0]);
    changed.invoke();
}
void ModeratorAutoReplies::reloadFixtureRules()
{
    if (getApp()->isTest()) impl_->loadRules();
}
#endif
bool ModeratorAutoReplies::hasGrant() const { return impl_->granted(); }
bool ModeratorAutoReplies::canAutomoderate(TwitchChannel &channel) const
{
    return impl_->permittedChannel(channel);
}
bool permittedAutoModeration(TwitchChannel &channel)
{
    auto *module = ModeratorAutoReplies::existing();
    return module && module->canAutomoderate(channel);
}
QStringList ModeratorAutoReplies::channels() const
{
    if (!impl_->rolesValid())
        return {};
    auto list = impl_->roles.values();
    list.sort(Qt::CaseInsensitive);
    return list;
}

void ModeratorAutoReplies::observeStream(TwitchChannel &channel)
{
    auto &s = *impl_;
    if (!s.permittedChannel(channel) || !moderator::validUserId(channel.roomId())) return;
    const auto status = *channel.accessStreamStatus();
    if (!status.live || !status.startedAt.isValid()) return;
    const auto now = QDateTime::currentSecsSinceEpoch();
    const auto start = status.startedAt.toSecsSinceEpoch();
    const auto uid = channel.roomId();
    if (getApp()->isTest())
        s.categoryStates[uid].observe(status.streamId, status.gameId, status.game, start, now);
    else
    {
        const QDir dir(QDir(getApp()->getPaths().settingsDirectory).filePath("moderator-categories-" + s.account));
        s.categoryStates[uid] = moderator::observeCategoryFile(dir, uid, status.streamId,
            status.gameId, status.game, start, now);
    }
}

void ModeratorAutoReplies::maybeReply(TwitchChannel &channel, Communi::TagsRef tags,
                                       const QString &text)
{
    auto &s = *impl_;
    if (!s.enabled || !s.permittedChannel(channel))
        return;
    const auto id = tags.getOrEmpty("id");
    const auto sender = tags.getOrEmpty("user-id");
    if (id.isEmpty() || id.size() > 128 || !moderator::validUserId(sender) || sender == s.account ||
        s.seenEvents.contains(id))
        return;
    const auto roomId = tags.getOrEmpty("room-id");
    const auto sourceRoom = tags.getOrEmpty("source-room-id");
    if ((!sourceRoom.isEmpty() && (roomId.isEmpty() || sourceRoom != roomId)) ||
        tags.getOrEmpty("msg-id") == "sharedchatnotice")
        return;
    const auto &restriction = channel.ownRestrictionOf(s.account);
    using Restriction = TwitchChannel::OwnRestriction::Kind;
    if (restriction.kind == Restriction::Banned ||
        (restriction.kind == Restriction::TimedOut && restriction.until > QDateTime::currentDateTimeUtc()))
        return;
    // Reserve even a message received during a cooldown. A repeated event
    // after the cooldown must not turn into a new intent to send.
    if (!s.reserve(id))
        return;
    if (!moderator::acceptsRole(s.senderRoles, moderator::senderRole(tags))) return;
    auto &last = s.lastReplies[s.account + '/' + channel.getName()];
    auto &global = s.lastAccountReply[s.account];
    if (fresh(last, 1000 * std::max(s.cooldown, s.policy->minimumCooldown)) ||
        fresh(global, 1000))
        return;
    std::vector<moderator::MatchRule> inputs;
    QList<const Rule *> candidates;
    for (const auto &rule : s.rules)
    {
        if (!rule.enabled || rule.channel != channel.getName() || !validRule(rule)) continue;
        if (inputs.size() >= static_cast<size_t>(s.policy->maximumRules)) break;
        inputs.push_back({rule.triggers, rule.exact, rule.allWords, rule.wordSets});
        candidates.append(&rule);
    }
    const auto matched = s.matcher->match(text, inputs);
    if (!matched)
        return;
    auto reply = candidates.at(*matched)->reply.trimmed();
    if (reply.contains(u'{'))
    {
        const auto status = channel.accessStreamStatus();
        auto expanded = moderator::expandReply(reply, status->live, status->streamId,
            status->gameId, status->game, s.categoryStates.value(channel.roomId()),
            s.manualTimers.value(channel.getName()), QDateTime::currentSecsSinceEpoch());
        if (!expanded) return;
        reply = *expanded;
    }
    if (!moderator::validReply(reply) || !s.permittedChannel(channel))
        return;
    if (!moderatorextension::claimReplyEvent(channel.roomId(),id))
        return;
    last.start(); global.start();
    channel.sendReply(reply, id);
}

void ModeratorAutoReplies::open(QWidget *parent)
{
    if (!hasGrant()) return;
    if (automationdialog::raiseExisting("NoctisAutoReplyEditor")) return;
    moderatorextension::UiLease moduleUi;
    const auto &bundle=moderatorextension::active();
    auto &s = *impl_;
    const auto uid = s.account;
    const auto allowed = channels();
    const auto revision = s.policy->revision;
    const auto minimumCooldown = s.policy->minimumCooldown;
    const auto maximumRules = s.policy->maximumRules;
    RuleDialog dialog(parent);
    dialog.setObjectName("NoctisAutoReplyEditor");
    dialog.setWindowTitle(uiText("Chat auto-replies"));
    dialog.resize(920, 630);
    const auto scale = std::clamp(getSettings()->getClampedUiScale(), 1.F, 1.4F);
    auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, scale);
    font.setWeight(std::max(font.weight(), QFont::Medium));
    dialog.setFont(font);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    auto *primary = new QTabBar(&dialog);
    primary->setObjectName("ModeratorReplyTabs");
    primary->setDrawBase(false);
    primary->setExpanding(false);
    const QStringList nativePages{"rules","options","timer"};
    const QStringList titles{"Reply rules","Options","Manual timer"};
    for(const auto &id:bundle.replyOrder)
    {
        const int page=nativePages.indexOf(id);
        const int tab=primary->addTab(bundle.label(id,titles[page]));
        primary->setTabData(tab,page);
    }
    auto *enabled = new QCheckBox(uiText("Enable auto-replies"), &dialog);
    auto *help = new QPushButton(uiText("Help"), &dialog);
    help->setObjectName("ModeratorHelpButton");
    QObject::connect(help, &QPushButton::clicked, &dialog, [&dialog, font] { openModeratorHelp(dialog, font); });
    enabled->setChecked(s.enabled);
    header->addWidget(primary);
    header->addStretch();
    header->addWidget(help);
    auto *moderation = new QPushButton(bundle.label("moderation","Automatic moderation"), &dialog);
    moderation->setObjectName("AutoModerationButton");
    QObject::connect(moderation, &QPushButton::clicked, &dialog, [&dialog] { openAutoModeration(&dialog); });
    header->addWidget(moderation);
    header->addWidget(enabled);
    layout->addLayout(header);
    auto *pages = new QStackedWidget(&dialog);
    pages->setObjectName("ModeratorReplyPages");
    layout->addWidget(pages, 1);

    auto *rulesPage = new QWidget(pages);
    auto *ruleLayout = new QVBoxLayout(rulesPage);
    ruleLayout->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabBar(rulesPage);
    tabs->setObjectName("ModeratorChannelTabs");
    tabs->setDrawBase(false);
    tabs->setUsesScrollButtons(true);
    tabs->setExpanding(false);
    tabs->setElideMode(Qt::ElideNone);
    for (const auto &channel : allowed) tabs->addTab(channel);
    if (auto *window = dynamic_cast<Window *>(parent))
        if (auto *page = window->getNotebook().getSelectedPage())
            if (auto *split = page->getSelectedSplit())
                if (auto channel = split->getSelectedChannel())
                {
                    const auto index = allowed.indexOf(channel->getName());
                    if (index >= 0) tabs->setCurrentIndex(static_cast<int>(index));
                }
    ruleLayout->addWidget(tabs);
    auto *hint = new QLabel(uiText("No verified moderator channels are currently available."), rulesPage);
    hint->setObjectName("SecondaryText");
    hint->setWordWrap(true);
    hint->setVisible(allowed.isEmpty());
    ruleLayout->addWidget(hint);
    auto *splitter = new QSplitter(Qt::Horizontal, rulesPage);
    auto *table = new QTableWidget(0, 2, splitter);
    table->setObjectName("ModeratorRulesTable");
    table->setHorizontalHeaderLabels({uiText("Rule or group"), uiText("Status")});
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setMinimumWidth(qRound(180 * scale));

    auto *editor = new QFrame(splitter);
    editor->setObjectName("RuleEditor");
    auto *fields = new QVBoxLayout(editor);
    fields->setContentsMargins(14, 12, 14, 12);
    auto *form = new QFormLayout;
    auto *channelName = new QLabel(editor);
    auto *group = new QLineEdit(editor);
    group->setObjectName("ModeratorGroupName");
    group->setMaxLength(80);
    group->setPlaceholderText(uiText("Optional group name"));
    form->addRow(uiText("Channel"), channelName);
    form->addRow(uiText("Group"), group);
    auto *matchMode = new QComboBox(editor);
    matchMode->setObjectName("ModeratorMatchMode");
    matchMode->addItem(uiText("Full match"));
    matchMode->addItem(uiText("Contains phrase"));
    matchMode->addItem(uiText("All words"));
    form->addRow(uiText("Matching mode"), matchMode);
    fields->addLayout(form);
    auto *ruleEnabled = new QCheckBox(uiText("Enable rule"), editor);
    auto *flags = new QHBoxLayout;
    flags->addWidget(ruleEnabled); flags->addStretch();
    fields->addLayout(flags);
    const int lineHeight = QFontMetrics(font).lineSpacing();
    auto *edit = new QPlainTextEdit(editor);
    edit->setObjectName("ModeratorTriggerVariants");
    edit->setPlaceholderText(uiText("One question per line"));
    const auto updateTriggerHint = [edit, matchMode] {
        edit->setPlaceholderText(uiText(matchMode->currentIndex() == 2
            ? "All words on any one line must appear in the message, in any order."
            : "One question per line"));
    };
    QObject::connect(matchMode, &QComboBox::currentIndexChanged, &dialog, updateTriggerHint);
    edit->setTabChangesFocus(true);
    edit->setMinimumHeight(3 * lineHeight + 16);
    edit->document()->setDocumentMargin(7);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *reply = new QPlainTextEdit(editor);
    reply->setObjectName("ModeratorReplyText");
    reply->setTabChangesFocus(true);
    reply->setMinimumHeight(3 * lineHeight + 16);
    auto *textFields = new QSplitter(Qt::Vertical, editor);
    textFields->setChildrenCollapsible(false);
    textFields->setHandleWidth(7);
    const auto addTextField = [&](const QString &caption, QPlainTextEdit *input) {
        auto *section = new QWidget(textFields);
        auto *sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->addWidget(new QLabel(caption, section));
        sectionLayout->addWidget(input, 1);
        textFields->addWidget(section);
    };
    addTextField(uiText("Question variants"), edit);
    addTextField(uiText("Reply text"), reply);
    fields->addWidget(textFields, 1);
    auto *variables = new QLabel(uiText("Variables: {game}, {game_time}, {game_since}, {game_total}, {timer}"), editor);
    variables->setObjectName("SecondaryText");
    variables->setWordWrap(true);
    fields->addWidget(variables);
    splitter->addWidget(table);
    splitter->addWidget(editor);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({290, 590});
    ruleLayout->addWidget(splitter, 1);
    pages->addWidget(rulesPage);

    auto *optionsPage = new QWidget(pages);
    auto *options = new QVBoxLayout(optionsPage);
    options->setSpacing(12);
    auto *restriction = new QLabel(uiText("Auto-replies are available only on your verified moderator channels."), optionsPage);
    restriction->setObjectName("SecondaryText");
    restriction->setWordWrap(true);
    options->addWidget(restriction);
    auto *cooldownRow = new QHBoxLayout;
    auto *cooldown = new QSpinBox(optionsPage);
    cooldown->setObjectName("ModeratorReplyCooldown");
    cooldown->setRange(minimumCooldown, 300);
    cooldown->setValue(std::max(s.cooldown, minimumCooldown));
    cooldown->setSuffix(uiText(" sec"));
    if (minimumCooldown == 0)
        cooldown->setSpecialValueText(uiText("No extra pause"));
    cooldown->setKeyboardTracking(false);
    cooldownRow->addWidget(new QLabel(uiText("Pause between replies in a channel"), optionsPage));
    cooldownRow->addWidget(cooldown);
    cooldownRow->addStretch();
    options->addLayout(cooldownRow);
    auto *rolesRow = new QHBoxLayout;
    rolesRow->addWidget(new QLabel(uiText("Reply to"), optionsPage));
    QList<std::pair<moderator::SenderRole, QCheckBox *>> roleChecks;
    for (const auto &option : moderator::ROLE_OPTIONS)
    {
        auto *check = new QCheckBox(uiText(option.label), optionsPage);
        check->setObjectName(QStringLiteral("ModeratorSenderRole_") + QString::fromLatin1(option.key));
        check->setChecked(moderator::acceptsRole(s.senderRoles, option.role));
        rolesRow->addWidget(check);
        roleChecks.append({option.role, check});
    }
    rolesRow->addStretch();
    options->addLayout(rolesRow);
    auto *trackingHelp = new QLabel(uiText("Category observations are saved across restarts. If a long gap prevents confirming the elapsed time, it is shown as unknown."), optionsPage);
    trackingHelp->setObjectName("SecondaryText");
    trackingHelp->setWordWrap(true);
    options->addWidget(trackingHelp);
    options->addStretch();
    pages->addWidget(optionsPage);

    auto *timerPage = new QWidget(pages);
    auto *timerLayout = new QVBoxLayout(timerPage);
    auto *timerForm = new QFormLayout;
    auto *timerChannel = new QComboBox(timerPage);
    timerChannel->setObjectName("ModeratorTimerChannel");
    timerChannel->addItems(allowed);
    timerForm->addRow(uiText("Channel"), timerChannel);
    auto *elapsed = new QLabel(timerPage);
    elapsed->setObjectName("ModeratorTimerElapsed");
    timerForm->addRow(uiText("Elapsed time"), elapsed);
    auto *startMode = new QComboBox(timerPage);
    startMode->setObjectName("ModeratorTimerStartMode");
    startMode->addItems({uiText("Minutes ago"), uiText("Local date and time")});
    auto *minutesAgo = new QSpinBox(timerPage);
    minutesAgo->setObjectName("ModeratorTimerMinutes");
    minutesAgo->setRange(0, 525600);
    minutesAgo->setSpecialValueText(uiText("Now"));
    auto *startDate = new QDateTimeEdit(timerPage);
    startDate->setObjectName("ModeratorTimerDate");
    startDate->setDisplayFormat("dd.MM.yyyy  HH:mm:ss");
    auto *startOptions = new QHBoxLayout;
    startOptions->addWidget(startMode);
    startOptions->addWidget(minutesAgo);
    startOptions->addWidget(startDate);
    startOptions->addStretch();
    timerForm->addRow(uiText("Timer start"), startOptions);
    timerLayout->addLayout(timerForm);
    auto *timerAction = new QPushButton(timerPage);
    timerAction->setObjectName("ModeratorTimerAction");
    timerAction->setAutoDefault(false);
    timerAction->setMinimumWidth(std::max(QFontMetrics(font).horizontalAdvance(uiText("Start timer")),
        QFontMetrics(font).horizontalAdvance(uiText("Stop timer"))) + qRound(24 * scale));
    auto *timerApply = new QPushButton(uiText("Apply start time"), timerPage);
    timerApply->setObjectName("ModeratorTimerApply");
    timerApply->setAutoDefault(false);
    auto *timerButtons = new QHBoxLayout;
    timerButtons->addWidget(timerApply); timerButtons->addWidget(timerAction); timerButtons->addStretch();
    timerLayout->addLayout(timerButtons);
    auto *timerHelp = new QLabel(uiText("Use {timer} in a reply. It is sent only while this channel's timer is running."), timerPage);
    timerHelp->setObjectName("SecondaryText");
    timerHelp->setWordWrap(true);
    timerLayout->addWidget(timerHelp);
    timerLayout->addStretch();
    pages->addWidget(timerPage);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(uiText("Add rule or group"), &dialog);
    auto *remove = new QPushButton(uiText("Remove selected"), &dialog);
    auto *save = new QPushButton(uiText("Save rules"), &dialog);
    save->setObjectName("SaveRules");
    auto *close = new QPushButton(uiText("Close"), &dialog);
    buttons->addWidget(add); buttons->addWidget(remove); buttons->addStretch();
    buttons->addWidget(moderatorextension::moduleButton(&dialog));
    buttons->addWidget(save); buttons->addWidget(close);
    layout->addLayout(buttons);
    auto *status = new QLabel(&dialog);
    status->setObjectName("ModeratorReplyStatus");
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);

    auto draft = s.rules;
    QList<int> visibleRows;
    int activeRule = -1;
    bool filling = false;
    auto updateRule = [&] {
        if (filling || activeRule < 0 || activeRule >= draft.size()) return;
        auto &r = draft[activeRule];
        r.title = group->text().trimmed();
        r.triggers = edit->toPlainText().split(u'\n');
        r.reply = reply->toPlainText().trimmed().replace(u'\n', u' ');
        r.exact = matchMode->currentIndex() != 1;
        r.allWords = matchMode->currentIndex() == 2;
        r.enabled = ruleEnabled->isChecked();
        const auto row = visibleRows.indexOf(activeRule);
        if (row >= 0 && row < table->rowCount())
        {
            table->item(row, 0)->setText(r.label());
            table->item(row, 1)->setText(uiText(r.enabled ? "Enabled" : "Disabled"));
        }
    };
    auto selectRow = [&](int row) {
        updateRule();
        filling = true;
        activeRule = row >= 0 && row < visibleRows.size() ? visibleRows.at(row) : -1;
        const bool valid = activeRule >= 0;
        group->setEnabled(valid);
        edit->setEnabled(valid); reply->setEnabled(valid); matchMode->setEnabled(valid);
        ruleEnabled->setEnabled(valid); remove->setEnabled(valid);
        group->setText(valid ? draft[activeRule].title : QString{});
        edit->setPlainText(valid ? draft[activeRule].triggers.join(u'\n') : QString{});
        reply->setPlainText(valid ? draft[activeRule].reply : QString{});
        matchMode->setCurrentIndex(!valid ? 0 : draft[activeRule].allWords ? 2 : draft[activeRule].exact ? 0 : 1);
        updateTriggerHint();
        ruleEnabled->setChecked(!valid || draft[activeRule].enabled);
        filling = false;
    };
    auto loadChannel = [&](int tab) {
        updateRule(); activeRule = -1; visibleRows.clear();
        QSignalBlocker blocker(table);
        table->setRowCount(0);
        const bool valid = tab >= 0 && tab < allowed.size();
        channelName->setText(valid ? allowed[tab] : QString{});
        if (valid)
        {
            for (int i = 0; i < draft.size(); ++i)
            {
                const auto &r = draft[i];
                if (r.channel != allowed[tab]) continue;
                visibleRows.append(i);
                const int row = table->rowCount();
                table->insertRow(row);
                table->setItem(row, 0, new QTableWidgetItem(r.label()));
                table->setItem(row, 1, new QTableWidgetItem(uiText(r.enabled ? "Enabled" : "Disabled")));
            }
        }
        if (table->rowCount()) table->selectRow(0);
        selectRow(table->rowCount() ? 0 : -1);
        add->setEnabled(valid && draft.size() < MAX_LOCAL_RULES && visibleRows.size() < maximumRules);
        save->setEnabled(valid);
    };
    auto accessStillValid = [&] {
        return hasGrant() && s.account == uid && s.policy->revision == revision && channels() == allowed;
    };
    pajlada::Signals::ScopedConnection changedConnection(changed.connect([&] {
        if (!accessStillValid()) dialog.reject();
    }));
    auto saveOptions = [&](bool newEnabled, int newCooldown, unsigned mask) {
        if (!accessStillValid() || !s.rolesValid()) { dialog.reject(); return false; }
        if (!newEnabled) s.enabled = false; // stop immediately, even if writing fails
        const bool saved = s.save(newEnabled, newCooldown, s.rules, mask, s.manualTimers);
        if (saved) status->clear();
        else status->setText(uiText("Unable to save rules."));
        return saved;
    };
    QObject::connect(enabled, &QCheckBox::toggled, &dialog, [&](bool checked) {
        if (!saveOptions(checked, s.cooldown, s.senderRoles))
        {
            const QSignalBlocker blocker(enabled);
            enabled->setChecked(s.enabled);
        }
    });
    QObject::connect(cooldown, &QSpinBox::valueChanged, &dialog, [&](int value) {
        if (!saveOptions(s.enabled, value, s.senderRoles))
        {
            const QSignalBlocker blocker(cooldown);
            cooldown->setValue(std::max(s.cooldown, minimumCooldown));
        }
    });
    for (const auto &[role, check] : roleChecks)
    {
        QObject::connect(check, &QCheckBox::toggled, &dialog, [&, role, check](bool value) {
            const auto bit = static_cast<unsigned>(role);
            const auto mask = value ? s.senderRoles | bit : s.senderRoles & ~bit;
            if (!saveOptions(s.enabled, s.cooldown, mask))
            {
                const QSignalBlocker blocker(check);
                check->setChecked(moderator::acceptsRole(s.senderRoles, role));
            }
        });
    }

    QObject::connect(tabs, &QTabBar::currentChanged, &dialog, loadChannel);
    QObject::connect(table, &QTableWidget::currentCellChanged, &dialog,
                     [&](int row, int, int, int) { selectRow(row); });
    QObject::connect(group, &QLineEdit::textChanged, &dialog, updateRule);
    QObject::connect(edit, &QPlainTextEdit::textChanged, &dialog, updateRule);
    QObject::connect(reply, &QPlainTextEdit::textChanged, &dialog, updateRule);
    QObject::connect(matchMode, &QComboBox::currentIndexChanged, &dialog, updateRule);
    QObject::connect(ruleEnabled, &QCheckBox::toggled, &dialog, updateRule);
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        updateRule();
        if (!accessStillValid() || !s.rolesValid()) { dialog.reject(); return; }
        const auto tab = tabs->currentIndex();
        if (tab < 0 || tab >= allowed.size() || draft.size() >= MAX_LOCAL_RULES ||
            visibleRows.size() >= maximumRules) return;
        draft.append(Rule{allowed[tab], {}, {}, true, true});
        activeRule = -1;
        loadChannel(tab);
        table->selectRow(table->rowCount() - 1);
        selectRow(table->rowCount() - 1);
        edit->setFocus();
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (activeRule < 0) return;
        draft.removeAt(activeRule); activeRule = -1; loadChannel(tabs->currentIndex());
    });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        updateRule();
        if (!accessStillValid() || !s.rolesValid()) { dialog.reject(); return; }
        const auto selectLocation = [&](qsizetype rule, qsizetype line) {
            tabs->setCurrentIndex(static_cast<int>(allowed.indexOf(draft.at(rule).channel)));
            const auto row = visibleRows.indexOf(static_cast<int>(rule));
            if (row >= 0)
            {
                table->selectRow(static_cast<int>(row));
                selectRow(static_cast<int>(row));
            }
            replyquestions::selectLine(*edit, line);
        };
        for (qsizetype row = 0; row < draft.size(); ++row)
        {
            const auto &r = draft.at(row);
            if (!r.allWords || !allowed.contains(r.channel)) continue;
            for (qsizetype line = 0; line < r.triggers.size(); ++line)
            {
                const auto &phrase = r.triggers.at(line);
                if (!phrase.trimmed().isEmpty() && autoreplywords::tokens(phrase).isEmpty())
                {
                    selectLocation(row, line);
                    status->setText(uiText("Line %1 must contain at least one word.").arg(line + 1));
                    return;
                }
            }
        }
        const auto conflicts = replyquestions::findConflicts(draft, activeRule,
            [&](const Rule &rule) { return allowed.contains(rule.channel); },
            [](const Rule &rule, const QString &phrase) { return autoreplywords::conflictKey(phrase, rule.allWords); });
        if (conflicts.first)
        {
            const auto &location = conflicts.first->second;
            status->setText(replyquestions::describe(conflicts, draft));
            selectLocation(location.rule, location.line);
            return;
        }
        auto rules = draft;
        QHash<QString, int> counts;
        for (auto &r : rules)
        {
            if (allowed.contains(r.channel))
            {
                for (auto &trigger : r.triggers) trigger = trigger.simplified();
                r.triggers.removeAll(QString{});
            }
            if (!validRule(r) || (allowed.contains(r.channel) && ++counts[r.channel] > maximumRules))
            {
                status->setText(uiText("Check the trigger and reply before saving."));
                return;
            }
        }
        status->setText(s.save(s.enabled, cooldown->value(), rules, s.senderRoles, s.manualTimers)
            ? uiText("Auto-reply rules saved.") : uiText("Unable to save rules."));
    });

    auto refreshTimer = [&] {
        const auto name = timerChannel->currentText();
        const auto start = s.manualTimers.value(name);
        const auto seconds = std::max<qint64>(0, QDateTime::currentSecsSinceEpoch() - start);
        elapsed->setText(start ? QStringLiteral("%1:%2:%3").arg(seconds / 3600)
                         .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
                         .arg(seconds % 60, 2, 10, QLatin1Char('0')) : uiText("Timer stopped"));
        timerAction->setText(uiText(start ? "Stop timer" : "Start timer"));
        timerAction->setEnabled(!name.isEmpty());
    };
    auto loadTimer = [&] {
        const auto start = s.manualTimers.value(timerChannel->currentText());
        minutesAgo->setValue(0);
        startDate->setDateTime(start ? QDateTime::fromSecsSinceEpoch(start).toLocalTime() : QDateTime::currentDateTime());
        startMode->setCurrentIndex(start ? 1 : 0);
        minutesAgo->setVisible(!start);
        startDate->setVisible(start != 0);
        startMode->setEnabled(timerChannel->currentIndex() >= 0);
        timerApply->setVisible(start != 0);
        refreshTimer();
    };
    auto saveTimer = [&](bool stop) {
        if (!accessStillValid() || !s.rolesValid()) { dialog.reject(); return; }
        const auto name = timerChannel->currentText();
        if (!allowed.contains(name)) return;
        const auto now = QDateTime::currentSecsSinceEpoch();
        const auto start = startMode->currentIndex() == 0 ? now - qint64(minutesAgo->value()) * 60 : startDate->dateTime().toSecsSinceEpoch();
        if (!stop && (start <= 0 || start > now))
        {
            status->setText(uiText("Choose a start time in the past or present."));
            return;
        }
        if (!s.setTimer(name, stop ? 0 : start))
        {
            status->setText(uiText("Unable to save rules."));
            return;
        }
        status->clear(); loadTimer();
    };
    QObject::connect(startMode, &QComboBox::currentIndexChanged, &dialog, [=](int mode) {
        minutesAgo->setVisible(mode == 0);
        startDate->setVisible(mode == 1);
    });
    QObject::connect(timerChannel, &QComboBox::currentTextChanged, &dialog, loadTimer);
    QObject::connect(timerApply, &QPushButton::clicked, &dialog, [&] { saveTimer(false); });
    QObject::connect(timerAction, &QPushButton::clicked, &dialog, [&] {
        saveTimer(s.manualTimers.contains(timerChannel->currentText()));
    });
    QTimer timerTick;
    timerTick.setInterval(1000);
    QObject::connect(&timerTick, &QTimer::timeout, &dialog, refreshTimer);
    QObject::connect(primary, &QTabBar::currentChanged, &dialog, [&](int index) {
        const int page=primary->tabData(index).toInt();
        pages->setCurrentIndex(page);
        for (auto *action : {add, remove, save}) action->setVisible(page == 0 && !allowed.isEmpty());
        if (page == 2)
        {
            const auto name = tabs->tabText(tabs->currentIndex());
            if (timerChannel->findText(name) >= 0) timerChannel->setCurrentText(name);
            loadTimer(); timerTick.start();
        }
        else timerTick.stop();
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (allowed.isEmpty())
    {
        tabs->hide(); splitter->hide(); add->hide(); remove->hide(); save->hide();
        enabled->setEnabled(false); cooldown->setEnabled(false);
        for (const auto &[role, check] : roleChecks) check->setEnabled(false);
        timerPage->setEnabled(false);
    }
    loadChannel(tabs->currentIndex());
    loadTimer();
    Q_EMIT primary->currentChanged(primary->currentIndex());
    installMoltorinoDialogTheme(&dialog, [&dialog, table, textFields, font] {
        const auto *t = getTheme();
        dialog.setStyleSheet(QStringLiteral(R"(
QDialog, QStackedWidget, QTableWidget { background:%1; color:%2; }
QLabel, QCheckBox { color:%2; background:transparent; }
QLabel#SecondaryText, QLabel#ModeratorReplyStatus { color:%8; }
QCheckBox::indicator { width:14px; height:14px; border:1px solid %8; border-radius:3px; background:%9; }
QCheckBox::indicator:checked { image:url(:/buttons/menu-check-%10.svg); border-color:%7; }
QFrame#RuleEditor { background:%3; border:1px solid %4; border-radius:7px; }
QTableWidget { alternate-background-color:%3; border:0; }
QTableWidget::item { padding:3px 8px; border-bottom:1px solid %4; }
QTableWidget::item:selected { background:%5; color:%6; }
QHeaderView::section { background:%1; color:%8; border:0; border-bottom:1px solid %4; padding:5px 8px; }
QTabBar::tab { background:transparent; color:%2; padding:5px 12px; border-bottom:1px solid %4; }
QTabBar::tab:selected { color:%7; border-bottom:2px solid %7; }
QPushButton { background:transparent; color:%7; border:0; padding:5px 8px; }
QPushButton:hover, QTabBar::tab:hover { background:%3; color:%2; }
QPushButton:focus { text-decoration:underline; }
QPushButton#SaveRules { background:%5; color:%6; border-radius:4px; }
QPushButton#ModeratorHelpButton { background:%3; color:%7; border:1px solid %4; border-radius:5px; padding:4px 10px; }
QPushButton#ModeratorHelpButton:hover { border-color:%7; color:%2; }
QPushButton#AutoModerationButton { background:%3; color:%7; border:1px solid %4; border-radius:5px; padding:4px 10px; }
QDateTimeEdit, QSpinBox, QComboBox, QPlainTextEdit, QLineEdit { background:%9; color:%2; border:1px solid %4; border-radius:5px; padding:5px 7px; }
QDateTimeEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus, QLineEdit:focus { border-color:%7; }
QPlainTextEdit#ModeratorTriggerVariants { padding:0; }
QPlainTextEdit QScrollBar:vertical { width:12px; background:transparent; margin:3px 2px; }
QPlainTextEdit QScrollBar::handle:vertical { background:%8; border-radius:4px; min-height:22px; }
QPlainTextEdit QScrollBar::add-line:vertical, QPlainTextEdit QScrollBar::sub-line:vertical { height:0; }
QPlainTextEdit QScrollBar::add-page:vertical, QPlainTextEdit QScrollBar::sub-page:vertical { background:transparent; }
QSplitter::handle:vertical { background:%3; }
QSplitter::handle:vertical:hover { background:%4; }
)").arg(t->messages.backgrounds.regular.name(), t->messages.textColors.regular.name(),
        t->messages.backgrounds.alternate.name(), t->tabs.dividerLine.name(),
        t->tabs.selected.backgrounds.regular.name(), t->tabs.selected.text.name(),
        t->tabs.selected.line.regular.name(), t->messages.textColors.system.name(),
        t->splits.input.background.name()).arg(t->isLightTheme() ? "light" : "dark"));
        for (auto *child : dialog.findChildren<QWidget *>()) child->setFont(font);
        int minimumTextHeight = textFields->handleWidth();
        for (int i = 0; i < textFields->count(); ++i)
        {
            auto *section = textFields->widget(i);
            section->layout()->activate();
            const auto height = section->layout()->minimumSize().height();
            section->setMinimumHeight(height);
            minimumTextHeight += height;
        }
        textFields->setMinimumHeight(minimumTextHeight);
        table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 14);
    });
    automationdialog::run(dialog);
    timerTick.stop();
    for (auto *widget : dialog.findChildren<QWidget *>())
        QObject::disconnect(widget, nullptr, &dialog, nullptr);
}
}  // namespace chatterino
