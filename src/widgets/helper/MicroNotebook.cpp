#include "widgets/helper/MicroNotebook.hpp"

#include "Application.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/buttons/LabelButton.hpp"

#include <QFrame>

#include <algorithm>

namespace {

using namespace chatterino;

QWidget *makeLine(bool horizontal)
{
    auto *line = new QFrame;
    line->setFrameShape(horizontal ? QFrame::HLine : QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setLineWidth(1);
    return line;
}

QWidget *makeHLine()
{
    return makeLine(true);
}
class MicroNotebookButton : public LabelButton
{
public:
    MicroNotebookButton(const QString &text)
        : LabelButton(text, nullptr, {6, 3})
    {
        this->setFont(getApp()->getFonts()->getFont(
            FontStyle::ChatMedium, std::clamp(getSettings()->getClampedUiScale(), 1.0F, 1.3F)));
    }

    void setSelected(bool selected)
    {
        if (this->selected == selected)
        {
            return;
        }
        this->selected = selected;
        this->invalidateContent();
    }

protected:
    void scaleChangedEvent(float /*scale*/) override
    {
        this->setFont(getApp()->getFonts()->getFont(
            FontStyle::ChatMedium, std::clamp(getSettings()->getClampedUiScale(), 1.0F, 1.3F)));
    }

    void paintContent(QPainter &painter) override
    {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const QRectF pill = QRectF(this->rect()).adjusted(1, 1, -1, -1);
        if (this->selected)
        {
            painter.setBrush(getTheme()->tabs.selected.backgrounds.regular);
            painter.drawRoundedRect(pill, 6, 6);
            painter.setBrush(getTheme()->tabs.selected.line.regular);
            const auto width = std::min(qreal(24), pill.width() * .4);
            painter.drawRoundedRect(
                QRectF(pill.center().x() - width / 2, pill.bottom() - 3,
                       width, 2), 1, 1);
        }
        painter.restore();
    }

private:
    bool selected = false;
};
}  // namespace

namespace chatterino {

MicroNotebook::MicroNotebook(QWidget *parent)
    : QWidget(parent)
    , topWidget(new QWidget)
    , horizontalSeparator(makeHLine())
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setSpacing(0);

    this->topWidget->setLayout(&this->topBar);
    rootLayout->addWidget(this->topWidget);
    rootLayout->addWidget(this->horizontalSeparator);

    auto *pageWidget = new QWidget;
    pageWidget->setLayout(&this->layout);
    rootLayout->addWidget(pageWidget, 1);

    rootLayout->setContentsMargins({});
    this->layout.setContentsMargins({});
    this->topBar.setContentsMargins({10, 8, 10, 5});
    this->topBar.setSpacing(6);
}

int MicroNotebook::addPage(QWidget *page, QString name)
{
    int index = this->layout.addWidget(page);
    auto *pageButton = new MicroNotebookButton(name);
    this->topBar.addWidget(pageButton);
    QObject::connect(pageButton, &MicroNotebookButton::leftClicked, this,
                     [this, index] {
                         this->layout.setCurrentIndex(index);
                     });
    QObject::connect(&this->layout, &QStackedLayout::currentChanged, pageButton,
                     [pageButton, index](int currentIndex) {
                         pageButton->setSelected(index == currentIndex);
                     });
    pageButton->setSelected(this->layout.currentIndex() == index);

    this->items.emplace_back(Item{.name = std::move(name), .index = index});
    return index;
}

void MicroNotebook::select(QWidget *page)
{
    this->layout.setCurrentWidget(page);
}

bool MicroNotebook::isSelected(QWidget *page) const
{
    return this->layout.currentWidget() == page;
}

void MicroNotebook::setShowHeader(bool showHeader)
{
    this->topWidget->setVisible(showHeader);
    this->horizontalSeparator->setVisible(showHeader);
}

int MicroNotebook::headerHeightHint() const
{
    if (this->topWidget->isHidden())
    {
        return 0;
    }
    return this->topWidget->sizeHint().height() +
           this->horizontalSeparator->sizeHint().height();
}

}  // namespace chatterino
