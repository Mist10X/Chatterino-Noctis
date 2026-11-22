// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/UpdateBanner.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "common/UiScale.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/Updates.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontMetricsF>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

namespace {

/// The version whose card was closed: only until the next start, so an
/// update is not forgotten for good by one click on the cross.
QString &dismissed()
{
    static QString version;
    return version;
}

QString dismissedVersion()
{
    return dismissed();
}

void setDismissedVersion(const QString &version)
{
    dismissed() = version;
}

class Banner final : public QWidget
{
public:
    explicit Banner(QWidget *window)
        : QWidget(window)
        , window_(window)
    {
        this->setMouseTracking(true);
        this->setCursor(Qt::PointingHandCursor);
        window->installEventFilter(this);
    }

    float scale() const
    {
        return contentUiScale(getSettings()->getClampedUiScale());
    }

    QSize sizeHint() const override
    {
        const auto s = this->scale();
        const int available = this->window_->width() - int(24 * s);
        return {std::min(int(420 * s), std::max(int(220 * s), available)),
                int(58 * s)};
    }

    void place()
    {
        const auto s = this->scale();
        const auto size = this->sizeHint();
        this->setGeometry((this->window_->width() - size.width()) / 2,
                          int(36 * s), size.width(), size.height());
        this->raise();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->window_ && event->type() == QEvent::Resize &&
            this->isVisible())
        {
            this->place();
        }
        return false;
    }

    void paintEvent(QPaintEvent * /*event*/) override
    {
        const auto s = this->scale();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool light = getTheme()->isLightTheme();
        const auto accent = getTheme()->accent;
        painter.setPen(QPen(accent.darker(light ? 110 : 160), 1));
        painter.setBrush(QColor(
            light ? "#f4f0fb" : (this->hovered_ ? "#2a2535" : "#221f2b")));
        QPainterPath body;
        body.addRoundedRect(card, 8 * s, 8 * s);
        painter.drawPath(body);
        QPainterPath strip;
        strip.addRoundedRect(
            QRectF(card.left(), card.top(), 4 * s, card.height()), 2 * s,
            2 * s);
        // The accent must follow the real outer silhouette: its smaller
        // radius alone would extend beyond the card in both left corners.
        painter.fillPath(body.intersected(strip), accent);

        auto *fonts = getApp()->getFonts();
        const qreal pad = 12 * s;
        const qreal closeSize = 16 * s;
        const qreal textWidth = card.width() - 2 * pad - closeSize;
        const auto bold = fonts->getFont(FontStyle::ChatMediumBold, s);
        const auto normal = fonts->getFont(FontStyle::ChatMedium, s);
        const QFontMetricsF boldMetrics(bold);
        const QFontMetricsF normalMetrics(normal);

        const qreal top = card.top() + 8 * s;
        painter.setFont(bold);
        painter.setPen(light ? QColor("#3c2a63") : QColor("#f0ecfa"));
        painter.drawText(
            QPointF(card.left() + pad, top + boldMetrics.ascent()),
            boldMetrics.elidedText(
                uiText("Update %1 is available")
                    .arg(getApp()->getUpdates().getOnlineVersion()),
                Qt::ElideRight, textWidth));
        painter.setFont(normal);
        painter.setPen(light ? QColor("#555") : QColor("#b6b2c2"));
        const qreal second = top + boldMetrics.height() + 3 * s;
        painter.drawText(
            QPointF(card.left() + pad, second + normalMetrics.ascent()),
            normalMetrics.elidedText(
                uiText("Click to see what is new and update"), Qt::ElideRight,
                textWidth));

        const QRectF cross(card.right() - pad - closeSize * 0.7,
                           card.top() + 9 * s, closeSize * 0.7,
                           closeSize * 0.7);
        painter.setPen(
            QPen(light ? QColor("#777") : QColor("#9a96a6"), 1.4 * s));
        painter.drawLine(cross.topLeft(), cross.bottomRight());
        painter.drawLine(cross.topRight(), cross.bottomLeft());
        this->closeRect_ = cross.adjusted(-6 * s, -6 * s, 6 * s, 6 * s);
    }

    void enterEvent(QEnterEvent * /*event*/) override
    {
        this->hovered_ = true;
        this->update();
    }

