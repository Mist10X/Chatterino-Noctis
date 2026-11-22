#include "providers/twitch/ModdexRoles.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <QCache>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <utility>

namespace chatterino::moddex {
namespace {

constexpr int REQUEST_TIMEOUT_MS = 8000;
constexpr int CACHE_TTL_MS = 60000;
constexpr int PAGE_LIMIT = 25;
constexpr qsizetype MAX_BYTES = 2 * 1024 * 1024;
constexpr qsizetype MAX_CURSOR_LENGTH = 8192;

bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,20}$"));
    return pattern.match(id).hasMatch();
}

bool validLogin(const QString &login)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9_]{1,25}$"));
    return pattern.match(login).hasMatch();
}

QDateTime parseDate(const QJsonValue &value)
{
    const auto timestamp = value.toString();
    auto date = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!date.isValid())
    {
        date = QDateTime::fromString(timestamp, Qt::ISODate);
    }
    return date.toUTC();
}

std::optional<Role> parseRole(const QJsonValue &value, bool past)
{
    if (!value.isObject())
    {
        return std::nullopt;
    }
    const auto row = value.toObject();
    Role role;
    role.channelId = row.value("id").toString();
    role.login = row.value("login").toString();
    if (!validId(role.channelId) || !validLogin(role.login))
    {
        return std::nullopt;
    }
    role.displayName = row.value("name").toString();
    if (role.displayName.isEmpty())
    {
        role.displayName = role.login;
    }

    const QUrl avatar(row.value("avatar").toString());
    if (avatar.isValid() && avatar.scheme() == QStringLiteral("https") &&
        avatar.host() == QStringLiteral("static-cdn.jtvnw.net") &&
        avatar.userInfo().isEmpty())
    {
        role.avatar = avatar;
    }
    role.grantedAt = parseDate(row.value("grantedAt"));
    if (!role.grantedAt.isValid())
    {
        role.grantedAt = parseDate(row.value("granted"));
    }
    // Preserve the distinction from an active role even if an old date is missing.
    if (past)
    {
        role.endedAt = parseDate(row.value("endedAt"));
    }
    return role;
}

std::optional<RolePage> parsePage(const QJsonValue &value, bool past,
                                  const QString &previousCursor = {})
{
    const auto page = value.toObject();
    if (!page.value("items").isArray() || !page.value("hasMore").isBool())
    {
        return std::nullopt;
    }
    const auto items = page.value("items").toArray();
    if (items.size() > PAGE_LIMIT)
    {
        return std::nullopt;
    }
    RolePage result;
    result.hasMore = page.value("hasMore").toBool();
    if (result.hasMore)
    {
        result.cursor = page.value("cursor").toString();
        if (result.cursor.isEmpty() || result.cursor == previousCursor ||
            result.cursor.size() > MAX_CURSOR_LENGTH)
        {
            return std::nullopt;
        }
    }
    const auto total = page.value("total");
    if (total.isDouble())
    {
        const auto count = total.toInteger(-1);
        if (count < 0)
        {
            return std::nullopt;
        }
        result.total = count;
    }
    else if (!total.isNull() && !total.isUndefined())
    {
        return std::nullopt;
    }
    result.items.reserve(items.size());
    for (const auto &value : items)
    {
        auto item = parseRole(value, past);
        if (!item)
        {
            return std::nullopt;
        }
        result.items.push_back(std::move(*item));
    }
    return result;
}

std::optional<CurrentRoles> parseCurrent(const QJsonObject &json,
                                         const QString &uid)
{
    // A response associated with a different account must not reach this card.
    if (json.value("id").toString() != uid)
    {
        return std::nullopt;
    }
    const auto roleLists = json.value("roleLists").toObject();
    auto moderators = parsePage(roleLists.value("mods"), false);
    auto vips = parsePage(roleLists.value("vips"), false);
    if (!moderators || !vips)
    {
        return std::nullopt;
    }
    return CurrentRoles{std::move(*moderators), std::move(*vips)};
}

QUrl makeUrl(const QUrl &base, const QString &uid, const QString &suffix,
             const QString &query)
{
    QUrl url(base);
    url.setPath(QStringLiteral("/v1/users/by-id/") + uid + suffix);
    url.setQuery(query, QUrl::StrictMode);
    url.setFragment({});
    return url;
}

template <typename Result>
struct CacheEntry {
    Result data;
    QElapsedTimer fetchedAt;
};

