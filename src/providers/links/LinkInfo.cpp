// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/LinkInfo.hpp"
#include "providers/links/ImagePreview.hpp"

#include "debug/AssertInGuiThread.hpp"

#include <QString>

namespace chatterino {

LinkInfo::LinkInfo(QString url)
    : QObject(nullptr)
    , originalUrl_(url)
    , url_(std::move(url))
    , tooltip_(this->url_)
{
}

LinkInfo::~LinkInfo() = default;

LinkInfo::State LinkInfo::state() const
{
    return this->state_;
}

QString LinkInfo::url() const
{
    return this->url_;
}

QString LinkInfo::originalUrl() const
{
    return this->originalUrl_;
}

bool LinkInfo::isPending() const
{
    return this->state_ == State::Created;
}

bool LinkInfo::canResolve() const
{
    return this->isPending() || (this->hasError() && this->retryAfter_.hasExpired());
}

int LinkInfo::retryDelay() const
{
    return static_cast<int>(this->retryAfter_.remainingTime());
}

bool LinkInfo::isTransientFailure() const
{
    return this->hasError() && this->transientFailure_;
}

void LinkInfo::deferRetry(qint64 delayMs, bool transient)
{
    this->transientFailure_ = transient;
    this->retryAfter_.setRemainingTime(delayMs);
}

bool LinkInfo::isLoading() const
{
    return this->state_ == State::Loading;
}

bool LinkInfo::isLoaded() const
{
    return this->state_ > State::Loading;
}

bool LinkInfo::isResolved() const
{
    return this->state_ == State::Resolved;
}

bool LinkInfo::hasError() const
{
    return this->state_ == State::Errored;
}

bool LinkInfo::hasThumbnail() const
{
    return this->thumbnail_ && !this->thumbnail_->url().string.isEmpty() &&
           !imagepreview::wasDeleted(this->originalUrl_);
}

QString LinkInfo::tooltip() const
{
    return this->tooltip_;
}

ImagePtr LinkInfo::thumbnail() const
{
    return this->thumbnail_;
}

void LinkInfo::setState(State state)
{
    assertInGuiThread();
    assert(state >= this->state_ ||
           (this->hasError() && state == State::Loading));

    if (this->state_ == state)
    {
        return;
    }

    this->state_ = state;
    this->stateChanged(state);
}

void LinkInfo::setResolvedUrl(QString resolvedUrl)
{
    assertInGuiThread();
    this->url_ = std::move(resolvedUrl);
}

void LinkInfo::setTooltip(QString tooltip)
{
    assertInGuiThread();
    this->tooltip_ = std::move(tooltip);
}

void LinkInfo::setThumbnail(ImagePtr thumbnail)
{
    assertInGuiThread();
    this->thumbnail_ = std::move(thumbnail);
}

}  // namespace chatterino
