// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/ImageUploader.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Env.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "debug/Benchmark.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/links/ImagePreview.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"

#include <QBuffer>
#include <QDesktopServices>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QPointer>
#include <QSaveFile>

#include <utility>

namespace {

constexpr int UPLOAD_DELAY = 2000;
constexpr int UPLOAD_TIMEOUT_MS = 120000;

bool validUploadLink(const QString &link)
{
    const QUrl url(link, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           (url.scheme() == u"https" || url.scheme() == u"http");
}

std::optional<QByteArray> convertToPng(const QImage &image)
{
    QByteArray imageData;
    QBuffer buf(&imageData);
    buf.open(QIODevice::WriteOnly);
    bool success = image.save(&buf, "png");
    if (success)
    {
        return imageData;
    }

    return std::nullopt;
}

}  // namespace

namespace chatterino::imageuploader::detail {

QString getJSONValue(QJsonValue responseJson, QStringView jsonPattern)
{
    for (auto key : jsonPattern.tokenize(u'.'))
    {
        if (responseJson.isObject())
        {
            responseJson = responseJson[key];
        }
        else if (responseJson.isArray())
        {
            bool ok = false;
            auto idx = key.toLongLong(&ok);
            if (ok)
            {
                responseJson = responseJson[idx];
            }
        }
        else
        {
            break;
        }
    }
    return responseJson.toString();
}

QString getLinkFromResponse(const NetworkResult &response, QString pattern)
{
    static const QRegularExpression regExp("{(.+)}",
        QRegularExpression::InvertedGreedinessOption);
    const auto jsonRoot = response.parseJsonValue();
    QString output;
    qsizetype last = 0;
    auto matches = regExp.globalMatch(pattern);
    while (matches.hasNext())
    {
        const auto match = matches.next();
        output += QStringView(pattern).sliced(last, match.capturedStart() - last);
        output += getJSONValue(jsonRoot, match.capturedView(1));
        last = match.capturedEnd();
    }
    output += QStringView(pattern).sliced(last);
    return output;
}

}  // namespace chatterino::imageuploader::detail

namespace chatterino {

using namespace imageuploader::detail;

QString ImageUploader::logFilePath() const
{
    return combinePath(
        getApp()->getPaths().messageLogsIn(getSettings()->logPath.getValue()),
        "ImageUploader.json");
}

void ImageUploader::loadDeletionLinks()
{
    if (this->deletionLinksLoaded_)
    {
        return;
    }
    this->deletionLinksLoaded_ = true;
    QFile file(this->logFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    const auto entries = QJsonDocument::fromJson(file.readAll()).array();
    QHash<QString, QString> deleted;
    for (const auto &value : entries)
    {
        const auto entry = value.toObject();
        const auto link = entry.value("imageLink").toString();
        const auto deletionLink = entry.value("deletionLink").toString();
        if (link.isEmpty())
        {
            continue;
        }
        const auto key = imagepreview::key(link);
        if (entry.value("deleted").toBool())
        {
            this->deletionLinks_.remove(key);
            deleted.insert(key, link);
        }
        else
        {
            deleted.remove(key);
            if (!deletionLink.isEmpty())
            {
                this->deletionLinks_.insert(key, deletionLink);
            }
            else
            {
                this->deletionLinks_.remove(key);
            }
        }
    }
    for (const auto &url : deleted)
    {
        imagepreview::forget(url);
    }
}

QString ImageUploader::deletionLinkFor(const QString &imageLink)
{
    this->loadDeletionLinks();
    return this->deletionLinks_.value(imagepreview::key(imageLink));
}

QStringList ImageUploader::deletableImages(const Message &message)
{
    if (message.platform != MessagePlatform::AnyOrTwitch ||
        message.loginName.isEmpty() || message.flags.has(MessageFlag::System))
    {
        return {};
    }
    const auto account =
        getApp()->getAccounts()->twitch.findUserByUsername(message.loginName);
    if (!account || account->isAnon() ||
        (!message.userID.isEmpty() && account->getUserId() != message.userID))
    {
        return {};
    }
    QStringList urls;
    QSet<QString> keys;
    for (const auto &element : message.elements)
    {
        auto *link = dynamic_cast<LinkElement *>(element.get());
        if (!link || link->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }
        const auto url = link->linkInfo()->originalUrl();
        const auto key = imagepreview::key(url);
        if (!keys.contains(key) && !this->deletionLinkFor(url).isEmpty())
        {
            keys.insert(key);
            urls.append(url);
        }
    }
    return urls;
}

void ImageUploader::markDeleted(const QString &imageLink)
{
    const auto key = imagepreview::key(imageLink);
    this->deletionLinks_.remove(key);

    const auto path = this->logFilePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    auto entries = QJsonDocument::fromJson(file.readAll()).array();
    file.close();
    for (qsizetype i = 0; i < entries.size(); ++i)
    {
        auto entry = entries[i].toObject();
        if (imagepreview::key(entry.value("imageLink").toString()) == key)
        {
            entry["deleted"] = true;
            entries[i] = entry;
        }
    }
    QSaveFile save(path);
    if (save.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        save.write(QJsonDocument(entries).toJson());
        save.commit();
    }
}

void ImageUploader::deleteImage(const QString &imageLink,
                                std::function<void(DeleteResult, QString)> done)
{
    const auto deletionLink = this->deletionLinkFor(imageLink);
    if (deletionLink.isEmpty())
    {
        done(DeleteResult::Failed,
             uiText("No deletion link was saved for this picture"));
        return;
    }
    const QUrl url(deletionLink);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.scheme() != u"https" && url.scheme() != u"http"))
    {
        done(DeleteResult::Failed, uiText("Invalid deletion link"));
        return;
    }
    // kappa.lol, segs.lol, ...: the page /delete?KEY asks /api/delete?key=KEY
    const auto key = url.query(QUrl::FullyEncoded);
    if (url.scheme() != QStringLiteral("https") ||
        url.path() != QStringLiteral("/delete") || key.isEmpty() ||
        key.contains('=') || key.contains('&'))
    {
        if (QDesktopServices::openUrl(url))
        {
            done(DeleteResult::OpenedInBrowser, {});
        }
        else
        {
            done(DeleteResult::Failed, uiText("Could not open the deletion page"));
        }
        return;
    }
    const auto imageKey = imagepreview::key(imageLink);
    if (this->deletionsInFlight_.contains(imageKey))
    {
        done(DeleteResult::Failed, uiText("Deletion already in progress"));
        return;
    }
    this->deletionsInFlight_.insert(imageKey);
    QUrl api(url);
    api.setPath(QStringLiteral("/api/delete"));
    api.setQuery(QStringLiteral("key=") + key);
    auto finish = [this, imageKey, imageLink, done](const NetworkResult &result) {
        this->deletionsInFlight_.remove(imageKey);
        if (result.parseJson().value("success").toBool())
        {
            this->markDeleted(imageLink);
            imagepreview::forget(imageLink);
            done(DeleteResult::Deleted, {});
            return;
        }
        if (result.status() == 400 || result.status() == 404)
        {
            done(DeleteResult::Failed,
                 uiText("The site does not have this picture any more"));
            return;
        }
        done(DeleteResult::Failed, result.formatError());
    };
    NetworkRequest(api)
        .timeout(10000)
        .onSuccess(finish)
        .onError(finish)
        .execute();
}

void ImageUploader::logToFile(const QString &originalFilePath,
                              const QString &imageLink,
                              const QString &deletionLink, ChannelPtr channel)
{
    const QString logFileName = this->logFilePath();
    if (!deletionLink.isEmpty())
    {
        this->loadDeletionLinks();
        this->deletionLinks_.insert(imagepreview::key(imageLink),
                                    deletionLink);
    }

    QFile logReadFile(logFileName);
    bool isLogFileOkay =
        logReadFile.open(QIODevice::ReadWrite | QIODevice::Text);
    if (!isLogFileOkay)
    {
        channel->addSystemMessage(
            QString("Failed to open log file with links at ") + logFileName);
        return;
    }
    auto logs = logReadFile.readAll();
    if (logs.isEmpty())
    {
        logs = QJsonDocument(QJsonArray()).toJson();
    }
    logReadFile.close();

    QJsonObject newLogEntry;
    newLogEntry["channelName"] = channel->getName();
    newLogEntry["deletionLink"] =
        deletionLink.isEmpty() ? QJsonValue(QJsonValue::Null) : deletionLink;
    newLogEntry["imageLink"] = imageLink;
    newLogEntry["localPath"] = originalFilePath.isEmpty()
                                   ? QJsonValue(QJsonValue::Null)
                                   : originalFilePath;
    newLogEntry["timestamp"] = QDateTime::currentSecsSinceEpoch();

    QSaveFile logSaveFile(logFileName);
    if (!logSaveFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        qCDebug(chatterinoImageuploader)
            << "Failed to open log file" << logSaveFile.errorString();
        return;
    }
    QJsonArray entries = QJsonDocument::fromJson(logs).array();
    entries.push_back(newLogEntry);
    logSaveFile.write(QJsonDocument(entries).toJson());
    logSaveFile.commit();
}

void ImageUploader::sendImageUploadRequest(RawImageData imageData,
                                           ChannelPtr channel,
                                           QPointer<ResizingTextEdit> textEdit)
{
    QUrl url(getSettings()->imageUploaderUrl.getValue().isEmpty()
                 ? getSettings()->imageUploaderUrl.getDefaultValue()
                 : getSettings()->imageUploaderUrl);
    QString formField(
        getSettings()->imageUploaderFormField.getValue().isEmpty()
            ? getSettings()->imageUploaderFormField.getDefaultValue()
            : getSettings()->imageUploaderFormField);
    auto extraHeaders =
        parseHeaderList(getSettings()->imageUploaderHeaders.getValue());
    QString originalFilePath = imageData.filePath;
    this->updated.invoke({
        .kind = ImageUploadUpdate::Kind::Started,
        .textEdit = textEdit.data(),
        .imageData = imageData.data,
        .queued = this->uploadQueue_.size() > 0 ? this->uploadQueue_.size() - 1
                                                : 0,
    });

    QHttpMultiPart *payload = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part = QHttpPart();
    part.setBody(imageData.data);
    part.setHeader(QNetworkRequest::ContentTypeHeader, imageData.mimeType);
    part.setHeader(QNetworkRequest::ContentLengthHeader,
                   QVariant(imageData.data.length()));
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QString(R"(form-data; name="%1"; filename="control_v.%2")")
                       .arg(formField)
                       .arg(imageData.format));
    payload->append(part);

