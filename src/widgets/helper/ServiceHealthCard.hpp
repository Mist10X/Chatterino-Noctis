#pragma once

#include "common/UiLanguage.hpp"
#include "common/network/ServiceHealth.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/buttons/PixmapButton.hpp"

#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace chatterino {

class ServiceHealthCard final : public QWidget
{
public:
    explicit ServiceHealthCard(PixmapButton *anchor)
        : QWidget(anchor, Qt::Popup)
        , anchor_(anchor)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        auto *frame = new QFrame(this);
        frame->setObjectName("ServiceHealthCard");
        outer->addWidget(frame);
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(14, 10, 14, 10);
        layout->addWidget(new QLabel(uiText("Some services are unavailable"), frame));
        rows_ = new QLabel(frame);
        layout->addWidget(rows_);
        auto *hint = new QLabel(uiText("If your VPN is off, try turning it on."), frame);
        hint->setObjectName("NetworkHint");
        layout->addWidget(hint);
        auto *retry = new QPushButton(uiText("Check again"), frame);
        layout->addWidget(retry, 0, Qt::AlignLeft);
        connect(retry, &QPushButton::clicked, this, [] {
            for (auto service : {servicehealth::SevenTV, servicehealth::FFZ, servicehealth::History})
                if (servicehealth::entries[service].unavailable)
                    servicehealth::probe(service);
        });
        QPixmap icon(64, 64);
        icon.fill(Qt::transparent);
        QPainter painter(&icon);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#C9A566"), 5, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(1, 14, 54, 53), 48 * 16, 84 * 16);
        painter.drawArc(QRectF(11, 26, 34, 30), 45 * 16, 90 * 16);
        painter.drawPoint(QPointF(28, 45));
        painter.drawLine(52, 29, 52, 41);
        painter.drawPoint(52, 50);
        painter.end();
        anchor->setPixmap(icon);
        anchor->setAccessibleName(uiText("Service availability"));
        anchor->setToolTip(uiText("Service availability"));
        refresh();
    }

    void refresh()
    {
        QStringList rows;
        if (servicehealth::entries[servicehealth::SevenTV].unavailable)
            rows << uiText("7TV channel emotes");
        if (servicehealth::entries[servicehealth::FFZ].unavailable)
            rows << uiText("FrankerFaceZ channel emotes");
        if (servicehealth::entries[servicehealth::History].unavailable)
            rows << uiText("Recent chat history");
        rows_->setText(rows.join(u'\n'));
        anchor_->setVisible(!rows.isEmpty());
        if (rows.isEmpty()) hide();
        else if (isVisible()) position();
    }

    void open()
    {
        const auto *t = getTheme();
        setFont(getApp()->getFonts()->getFont(FontStyle::ChatMedium, anchor_->scale()));
        setStyleSheet(QStringLiteral(
            "QFrame#ServiceHealthCard {background:%1; border:1px solid %2; border-radius:10px;}"
            "QLabel {color:%3;} QLabel#NetworkHint {color:%4;}"
            "QPushButton {background:%5; color:%3; border:0; border-radius:5px; padding:4px 9px;}")
            .arg(t->messages.backgrounds.alternate.name(), t->tabs.dividerLine.name(),
                 t->messages.textColors.regular.name(), t->messages.textColors.system.name(),
                 t->tabs.selected.backgrounds.regular.name()));
        position();
        show();
    }

private:
    void position()
    {
        adjustSize();
        const auto area = anchor_->screen()->availableGeometry();
        const auto below = anchor_->mapToGlobal(anchor_->rect().bottomRight());
        move(qBound(area.left(), below.x() - width(), std::max(area.left(), area.right() - width())),
             below.y() + height() < area.bottom() ? below.y() + 3 :
                 std::max(area.top(), anchor_->mapToGlobal(QPoint{}).y() - height()));
    }

    PixmapButton *anchor_;
    QLabel *rows_;
};

}  // namespace chatterino
