#pragma once

#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <numeric>
#include <vector>

namespace chatterino {

// Keep localized captions readable without letting arbitrarily long cell
// contents dictate the width of the entire moderation editor.
class AutoModerationTable final : public QTableWidget
{
public:
    AutoModerationTable(int columns, QWidget *parent)
        : QTableWidget(0, columns, parent)
    {
        this->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        this->horizontalHeader()->setStretchLastSection(false);
        this->horizontalHeader()->setTextElideMode(Qt::ElideNone);
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        this->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        this->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->viewport() && event->type() == QEvent::Resize)
            this->scheduleFit();
        return QTableWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        QTableWidget::showEvent(event);
        this->fitColumns();
    }

    void changeEvent(QEvent *event) override
    {
        QTableWidget::changeEvent(event);
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)
            this->scheduleFit();
    }

private:
    void scheduleFit()
    {
        if (this->fitScheduled_)
            return;
        this->fitScheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            this->fitScheduled_ = false;
            this->fitColumns();
        });
    }

    bool fitScheduled_ = false;

    void fitColumns()
    {
        auto *header = this->horizontalHeader();
        const QFontMetrics metrics(header->font());
        std::vector<int> widths;
        for (int column = 0; column < this->columnCount(); ++column)
        {
            const auto *item = this->horizontalHeaderItem(column);
            const int caption = item ? metrics.horizontalAdvance(item->text()) : 0;
            widths.push_back(std::max(header->minimumSectionSize(), caption + 18));
        }
        if (widths.empty())
            return;
        const int extra = std::max(0, this->viewport()->width() - 2 -
                                        std::accumulate(widths.begin(), widths.end(), 0));
        if (widths.size() == 6)
        {
            // Phrase/reward and reason get the spare room; status, action,
            // duration and parameters keep their readable natural minima.
            const int subjectExtra = (extra * 3) / 5;
            widths[1] += subjectExtra;
            widths[4] += extra - subjectExtra;
        }
        else
        {
            widths.back() += extra;
        }
        for (int column = 0; column < this->columnCount(); ++column)
            if (this->columnWidth(column) != widths[column])
                this->setColumnWidth(column, widths[column]);
        this->updateGeometries();
    }
};

}  // namespace chatterino
