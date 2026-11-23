// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

// Link-preview regression checks against a local stand-in for the link
// resolver (fictional answers only). Uses the real LinkResolver, LinkInfo,
// MessageBuilder and NetworkManager; nothing leaves 127.0.0.1.

#include "common/Env.hpp"
#include "common/LinkParser.hpp"
#include "common/network/NetworkManager.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/ImagePreview.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/twitch/ClipPreview.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/Settings.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <vector>
#include <memory>

namespace linkpreview {

class FakeLinkResolverServer final : public QTcpServer
{
public:
    QHash<QString, int> requests;
    bool sawCredentials = false;
    int thumbnailAttempts = 0;

    FakeLinkResolverServer()
    {
        QObject::connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = this->nextPendingConnection())
            {
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] {
                                     this->answer(socket);
                                 });
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
            }
        });
    }

private:
    void answer(QTcpSocket *socket)
    {
        const auto request = QString::fromUtf8(socket->readAll());
        const auto lower = request.toLower();
        if (lower.contains("authorization:") || lower.contains("oauth") ||
            lower.contains("client-id:"))
        {
            this->sawCredentials = true;
        }
        const auto path = request.section(' ', 1, 1);
        if (path == QStringLiteral("/test-thumbnail.png"))
        {
            if (++this->thumbnailAttempts == 1)
            {
                socket->write("HTTP/1.1 503 Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            }
            else
            {
                QImage pixel(20, 10, QImage::Format_RGB32);
                pixel.fill(Qt::red);
                QBuffer png;
                png.open(QIODevice::WriteOnly);
                pixel.save(&png, "PNG");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: " +
                              QByteArray::number(png.data().size()) + "\r\n\r\n" + png.data());
            }
            socket->disconnectFromHost();
            return;
        }
        const auto target =
            QUrl::fromPercentEncoding(path.section("/r/", 1).toUtf8());
        this->requests[target] += 1;

        if (target.contains("slow.example"))
        {
            return;  // never answers: the client must time out by itself
        }
        QJsonObject body;
        auto tooltip = [](const QString &html) {
            return QString::fromUtf8(QUrl::toPercentEncoding(html));
        };
        if (target.contains("flaky.example") && this->requests.value(target) == 1)
        {
            body = {{"status", 503}};
        }
        else if (target.contains("flaky.example") || target.contains("/clip/"))
        {
            body = {{"status", 200},
                    {"tooltip", tooltip("<b>Fictional clip title</b><br>"
                                        "<b>Channel:</b> fake_channel<br>"
                                        "<b>Duration:</b> 0:30")},
                    {"thumbnail", "https://clips-media.example.org/fake.jpg"}};
        }
        else if (target.contains("evilthumb"))
        {
            body = {{"status", 200},
                    {"tooltip", tooltip("<b>Page with a local thumbnail</b>")},
                    {"thumbnail", "http://127.0.0.1:1/private.png"}};
        }
        else if (target.contains("blocked.example"))
        {
            body = {{"status", 403}, {"message", "forbidden"}};
        }
        else
        {
            body = {{"status", 200},
                    {"tooltip", tooltip("<b>Fictional page title</b>")}};
        }
        const auto json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                      "Connection: close\r\nContent-Length: " +
                      QByteArray::number(json.size()) + "\r\n\r\n" + json);
        socket->disconnectFromHost();
    }
};