    NetworkRequest(url, NetworkRequestType::Post)
        .headerList(extraHeaders)
        .multiPart(payload)
        .timeout(UPLOAD_TIMEOUT_MS)
        .onSuccess(
            [textEdit, channel, originalFilePath, this](NetworkResult result) {
                this->handleSuccessfulUpload(result, originalFilePath, channel,
                                             textEdit);
            })
        .onError([channel, textEdit, this](NetworkResult result) -> bool {
            this->handleFailedUpload(result, channel, textEdit);
            return true;
        })
        .execute();
}

void ImageUploader::handleFailedUpload(const NetworkResult &result,
                                       ChannelPtr channel,
                                       QPointer<ResizingTextEdit> textEdit)
{
    this->updated.invoke({
        .kind = ImageUploadUpdate::Kind::Failed,
        .textEdit = textEdit.data(),
    });

    auto errorMessage =
        QString("An error happened while uploading your image: %1")
            .arg(result.formatError());

    auto obj = result.parseJson();
    if (!obj.isEmpty())
    {
        auto apiCode = obj.value("code");
        if (!apiCode.isUndefined())
        {
            auto codeString = apiCode.toVariant().toString();
            codeString.truncate(20);
            errorMessage += QString(" - code: %1").arg(codeString);
        }

        auto apiError = obj.value("error").toString();
        if (!apiError.isEmpty())
        {
            apiError.truncate(300);
            errorMessage += QString(" - error: %1").arg(apiError.trimmed());
        }
    }

    channel->addSystemMessage(errorMessage);

    while (!this->uploadQueue_.empty())
    {
        this->uploadQueue_.pop();
    }
    this->uploadMutex_.unlock();
}