template <typename Result>
QCache<QUrl, CacheEntry<Result>> &cache()
{
    // Keep only displayed roles, not the extra profile details in the API response.
    static QCache<QUrl, CacheEntry<Result>> entries(128);
    return entries;
}

template <typename Result>
void deliverLater(QObject *lifetime,
                  std::function<void(ResultStatus, Result)> done,
                  ResultStatus status, Result result = {})
{
    QTimer::singleShot(
        0, lifetime,
        [done = std::move(done), status, result = std::move(result)]() mutable {
            done(status, std::move(result));
        });
}

template <typename Result, typename Parser>
void request(const QUrl &url, QObject *lifetime, Parser parse,
             std::function<void(ResultStatus, Result)> done)
{
    if (auto *entry = cache<Result>().object(url))
    {
        if (!entry->fetchedAt.hasExpired(CACHE_TTL_MS))
        {
            deliverLater(lifetime, std::move(done), ResultStatus::Ok,
                         entry->data);
            return;
        }
        cache<Result>().remove(url);
    }

    const QPointer<QObject> guard(lifetime);
    NetworkRequest(url)
        .caller(lifetime)
        .timeout(REQUEST_TIMEOUT_MS)
        .followRedirects(false)
        .header("Accept", "application/json")
        .onSuccess([guard, url, parse = std::move(parse),
                    done](const NetworkResult &response) mutable {
            if (!guard)
            {
                return;
            }
            if (response.status() == 404)
            {
                deliverLater(guard.data(), std::move(done),
                             ResultStatus::Unavailable, Result{});
                return;
            }
            if (response.status() != 200 ||
                response.getData().size() > MAX_BYTES)
            {
                deliverLater(guard.data(), std::move(done), ResultStatus::Error,
                             Result{});
                return;
            }
            auto data = parse(response.parseJson());
            if (!data)
            {
                deliverLater(guard.data(), std::move(done), ResultStatus::Error,
                             Result{});
                return;
            }
            auto *entry = new CacheEntry<Result>{*data, {}};
            entry->fetchedAt.start();
            cache<Result>().insert(url, entry);
            deliverLater(guard.data(), std::move(done), ResultStatus::Ok,
                         std::move(*data));
        })
        .onError([guard, done](const NetworkResult &response) mutable {
            if (guard)
            {
                const auto status = response.status() == 404
                                        ? ResultStatus::Unavailable
                                        : ResultStatus::Error;
                deliverLater(guard.data(), std::move(done), status, Result{});
            }
        })
        .execute();
}

}  // namespace

RoleProvider::RoleProvider(QUrl baseUrl)
    : baseUrl_(std::move(baseUrl))
{
}

void RoleProvider::requestCurrent(
    const QString &uid, QObject *lifetime,
    std::function<void(ResultStatus, CurrentRoles)> done)
{
    if (!lifetime)
    {
        return;
    }
    if (!validId(uid))
    {
        deliverLater(lifetime, std::move(done), ResultStatus::Error,
                     CurrentRoles{});
        return;
    }
    request(
        makeUrl(this->baseUrl_, uid, {}, QStringLiteral("include=roles")),
        lifetime,
        [uid](const QJsonObject &json) {
            return parseCurrent(json, uid);
        },
        std::move(done));
}

void RoleProvider::requestPage(const QString &uid, RoleKind kind, bool past,
                               const QString &cursor, QObject *lifetime,
                               std::function<void(ResultStatus, RolePage)> done)
{
    if (!lifetime)
    {
        return;
    }
    if (!validId(uid) || cursor.size() > MAX_CURSOR_LENGTH)
    {
        deliverLater(lifetime, std::move(done), ResultStatus::Error,
                     RolePage{});
        return;
    }
    auto suffix = kind == RoleKind::Moderator ? QStringLiteral("/mods")
                                              : QStringLiteral("/vips");
    if (past)
    {
        suffix += QStringLiteral("/past");
    }
    auto query = QStringLiteral("limit=%1").arg(PAGE_LIMIT);
    if (!cursor.isEmpty())
    {
        query += QStringLiteral("&cursor=") +
                 QString::fromLatin1(QUrl::toPercentEncoding(cursor));
    }
    request(
        makeUrl(this->baseUrl_, uid, suffix, query), lifetime,
        [past, cursor](const QJsonObject &json) {
            return parsePage(json, past, cursor);
        },
        std::move(done));
}

}  // namespace chatterino::moddex
