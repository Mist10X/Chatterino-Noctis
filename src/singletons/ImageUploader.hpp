// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QDeadlineTimer>
#include <QHash>
#include <QMimeData>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringView>
#include <QTextCursor>
#include <QUrl>

#include <functional>
#include <memory>
#include <queue>

namespace chatterino {

class ResizingTextEdit;
class Channel;
class NetworkResult;
struct Message;
using ChannelPtr = std::shared_ptr<Channel>;

}  // namespace chatterino

namespace chatterino::imageuploader::detail {

QString getJSONValue(QJsonValue responseJson, QStringView jsonPattern);

QString getLinkFromResponse(const NetworkResult &response, QString pattern);

}  // namespace chatterino::imageuploader::detail

namespace chatterino {

struct RawImageData {
    QByteArray data;
    QString format;
    QString filePath;
    QString mimeType;
};

/// What the preview above the input shows while a picture is uploaded.
struct ImageUploadUpdate {
    enum class Kind { Started, Finished, Failed };

    Kind kind = Kind::Started;
    /// The input the link goes into.
    ResizingTextEdit *textEdit = nullptr;
    /// Started: the picture itself.
    QByteArray imageData;
    /// Finished.
    QString link;
    QString deletionLink;
    /// Started: pictures still waiting after this one.
    size_t queued = 0;
};

class ImageUploader final : public QObject
{
    friend class ImageUploaderTestAccess;
public:
    enum class DeleteResult { Deleted, OpenedInBrowser, Failed };

    std::pair<std::queue<RawImageData>, QString> getImages(
        const QMimeData *source) const;

    void upload(std::queue<RawImageData> images, ChannelPtr channel,
                QPointer<ResizingTextEdit> outputTextEdit);

    /// The deletion link saved for a picture uploaded from here (also in an
    /// earlier session), or empty.
    QString deletionLinkFor(const QString &imageLink);

    /// Public URLs uploaded from this installation and posted by a saved Twitch account.
    QStringList deletableImages(const Message &message);

    /// Deletes a picture uploaded from here. kappa.lol and olrite.lol
    /// are asked directly; for other sites the deletion page is opened in
    /// the browser. `done` gets an error text on failure (GUI thread).
    void deleteImage(const QString &imageLink,
                     std::function<void(DeleteResult, QString)> done);

    /// Upload progress for the preview above the input (GUI thread).
    pajlada::Signals::Signal<const ImageUploadUpdate &> updated;

private:
    QString logFilePath() const;
    void loadDeletionLinks();
    void markDeleted(const QString &imageLink);

    void uploadNextImage(ChannelPtr channel, QPointer<ResizingTextEdit> textEdit);
    struct PendingImage;
    void submitImage(const std::shared_ptr<PendingImage> &image, bool secondary);
    void rememberKappaHealth(bool available);

    void handleSuccessfulUpload(QString link, QString deletionLink,
                                const PendingImage &image);
    void handleFailedUpload(const NetworkResult &result, const QString &host,
                            ChannelPtr channel,
                            QPointer<ResizingTextEdit> textEdit);
    void finishUpload();

    void logToFile(const QString &originalFilePath, const QString &imageLink,
                   const QString &deletionLink, ChannelPtr channel);

    bool uploading_ = false;
    QUrl kappaEndpoint_{QStringLiteral("https://kappa.lol/api/upload")};
    QUrl olriteEndpoint_{QStringLiteral("https://olrite.lol/api/upload")};
    QDeadlineTimer kappaHealthUntil_{0};
    bool kappaAvailable_ = true;
    QTextCursor pastePosition_;
    QMetaObject::Connection draftChange_;
    std::queue<RawImageData> uploadQueue_;
    /// imagepreview::key of the picture -> its deletion link
    QHash<QString, QString> deletionLinks_;
    QSet<QString> deletionsInFlight_;
    bool deletionLinksLoaded_ = false;
};
}  // namespace chatterino