void ImageUploader::handleSuccessfulUpload(const NetworkResult &result,
                                           QString originalFilePath,
                                           ChannelPtr channel,
                                           QPointer<ResizingTextEdit> textEdit)
{
    QString link =
        getSettings()->imageUploaderLink.getValue().isEmpty()
            ? result.getData()
            : getLinkFromResponse(result, getSettings()->imageUploaderLink);
    QString deletionLink =
        getSettings()->imageUploaderDeletionLink.getValue().isEmpty()
            ? ""
            : getLinkFromResponse(result,
                                  getSettings()->imageUploaderDeletionLink);
    link = link.trimmed();
    deletionLink = deletionLink.trimmed();
    if (!validUploadLink(link))
    {
        this->updated.invoke({.kind = ImageUploadUpdate::Kind::Failed,
                              .textEdit = textEdit.data()});
        channel->addSystemMessage(uiText("The upload service returned an invalid image link."));
        this->uploadQueue_ = {};
        this->uploadMutex_.unlock();
        return;
    }
    if (!validUploadLink(deletionLink))
    {
        deletionLink.clear();
    }
    this->logToFile(originalFilePath, link, deletionLink, channel);
    if (textEdit == nullptr)
    {
        while (!this->uploadQueue_.empty())
        {
            this->uploadQueue_.pop();
        }
        this->uploadMutex_.unlock();
        return;
    }
    if (this->pastePosition_.document() == textEdit->document() && !link.isEmpty())
    {
        auto &cursor = this->pastePosition_;
        // Typing at a stationary cursor can extend its selection. Insert at
        // the saved edge without replacing any of the newly typed caption.
        cursor.clearSelection();
        QString insertion = link + ' ';
        if (cursor.positionInBlock() > 0 &&
            !cursor.document()->characterAt(cursor.position() - 1).isSpace())
        {
            insertion.prepend(' ');
        }
        cursor.setKeepPositionOnInsert(false);
        cursor.insertText(insertion);
        cursor.setKeepPositionOnInsert(true);
    }
    // The preview above the input shows the picture, the link and a delete
    // button; the chat gets no "uploaded" message any more.
    this->updated.invoke({
        .kind = ImageUploadUpdate::Kind::Finished,
        .textEdit = textEdit.data(),
        .link = link,
        .deletionLink = deletionLink,
    });
    if (this->uploadQueue_.empty())
    {
        this->uploadMutex_.unlock();
    }
    else
    {
        QTimer::singleShot(UPLOAD_DELAY, [channel, textEdit, this]() {
            this->sendImageUploadRequest(this->uploadQueue_.front(), channel,
                                         textEdit);
            this->uploadQueue_.pop();
        });
    }
}

