// SPDX-License-Identifier: MIT
#pragma once

#include "UploadDraftChecks.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/ImageUploadRouting.hpp"

#include <QDeadlineTimer>

namespace chatterino {

class ImageUploaderTestAccess {
public:
    static void localHosts(ImageUploader &uploader, QUrl primary, QUrl backup)
    {
        uploader.kappaEndpoint_ = std::move(primary);
        uploader.olriteEndpoint_ = std::move(backup);
    }
    static void expireHealth(ImageUploader &uploader)
    {
        uploader.kappaHealthUntil_ = QDeadlineTimer(0);
    }
};

}  // namespace chatterino

namespace chatterino::preview::uploadchecks {

struct UploadEndpoint {
    QTcpServer server;
    int heads = 0;
    int posts = 0;
    int deletes = 0;
    int deleteStatus = 200;
    int probeStatus = 200;
    int postStatus = 200;
    bool dropPost = false;
    bool stopAfterProbe = false;
    bool transportClean = true;
    QByteArray response, lastHeaders, lastBody;
    QByteArray deleteResponse = R"({"success":true})";
    QByteArray lastDeleteRequest;
    std::function<void()> onHead;

    QUrl url(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/%2")
                        .arg(server.serverPort()).arg(path));
    }

    bool listen()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *s = server.nextPendingConnection())
            {
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const auto input = s->peek(s->bytesAvailable());
                    const auto headerEnd = input.indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    const auto headers = input.first(headerEnd).toLower();
                    const bool head = headers.startsWith("head ");
                    const bool deleting = headers.startsWith("post /api/delete/");
                    qint64 bytes = 0;
                    for (const auto &line : headers.split('\n'))
                        if (line.startsWith("content-length:"))
                            bytes = line.sliced(15).trimmed().toLongLong();
                    if (bytes > input.size() - headerEnd - 4)
                        return;
                    const auto body = s->read(headerEnd + 4 + bytes).sliced(headerEnd + 4);
                    if (head)
                    {
                        ++heads;
                        transportClean &= body.isEmpty();
                        if (onHead)
                            onHead();
                        if (stopAfterProbe)
                            server.close();
                    }
                    else if (deleting)
                    {
                        ++deletes;
                        lastDeleteRequest = headers;
                        transportClean &= body == "{}";
                    }
                    else
                    {
                        ++posts;
                        lastBody = body;
                        lastHeaders = headers;
                        transportClean &= headers.startsWith("post ") &&
                            headers.contains("multipart/form-data") &&
                            body.contains(QByteArray::fromHex("89504e470d0a1a0a"));
                        if (dropPost)
                        {
                            s->abort();
                            return;
                        }
                    }
                    const auto output = head ? QByteArray() : deleting ? deleteResponse : response;
                    const auto code = head ? probeStatus : deleting ? deleteStatus : postStatus;
                    s->write("HTTP/1.1 " + QByteArray::number(code) +
                             " Fixture\r\nConnection: close\r\nContent-Length: " +
                             QByteArray::number(output.size()) + "\r\n\r\n" + output);
                    s->disconnectFromHost();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

inline int checkRouting(const QString &reportPath)
{
    UploadEndpoint primary, backup, custom;
    QTemporaryDir logs;
    if (!primary.listen() || !backup.listen() || !custom.listen() || !logs.isValid())
        return 40;
    NetworkManager::init();
    auto *s = getSettings();
    auto *uploader = getApp()->getImageUploader();
    auto stock = [&] {
        s->imageUploaderEnabled = true;
        s->imageUploaderKappaFallback = true;
        s->imageUploaderUrl = s->imageUploaderUrl.getDefaultValue();
        s->imageUploaderHeaders = QString();
        s->imageUploaderFormField = "file";
        s->imageUploaderLink = "{link}";
        s->imageUploaderDeletionLink = "{delete}";
        s->enableChannelPointsDisplay = false;
        s->askOnImageUpload = false;
        s->logPath = logs.path();
    };
    stock();
    ImageUploaderTestAccess::localHosts(*uploader, primary.url("stock"), backup.url("backup"));
    auto expire = [&] { ImageUploaderTestAccess::expireHealth(*uploader); };

    auto chat = std::make_shared<ChatSink>();
    Split split(nullptr);
    split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(chat)));
    auto *edit = dynamic_cast<ResizingTextEdit *>(split.getInput().findChild<QTextEdit *>());
    if (!edit)
    {
        NetworkManager::deinit();
        return 41;
    }
    QMimeData source;
    QImage sample(8, 5, QImage::Format_ARGB32);
    sample.fill(Qt::magenta);
    source.setImageData(sample);
    int finished = 0, failed = 0;
    QString link, deletion;
    pajlada::Signals::SignalHolder holder;
    holder.managedConnect(uploader->updated, [&](const ImageUploadUpdate &event) {
        if (event.kind == ImageUploadUpdate::Kind::Finished)
        {
            ++finished;
            link = event.link;
            deletion = event.deletionLink;
        }
        if (event.kind == ImageUploadUpdate::Kind::Failed)
            ++failed;
    });

    QJsonObject checks;
    bool passed = true;
    auto check = [&](const QString &name, bool ok) {
        checks.insert(name, ok);
        passed &= ok;
    };
    auto paste = [&](const QString &expectedLink) {
        const int successes = finished, errors = failed;
        link.clear();
        deletion.clear();
        edit->setPlainText("before");
        edit->moveCursor(QTextCursor::End);
        (edit->*&PasteAccess::insertFromMimeData)(&source);
        edit->insertPlainText(" after");
        const bool done = publiclogchecks::waitUntil(
            [&]{ return finished > successes || failed > errors; }, 5000);
        const bool status = expectedLink.isEmpty()
            ? failed == errors + 1 && finished == successes
            : finished == successes + 1 && failed == errors && link == expectedLink;
        const auto expectedText = expectedLink.isEmpty() ? QStringLiteral("before after") :
            QStringLiteral("before %1 after").arg(expectedLink);
        return done && status && edit->toPlainText() == expectedText && edit->textCursor().atEnd();
    };
    const QString primaryURL = QStringLiteral("https://kappa.lol/fixtureA");
    const QString backupURL = QStringLiteral("https://olrite.lol/i/fixtureB");
    const QString backupDeleteURL = QStringLiteral("https://olrite.lol/delete/fixture-secret-token");
    const QString deleteURL = QStringLiteral("https://kappa.lol/delete?fixture-key");
    primary.response = QJsonDocument(QJsonObject{{"link",primaryURL},{"delete",deleteURL}}).toJson();
    auto backupReply = [&](const QString &url, const QString &deletionUrl = QString()) {
        return QJsonDocument(QJsonObject{{"success", true}, {"url", url},
                                         {"delete", deletionUrl}}).toJson();
    };
    backup.response = backupReply(backupURL, backupDeleteURL);

    using imageuploader::detail::olriteImageLink;
    using imageuploader::detail::olriteDeletionToken;
    check("knownBackupLinksAlwaysHaveExplicitLowercaseHttps",
          olriteImageLink("//OLRITE.LOL/i/fixtureB") == backupURL &&
          olriteImageLink("HTTP://OLRITE.LOL/i/fixtureB") == backupURL &&
          olriteImageLink("HTTPS://olrite.lol/i/fixtureB") == backupURL &&
          olriteImageLink(" https://olrite.lol/i/fixtureB\n") == backupURL &&
          olriteImageLink("/i/fixtureB") == backupURL);
    check("backupRejectsCredentialsPortsAndNonLinks",
          olriteImageLink("https://fixture@olrite.lol/abcd").isEmpty() &&
          olriteImageLink("https://olrite.lol:444/abcd").isEmpty() &&
          olriteImageLink("file:///fixture.png").isEmpty() &&
          olriteImageLink("https://olrite.lol/").isEmpty() &&
          olriteImageLink("olrite.lol/a\nother").isEmpty() &&
          olriteImageLink(backupDeleteURL).isEmpty());
    check("deletionTokenMustBeFromExpectedHostAndPath",
          olriteDeletionToken(QUrl(backupDeleteURL)) == "fixture-secret-token" &&
          olriteDeletionToken(QUrl("https://olrite.lol/d/fixture-secret-token")) == "fixture-secret-token" &&
          olriteDeletionToken(QUrl("https://olrite.lol.bad.invalid/delete/token")).isEmpty() &&
          olriteDeletionToken(QUrl("https://olrite.lol/delete/token?extra=1")).isEmpty() &&
          olriteDeletionToken(QUrl("https://olrite.lol/i/token")).isEmpty());
    primary.probeStatus = 503;
    check("failedPreflightSendsBodyOnlyToBackup", paste(backupURL) &&
          primary.heads == 1 && primary.posts == 0 && backup.posts == 1 && deletion == backupDeleteURL &&
          uploader->deletionLinkFor(backupURL) == backupDeleteURL &&
          backup.lastHeaders.startsWith("post /backup http/1.") &&
          !backup.lastHeaders.contains("authorization:") &&
          !backup.lastHeaders.contains("x-fixture-secret:") &&
          backup.lastHeaders.contains("x-olrite-preserve: true") &&
          backup.lastBody.contains("name=\"file\""));
    backup.response = backupReply("olrite.lol/i/fixtureB", backupDeleteURL);
    check("cachedOutageAndBareBackupHostname", paste(backupURL) &&
          primary.heads == 1 && primary.posts == 0 && backup.posts == 2);
    backup.response = backupReply("javascript:alert(1)", backupDeleteURL);
    check("untrustedSchemePreservesDraft", paste({}) && backup.posts == 3 && primary.posts == 0);
    backup.response = backupReply("https://olrite.lol.bad.invalid/trap", backupDeleteURL);
    check("lookalikeHostPreservesDraft", paste({}) && backup.posts == 4);
    backup.response = backupReply(backupURL, backupDeleteURL);
    expire();
    primary.probeStatus = 200;
    check("recoveredPrimaryChosenOnNextHealthCheck", paste(primaryURL) &&
          primary.heads == 2 && primary.posts == 1 && backup.posts == 4 &&
          deletion == deleteURL && primary.lastBody.contains("name=\"file\""));

    primary.dropPost = true;
    check("lostReplyNeverSendsSameImageToBackup", paste({}) && primary.heads == 2 &&
          primary.posts == 2 && backup.posts == 4);
    primary.dropPost = false;
    check("newExplicitPasteCanUseBackup", paste(backupURL) &&
          primary.posts == 2 && backup.posts == 5);

    const auto beforeRefusal = primary.posts;
    const auto beforeRefusalBackup = backup.posts;
    for (int code : {403, 429})
    {
        expire();
        primary.probeStatus = code;
        check(QStringLiteral("refusedPreflightStops%1").arg(code), paste({}) &&
              primary.posts == beforeRefusal && backup.posts == beforeRefusalBackup);
    }
    expire();
    primary.probeStatus = 404;
    check("postOnlyPrimaryRemainsUsable", paste(primaryURL) &&
          primary.posts == beforeRefusal + 1 && backup.posts == beforeRefusalBackup);
    expire();
    primary.probeStatus = 503;
    const auto beforeChange = primary.posts + backup.posts + custom.posts;
    primary.onHead = [&]{ s->imageUploaderUrl = custom.url("changed").toString(); };
    check("changingUploadConfigurationDuringProbeStopsSubmission",
          paste({}) && primary.posts + backup.posts + custom.posts == beforeChange);
    primary.onHead = {};
    stock();
    expire();
    const int beforeDisable = primary.posts + backup.posts + custom.posts;
    primary.onHead = [&]{ s->imageUploaderEnabled = false; };
    check("disablingUploaderDuringProbeSendsNoBody", paste({}) &&
          primary.posts + backup.posts + custom.posts == beforeDisable);
    primary.onHead = {};
    stock();
    expire();
    const int probesBeforeHeaders = primary.heads;
    const auto fallbackBeforeHeaders = backup.posts;
    s->imageUploaderHeaders = "X-Fixture-Secret: sample";
    check("authenticatedStockUploaderIsNeverRerouted", paste(primaryURL) &&
          primary.heads == probesBeforeHeaders && backup.posts == fallbackBeforeHeaders &&
          primary.lastHeaders.contains("x-fixture-secret: sample"));

    const QString customURL = "https://custom.invalid/r";
    custom.response = QJsonDocument(QJsonObject{{"picture", customURL}}).toJson();
    s->imageUploaderUrl = custom.url("custom?mode=private").toString();
    s->imageUploaderLink = "{picture}";
    s->imageUploaderFormField = "customField";
    check("customUploaderKeepsHeadersFieldParserAndQuery", paste(customURL) &&
          custom.posts == 1 && custom.heads == 0 && backup.posts == fallbackBeforeHeaders &&
          custom.lastHeaders.contains("x-fixture-secret: sample") &&
          custom.lastHeaders.startsWith("post /custom?mode=private http/1.") &&
          custom.lastBody.contains("name=\"customField\""));
    custom.postStatus = 503;
    check("customUploaderFailureNeverChangesHost", paste({}) && custom.posts == 2 &&
          primary.heads == probesBeforeHeaders && backup.posts == fallbackBeforeHeaders);
    stock();
    s->imageUploaderKappaFallback = false;
    check("disabledPolicyUsesOnlyConfiguredUploader", paste(primaryURL) &&
          primary.heads == probesBeforeHeaders && backup.posts == fallbackBeforeHeaders);
    stock();
    expire();
    // A successful HEAD can race a listener shutting down. The POST attempt
    // cannot connect, so no primary server receives any image bytes.
    primary.probeStatus = 200;
    primary.stopAfterProbe = true;
    const int beforeClosedPost = primary.posts;
    check("refusedConnectionAfterHealthyProbeTriesBackupOnce", paste(backupURL) &&
          primary.posts == beforeClosedPost && backup.posts == fallbackBeforeHeaders + 1);

    ImageUploader restored;
    ImageUploaderTestAccess::localHosts(restored, primary.url("stock"), backup.url("backup"));
    check("backupDeletionLinkSurvivesRestart", restored.deletionLinkFor(backupURL) == backupDeleteURL);
    auto &accounts = getApp()->getAccounts()->twitch.accounts;
    const int ownerIndex = accounts.append(
        std::make_shared<TwitchAccount>("fixture_owner", "", "", "9000001"));
    MessageBuilder ownerBuilder;
    ownerBuilder.message().platform = MessagePlatform::AnyOrTwitch;
    ownerBuilder.message().loginName = "fixture_owner";
    ownerBuilder.message().userID = "9000001";
    ownerBuilder.emplace<LinkElement>(LinkElement::Parsed{backupURL, backupURL},
                                       backupURL, MessageElementFlag::Text);
    const auto ownerMessage = ownerBuilder.release();
    check("ownScreenshotGetsChatDeleteActionAfterRestart",
          restored.deletableImages(*ownerMessage) == QStringList{backupURL});
    ownerMessage->userID = "9000002";
    check("otherUserCannotGetOurScreenshotDeleteAction",
          restored.deletableImages(*ownerMessage).isEmpty());
    ownerMessage->userID = "9000001";
    bool deleteFinished = false;
    ImageUploader::DeleteResult deleteResult = ImageUploader::DeleteResult::Failed;
    auto completed = [&](ImageUploader::DeleteResult result, const QString &) {
        deleteResult = result;
        deleteFinished = true;
    };
    backup.deleteStatus = 500;
    backup.deleteResponse = R"({"success":false})";
    restored.deleteImage(backupURL, completed);
    check("failedDeletionKeepsPermanentDeletionLink",
          publiclogchecks::waitUntil([&]{ return deleteFinished; }, 3000) &&
          deleteResult == ImageUploader::DeleteResult::Failed &&
          restored.deletionLinkFor(backupURL) == backupDeleteURL && backup.deletes == 1);
    backup.deleteStatus = 200;
    backup.deleteResponse = R"({"success":true})";
    deleteFinished = false;
    restored.deleteImage(backupURL, completed);
    bool duplicateRejected = false;
    restored.deleteImage(backupURL, [&](ImageUploader::DeleteResult result, const QString &) {
        duplicateRejected = result == ImageUploader::DeleteResult::Failed;
    });
    check("backupDeletesThroughApiAndNeverDuplicates",
          publiclogchecks::waitUntil([&]{ return deleteFinished; }, 3000) &&
          duplicateRejected && deleteResult == ImageUploader::DeleteResult::Deleted &&
          restored.deletionLinkFor(backupURL).isEmpty() && backup.deletes == 2 &&
          backup.lastDeleteRequest.startsWith("post /api/delete/fixture-secret-token http/1.") &&
          !backup.lastDeleteRequest.contains("authorization:"));
    ImageUploader afterDeletion;
    check("deletedBackupLinkStaysDeletedAfterRestart",
          afterDeletion.deletionLinkFor(backupURL).isEmpty() &&
          afterDeletion.deletionLinkFor(primaryURL) == deleteURL &&
          afterDeletion.deletableImages(*ownerMessage).isEmpty());
    accounts.removeAt(ownerIndex);
    const QString missingTokenURL = "https://olrite.lol/i/fixtureC";
    backup.response = backupReply(missingTokenURL, "https://evil.invalid/delete/token");
    check("invalidDeletionUrlDoesNotBecomeADeleteButton",
          paste(missingTokenURL) && deletion.isEmpty() &&
          uploader->deletionLinkFor(missingTokenURL).isEmpty());
    check("legacyNuulsLinkDoesNotAcquireInventedDeletionToken",
          uploader->deletionLinkFor("https://i.nuuls.com/old.png").isEmpty());

    check("payloadOnlyThroughSelectedRoutesAndNoChatSends",
          primary.transportClean && backup.transportClean && custom.transportClean && chat->sent == 0);
    QFile report(reportPath);
    const bool saved = report.open(QIODevice::WriteOnly) &&
                       report.write(QJsonDocument(checks).toJson()) > 0;
    NetworkManager::deinit();
    return saved && passed ? 0 : 42;
}

}  // namespace chatterino::preview::uploadchecks
