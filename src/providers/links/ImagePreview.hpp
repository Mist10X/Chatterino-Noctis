// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <memory>

namespace chatterino {

class Image;
class LinkInfo;
using ImagePtr = std::shared_ptr<Image>;

/// Pictures under messages with a link to an image (a screenshot on
/// kappa.lol, a direct .png link, ...). The picture comes through the link
/// resolver's thumbnail proxy, so whoever posted the link never sees the
/// viewer's IP address.
namespace imagepreview {

/// The sites from the setting, e.g. "kappa.lol segs.lol" -> two hosts.
QStringList sites(const QString &setting);

/// Whether a link points at a picture worth showing under the message: an
/// image file on any site, or a page on one of the image sites.
bool isImageLink(const QString &url, const QStringList &sites);
bool isImageLink(const QString &url);

/// Same link written differently ("http://", "www.", "/" at the end, case
/// of the host) gives the same key.
QString key(const QString &url);

/// The higher-resolution local upload, if still cached.
ImagePtr cachedUpload(const QString &url);

/// Return a locally prepared or resolved thumbnail, starting one shared
/// metadata request on demand. Called from layout on the GUI thread.
ImagePtr getOrRequest(LinkInfo &info);

/// The picture was deleted from its site: no preview anywhere any more.
void forget(const QString &url);
bool wasDeleted(const QString &url);

/// Keep the thumbnail of a newly uploaded picture for its chat preview.
void rememberUpload(const QString &url, ImagePtr thumbnail);

}  // namespace imagepreview

}  // namespace chatterino
