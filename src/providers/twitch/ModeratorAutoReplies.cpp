#include "providers/twitch/ModeratorAutoReplies.hpp"
#include "providers/twitch/ModeratorModule.hpp"

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
           }) && moderator::validReply(r.reply);
}

QStringList parseTriggers(const QString &text)
{
    QStringList triggers;
    QSet<QString> seen;
    for (const auto &line : text.split(u'\n'))
    {
        const auto trimmed = line.simplified();
        const auto key = trimmed.toCaseFolded();
        if (!key.isEmpty() && !seen.contains(key))
        {
            seen.insert(key);
            triggers.append(trimmed);
        }
    }
    return triggers;
}

class RuleDialog final : public QDialog {
public:
    explicit RuleDialog(QWidget *parent) : QDialog(parent) {}
};
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
    void loadRules()
    {
        rules.clear();
        enabled = false;
        cooldown = 10;
        QFile file(fileName());
        if (fileName().isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            return;
        const auto data = QJsonDocument::fromJson(file.readAll()).object();
        enabled = data.value("enabled").toBool(false);
        cooldown = std::clamp(data.value("cooldown").toInt(10), 3, 300);
        for (const auto &entry : data.value("rules").toArray())
        {
            const auto object = entry.toObject();
            Rule r{object.value("channel").toString(), {}, object.value("reply").toString(),
                   object.value("exact").toBool(true), object.value("enabled").toBool(true)};
            for (const auto &value : object.value("triggers").toArray())
                r.triggers.append(value.toString());
            if (validRule(r) && rules.size() < MAX_LOCAL_RULES)
                rules.append(std::move(r));
        }
    }
    bool save(bool newEnabled, int newCooldown, const QList<Rule> &newRules)
    {
        if (!granted() || !rolesValid() || newRules.size() > MAX_LOCAL_RULES)
            return false;
        QJsonArray array;
        for (const auto &r : newRules)
        {
            if (!validRule(r))
                return false;
            QJsonArray triggers;
            for (const auto &trigger : r.triggers)
                triggers.append(trigger);
            array.append(QJsonObject{{"channel", r.channel}, {"triggers", triggers},
                                     {"reply", r.reply}, {"exact", r.exact}, {"enabled", r.enabled}});
        }
        QSaveFile file(fileName());
        const auto raw = QJsonDocument(QJsonObject{{"enabled", newEnabled},
            {"cooldown", newCooldown}, {"rules", array}}).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(raw) != raw.size() || !file.commit())
            return false;
        rules = newRules;
        enabled = newEnabled;
        cooldown = newCooldown;
        return true;
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
                const auto user = getApp()->getAccounts()->twitch.getCurrent();
                QSet<QString> confirmed;
                for (const auto &name : result)
                {
                    const auto login = name.toLower();
                    if (moderator::validChannel(login) &&
                        login != user->getUserName().toLower())
                        confirmed.insert(login);
                }
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
    const QString &roleAccount, const QStringList &roles, const QString &ruleChannel)
{
    if (!getApp()->isTest())
        return;
    auto &s = *impl_;
    s.account = currentId();
    s.policy = policy;
    s.matcher = std::make_unique<moderator::Matcher>(policy.source);
    s.roleAccount = roleAccount;
    s.roles = QSet<QString>(roles.begin(), roles.end());
    s.policyFresh.start();
    s.roleFresh.start();
    s.enabled = true;
    s.cooldown = 5;
    s.rules = {{ruleChannel, {"fixture question"}, "fixture answer", true, true}};
    changed.invoke();
}
#endif
bool ModeratorAutoReplies::hasGrant() const { return impl_->granted(); }
QStringList ModeratorAutoReplies::channels() const
{
    if (!impl_->rolesValid())
        return {};
    auto list = impl_->roles.values();
    list.sort(Qt::CaseInsensitive);
    return list;
}

void ModeratorAutoReplies::maybeReply(TwitchChannel &channel, Communi::TagsRef tags,
                                       const QString &text)
{
    auto &s = *impl_;
    if (!s.enabled || !s.rolesValid() || channel.isAnonymous() || channel.isBroadcaster() ||
        !moderator::permitted(s.policy ? &*s.policy : nullptr, currentId(), s.roleAccount,
            s.roles, channel.getName(), s.rolesValid(), channel.isMod()))
        return;
    const auto id = tags.getOrEmpty("id");
    const auto sender = tags.getOrEmpty("user-id");
    if (id.isEmpty() || id.size() > 128 || sender.isEmpty() || sender == s.account ||
        s.seenEvents.contains(id) || tags.getOrEmpty("mod") == "1")
        return;
    const auto roomId = tags.getOrEmpty("room-id");
    const auto sourceRoom = tags.getOrEmpty("source-room-id");
    if ((!sourceRoom.isEmpty() && (roomId.isEmpty() || sourceRoom != roomId)) ||
        tags.getOrEmpty("msg-id") == "sharedchatnotice")
        return;
    const auto badges = tags.getOrEmpty("badges");
    for (const auto &badge : badges.split(u','))
        if (badge.startsWith("broadcaster/") || badge.startsWith("moderator/") ||
            badge.startsWith("lead_moderator/") || badge.startsWith("vip/") || badge.startsWith("staff/"))
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
        inputs.push_back({rule.triggers, rule.exact});
        candidates.append(&rule);
    }
    const auto matched = s.matcher->match(text, inputs);
    if (!matched)
        return;
    const auto reply = candidates.at(*matched)->reply.trimmed();
    if (!moderator::validReply(reply) || !s.rolesValid() || !channel.isMod())
        return;
    last.start(); global.start();
    channel.sendReply(reply, id);
}