inline bool waitUntil(const std::function<bool()> &done, int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

/// Returns the number of failed checks; appends {name, ok} rows to `checks`.
inline int runLinkPreviewChecks(chatterino::Settings &settings,
                                QJsonArray &checks)
{
    using namespace chatterino;
    int failed = 0;
    auto check = [&](const QString &name, bool ok) {
        failed += ok ? 0 : 1;
        checks.append(QJsonObject{{"name", name}, {"ok", ok}});
    };

    FakeLinkResolverServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
    {
        check("fake resolver listens on 127.0.0.1", false);
        return failed;
    }
    // Env is read lazily, on the first link request below.
    qputenv("CHATTERINO2_LINK_RESOLVER_URL",
            QStringLiteral("http://127.0.0.1:%1/r/%2")
                .arg(server.serverPort())
                .arg(QStringLiteral("%1"))
                .toUtf8());
    check("resolver URL points at the local stand-in",
          Env::get().linkResolverUrl.startsWith(
              QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
    NetworkManager::init();
    settings.linkInfoTooltip = true;
    settings.unshortLinks = false;
    settings.showLinkTitles = false;
    LinkResolver resolver;

    {
        MessageBuilder builder;
        builder.emplace<TextElement>(QStringLiteral("Noctis 123 / Текст"), MessageElementFlag::Text);
        auto message = builder.release();
        MessageColors colors{};
        colors.regularText = Qt::white;
        colors.regularBg = Qt::black;
        auto render = [&](MessageLayout &layout, qreal density) {
            constexpr int width = 221;
            layout.layout({.messageColors = colors, .flags = MessageElementFlag::Text,
                           .width = width, .imageScale = float(density), .message = *message}, false);
            QImage raster(QSize(width, layout.getHeight()) * density, QImage::Format_ARGB32_Premultiplied);
            raster.setDevicePixelRatio(density);
            raster.fill(Qt::black);
            QPainter painter(&raster);
            layout.paint({.painter = painter, .selection = Selection{},
                          .colorProvider = ColorProvider::instance(), .messageColors = colors,
                          .preferences = MessagePreferences{}, .canvasWidth = width});
            painter.end();
            return raster;
        };
        MessageLayout moved(message);
        MessageLayout fresh(message);
        const auto at100 = render(moved, 1.0);
        auto blank = at100;
        blank.fill(Qt::black);
        check("chat text is rerasterized after switching to a fractional-DPI display",
              at100 != blank && render(moved, 1.5) == render(fresh, 1.5));
        check("chat text recovers its native pixels when moved back to 100% DPI",
              render(moved, 1.0) == at100);
    }

    // 1. a message with a link does not ask anything by itself
    const auto clip =
        QStringLiteral("https://www.twitch.tv/fake_channel/clip/FakeClip-1");
    {
        MessageBuilder builder;
        const QString source = clip;
        builder.addLink(*linkparser::parse(source), source);
        waitUntil([] { return false; }, 300);
    }
    check("a message with a link sends no request on arrival",
          server.requests.value(clip) == 0);

    // 2. hover-time resolve: Twitch clip with title and thumbnail
    LinkInfo first(clip);
    resolver.resolve(&first);
    waitUntil([&] { return first.isLoaded(); }, 5000);
    check("clip preview resolves with title, channel and thumbnail",
          first.isResolved() &&
              first.tooltip().contains("Fictional clip title") &&
              first.tooltip().contains("Duration") && first.hasThumbnail());

    // 3. the same link elsewhere comes from the cache
    LinkInfo again(clip);
    resolver.resolve(&again);
    check("the same link again is answered from the cache",
          again.isResolved() && server.requests.value(clip) == 1);

    // 4. two hovers before an answer share one request
    const auto plain = QStringLiteral("https://plain.example.org/article");
    LinkInfo a(plain);
    LinkInfo b(plain);
    resolver.resolve(&a);
    resolver.resolve(&b);
    waitUntil([&] { return a.isLoaded() && b.isLoaded(); }, 5000);
    check("simultaneous hovers share one request",
          a.isResolved() && b.isResolved() &&
              server.requests.value(plain) == 1 &&
              a.tooltip().contains("Fictional page title") && !a.hasThumbnail());

    // 5. a thumbnail on a local address is not loaded
    LinkInfo evil(QStringLiteral("https://evilthumb.example.org/"));
    resolver.resolve(&evil);
    waitUntil([&] { return evil.isLoaded(); }, 5000);
    check("thumbnail on a local address is refused",
          evil.isResolved() && !evil.hasThumbnail());

    // 6. a site that refuses previews: simple card with site and address
    LinkInfo blocked(QStringLiteral("https://blocked.example.org/page"));
    resolver.resolve(&blocked);
    waitUntil([&] { return blocked.isLoaded(); }, 5000);
    check("site without preview gets a simple card with site and address",
          blocked.hasError() &&
              blocked.tooltip().contains("blocked.example.org") &&
              blocked.tooltip().contains("https://blocked.example.org/page"));

    const auto screenshot = QStringLiteral("https://flaky.example.org/pic.png?token=x%2Fy&v=2");
    LinkInfo inlineInfo(screenshot);
    LinkInfo hoverInfo(screenshot);
    imagepreview::getOrRequest(inlineInfo);
    resolver.resolve(&hoverInfo);
    waitUntil([&] { return hoverInfo.isLoaded() && inlineInfo.isLoaded(); }, 3000);
    imagepreview::getOrRequest(inlineInfo);
    resolver.resolve(&hoverInfo);
    check("inline picture and title share their request and transient cooldown",
          inlineInfo.isTransientFailure() && hoverInfo.isTransientFailure() &&
              !hoverInfo.canResolve() && server.requests.value(screenshot) == 1);

    auto thumbnail = Image::fromLinkThumbnail({QStringLiteral("http://127.0.0.1:%1/test-thumbnail.png")
                                         .arg(server.serverPort())});
    thumbnail->pixmapOrLoad();
    // 7. a site that never answers times out
    LinkInfo slow(QStringLiteral("https://slow.example.org/"));
    QElapsedTimer timer;
    timer.start();
    resolver.resolve(&slow);
    waitUntil([&] { return slow.isLoaded(); }, 15000);
    check("an unanswered request ends with an error after the timeout",
          slow.hasError() && timer.elapsed() >= 7000 &&
              timer.elapsed() < 12000 &&
              slow.tooltip().contains("slow.example.org"));

    check("thumbnail download recovers after a temporary image-server error",
          thumbnail->loaded() && server.thumbnailAttempts == 2);

    waitUntil([&] { return inlineInfo.canResolve() && hoverInfo.canResolve(); }, 10000);
    imagepreview::getOrRequest(inlineInfo);
    resolver.resolve(&hoverInfo);
    waitUntil([&] { return hoverInfo.isResolved() && inlineInfo.isResolved(); }, 3000);
    check("same message recovers after temporary preview failure with one request",
          hoverInfo.hasThumbnail() && imagepreview::getOrRequest(inlineInfo) &&
              imagepreview::getOrRequest(inlineInfo) == hoverInfo.thumbnail() &&
              server.requests.value(screenshot) == 2);
    imagepreview::forget(screenshot);
    check("deleting an inline picture suppresses an already resolved preview",
          !imagepreview::getOrRequest(inlineInfo) && !hoverInfo.hasThumbnail());

    // 8. switched off: nothing is requested
    settings.linkInfoTooltip = false;
    const auto off = QStringLiteral("https://off.example.org/");
    LinkInfo disabled(off);
    resolver.resolve(&disabled);
    waitUntil([] { return false; }, 300);
    check("with previews off nothing is requested",
          disabled.state() == LinkInfo::State::Created &&
              server.requests.value(off) == 0);

    {
        const QString url = QStringLiteral("https://example.org/a/b?q=one&lang=ru#frag");
        const QString quoted = QStringLiteral("«") + url + QStringLiteral("»,");
        const auto parsed = linkparser::parse(quoted);
        check("Russian quotation and punctuation stay outside the clickable URL",
              parsed && parsed->link == url && parsed->prefix(quoted) == QStringLiteral("«") &&
                  parsed->suffix(quoted) == QStringLiteral("»,"));
        if (parsed)
        {
            settings.wrapLinksAtBreaks = true;
            MessageBuilder builder;
            builder.addLink(*parsed, quoted);
            auto msg = builder.release();
            MessageLayout layout(msg);
            MessageColors colors{};
            layout.layout({.messageColors = colors, .flags = MessageElementFlag::Text,
                           .width = 110, .message = *msg}, false);
            QString copied;
            layout.addSelectionText(copied);
            check("a URL copied across wrapped lines has no inserted spaces",
                  copied.trimmed() == quoted);
        }
    }
    const auto hosts = imagepreview::sites(QStringLiteral("kappa.lol"));
    const auto backupHosts = imagepreview::sites(QStringLiteral("olrite.lol"));
    check("olriteImageAndViewerLinksArePreviewsButDeletionLinkIsNot",
          imagepreview::isImageLink(QStringLiteral("https://olrite.lol/fixtureID"), backupHosts) &&
              imagepreview::isImageLink(QStringLiteral("https://olrite.lol/i/fixtureID"), backupHosts) &&
              !imagepreview::isImageLink(QStringLiteral("https://olrite.lol/delete/fixtureToken"), backupHosts) &&
              !imagepreview::isImageLink(QStringLiteral("https://olrite.lol/recent"), backupHosts));
    check("lowercase screenshot IDs and long IDs are recognised, service pages excluded",
          imagepreview::isImageLink(QStringLiteral("https://kappa.lol/abcxyz"), hosts) &&
              imagepreview::isImageLink(QStringLiteral("https://kappa.lol/group/abcdefghijklmnopqr"), hosts) &&
              !imagepreview::isImageLink(QStringLiteral("https://kappa.lol/uploaders"), hosts));
    check("different ports and encoded separators remain distinct picture identities",
          imagepreview::key(QStringLiteral("https://example.org:8443/a.png")) !=
              imagepreview::key(QStringLiteral("https://example.org/a.png")) &&
          imagepreview::key(QStringLiteral("https://example.org/a%2Fb.png")) !=
              imagepreview::key(QStringLiteral("https://example.org/a/b.png")));

    // 9. titles in chat (like Chatterino+): the link shows "title (domain)"
    const QString clonedUrl = "https://history.example.org/article";
    MessageBuilder sourceBuilder;
    sourceBuilder.addLink(*linkparser::parse(clonedUrl), clonedUrl);
    auto originalMessage = sourceBuilder.release();
    auto clonedMessage = originalMessage->clone();
    check("title-disabled source is initially unresolved",
          server.requests.value(clonedUrl) == 0);

    settings.showLinkTitles = true;
    {
        MessageLayout layout(clonedMessage);
        MessageColors colors{};
        layout.layout({.messageColors = colors, .flags = MessageElementFlag::Text,
                       .width = 350, .message = *clonedMessage}, false);
        const LinkElement *clonedLink = nullptr;
        for (const auto &element : clonedMessage->elements)
        {
            if (auto *candidate = dynamic_cast<const LinkElement *>(element.get()))
                clonedLink = candidate;
        }
        waitUntil([&] { return clonedLink && !clonedLink->titleText().isEmpty(); }, 3000);
        check("unresolved history or cloned link resolves on first layout without hover",
              clonedLink && clonedLink->titleText() == "Fictional page title (history.example.org)" &&
                  server.requests.value(clonedUrl) == 1);
    }

    const QString flakyTitle = "https://flaky.example.org/automatic-title";
    MessageBuilder flakyBuilder;
    flakyBuilder.addLink(*linkparser::parse(flakyTitle), flakyTitle);
    auto flakyMessage = flakyBuilder.release();
    const LinkElement *flakyLink = nullptr;
    for (const auto &element : flakyMessage->elements)
    {
        if (auto *candidate = dynamic_cast<const LinkElement *>(element.get()))
            flakyLink = candidate;
    }
    waitUntil([&] { return server.requests.value(flakyTitle) == 1; }, 3000);
    waitUntil([&] { return flakyLink && !flakyLink->titleText().isEmpty(); }, 20000);
    check("transient title failure retries automatically without hover or relayout",
          flakyLink && !flakyLink->titleText().isEmpty() &&
              server.requests.value(flakyTitle) == 2);

    const auto titled =
        QStringLiteral("https://www.twitch.tv/fake_channel/clip/FakeClip-2");
    {
        MessageBuilder builder;
        const QString source = titled;
        builder.addLink(*linkparser::parse(source), source);
        auto message = builder.release();
        const LinkElement *link = nullptr;
        for (const auto &element : message->elements)
        {
            if (auto *candidate =
                    dynamic_cast<const LinkElement *>(element.get()))
            {
                link = candidate;
            }
        }
        waitUntil([&] { return link && !link->titleText().isEmpty(); }, 5000);
        check("with titles on, a link shows \"title (domain)\" in chat",
              link != nullptr && server.requests.value(titled) == 1 &&
                  link->titleText() ==
                      QStringLiteral("Fictional clip title (twitch.tv)"));
        MessageColors colors{};
        MessageLayout layout(message);
        layout.layout({.messageColors = colors, .flags = MessageElementFlag::Text,
                       .width = 105, .message = *message}, false);
        QString copied;
        layout.addSelectionText(copied);
        check("selecting a wrapped page title copies the original URL once",
              copied.trimmed() == titled);
        QString partial;
        layout.addSelectionText(partial, 1, 3);
        check("selecting part of a shortened title copies the full URL",
              partial == titled);

        MessageBuilder repeated;
        repeated.emplace<TextElement>("before", MessageElementFlag::Text, MessageColor::Text);
        for (int i = 0; i < 2; ++i)
        {
            auto *entry = repeated.emplace<LinkElement>(
                LinkElement::Parsed{.lowercase = titled, .original = titled},
                titled, MessageElementFlag::Text, MessageColor::Link);
            entry->linkInfo()->setTooltip("<b>A long page title</b>");
            entry->linkInfo()->setState(LinkInfo::State::Resolved);
        }
        repeated.emplace<TextElement>("after", MessageElementFlag::Text, MessageColor::Text);
        auto mixedMessage = repeated.release();
        MessageLayout mixedLayout(mixedMessage);
        mixedLayout.layout({.messageColors = colors, .flags = MessageElementFlag::Text,
                            .width = 105, .message = *mixedMessage}, false);
        QString mixedCopy;
        mixedLayout.addSelectionText(mixedCopy);
        check("copy keeps surrounding text and repeated links",
              mixedCopy.trimmed() == "before " + titled + " " + titled + " after");
    }
    settings.showLinkTitles = false;

    // 10. clip cards (like Chatterino+): which links get one, and where
    check("clip links are recognised, other links are not",
          clippreview::clipSlug(QStringLiteral(
              "https://www.twitch.tv/morphe_ya/clip/"
              "PunchyStormyPenguinOhMyDog-oV2QpqBiymnVVR3r")) ==
                  QStringLiteral(
                      "PunchyStormyPenguinOhMyDog-oV2QpqBiymnVVR3r") &&
              clippreview::clipSlug(QStringLiteral(
                  "https://clips.twitch.tv/AbcDef-12")) ==
                  QStringLiteral("AbcDef-12") &&
              clippreview::clipSlug(
                  QStringLiteral("twitch.tv/x/clip/Some_Clip-1")) ==
                  QStringLiteral("Some_Clip-1") &&
              clippreview::clipSlug(QStringLiteral("https://www.twitch.tv/x"))
                  .isEmpty() &&
              clippreview::clipSlug(
                  QStringLiteral("https://clips.twitch.tv/create"))
                  .isEmpty() &&
              clippreview::clipSlug(
                  QStringLiteral("https://example.org/x/clip/Some_Clip-1"))
                  .isEmpty());
    {
        MessageBuilder builder;
        const QString text = clip;
        builder.emplace<TextElement>(QStringLiteral("look"),
                                     MessageElementFlag::Text);
        builder.addLink(*linkparser::parse(text), text);
        builder.addLink(*linkparser::parse(text), text);  // same clip twice
        builder.emplace<TextElement>(QStringLiteral("lol"),
                                     MessageElementFlag::Text);
        auto message = builder.release();
        int cards = 0;
        bool last = false;
        for (const auto &element : message->elements)
        {
            last = dynamic_cast<const ClipCardElement *>(element.get()) !=
                   nullptr;
            cards += last ? 1 : 0;
        }
        check("one card per clip, after the whole message text",
              cards == 1 && last);
    }
    const bool ru = activeUiLanguage().startsWith(QStringLiteral("ru"));
    check("card texts: duration, views, age",
          clippreview::formatDuration(6.4) == QStringLiteral("0:06") &&
              clippreview::formatDuration(3723) == QStringLiteral("1:02:03") &&
              clippreview::formatViews(1) == (ru ? QString::fromUtf8("1 просмотр") : QStringLiteral("1 view")) &&
              clippreview::formatViews(3) == (ru ? QString::fromUtf8("3 просмотра") : QStringLiteral("3 views")) &&
              clippreview::formatViews(25) == (ru ? QString::fromUtf8("25 просмотров") : QStringLiteral("25 views")) &&
              clippreview::formatViews(111) == (ru ? QString::fromUtf8("111 просмотров") : QStringLiteral("111 views")) &&
              clippreview::formatAge(
                  QDateTime::currentDateTimeUtc().addSecs(-10),
                  QDateTime::currentDateTimeUtc()) ==
                  (ru ? QString::fromUtf8("только что") : QStringLiteral("just now")) &&
              clippreview::formatAge(
                  QDateTime::currentDateTimeUtc().addSecs(-7200),
                  QDateTime::currentDateTimeUtc()) ==
                  (ru ? QString::fromUtf8("2 ч назад") : QStringLiteral("2 h ago")));
    check("without a signed-in account no clip request leaves the app",
          clippreview::get(QStringLiteral("FakeClip-1")) == nullptr);

    {
        // A scrollback larger than the metadata LRU must not start requesting
        // the same 301 screenshots every time a thumbnail triggers layout.
        std::vector<std::unique_ptr<LinkInfo>> scrollback;
        const int previous = server.requests.size();
        for (int i = 0; i < 301; ++i)
        {
            auto &info = scrollback.emplace_back(std::make_unique<LinkInfo>(
                QStringLiteral("https://pics.example.org/clip/%1.png").arg(i)));
            imagepreview::getOrRequest(*info);
        }
        const auto allResolved = [&] {
            return std::ranges::all_of(scrollback, [](const auto &info) {
                return info->isResolved();
            });
        };
        waitUntil(allResolved, 12000);
        bool retained = allResolved();
        for (const auto &info : scrollback)
        {
            retained &= imagepreview::getOrRequest(*info) != nullptr;
        }
        waitUntil([] { return false; }, 200);
        check("301 visible screenshots retain their result across resolver LRU eviction",
              retained && server.requests.size() == previous + 301 &&
              std::ranges::all_of(scrollback, [&](const auto &info) {
                  return server.requests.value(info->originalUrl()) == 1;
              }));
    }

    check("no token or client id is sent to the resolver",
          !server.sawCredentials);
    return failed;
}

}  // namespace linkpreview