    void leaveEvent(QEvent * /*event*/) override
    {
        this->hovered_ = false;
        this->update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            return;
        }
        // Closed: not again for this version (the title bar button stays).
        setDismissedVersion(getApp()->getUpdates().getOnlineVersion());
        this->hide();
        if (!this->closeRect_.contains(event->position()))
        {
            showUpdatePrompt(this->window_);
        }
    }

private:
    QWidget *window_;
    bool hovered_ = false;
    QRectF closeRect_;
};

}  // namespace

QDialog *createUpdatePrompt(QWidget *parent, const QString &versionText,
                            const QString &notes, bool installed,
                            bool downloading, std::function<void()> update)
{
    auto *dialog = new QDialog(parent);
    dialog->setObjectName("NoctisUpdatePrompt");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(uiText("Chatterino Noctis update"));
    dialog->setWindowIcon(QApplication::windowIcon());
    const auto scale = std::clamp(contentUiScale(getSettings()->getClampedUiScale()),
                                  1.F, 1.18F);
    const auto metric = [scale](int value) { return qRound(value * scale); };
    auto *fonts = getApp()->getFonts();
    auto bodyFont = fonts->getFont(FontStyle::ChatMedium, scale);
    bodyFont.setWeight(QFont::Medium);
    dialog->setFont(bodyFont);
    dialog->setStyleSheet(QStringLiteral(R"(
        QDialog#NoctisUpdatePrompt { background: palette(window); }
        QLabel { color: palette(window-text); background: transparent; }
        QLabel#UpdateHeading { color: palette(window-text); }
        QLabel#SecondaryText { color: palette(placeholder-text); }
        QTextEdit#UpdateNotes { color: palette(text); background: palette(base);
            border: 1px solid palette(mid); border-radius: 8px; padding: 9px; }
        QPushButton { color: palette(button-text); background: palette(button);
            border: 1px solid palette(mid); border-radius: 6px; padding: 6px 14px; }
        QPushButton#UpdatePrimary { border-color: palette(link); background: palette(highlight);
            color: palette(highlighted-text); }
        QPushButton:hover { border-color: palette(link); }
        QPushButton:focus { border-color: palette(link); }
        QScrollBar:vertical { background: transparent; width: 7px; margin: 2px 1px; }
        QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; min-height: 20px; }
        QScrollBar::handle:vertical:hover { background: palette(placeholder-text); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    )"));

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(metric(16), metric(14), metric(16), metric(14));
    layout->setSpacing(metric(9));
    auto *heading = new QLabel(versionText, dialog);
    heading->setObjectName("UpdateHeading");
    heading->setTextFormat(Qt::PlainText);
    heading->setWordWrap(true);
    auto headingFont = fonts->getFont(FontStyle::ChatLarge, scale);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    QTextEdit *releaseNotes = nullptr;
    if (!downloading && !notes.trimmed().isEmpty())
    {
        auto *section = new QLabel(uiText("Here is what changed:"), dialog);
        section->setObjectName("SecondaryText");
        section->setFont(bodyFont);
        layout->addWidget(section);
        releaseNotes = new QTextEdit(dialog);
        releaseNotes->setObjectName("UpdateNotes");
        releaseNotes->setReadOnly(true);
        releaseNotes->setAcceptRichText(false);
        releaseNotes->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        releaseNotes->setLineWrapMode(QTextEdit::WidgetWidth);
        releaseNotes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        releaseNotes->setMinimumHeight(metric(110));
        releaseNotes->setFont(bodyFont);
        releaseNotes->document()->setDefaultFont(bodyFont);
        layout->addWidget(releaseNotes, 1);
    }

    auto *explanation = new QLabel(
        downloading ? uiText("The update is downloading. The program will restart by "
                            "itself when it is installed.")
                    : installed ? uiText("The update is downloaded and installed; the "
                                        "settings stay. The program restarts by itself.")
                                : uiText("This copy is not installed with the setup: the "
                                        "download page opens."), dialog);
    explanation->setObjectName("SecondaryText");
    explanation->setFont(bodyFont);
    explanation->setTextFormat(Qt::PlainText);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto *buttons = new QDialogButtonBox(dialog);
    auto *later = buttons->addButton(downloading ? uiText("Close") : uiText("Later"),
                                    QDialogButtonBox::RejectRole);
    later->setObjectName("UpdateLater");
    later->setFont(bodyFont);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    if (!downloading)
    {
        auto *primary = buttons->addButton(
            installed ? uiText("Update now") : uiText("Open the download page"),
            QDialogButtonBox::AcceptRole);
        primary->setObjectName("UpdatePrimary");
        primary->setFont(bodyFont);
        primary->setDefault(true);
        QObject::connect(primary, &QPushButton::clicked, dialog,
                         [dialog, action = std::move(update)] {
            dialog->accept();
            if (action)
                action();
        });
    }
    layout->addWidget(buttons);

    // Render the release text as inert, selectable text. GitHub notes may
    // contain Markdown or HTML; embedded content must never become a web
    // view or trigger network requests.
    installMoltorinoDialogTheme(dialog, [releaseNotes, notes, bodyFont] {
        if (!releaseNotes)
            return;
        const auto oldScroll = releaseNotes->verticalScrollBar()->value();
        releaseNotes->clear();
        QTextCursor cursor(releaseNotes->document());
        const QRegularExpression headingPattern(QStringLiteral("^#{1,6}\\s+"));
        const QRegularExpression bulletPattern(QStringLiteral("^[-*+]\\s+"));
        bool first = true;
        for (auto line : notes.split(u'\n'))
        {
            line = line.trimmed();
            if (line.isEmpty())
                continue;
            const auto header = headingPattern.match(line);
            const auto bullet = bulletPattern.match(line);
            QTextBlockFormat block;
            block.setBottomMargin(7);
            QTextCharFormat text;
            text.setFont(bodyFont);
            text.setForeground(getTheme()->window.text);
            if (header.hasMatch())
            {
                line.remove(0, header.capturedLength());
                auto font = bodyFont;
                font.setWeight(QFont::DemiBold);
                text.setFont(font);
                text.setForeground(getTheme()->accent);
                block.setTopMargin(first ? 0 : 6);
            }
            else if (bullet.hasMatch())
            {
                line.remove(0, bullet.capturedLength());
                line.prepend(QStringLiteral("•  "));
                block.setLeftMargin(12);
                block.setTextIndent(-12);
            }
            if (!first)
                cursor.insertBlock();
            cursor.setBlockFormat(block);
            cursor.setCharFormat(text);
            cursor.insertText(line, text);
            first = false;
        }
        releaseNotes->moveCursor(QTextCursor::Start);
        releaseNotes->verticalScrollBar()->setValue(oldScroll);
    });
    const auto available = (parent ? parent->screen() : dialog->screen())->availableGeometry();
    dialog->resize(std::min(metric(500), available.width() - 24),
                   std::min(metric(releaseNotes ? 410 : 170), available.height() - 32));
    return dialog;
}

void showUpdatePrompt(QWidget *parent)
{
    static QPointer<QDialog> open;
    if (open && open->isVisible())
    {
        open->raise();
        open->activateWindow();
        return;
    }
    auto &updates = getApp()->getUpdates();
    open = createUpdatePrompt(parent, updates.buildUpdateAvailableText(),
                              updates.getReleaseNotes(), Updates::isInstalledCopy(),
                              updates.getStatus() == Updates::Downloading,
                              [] { getApp()->getUpdates().installUpdates(); });
    open->show();
    open->raise();
    open->activateWindow();
}

void initUpdateBanner(QWidget *window, pajlada::Signals::SignalHolder &holder)
{
    QPointer<Banner> banner = new Banner(window);
    banner->hide();
    const auto changed = [banner](Updates::Status status) {
        if (banner.isNull())
        {
            return;
        }
        const auto &version = getApp()->getUpdates().getOnlineVersion();
        if (status == Updates::UpdateAvailable &&
            version != dismissedVersion())
        {
            banner->show();
            banner->place();
        }
        else if (status != Updates::UpdateAvailable)
        {
            banner->hide();
        }
    };
    holder.managedConnect(getApp()->getUpdates().statusUpdated, changed);
}

}  // namespace chatterino
