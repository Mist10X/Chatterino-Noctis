// SPDX-License-Identifier: MIT
#pragma once

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkManager.hpp"
#include "PublicLogsChecks.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/Settings.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextDocument>

namespace chatterino::preview::uploadchecks {

// Expose the inherited paste hook without replacing the actual SplitInput
// editor or reading/writing the desktop clipboard.
struct PasteAccess : ResizingTextEdit {
    using ResizingTextEdit::insertFromMimeData;
};

struct ChatSink final : Channel {
    ChatSink() : Channel("upload_draft_fixture", Channel::Type::Misc) {}
    bool canSendMessage() const override { return true; }
    void sendMessage(const QString &) override { ++sent; }
    int sent = 0;
};

class Receiver {
public:
    QTcpServer server;
    QPointer<QTcpSocket> pending;
    int requests = 0;
    bool drop = false;
    bool valid = true;

    bool start()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *s = server.nextPendingConnection())
            {
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const auto bytes = s->peek(s->bytesAvailable());
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const auto header = bytes.first(end).toLower();
                    qint64 length = -1;
                    for (const auto &line : header.split('\n'))
                        if (line.startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    if (length < 0 || bytes.size() - end - 4 < length)
                        return;
                    const auto body = s->read(end + 4 + length).sliced(end + 4);
                    valid &= header.startsWith("post /upload http/1.") &&
                             header.contains("multipart/form-data") &&
                             body.contains(QByteArray::fromHex("89504e470d0a1a0a"));
                    ++requests;
                    if (drop)
                        s->abort(); // service already consumed the POST
                    else
                        pending = s;
                });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
    bool ready() const
    {
        return publiclogchecks::waitUntil([&]{ return !pending.isNull(); }, 5000);
    }
    void reply(const QString &url)
    {
        const auto body = url.toUtf8();
        pending->write("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: " +
                       QByteArray::number(body.size()) + "\r\n\r\n" + body);
        pending.clear();
    }
};

