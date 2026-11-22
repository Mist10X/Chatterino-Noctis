#pragma once

#include <QCursor>
#include <QMouseEvent>
#include <QRegion>
#include <QWidget>

#include <algorithm>

namespace chatterino {

// An invisible mouse-sensitive ring around the user card. Unlike a corner-only
// QSizeGrip, it leaves the entire interior available to the card's controls.
class UsercardResizeGrip final : public QWidget
{
public:
    explicit UsercardResizeGrip(QWidget *window)
        : QWidget(window)
        , window_(window)
    {
        Q_ASSERT(window);
        this->setObjectName(QStringLiteral("UsercardResizeGrip"));
        this->setAttribute(Qt::WA_NoSystemBackground);
        this->setAutoFillBackground(false);
        this->setMouseTracking(true);
        this->setFocusPolicy(Qt::NoFocus);
        this->syncGeometry(1.F);
        this->show();
    }

    static int edgeBand(float scale)
    {
        return std::max(5, qRound(5.F * scale));
    }

    void syncGeometry(float scale, int bodyInset = 0)
    {
        const auto band = edgeBand(scale) + bodyInset;
        if (this->geometry() == this->window_->rect() && this->band_ == band && !this->mask().isEmpty())
            return;
        this->band_ = band;
        this->setGeometry(this->window_->rect());
        const auto interior = this->rect().adjusted(this->band_, this->band_,
                                                   -this->band_, -this->band_);
        auto ring = QRegion(this->rect());
        if (interior.isValid())
        {
            ring -= QRegion(interior);
        }
        if (this->mask() != ring)
        {
            this->setMask(ring);
        }
        this->raise();
    }

    [[nodiscard]] bool isResizing() const
    {
        return this->activeEdges_ != Qt::Edges{};
    }

    [[nodiscard]] static Qt::Edges edgesAt(QPoint point, const QRect &rect,
                                           int band)
    {
        Qt::Edges edges;
        if (!rect.contains(point) || band <= 0)
        {
            return edges;
        }

        if (point.x() < rect.left() + band)
        {
            edges |= Qt::LeftEdge;
        }
        else if (point.x() > rect.right() - band)
        {
            edges |= Qt::RightEdge;
        }

        if (point.y() < rect.top() + band)
        {
            edges |= Qt::TopEdge;
        }
        else if (point.y() > rect.bottom() - band)
        {
            edges |= Qt::BottomEdge;
        }
        return edges;
    }

    [[nodiscard]] static QRect resizedGeometry(QRect original, Qt::Edges edges,
                                               QPoint delta, QSize minimum,
                                               QSize maximum)
    {
        const auto minWidth = std::max(1, minimum.width());
        const auto minHeight = std::max(1, minimum.height());
        const auto maxWidth = std::max(minWidth, maximum.width());
        const auto maxHeight = std::max(minHeight, maximum.height());
        auto width = original.width();
        auto height = original.height();

        if (edges.testFlag(Qt::LeftEdge))
        {
            width = std::clamp(width - delta.x(), minWidth, maxWidth);
        }
        else if (edges.testFlag(Qt::RightEdge))
        {
            width = std::clamp(width + delta.x(), minWidth, maxWidth);
        }
        if (edges.testFlag(Qt::TopEdge))
        {
            height = std::clamp(height - delta.y(), minHeight, maxHeight);
        }
        else if (edges.testFlag(Qt::BottomEdge))
        {
            height = std::clamp(height + delta.y(), minHeight, maxHeight);
        }

        const auto x = edges.testFlag(Qt::LeftEdge)
                           ? original.x() + original.width() - width
                           : original.x();
        const auto y = edges.testFlag(Qt::TopEdge)
                           ? original.y() + original.height() - height
                           : original.y();
        return {x, y, width, height};
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            const auto edges = edgesAt(event->position().toPoint(), this->rect(),
                                       this->band_);
            if (edges != Qt::Edges{})
            {
                this->activeEdges_ = edges;
                this->originalGeometry_ = this->window_->geometry();
                this->globalOrigin_ = event->globalPosition();
                this->updateCursor(edges);
                event->accept();
                return;
            }
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (this->isResizing())
        {
            const auto delta = (event->globalPosition() - this->globalOrigin_)
                                   .toPoint();
            this->window_->setGeometry(resizedGeometry(
                this->originalGeometry_, this->activeEdges_, delta,
                this->window_->minimumSize(), this->window_->maximumSize()));
            event->accept();
            return;
        }

        this->updateCursor(edgesAt(event->position().toPoint(), this->rect(),
                                   this->band_));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->isResizing())
        {
            this->activeEdges_ = {};
            this->updateCursor(edgesAt(event->position().toPoint(), this->rect(),
                                       this->band_));
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (!this->isResizing())
        {
            this->unsetCursor();
        }
        QWidget::leaveEvent(event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)
        {
            this->activeEdges_ = {};
            this->unsetCursor();
        }
        return QWidget::event(event);
    }

private:
    void updateCursor(Qt::Edges edges)
    {
        const auto left = edges.testFlag(Qt::LeftEdge);
        const auto right = edges.testFlag(Qt::RightEdge);
        const auto top = edges.testFlag(Qt::TopEdge);
        const auto bottom = edges.testFlag(Qt::BottomEdge);

        if ((left && top) || (right && bottom))
        {
            this->setCursor(Qt::SizeFDiagCursor);
        }
        else if ((right && top) || (left && bottom))
        {
            this->setCursor(Qt::SizeBDiagCursor);
        }
        else if (left || right)
        {
            this->setCursor(Qt::SizeHorCursor);
        }
        else if (top || bottom)
        {
            this->setCursor(Qt::SizeVerCursor);
        }
        else
        {
            this->unsetCursor();
        }
    }

    QWidget *window_;
    int band_ = 5;
    Qt::Edges activeEdges_;
    QRect originalGeometry_;
    QPointF globalOrigin_;
};

}  // namespace chatterino
