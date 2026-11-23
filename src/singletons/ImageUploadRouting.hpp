// SPDX-License-Identifier: MIT
#pragma once

#include "common/network/NetworkResult.hpp"

#include <QUrl>
#include <QRegularExpression>
#include <algorithm>
#include <utility>

namespace chatterino::imageuploader::detail {

inline QString builtInImageLink(QString text, const QString &host)
{
    text = text.trimmed();
    if (text.size() > 4096 || text.contains(u'\\') ||
        std::any_of(text.begin(), text.end(), [](QChar c) {
            return c.isSpace() || !c.isPrint();
        }))
        return {};
    if (text.startsWith("//"))
        text.prepend("https:");
    else if (text.startsWith(host + "/", Qt::CaseInsensitive))
        text.prepend("https://");
    QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") ||
        url.authority().compare(host, Qt::CaseInsensitive) != 0 ||
        url.path().size() < 2)
        return {};
    url.setScheme("https");
    return url.toString(QUrl::FullyEncoded);
}

inline QString olriteLink(QString text)
{
    text = text.trimmed();
    if (text.startsWith('/') && !text.startsWith("//"))
        text.prepend("https://olrite.lol");
    return builtInImageLink(text, QStringLiteral("olrite.lol"));
}

inline QString olriteDeletionToken(const QUrl &url)
{
    if (url.scheme() != u"https" || url.authority() != u"olrite.lol" ||
        url.hasQuery() || url.hasFragment())
        return {};
    static const QRegularExpression pattern(
        QStringLiteral("^/(?:api/delete|delete|d)/([A-Za-z0-9_-]{1,128})$"));
    return pattern.match(url.path(QUrl::FullyEncoded)).captured(1);
}

inline QString olriteImageLink(QString text)
{
    const auto link = olriteLink(std::move(text));
    const QUrl url(link);
    if (url.path() == u"/delete" || url.path() == u"/uploaders" ||
        url.path() == u"/upload" || url.path() == u"/account" ||
        url.path() == u"/privacy" || url.path() == u"/recent" || url.path() == u"/stats")
        return {};
    static const QRegularExpression pattern(
        QStringLiteral("^/(?:i/)?[A-Za-z0-9_-]{4,64}(?:\\.(?:png|jpe?g|gif|webp|avif))?$"));
    return !url.hasQuery() && !url.hasFragment() &&
                   pattern.match(url.path()).hasMatch()
               ? link
               : QString();
}

inline bool failedBeforeUpload(const NetworkResult &result)
{
    return !result.status() &&
           (result.error() == QNetworkReply::HostNotFoundError ||
            result.error() == QNetworkReply::ConnectionRefusedError);
}

inline bool hostUnavailable(const NetworkResult &result)
{
    if (const auto status = result.status())
        return *status == 500 || *status == 502 || *status == 503 || *status == 504;
    return failedBeforeUpload(result) ||
           result.error() == QNetworkReply::RemoteHostClosedError ||
           result.error() == QNetworkReply::TimeoutError ||
           result.error() == QNetworkReply::TemporaryNetworkFailureError ||
           result.error() == QNetworkReply::ContentReSendError;
}

}  // namespace chatterino::imageuploader::detail