inline int run(const QString &reportPath)
{
    Receiver host;
    QTemporaryDir files;
    if (!host.start() || !files.isValid())
        return 40;
    auto *s = getSettings();
    s->imageUploaderEnabled = true;
    s->askOnImageUpload = false;
    s->imageUploaderUrl = QStringLiteral("http://127.0.0.1:%1/upload").arg(host.server.serverPort());
    s->imageUploaderHeaders = QString();
    s->imageUploaderLink = QString();
    s->imageUploaderDeletionLink = QString();
    s->logPath = files.path();
    s->enableChannelPointsDisplay = false;
    NetworkManager::init();

    auto chat = std::make_shared<ChatSink>();
    Split split(nullptr);
    split.setChannel(IndirectChannel(std::static_pointer_cast<Channel>(chat)));
    auto *edit = dynamic_cast<ResizingTextEdit *>(split.getInput().findChild<QTextEdit *>());
    if (!edit)
    {
        NetworkManager::deinit();
        return 41;
    }
    QImage sample(12, 9, QImage::Format_ARGB32);
    sample.fill(Qt::blue);
    QMimeData clipboard;
    clipboard.setImageData(sample);
    const auto paste = [&](const QMimeData &source) {
        (edit->*&PasteAccess::insertFromMimeData)(&source);
    };
    const auto select = [&](int anchor, int position) {
        QTextCursor cur(edit->document());
        cur.setPosition(anchor);
        cur.setPosition(position, QTextCursor::KeepAnchor);
        edit->setTextCursor(cur);
    };
    const auto draft = [&](const QString &text, int index) {
        edit->setPlainText(text);
        select(index, index);
    };

    int finished = 0, failed = 0;
    pajlada::Signals::SignalHolder holder;
    holder.managedConnect(getApp()->getImageUploader()->updated, [&](const ImageUploadUpdate &update) {
        if (update.textEdit == edit)
        {
            finished += update.kind == ImageUploadUpdate::Kind::Finished;
            failed += update.kind == ImageUploadUpdate::Kind::Failed;
        }
    });
    QJsonObject checks;
    bool passed = true;
    const auto check = [&](const QString &name, bool ok) {
        checks.insert(name, ok);
        passed &= ok;
    };
    const QString link = QStringLiteral("https://uploads.invalid/example");
    const auto complete = [&](const QString &url) {
        if (!host.ready())
            return false;
        const int before = finished;
        host.reply(url);
        return publiclogchecks::waitUntil([&]{ return finished > before; }, 5000);
    };
    const auto type = [&](const QString &text) {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, text);
        QApplication::sendEvent(edit, &key);
    };
    draft("caption", 7);
    paste(clipboard);
    type("more");
    check("endPasteKeepsImmediatelyTypedCaption", complete(link) &&
          edit->toPlainText() == "caption " + link + " more" &&
          edit->textCursor().atEnd());

    draft("right", 0);
    paste(clipboard);
    type("hello");
    check("startPasteKeepsBothSidesAndCaret", complete(link) &&
          edit->toPlainText() == link + " helloright" &&
          edit->textCursor().position() == link.size() + 6);

    draft("ab", 1);
    paste(clipboard);
    type("cd");
    select(0, 0);
    type("x");
    check("middlePasteFollowsEditsButDoesNotMoveCursor", complete(link) &&
          edit->toPlainText() == "xa " + link + " cdb" &&
          edit->textCursor().position() == 1);

    draft("left tail", 4);
    paste(clipboard);
    select(9, 5);
    check("responsePreservesBackwardSelection", complete(link) &&
          edit->toPlainText() == "left " + link + " tail" &&
          edit->textCursor().selectedText() == "tail" &&
          edit->textCursor().anchor() > edit->textCursor().position());

    draft("already typed ", 14);
    const int beforeDrop = host.requests;
    const int previousFailures = failed;
    host.drop = true;
    paste(clipboard);
    type("while waiting");
    const bool observedFailure = publiclogchecks::waitUntil(
        [&]{ return failed > previousFailures; }, 7000);
    check("consumedPostWithLostResponseHasNoReplay", observedFailure &&
          host.requests == beforeDrop + 1);
    check("dropPreservesBothDraftAndCaret",
          edit->toPlainText() == "already typed while waiting" && edit->textCursor().atEnd());
    host.drop = false;
    paste(clipboard);
    check("manualRetrySucceedsOnce", complete(link) &&
          host.requests == beforeDrop + 2 &&
          edit->toPlainText() == "already typed while waiting " + link + " ");

    const auto firstFile = files.filePath("one.png"), secondFile = files.filePath("two.png");
    const auto firstLink = link + "1", secondLink = link + "2";
    const bool saved = sample.save(firstFile) && sample.save(secondFile);
    QMimeData batch;
    batch.setUrls({QUrl::fromLocalFile(firstFile), QUrl::fromLocalFile(secondFile)});
    draft("pair", 4);
    const int beforeBatch = host.requests;
    paste(batch);
    type("caption");
    const bool firstOK = complete(firstLink);
    const bool secondOK = firstOK && complete(secondLink);
    check("queuedFilesKeepOrderAtOriginalAnchor", saved && secondOK &&
          edit->toPlainText() == "pair " + firstLink + " " + secondLink + " caption" &&
          host.requests == beforeBatch + 2);

    draft("previous", 8);
    paste(clipboard);
    const int beforeReplacement = failed;
    edit->clear();
    type("fresh draft");
    const bool acceptedReplacement = host.ready();
    if (acceptedReplacement)
        host.reply(link);
    check("clearedAndReusedEditorRejectsEarlierUpload", acceptedReplacement &&
          publiclogchecks::waitUntil([&]{ return failed > beforeReplacement; }, 5000) &&
          edit->toPlainText() == "fresh draft");

    // SplitInput reuses one document on normal draft changes (tested above).
    // Exercise the uploader's separate editor-rebinding contract without
    // replacing the document underneath unrelated SplitInput highlighters.
    QTextDocument oldDoc, newDoc;
    ResizingTextEdit rebound;
    rebound.setDocument(&oldDoc);
    rebound.setPlainText("old");
    rebound.moveCursor(QTextCursor::End);
    bool rejectedOldDocument = false;
    auto *uploader = getApp()->getImageUploader();
    holder.managedConnect(uploader->updated, [&](const ImageUploadUpdate &update) {
        if (update.textEdit == &rebound && update.kind == ImageUploadUpdate::Kind::Failed)
            rejectedOldDocument = true;
    });
    holder.managedConnect(rebound.imagePasted, [&](const QMimeData *mime) {
        auto [images, error] = uploader->getImages(mime);
        if (error.isEmpty() && !images.empty())
            uploader->upload(std::move(images), chat, &rebound);
    });
    (rebound.*&PasteAccess::insertFromMimeData)(&clipboard);
    rebound.setDocument(&newDoc);
    rebound.setPlainText("new");
    const bool gotRequest = host.ready();
    if (gotRequest)
        host.reply(link);
    check("responseNeverTargetsReplacementDocument", gotRequest &&
          publiclogchecks::waitUntil([&]{ return rejectedOldDocument; }, 5000) &&
          rebound.toPlainText() == "new" && oldDoc.toPlainText() == "old");

    check("noChatSends", chat->sent == 0);
    check("allRequestsMultipartPostWithPng", host.valid && host.requests > 0);
    checks.insert("postCount", host.requests);
    QFile report(reportPath);
    const bool written = report.open(QIODevice::WriteOnly) &&
                         report.write(QJsonDocument(checks).toJson()) > 0;
    NetworkManager::deinit();
    return passed && written ? 0 : 42;
}

}  // namespace chatterino::preview::uploadchecks
