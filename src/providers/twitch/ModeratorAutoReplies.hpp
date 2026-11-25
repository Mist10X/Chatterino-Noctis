#pragma once

#include <IrcTagsRef>
#include <QObject>
#include <QString>
#include <QStringList>
#include <pajlada/signals/signal.hpp>
#include <memory>

class QWidget;
namespace chatterino {
class TwitchChannel;
namespace moderator { struct Policy; }

// The public host is separate from the owner's unrestricted Personal tool.
class ModeratorAutoReplies final : public QObject {
public:
    static ModeratorAutoReplies &instance();
    static ModeratorAutoReplies *existing();
    ~ModeratorAutoReplies() override;
    bool hasGrant() const;
    QStringList channels() const;
    void open(QWidget *parent);
    void maybeReply(TwitchChannel &channel, Communi::TagsRef tags,
                    const QString &text);
    pajlada::Signals::NoArgSignal changed;
private:
#ifdef CHATTERINO_WITH_TESTS
    friend class ModeratorAutoRepliesTestAccess;
    // Only callable by the isolated test fixture, never by production UI,
    // settings, networking, or a command-line argument.
    void configureFixture(const moderator::Policy &policy, const QString &roleAccount,
                          const QStringList &roles, const QString &ruleChannel);
#endif
    explicit ModeratorAutoReplies(QObject *parent);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