void ModeratorAutoReplies::open(QWidget *parent)
{
    if (!hasGrant()) return;
    auto &s = *impl_;
    const auto uid = s.account;
    const auto allowed = channels();
    const auto revision = s.policy->revision;
    RuleDialog dialog(parent);
    dialog.setWindowTitle(uiText("Auto-replies"));
    dialog.resize(760, 560);
    const auto scale = std::clamp(getSettings()->getClampedUiScale(), 1.F, 1.4F);
    auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, scale);
    font.setWeight(std::max(font.weight(), QFont::Medium));
    dialog.setFont(font);
    auto *layout = new QVBoxLayout(&dialog);
    auto *top = new QHBoxLayout;
    auto *enabled = new QCheckBox(uiText("Enable auto-replies"), &dialog);
    enabled->setChecked(s.enabled);
    auto *cooldown = new QSpinBox(&dialog);
    cooldown->setRange(s.policy->minimumCooldown, 300);
    cooldown->setValue(std::max(s.cooldown, s.policy->minimumCooldown));
    cooldown->setSuffix(uiText(" s"));
    top->addWidget(enabled); top->addStretch();
    top->addWidget(new QLabel(uiText("Cooldown between replies"), &dialog));
    top->addWidget(cooldown);
    layout->addLayout(top);
    auto *hint = new QLabel(uiText("Auto-replies are available only on your verified moderator channels."), &dialog);
    hint->setObjectName("SecondaryText"); hint->setWordWrap(true); layout->addWidget(hint);
    auto *tabs = new QTabBar(&dialog);
    tabs->setUsesScrollButtons(true); tabs->setExpanding(false);
    for (const auto &channel : allowed) tabs->addTab(channel);
    if (auto *window = dynamic_cast<Window *>(parent))
        if (auto *page = window->getNotebook().getSelectedPage())
            if (auto *split = page->getSelectedSplit())
                if (auto channel = split->getSelectedChannel())
                {
                    const auto index = allowed.indexOf(channel->getName());
                    if (index >= 0) tabs->setCurrentIndex(static_cast<int>(index));
                }
    layout->addWidget(tabs);
    auto *table = new QTableWidget(0, 4, &dialog);
    table->setHorizontalHeaderLabels({uiText("Trigger variants"), uiText("Reply text"),
                                      uiText("Exact match"), uiText("Enabled")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 12);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table, 2);
    auto *edit = new QPlainTextEdit(&dialog);
    edit->setPlaceholderText(uiText("One trigger per line"));
    edit->setMinimumHeight(qRound(82 * scale));
    auto *reply = new QPlainTextEdit(&dialog);
    reply->setPlaceholderText(uiText("Reply text"));
    reply->setMinimumHeight(qRound(82 * scale));
    auto *flags = new QHBoxLayout;
    auto *exact = new QCheckBox(uiText("Exact match"), &dialog);
    auto *ruleEnabled = new QCheckBox(uiText("Enabled"), &dialog);
    flags->addWidget(exact); flags->addWidget(ruleEnabled); flags->addStretch();
    layout->addWidget(edit, 1); layout->addWidget(reply, 1); layout->addLayout(flags);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(uiText("Add rule"), &dialog);
    auto *remove = new QPushButton(uiText("Remove selected"), &dialog);
    auto *save = new QPushButton(uiText("Save rules"), &dialog);
    auto *close = new QPushButton(uiText("Close"), &dialog);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    auto *version = new QLabel(uiText("Module version: %1").arg(s.policy->version), &dialog);
    version->setObjectName("SecondaryText");
    buttons->addWidget(add); buttons->addWidget(remove); buttons->addStretch();
    buttons->addWidget(save); buttons->addWidget(close); layout->addLayout(buttons);
    auto *footer = new QHBoxLayout;
    footer->addWidget(status, 1); footer->addWidget(version);
    layout->addLayout(footer);
    auto draft = s.rules;
    QList<int> visibleRows;
    int activeRule = -1;
    bool filling = false;
    auto updateRule = [&] {
        if (filling || activeRule < 0 || activeRule >= draft.size()) return;
        auto &r = draft[activeRule];
        r.triggers = parseTriggers(edit->toPlainText());
        r.reply = reply->toPlainText().trimmed();
        r.exact = exact->isChecked(); r.enabled = ruleEnabled->isChecked();
        const auto row = visibleRows.indexOf(activeRule);
        if (row >= 0)
        {
            table->item(row, 0)->setText(r.triggers.join(" · "));
            table->item(row, 1)->setText(r.reply);
            table->item(row, 2)->setText(r.exact ? uiText("Yes") : uiText("No"));
            table->item(row, 3)->setText(r.enabled ? uiText("Yes") : uiText("No"));
        }
    };
    auto selectRow = [&](int row) {
        updateRule();
        filling = true;
        activeRule = row >= 0 && row < visibleRows.size() ? visibleRows.at(row) : -1;
        const bool valid = activeRule >= 0;
        edit->setEnabled(valid); reply->setEnabled(valid); exact->setEnabled(valid);
        ruleEnabled->setEnabled(valid); remove->setEnabled(valid);
        edit->setPlainText(valid ? draft[activeRule].triggers.join(u'\n') : QString{});
        reply->setPlainText(valid ? draft[activeRule].reply : QString{});
        exact->setChecked(!valid || draft[activeRule].exact);
        ruleEnabled->setChecked(!valid || draft[activeRule].enabled);
        filling = false;
    };
    auto loadChannel = [&](int tab) {
        updateRule(); activeRule = -1; visibleRows.clear();
        QSignalBlocker blocker(table);
        table->setRowCount(0);
        if (tab >= 0 && tab < allowed.size())
        {
            for (int i = 0; i < draft.size(); ++i)
            {
                const auto &r = draft[i];
                if (r.channel != allowed[tab]) continue;
                visibleRows.append(i);
                const int row = table->rowCount(); table->insertRow(row);
                table->setItem(row, 0, new QTableWidgetItem(r.triggers.join(" · ")));
                table->setItem(row, 1, new QTableWidgetItem(r.reply));
                table->setItem(row, 2, new QTableWidgetItem(r.exact ? uiText("Yes") : uiText("No")));
                table->setItem(row, 3, new QTableWidgetItem(r.enabled ? uiText("Yes") : uiText("No")));
            }
        }
        if (table->rowCount()) table->selectRow(0);
        selectRow(table->rowCount() ? 0 : -1);
    };
    QObject::connect(tabs, &QTabBar::currentChanged, &dialog, loadChannel);
    QObject::connect(table, &QTableWidget::currentCellChanged, &dialog,
                     [&](int row, int, int, int) { selectRow(row); });
    QObject::connect(edit, &QPlainTextEdit::textChanged, &dialog, updateRule);
    QObject::connect(reply, &QPlainTextEdit::textChanged, &dialog, updateRule);
    QObject::connect(exact, &QCheckBox::toggled, &dialog, updateRule);
    QObject::connect(ruleEnabled, &QCheckBox::toggled, &dialog, updateRule);
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        updateRule();
        if (!hasGrant() || !s.rolesValid()) { dialog.reject(); return; }
        const auto tab = tabs->currentIndex();
        if (tab < 0 || tab >= allowed.size() || draft.size() >= MAX_LOCAL_RULES) return;
        if (visibleRows.size() >= s.policy->maximumRules) return;
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
    auto accessStillValid = [&] {
        return hasGrant() && s.account == uid && s.policy->revision == revision &&
               channels() == allowed;
    };
    pajlada::Signals::ScopedConnection changedConnection(changed.connect([&] {
        if (!accessStillValid()) dialog.reject();
    }));
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        updateRule();
        if (!accessStillValid() || !s.rolesValid()) { dialog.reject(); return; }
        QHash<QString, int> counts;
        for (const auto &r : draft)
        {
            if (!validRule(r) || (allowed.contains(r.channel) && ++counts[r.channel] > s.policy->maximumRules))
            {
                status->setText(uiText("Check the trigger and reply before saving."));
                return;
            }
        }
        if (s.save(enabled->isChecked(), cooldown->value(), draft))
            status->setText(uiText("Auto-reply rules saved."));
        else
            status->setText(uiText("Unable to save rules."));
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(enabled, &QCheckBox::toggled, &dialog, [&](bool value) {
        if (!value && accessStillValid() && s.enabled)
        {
            s.enabled = false; // disable immediately, even with an unsaved editor draft
            s.save(false, s.cooldown, s.rules);
        }
    });
    if (allowed.isEmpty())
    {
        hint->setText(uiText("No verified moderator channels are currently available."));
        tabs->hide(); table->hide(); edit->hide(); reply->hide(); exact->hide(); ruleEnabled->hide();
        add->hide(); remove->hide(); save->hide(); enabled->setEnabled(false); cooldown->setEnabled(false);
    }
    loadChannel(tabs->currentIndex());
    installMoltorinoDialogTheme(&dialog);
    for (auto *child : dialog.findChildren<QWidget *>())
        child->setFont(font);
    dialog.setStyleSheet(dialog.styleSheet() + QStringLiteral(R"(
        QTabBar::tab { padding: 7px 12px; margin: 2px; border-radius: 6px; background: palette(base); }
        QTabBar::tab:selected { background: palette(highlight); color: palette(highlighted-text); }
        QPlainTextEdit, QTableWidget { border: 1px solid palette(mid); border-radius: 7px; background: palette(base); color: palette(text); padding: 5px; }
        QPushButton { padding: 7px 12px; border-radius: 6px; border: 1px solid palette(mid); background: palette(button); }
    )"));
    dialog.exec();
}
}  // namespace chatterino
