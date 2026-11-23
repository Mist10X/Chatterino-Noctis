// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/ImageUploader.hpp"
#include "singletons/ImageUploadRouting.hpp"

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
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QImageReader>
#include <QPointer>
#include <QSaveFile>
#include <QUuid>

#include <utility>

namespace {

constexpr int UPLOAD_DELAY = 2000;
constexpr int UPLOAD_TIMEOUT_MS = 120000;

chatterino::NetworkRequest publicUploadRequest(const QUrl &url, chatterino::NetworkRequestType method)
{
    return chatterino::NetworkRequest(url, method).followRedirects(false)
        .attribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual)
        .attribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual)
        .attribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
}

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
    // Kappa uses a query parameter; Olrite uses a token in the path.
    const auto olriteToken = olriteDeletionToken(url);
    const auto key = url.query(QUrl::FullyEncoded);
    if (olriteToken.isEmpty() && (url.scheme() != QStringLiteral("https") ||
        url.path() != QStringLiteral("/delete") || key.isEmpty() ||
        key.contains('=') || key.contains('&')))
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
    if (!olriteToken.isEmpty())
    {
        api = this->olriteEndpoint_;
        api.setPath(QStringLiteral("/api/delete/") + olriteToken);
        api.setQuery(QString());
    }
    else
    {
        api.setPath(QStringLiteral("/api/delete"));
        api.setQuery(QStringLiteral("key=") + key);
    }
    auto finish = [this, imageKey, imageLink, done, url, olriteToken](const NetworkResult &result) {
        this->deletionsInFlight_.remove(imageKey);
        if (result.parseJson().value("success").toBool())
        {
            this->markDeleted(imageLink);
            imagepreview::forget(imageLink);
            done(DeleteResult::Deleted, {});
            return;
        }
        // If the host requires its browser confirmation page, retain the
        // deletion link and let the owner confirm there. Never mark it deleted
        // without the service's successful response.
        if (!olriteToken.isEmpty() &&
            (result.status() == 401 || result.status() == 403 || result.status() == 405) &&
            QDesktopServices::openUrl(url))
        {
            done(DeleteResult::OpenedInBrowser, {});
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
    auto request = olriteToken.isEmpty()
                       ? NetworkRequest(api)
                       : publicUploadRequest(api, NetworkRequestType::Post);
    if (!olriteToken.isEmpty())
    {
        request = std::move(request).header("Content-Type", "application/json")
                      .payload("{}").singleUseUpload().hideRequestBody();
    }
    std::move(request)
        .caller(this)
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

namespace {
struct UploadSettings {
    QString url, field, headers, linkPattern, deletePattern;
    bool fallbackEnabled;

    bool operator==(const UploadSettings &) const = default;
    bool useKappaFallback() const
    {
        return this->fallbackEnabled &&
               this->url == QStringLiteral("https://kappa.lol/api/upload") &&
               this->field == u"file" && this->headers.isEmpty() &&
               this->linkPattern == u"{link}" &&
               (this->deletePattern.isEmpty() || this->deletePattern == u"{delete}");
    }
};

UploadSettings uploadSettings()
{
    const auto *s = getSettings();
    auto defaultIfBlank = [](const auto &setting) {
        return setting.getValue().isEmpty() ? setting.getDefaultValue() : setting.getValue();
    };
    return {defaultIfBlank(s->imageUploaderUrl), defaultIfBlank(s->imageUploaderFormField),
            s->imageUploaderHeaders.getValue(), s->imageUploaderLink.getValue(),
            s->imageUploaderDeletionLink.getValue(), s->imageUploaderKappaFallback.getValue()};
}

}  // namespace

struct ImageUploader::PendingImage {
    RawImageData data;
    ChannelPtr channel;
    QPointer<ResizingTextEdit> edit;
    UploadSettings settings;
};

void ImageUploader::rememberKappaHealth(bool available)
{
    this->kappaAvailable_ = available;
    this->kappaHealthUntil_ = QDeadlineTimer(available ? 60000 : 30000);
}

void ImageUploader::uploadNextImage(ChannelPtr channel, QPointer<ResizingTextEdit> textEdit)
{
    auto image = std::make_shared<PendingImage>(PendingImage{
        std::move(this->uploadQueue_.front()), channel, textEdit, uploadSettings()});
    this->uploadQueue_.pop();
    this->updated.invoke({
        .kind = ImageUploadUpdate::Kind::Started,
        .textEdit = textEdit.data(),
        .imageData = image->data.data,
        .queued = this->uploadQueue_.size(),
    });
    if (!image->settings.useKappaFallback())
    {
        this->submitImage(image, false);
        return;
    }
    if (!this->kappaHealthUntil_.hasExpired())
    {
        this->submitImage(image, !this->kappaAvailable_);
        return;
    }
    // Only the current item has a pending probe. The remaining batch stays
    // queued and uses the cached result. No checks run while the uploader is idle.
    const auto checked = [this, image](const NetworkResult &result) {
        const int status = result.status().value_or(0);
        if ((status >= 200 && status < 300) || status == 404 || status == 405)
        {
            // A POST-only route may intentionally return 404/405 to HEAD.
            this->rememberKappaHealth(true);
            this->submitImage(image, false);
        }
        else if (hostUnavailable(result))
        {
            this->rememberKappaHealth(false);
            this->submitImage(image, true);
        }
        else
        {
            this->handleFailedUpload(result, QStringLiteral("kappa.lol"),
                                     image->channel, image->edit);
        }
    };
    publicUploadRequest(this->kappaEndpoint_, NetworkRequestType::Head)
        .caller(this).timeout(2500).onSuccess(checked).onError(checked).execute();
}

void ImageUploader::submitImage(const std::shared_ptr<PendingImage> &image,
                                bool secondary)
{
    const auto &config = image->settings;
    const bool managed = config.useKappaFallback();
    if (managed && (!getSettings()->imageUploaderEnabled ||
                    uploadSettings() != config || !image->edit ||
                    this->pastePosition_.document() != image->edit->document()))
    {
        this->updated.invoke({.kind = ImageUploadUpdate::Kind::Failed,
                              .textEdit = image->edit.data()});
        this->finishUpload();
        return;
    }
    const auto url = secondary ? this->olriteEndpoint_ :
        config.url == QStringLiteral("https://kappa.lol/api/upload")
            ? this->kappaEndpoint_ : QUrl(config.url);
    const auto &imageData = image->data;
    const QString formField = secondary ? QStringLiteral("file") : config.field;
    // Use the same non-replayable POST as chat sends. A host can store the
    // image and then close the connection before it sends back a link.
    const auto boundary = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    const auto field = formField.toUtf8().replace("\r", "%0D").replace("\n", "%0A")
                           .replace("\"", "%22");
    QByteArray body = "--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"" + field +
        "\"; filename=\"control_v." + imageData.format.toUtf8() +
        "\"\r\nContent-Type: " + imageData.mimeType.toUtf8() + "\r\n\r\n";
    body.reserve(body.size() + imageData.data.size() + boundary.size() + 8);
    body += imageData.data;
    body += "\r\n--" + boundary + "--\r\n";

    auto request = managed ? publicUploadRequest(url, NetworkRequestType::Post)
                           : NetworkRequest(url, NetworkRequestType::Post)
                                 .headerList(parseHeaderList(config.headers));
    if (secondary)
    {
        request = std::move(request).header("X-Olrite-Preserve", "true");
    }
    std::move(request)
        .header("Content-Type", "multipart/form-data; boundary=" + boundary)
        .payload(body)
        .singleUseUpload()
        .hideRequestBody()
        .caller(this)
        .timeout(UPLOAD_TIMEOUT_MS)
        .onSuccess(
            [this, image, secondary, managed, host = url.host()](NetworkResult result) {
                const auto status = result.status().value_or(0);
                if (managed && (status < 200 || status >= 300))
                {
                    this->handleFailedUpload(result, host, image->channel, image->edit);
                    return;
                }
                if (managed && !secondary)
                    this->rememberKappaHealth(true);
                const auto &config = image->settings;
                QString link;
                QString deletionLink;
                if (secondary)
                {
                    const auto response = result.parseJson();
                    if (!response.value("success").toBool(true))
                    {
                        this->handleFailedUpload(result, host, image->channel, image->edit);
                        return;
                    }
                    link = olriteImageLink(response.value("url").toString());
                    for (const auto &field : {"delete", "delete_url", "deleteUrl", "deletion_url"})
                    {
                        const auto candidate = olriteLink(response.value(field).toString());
                        if (!olriteDeletionToken(QUrl(candidate)).isEmpty())
                        {
                            deletionLink = candidate;
                            break;
                        }
                    }
                    if (!link.isEmpty() && deletionLink.isEmpty())
                    {
                        image->channel->addSystemMessage(
                            uiText("The backup service did not return a deletion link."));
                    }
                }
                else
                {
                    link = config.linkPattern.isEmpty() ? QString::fromUtf8(result.getData())
                                                        : getLinkFromResponse(result, config.linkPattern);
                    if (!config.deletePattern.isEmpty())
                        deletionLink = getLinkFromResponse(result, config.deletePattern);
                    if (managed)
                    {
                        link = builtInImageLink(link, QStringLiteral("kappa.lol"));
                        deletionLink = builtInImageLink(deletionLink, QStringLiteral("kappa.lol"));
                    }
                }
                this->handleSuccessfulUpload(link, deletionLink, *image);
            })
        .onError([this, image, secondary, managed, host = url.host()](NetworkResult result) {
            if (managed && !secondary && hostUnavailable(result))
            {
                this->rememberKappaHealth(false);
                if (failedBeforeUpload(result))
                {
                    this->submitImage(image, true);
                    return;
                }
            }
            this->handleFailedUpload(result, host, image->channel, image->edit);
        })
        .execute();
}

void ImageUploader::handleFailedUpload(const NetworkResult &result,
                                       const QString &host,
                                       ChannelPtr channel,
                                       QPointer<ResizingTextEdit> textEdit)
{
    this->updated.invoke({
        .kind = ImageUploadUpdate::Kind::Failed,
        .textEdit = textEdit.data(),
    });

    QString errorMessage;
    if (result.error() == QNetworkReply::HostNotFoundError)
    {
        errorMessage = host.isEmpty()
            ? uiText("Upload server not found. Check its address in the image uploader settings.")
            : uiText("Upload server %1 could not be found in DNS. Check its address in the image uploader settings.").arg(host);
    }
    else if (result.error() == QNetworkReply::RemoteHostClosedError ||
             result.error() == QNetworkReply::ContentReSendError)
    {
        errorMessage = uiText("The upload service did not confirm the upload. You can paste the image again.");
    }
    else
    {
        errorMessage = uiText("Could not upload the image (%1).").arg(result.formatError());
    }

    if (const auto obj = result.parseJson();
        result.error() != QNetworkReply::HostNotFoundError && !obj.isEmpty())
    {
        auto apiCode = obj.value("code");
        if (!apiCode.isUndefined())
        {
            auto codeString = apiCode.toVariant().toString();
            codeString.truncate(20);
            errorMessage += QStringLiteral(" [%1]").arg(codeString);
        }

        auto apiError = obj.value("error").toString();
        if (!apiError.isEmpty())
        {
            apiError.truncate(300);
            errorMessage += QStringLiteral(" %1").arg(apiError.trimmed());
        }
    }

    channel->addSystemMessage(errorMessage);

    this->finishUpload();
}

void ImageUploader::finishUpload()
{
    QObject::disconnect(this->draftChange_);
    this->pastePosition_ = {};
    this->uploadQueue_ = {};
    this->uploading_ = false;
}
void ImageUploader::handleSuccessfulUpload(QString link, QString deletionLink,
                                          const PendingImage &image)
{
    const auto &channel = image.channel;
    const auto &textEdit = image.edit;
    link = link.trimmed();
    deletionLink = deletionLink.trimmed();
    if (!validUploadLink(link))
    {
        this->updated.invoke({.kind = ImageUploadUpdate::Kind::Failed,
                              .textEdit = textEdit.data()});
        channel->addSystemMessage(uiText("The upload service returned an invalid image link."));
        this->finishUpload();
        return;
    }
    if (!validUploadLink(deletionLink))
    {
        deletionLink.clear();
    }
    this->logToFile(image.data.filePath, link, deletionLink, channel);
    if (!textEdit || this->pastePosition_.isNull() ||
        this->pastePosition_.document() != textEdit->document())
    {
        this->updated.invoke({.kind = ImageUploadUpdate::Kind::Failed,
                              .textEdit = textEdit.data()});
        channel->addSystemMessage(uiText("Uploaded image: %1").arg(link));
        this->finishUpload();
        return;
    }
    {
        auto &cursor = this->pastePosition_;
        // Typing at a stationary cursor can extend its selection. Insert at
        // the saved edge without replacing any of the newly typed caption.
        cursor.clearSelection();
        QString insertion = link;
        if (cursor.positionInBlock() > 0 &&
            !cursor.document()->characterAt(cursor.position() - 1).isSpace())
        {
            insertion.prepend(' ');
        }
        const bool hasSeparator = cursor.position() < cursor.document()->characterCount() - 1 &&
            cursor.document()->characterAt(cursor.position()).isSpace();
        const auto active = textEdit->textCursor();
        const bool caretAtPaste = !active.hasSelection() && active.position() == cursor.position();
        if (!hasSeparator)
            insertion.append(' ');
        cursor.setKeepPositionOnInsert(false);
        cursor.insertText(insertion);
        if (hasSeparator)
            cursor.movePosition(QTextCursor::NextCharacter);
        if (caretAtPaste)
            textEdit->setTextCursor(cursor);
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
        this->finishUpload();
    }
    else
    {
        QTimer::singleShot(UPLOAD_DELAY, this, [this, channel, textEdit, settings = image.settings]() {
            if (!getSettings()->imageUploaderEnabled || uploadSettings() != settings ||
                !textEdit || this->pastePosition_.document() != textEdit->document())
            {
                this->finishUpload();
                return;
            }
            this->uploadNextImage(channel, textEdit);
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
    if (this->uploading_)
    {
        channel->addSystemMessage(uiText("Please wait until the upload finishes."));
        return;
    }

    assert(!images.empty());
    assert(this->uploadQueue_.empty());

    this->uploading_ = true;
    std::swap(this->uploadQueue_, images);
    this->pastePosition_ = outputTextEdit ? outputTextEdit->textCursor() : QTextCursor{};
    if (!this->pastePosition_.isNull())
    {
        this->pastePosition_.setPosition(this->pastePosition_.selectionStart());
        this->pastePosition_.setKeepPositionOnInsert(true);
        auto *document = this->pastePosition_.document();
        this->draftChange_ = QObject::connect(
            document, &QTextDocument::contentsChange, this,
            [this, document, previousLength = document->characterCount()]
            (int position, int removed, int added) mutable {
                // setPlainText/clear replace the document's final paragraph,
                // even though the QTextDocument object itself stays the same.
                if (position == 0 && removed > 0 &&
                    (removed >= previousLength || (!added && document->isEmpty())))
                {
                    this->pastePosition_ = {};
                }
                previousLength = document->characterCount();
            });
    }

    this->uploadNextImage(std::move(channel), std::move(outputTextEdit));
}

}  // namespace chatterino
