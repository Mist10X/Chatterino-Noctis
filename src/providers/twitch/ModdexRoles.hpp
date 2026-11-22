#pragma once

#include <QDateTime>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

class QObject;

namespace chatterino::moddex {

enum class RoleKind { Moderator, Vip };
enum class ResultStatus { Ok, Unavailable, Error };

struct Role {
    QString channelId;
    QString login;
    QString displayName;
    QUrl avatar;
    QDateTime grantedAt;
    QDateTime endedAt;
};

struct RolePage {
    std::vector<Role> items;
    std::optional<qint64> total;
    QString cursor;
    bool hasMore = false;
};

struct CurrentRoles {
    RolePage moderators;
    RolePage vips;
};

/// Public, indexed Twitch roles. All calls and callbacks are on the GUI thread.
/// Results are delivered asynchronously and suppressed when lifetime is destroyed.
class RoleProvider final
{
public:
    explicit RoleProvider(
        QUrl baseUrl = QUrl(QStringLiteral("https://api.moddex.tv")));

    /// Both current-role first pages in one request.
    void requestCurrent(const QString &uid, QObject *lifetime,
                        std::function<void(ResultStatus, CurrentRoles)> done);

    /// Subsequent active pages or any page of ended roles.
    void requestPage(const QString &uid, RoleKind kind, bool past,
                     const QString &cursor, QObject *lifetime,
                     std::function<void(ResultStatus, RolePage)> done);

private:
    QUrl baseUrl_;
};

}  // namespace chatterino::moddex
