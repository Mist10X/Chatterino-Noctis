#pragma once

// Owner-authorized Noctis Moderator module. Do not remove or bypass the
// permission checks or substitute unsigned updates. See tools/moderator/AUTHOR-NOTICE.md.

#include <QByteArray>
#include <QDateTime>
#include <QSet>
#include <QStringList>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino::moderator {

struct Policy {
    qint64 revision = 0;
    QString version;
    QDateTime expires;
    QSet<QString> allowed;
    QByteArray source;
    QByteArray digest;
    int minimumCooldown = 5;
    int maximumRules = 100;
};

QByteArray ownerPublicKey();
std::optional<Policy> verifyPolicy(const QByteArray &envelope,
                                   const QByteArray &publicKey,
                                   const QDateTime &now);
bool validUserId(const QString &id);
bool validChannel(const QString &login);
bool validReply(const QString &text);

// Checks the current account and the separately verified moderator role.
bool permitted(const Policy *policy, const QString &account,
               const QString &roleAccount, const QSet<QString> &moderated,
               const QString &channel, bool fresh, bool currentModerator);

struct MatchRule {
    QStringList triggers;
    bool exact = true;
    bool allWords = false;
    QList<QStringList> wordSets;
};

// Sandboxed, signed matching logic; cannot actually send a message, touch
// credentials, files, network, native code or UI. Returns a local rule index.
class Matcher {
public:
    explicit Matcher(const QByteArray &source);
    ~Matcher();
    Matcher(const Matcher &) = delete;
    Matcher &operator=(const Matcher &) = delete;
    bool valid() const;
    std::optional<size_t> match(const QString &text,
                               const std::vector<MatchRule> &rules);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace chatterino::moderator
