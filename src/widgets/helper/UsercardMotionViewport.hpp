#pragma once

#include <QPointer>
#include <QResizeEvent>
#include <QWidget>

#include <algorithm>

namespace chatterino {

// The right side of a user card contains a full ChannelView or roles page.
// Resizing that page at every badge-animation frame synchronously
// recalculates its visible messages and scrollbar. Keep the page at the
// largest participating height during the short transition instead. Normal
// QWidget parent clipping reveals/crops real laid-out content as the outer
// card moves; no scaled screenshot, stale substitute, or global chat switch.
class UsercardMotionViewport final : public QWidget
{
public:
    explicit UsercardMotionViewport(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setObjectName(QStringLiteral("UsercardMotionViewport"));
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    // Own a single content widget just as an ordinary layout would. This is
    // set once when constructing the card; the contained QTabWidget remains
    // the same instance across all animations and tab changes.
    void setContent(QWidget *content)
    {
        Q_ASSERT(content != nullptr);
        Q_ASSERT(!this->content_ || this->content_ == content);
        if (!content || this->content_)
        {
            return;
        }
        this->content_ = content;
        content->setParent(this);
        this->setSizePolicy(content->sizePolicy());
        this->syncContentGeometry();
        content->show();
        this->updateGeometry();
    }

    // targetHeight is the *viewport's* final height, not the whole popup.
    // Retain the larger endpoint across a fast reversal: only revealing it
    // and cropping it should happen in resizeEvent, never repeated reflow.
    void beginMotion(int targetHeight, bool bottomAligned)
    {
        if (!this->content_)
        {
            return;
        }
        this->motionActive_ = true;
        this->bottomAligned_ = bottomAligned;
        this->motionHeight_ = std::max({this->motionHeight_, targetHeight,
                                       this->height(), this->content_->height()});
        this->syncContentGeometry();
    }

    // Restore ordinary live sizing on completion, cancellation, or manual
    // resize. Calling this repeatedly is harmless.
    void endMotion()
    {
        if (!this->motionActive_)
        {
            return;
        }
        this->motionActive_ = false;
        this->motionHeight_ = 0;
        this->syncContentGeometry();
        this->updateGeometry();
    }

    [[nodiscard]] bool motionActive() const noexcept
    {
        return this->motionActive_;
    }

    QSize sizeHint() const override
    {
        return this->content_ ? this->content_->sizeHint()
                              : QWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        return this->content_
                   ? this->content_->minimumSizeHint().expandedTo(
                         this->content_->minimumSize())
                   : QWidget::minimumSizeHint();
    }

    bool hasHeightForWidth() const override
    {
        return this->content_ && this->content_->hasHeightForWidth();
    }

    int heightForWidth(int width) const override
    {
        return this->content_ ? this->content_->heightForWidth(width) : -1;
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        this->syncContentGeometry();
    }

    bool event(QEvent *event) override
    {
        // The child's layout can acquire a new minimum size (e.g. the
        // roles page loads). Pass its updated hint on to the outer layout,
        // without invalidating the fixed-height clipping during motion.
        if (event->type() == QEvent::LayoutRequest)
        {
            if (!this->motionActive_)
            {
                this->updateGeometry();
            }
        }
        return QWidget::event(event);
    }

private:
    void syncContentGeometry()
    {
        if (!this->content_)
        {
            return;
        }
        if (this->motionActive_)
        {
            // A manually or asynchronously enlarged host may exceed its
            // prediction. Do not expose an empty strip; expand the endpoint
            // only when really necessary. Width changes always propagate.
            this->motionHeight_ = std::max(this->motionHeight_, this->height());
        }
        const auto height = this->motionActive_ ? this->motionHeight_
                                                : this->height();
        const auto size = QSize(this->width(), height)
                              .boundedTo(this->content_->maximumSize())
                              .expandedTo(this->content_->minimumSize());
        const auto y = this->motionActive_ && this->bottomAligned_
                           ? this->height() - size.height()
                           : 0;
        const QRect desired(QPoint(0, y), size);
        if (desired != this->content_->geometry())
        {
            this->content_->setGeometry(desired);
        }
    }

    QPointer<QWidget> content_;
    int motionHeight_ = 0;
    bool motionActive_ = false;
    bool bottomAligned_ = false;
};

}  // namespace chatterino