std::pair<std::queue<RawImageData>, QString> ImageUploader::getImages(
    const QMimeData *source) const
{
    BenchmarkGuard benchmarkGuard("ImageUploader::getImages");

    auto tryUploadFromUrls =
        [&]() -> std::pair<std::queue<RawImageData>, QString> {
        if (!source->hasUrls())
        {
            return {{}, {}};
        }

        std::queue<RawImageData> images;

        QStringList skipped;
        for (const QUrl &url : source->urls())
        {
            const QString localPath = url.toLocalFile();
            if (localPath.isEmpty())
            {
                continue;
            }
            QFile file(localPath);
            if (file.open(QIODevice::ReadOnly))
            {
                QImageReader reader(&file);
                reader.setDecideFormatFromContent(true);
                const auto format = reader.format().toLower();
                const auto image = reader.read();
                if (!image.isNull())
                {
                    if (format == "gif" && file.seek(0))
                    {
                        images.push({file.readAll(), "gif", localPath, "image/gif"});
                        continue;
                    }
                    if (auto png = convertToPng(image))
                    {
                        images.push({std::move(*png), "png", localPath, "image/png"});
                        continue;
                    }
                }
            }
            skipped.append(QFileInfo(localPath).fileName());
        }
        return {std::move(images),
                skipped.isEmpty() ? QString() :
                    uiText("Skipped files that could not be read as images: %1")
                        .arg(skipped.join(QStringLiteral(", ")))};
    };

    auto tryUploadDirectly =
        [&]() -> std::pair<std::queue<RawImageData>, QString> {
        std::queue<RawImageData> images;

        for (const auto &mime : {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                                 QStringLiteral("image/gif")})
        {
            if (!source->hasFormat(mime))
            {
                continue;
            }
            auto bytes = source->data(mime);
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            reader.setDecideFormatFromContent(true);
            const auto format = reader.format().toLower();
            if (QString::fromLatin1(format) == mime.sliced(6) && !reader.read().isNull())
            {
                images.push({std::move(bytes), QString::fromLatin1(format), {}, mime});
                return {std::move(images), {}};
            }
        }

        auto image = qvariant_cast<QImage>(source->imageData());
        auto imageData = convertToPng(image);
        if (imageData)
        {
            images.push({*imageData, "png", "", "image/png"});
            return {images, {}};
        }

        return {{}, "Cannot upload file, failed to convert to png."};
    };

    const auto [urlImageData, urlError] = tryUploadFromUrls();

    if (!urlImageData.empty() || !urlError.isEmpty())
    {
        return {urlImageData, urlError};
    }

    const auto [directImageData, directError] = tryUploadDirectly();
    if (!directImageData.empty())
    {
        return {directImageData, {}};
    }

    return {
        {},

        urlError + directError,
    };
}

void ImageUploader::upload(std::queue<RawImageData> images, ChannelPtr channel,
                           QPointer<ResizingTextEdit> outputTextEdit)
{
    BenchmarkGuard benchmarkGuard("upload");
    if (!this->uploadMutex_.tryLock())
    {
        channel->addSystemMessage("Please wait until the upload finishes.");
        return;
    }

    assert(!images.empty());
    assert(this->uploadQueue_.empty());

    std::swap(this->uploadQueue_, images);
    this->pastePosition_ = outputTextEdit ? outputTextEdit->textCursor() : QTextCursor{};
    if (!this->pastePosition_.isNull())
    {
        this->pastePosition_.setPosition(this->pastePosition_.selectionStart());
        this->pastePosition_.setKeepPositionOnInsert(true);
    }

    this->sendImageUploadRequest(this->uploadQueue_.front(), std::move(channel),
                                 std::move(outputTextEdit));
    this->uploadQueue_.pop();
}

}  // namespace chatterino
