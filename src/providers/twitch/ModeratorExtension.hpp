#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>

class QWidget;
class QPushButton;

namespace chatterino::moderatorextension {

struct Text {
    QString en, ru;
    QString localized() const;
};
struct Page {
    Text title, body;
};
struct Choice {
    QString id;
    Text title;
};
struct Field {
    QString id, type;
    Text title;
    QJsonValue initial;
    int minimum=0, maximum=0;
    QList<Choice> choices;
};
struct RewardAction {
    QString id;
    Text title;
    int minimumSeconds = 0, maximumSeconds = 0;
    QStringList sources = {"reward"};
    QList<Field> fields;
    QJsonObject program; // bounded declarative expression tree, not executable code
};
struct Bundle {
    int revision = 0;
    QString version = "1.0.0";
    QByteArray digest;
    QHash<QString,Text> labels;
    QList<Page> pages;
    QList<RewardAction> rewardActions;
    QStringList replyOrder = {"rules","options","timer"};
    QStringList moderationOrder = {"words","rewards","history"};
    QString label(const QString &key, const QString &fallback) const;
    std::optional<RewardAction> rewardAction(const QString &id) const;
};

QByteArray ownerKey();
std::optional<Bundle> verify(const QByteArray &envelope, const QByteArray &key);
bool validActionId(const QString &id);
enum class Operation { Skip, Timeout, Untimeout, Reply };
struct Decision {
    Operation operation=Operation::Skip;
    int seconds=0;
    QString reply;
};
// Pure, bounded evaluation. The caller still checks grants/roles/target and
// dispatches at most one allowlisted action.
std::optional<Decision> decide(const RewardAction &action,const QJsonObject &parameters,
                               const QJsonObject &event);
std::optional<QJsonObject> editParameters(QWidget *parent,const RewardAction &action,
                                          const QJsonObject &current);

// Signed, versioned package only. It never owns access grants, credentials,
// account configuration, or permission to dispatch actions.
class Store {
public:
    enum class Result { Activated, Deferred, Unchanged, Rejected };
    Store(QString cache, QByteArray key);
    const Bundle &active() const { return active_; }
    bool pending() const { return pending_.has_value(); }
    Result offer(const QByteArray &envelope);
    void beginUi() { ++uiDepth_; }
    void endUi();
private:
    QString cache_;
    QByteArray key_;
    Bundle active_;
    std::optional<Bundle> pending_;
    int uiDepth_ = 0;
};

// Created only from authorized mod/owner paths; the underlying host remains
// responsible for current Twitch identity and permission on every action.
Store &store();
const Bundle &active();
void start();
class UiLease {
public:
    UiLease();
    ~UiLease();
    UiLease(const UiLease &)=delete;
    UiLease &operator=(const UiLease &)=delete;
private:
    Store *store_;
};
QPushButton *moduleButton(QWidget *parent);
std::optional<int> rewardSeconds(const QString &id);
bool claimReplyEvent(const QString &room,const QString &messageId);

} // namespace chatterino::moderatorextension
