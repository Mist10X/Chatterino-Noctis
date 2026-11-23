// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/ImagePreview.hpp"

#include "Application.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/LinkResolver.hpp"
#include "messages/Image.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QCoreApplication>
#include <QCache>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace chatterino::imagepreview {

namespace {

// Only locally prepared upload thumbnails need an extra cache. The resolver
// already caches remote metadata; each message retains its own result.
QCache<QString, ImagePtr> &uploads()
{
    // Units of one MiB, with a minimum cost of one per thumbnail.
    static QCache<QString, ImagePtr> value(32);
    return value;
}

QSet<QString> &deleted()
{
    static QSet<QString> value;
    return value;
}

void scheduleRelayout()
{
    // Several pictures arrive together: one relayout of the chats.
    static bool queued = false;
    if (queued || tryGetApp() == nullptr)
    {
        return;
    }
    queued = true;
    QTimer::singleShot(150, QCoreApplication::instance(), [] {
        queued = false;
        if (auto *app = tryGetApp(); app && app->getWindows())
        {
            app->getWindows()->forceLayoutChannelViews();
        }
    });
}

QUrl parse(const QString &url)
{
    return QUrl(url.contains(QStringLiteral("://"))
                    ? url
                    : QStringLiteral("https://") + url);
}

QString bareHost(const QUrl &url)
{
    auto host = url.host().toLower();
    if (host.startsWith(QStringLiteral("www.")))
    {
        host = host.mid(4);
    }
    return host;
}

bool isPublicHost(const QString &host)
{
    if (host.isEmpty() || !host.contains('.') ||
        host.endsWith(QStringLiteral(".local")) ||
        host.endsWith(QStringLiteral(".localhost")) ||
        host.endsWith(QStringLiteral(".lan")) ||
        host.endsWith(QStringLiteral(".internal")))
    {
        return false;
    }
    QHostAddress address;
    return !address.setAddress(host) ||
           !(address.isLoopback() || address.isPrivateUse() ||
             address.isLinkLocal() || address.isSiteLocal() ||
             address.isUniqueLocalUnicast());
}

}  // namespace

QStringList sites(const QString &setting)
{
    static const QRegularExpression separators(QStringLiteral("[\\s,;]+"));
    QStringList result;
    for (auto site : setting.split(separators, Qt::SkipEmptyParts))
    {
        // "https://www.kappa.lol/" -> "kappa.lol"
        site = bareHost(parse(site.trimmed()));
        if (!site.isEmpty() && !result.contains(site))
        {
            result.append(site);
        }
    }
    return result;
}

bool isImageLink(const QString &url, const QStringList &sites)
{
    const auto parsed = parse(url);
    const auto scheme = parsed.scheme().toLower();
    if (!parsed.isValid() ||
        (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")) ||
        !parsed.userInfo().isEmpty())
    {
        return false;
    }
    const auto host = bareHost(parsed);
    if (!isPublicHost(host))
    {
        return false;
    }

    static const QRegularExpression imageFile(
        QStringLiteral("\\.(png|jpe?g|jfif|gif|webp|avif|bmp)$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto path = parsed.path();
    if (imageFile.match(path).hasMatch())
    {
        return true;
    }

    const auto parts = path.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
    {
        return false;  // the site's main page
    }
    for (const auto &site : sites)
    {
        if (host == site || host.endsWith(u'.' + site))
        {
            // A post: kappa.lol/vCECFg, eblo.id/hH0WwpT, kappalol.fun/Y8zpTA
            // (or an album there, kappalol.fun/group/<id>). The sites' own
            // pages (eblo.id/videos, kappa.lol/uploaders) only have a logo.
            static const QRegularExpression postId(
                QStringLiteral("^[A-Za-z0-9_-]{4,64}$"));
            static const QSet<QString> sitePages{
                "delete", "uploaders", "upload", "videos", "board", "about",
                "profile", "group", "text", "login", "register", "settings",
                "terms", "privacy", "albums", "images", "contact", "help",
                "api", "docs", "account", "dashboard", "random", "rules",
                "stats", "recent",
            };
            if (parts.size() == 1)
            {
                return postId.match(parts.front()).hasMatch() &&
                       !sitePages.contains(parts.front().toLower());
            }
            return parts.size() == 2 &&
                   (parts.front() == QStringLiteral("group") ||
                    (host == QStringLiteral("olrite.lol") && parts.front() == QStringLiteral("i"))) &&
                   postId.match(parts.back()).hasMatch();
        }
    }
    return false;
}

bool isImageLink(const QString &url)
{
    auto hosts = sites(getSettings()->imagePreviewSites);
    if (getSettings()->imageUploaderKappaFallback && !hosts.contains("olrite.lol"))
    {
        hosts.append("olrite.lol");
    }
    return uploads().contains(key(url)) ||
           isImageLink(url, hosts);
}

QString key(const QString &url)
{
    const auto parsed = parse(url);
    auto path = parsed.path(QUrl::FullyEncoded);
    while (path.endsWith('/'))
    {
        path.chop(1);
    }
    auto result = bareHost(parsed);
    const int port = parsed.port();
    if (port != -1 && port != (parsed.scheme() == QStringLiteral("http") ? 80 : 443))
    {
        result += u':' + QString::number(port);
    }
    result += path;
    if (parsed.hasQuery())
    {
        result += u'?' + parsed.query(QUrl::FullyEncoded);
    }
    return result;
}

ImagePtr cachedUpload(const QString &url)
{
    const auto *uploaded = uploads().object(key(url));
    return uploaded ? *uploaded : nullptr;
}

ImagePtr getOrRequest(LinkInfo &info)
{
    const auto k = key(info.originalUrl());
    if (deleted().contains(k))
    {
        return nullptr;
    }
    if (auto *uploaded = uploads().object(k))
    {
        return *uploaded;
    }
    if (info.canResolve() && tryGetApp())
    {
        LinkResolver::resolveThumbnail(&info);
    }
    auto thumbnail = info.thumbnail();
    return thumbnail && thumbnail->url().string.startsWith(
                            QStringLiteral("https://"), Qt::CaseInsensitive)
               ? thumbnail
               : nullptr;
}

void forget(const QString &url)
{
    const auto k = key(url);
    deleted().insert(k);
    uploads().remove(k);
    scheduleRelayout();
}

bool wasDeleted(const QString &url)
{
    return !deleted().isEmpty() && deleted().contains(key(url));
}

void rememberUpload(const QString &url, ImagePtr thumbnail)
{
    const auto k = key(url);
    if (!thumbnail)
    {
        return;
    }
    deleted().remove(k);  // A host may reuse a URL for a new successful upload.
    const auto bytes = qint64(thumbnail->width()) * thumbnail->height() * 4;
    constexpr qint64 mib = 1024 * 1024;
    const auto cost = std::max<qint64>(1, (bytes + mib - 1) / mib);
    uploads().insert(k, new ImagePtr(std::move(thumbnail)), cost);
    scheduleRelayout();
}

}  // namespace chatterino::imagepreview
