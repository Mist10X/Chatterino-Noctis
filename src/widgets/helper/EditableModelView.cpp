// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/UiLanguage.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include "widgets/helper/RegExpItemDelegate.hpp"
#include "widgets/helper/TableStyles.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QModelIndex>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace chatterino {

EditableModelView::EditableModelView(QAbstractTableModel *model, bool movable)
    : tableView_(new QTableView(this))
    , model_(model)
{
    this->model_->setParent(this);
    this->tableView_->setModel(this->model_);

    this->tableView_->setWordWrap(false);
    this->tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    this->tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->tableView_->setDragDropMode(QTableView::DragDropMode::InternalMove);
    this->tableView_->setDragDropOverwriteMode(false);
    this->tableView_->setDefaultDropAction(Qt::DropAction::MoveAction);
    this->tableView_->verticalHeader()->setVisible(false);
    this->tableView_->horizontalHeader()->setSectionsClickable(false);

    TableRowDragStyle::applyTo(this->tableView_);

    QVBoxLayout *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);

    auto *buttons = new QHBoxLayout();
    this->buttons_ = buttons;
    vbox->addLayout(buttons);

    QPushButton *add = new QPushButton(uiText("Add"));
    buttons->addWidget(add);
    QObject::connect(add, &QPushButton::clicked, [this] {
        this->addButtonPressed.invoke();
    });

    QPushButton *remove = new QPushButton(uiText("Remove"));
    buttons->addWidget(remove);
    QObject::connect(remove, &QPushButton::clicked, [this] {
        auto selected = this->getTableView()->selectionModel()->selectedRows(0);

        std::vector<int> rows;
        for (auto &&index : selected)
        {
            rows.push_back(index.row());
        }

        std::sort(rows.begin(), rows.end(), std::greater{});

        for (auto &&row : rows)
        {
            this->model_->removeRow(row);
        }
    });

    if (movable)
    {
        QPushButton *moveUp = new QPushButton(uiText("Move up"));
        buttons->addWidget(moveUp);
        QObject::connect(moveUp, &QPushButton::clicked, this, [this] {
            this->moveRow(-1);
        });

        QPushButton *moveDown = new QPushButton(uiText("Move down"));
        buttons->addWidget(moveDown);
        QObject::connect(moveDown, &QPushButton::clicked, this, [this] {
            this->moveRow(1);
        });
    }

    buttons->addStretch();

    QObject::connect(this->model_, &QAbstractTableModel::rowsMoved, this,
                     [this](const QModelIndex &parent, int start, int end,
                            const QModelIndex &destination, int row) {
                         this->tableView_->selectRow(row);
                     });

    QObject::connect(this->model_, &QAbstractTableModel::rowsInserted, this,
                     [this](const QModelIndex &parent, int first, int last) {
                         this->tableView_->selectRow(last);
                     });

    vbox->addWidget(this->tableView_);
}
void EditableModelView::setValidationRegexp(QRegularExpression regexp)
{
    this->tableView_->setItemDelegate(new RegExpItemDelegate(this, regexp));
}

void EditableModelView::setTitles(std::initializer_list<QString> titles)
{
    int i = 0;
    for (const QString &title : titles)
    {
        if (this->model_->columnCount() == i)
        {
            break;
        }

        this->model_->setHeaderData(i++, Qt::Horizontal, uiText(title),
                                    Qt::DisplayRole);
    }
}

void EditableModelView::setReadableColumns(
    std::initializer_list<int> textColumns, bool wrapRows)
{
    this->textColumns_ = QList<int>(textColumns);
    auto *header = this->tableView_->horizontalHeader();
    header->setObjectName("settingsReadableHeader");
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setStretchLastSection(false);
    this->tableView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->tableView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->tableView_->setWordWrap(wrapRows);
    if (wrapRows)
    {
        this->tableView_->verticalHeader()->setSectionResizeMode(
            QHeaderView::ResizeToContents);
    }
    this->tableView_->installEventFilter(this);
    this->tableView_->viewport()->installEventFilter(this);
    header->installEventFilter(this);
    this->updateColumnLayout();
}

bool EditableModelView::eventFilter(QObject *object, QEvent *event)
{
    const auto type = event->type();
    if ((object == this->tableView_ || object == this->tableView_->viewport() ||
         object == this->tableView_->horizontalHeader()) &&
        (type == QEvent::Resize || type == QEvent::Show ||
         type == QEvent::FontChange || type == QEvent::StyleChange) &&
        !this->columnLayoutPending_)
    {
        // Wait for Qt to settle the viewport and header after a font/layout change.
        this->columnLayoutPending_ = true;
        QTimer::singleShot(0, this, [this] {
            this->columnLayoutPending_ = false;
            this->updateColumnLayout();
        });
    }
    return QWidget::eventFilter(object, event);
}

void EditableModelView::updateColumnLayout()
{
    auto *header = this->tableView_->horizontalHeader();
    const auto metrics = header->fontMetrics();
    QList<int> widths(header->count(), 0);
    int totalWidth = 0;
    int expandingCount = 0;
    int headerHeight = metrics.height();
    for (int column = 0; column < header->count(); ++column)
    {
        if (header->isSectionHidden(column))
        {
            continue;
        }
        int width = std::max(44, header->sectionSizeHint(column));
        const auto lines = this->model_->headerData(column, Qt::Horizontal)
                               .toString().split('\n');
        headerHeight = std::max(headerHeight, metrics.height() +
            (static_cast<int>(lines.size()) - 1) * metrics.lineSpacing());
        for (const auto &line : lines)
        {
            width = std::max(width, metrics.horizontalAdvance(line) + 16);
        }
        if (this->textColumns_.contains(column))
        {
            // Keep room for a readable phrase/name even when other columns
            // have long translations. Larger tables scroll horizontally.
            width = std::max(width, 150);
            ++expandingCount;
        }
        widths[column] = width;
        totalWidth += width;
    }
    header->setMinimumHeight(headerHeight + 8);
    int extra = std::max(0, this->tableView_->viewport()->width() - totalWidth);
    bool resized = false;
    for (int column = 0; column < widths.size(); ++column)
    {
        if (widths[column] == 0)
        {
            continue;
        }
        int width = widths[column];
        if (this->textColumns_.contains(column) && expandingCount > 0)
        {
            const auto share = extra / expandingCount--;
            width += share;
            extra -= share;
        }
        if (header->sectionSize(column) != width)
        {
            header->resizeSection(column, width);
            resized = true;
        }
    }
    if (resized)
    {
        // Refresh the scroll range and wrapped row heights when all sections
        // have their final widths, including the first time a page is opened.
        this->tableView_->doItemsLayout();
    }
}

QTableView *EditableModelView::getTableView()
{
    return this->tableView_;
}

QAbstractTableModel *EditableModelView::getModel()
{
    return this->model_;
}

void EditableModelView::addCustomButton(QWidget *widget)
{
    this->buttons_->addWidget(widget);
}

void EditableModelView::addRegexHelpLink()
{
    auto *regexHelpLabel =
        new QLabel(uiText("<a href='"
                   "https://chatterino.com/help/regex'>"
                   "<span style='color:#99f'>regex info</span></a>"));
    regexHelpLabel->setObjectName("settingsRegexHelp");
    regexHelpLabel->setOpenExternalLinks(true);
    regexHelpLabel->setWordWrap(true);
    regexHelpLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Use the toolbar's free space before wrapping the help link. A plain
    // word-wrapped label would keep its narrow size hint while the spacer
    // consumed the rest of the row.
    regexHelpLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    this->buttons_->addWidget(regexHelpLabel, 1);
}

bool EditableModelView::filterSearchResults(const QString &query,
                                            std::span<const int> columnSelect)
{
    bool searchFoundSomething = false;
    auto rowAmount = this->model_->rowCount();

    if (rowAmount == 0 && query.isEmpty())
    {
        return true;
    }

    for (int i = 0; i < rowAmount; i++)
    {
        bool foundMatch = false;

        for (int j : columnSelect)
        {
            QModelIndex idx = this->model_->index(i, j);
            QVariant a = this->model_->data(idx);
            if (a.toString().contains(query, Qt::CaseInsensitive))
            {
                foundMatch = true;
                searchFoundSomething = true;
                break;
            }
        }

        this->tableView_->setRowHidden(i, !foundMatch);
    }

    return searchFoundSomething;
}

void EditableModelView::filterSearchResultsHotkey(
    const QKeySequence &keySequenceQuery)
{
    auto rowAmount = this->model_->rowCount();

    for (int i = 0; i < rowAmount; i++)
    {
        QModelIndex idx = this->model_->index(i, 1);
        QVariant a = this->model_->data(idx);
        auto seq = qvariant_cast<QKeySequence>(a);

        if (keySequenceQuery.matches(seq) != QKeySequence::NoMatch)
        {
            this->tableView_->showRow(i);
        }
        else
        {
            this->tableView_->hideRow(i);
        }
    }
}

void EditableModelView::moveRow(int dir)
{
    auto selected = this->getTableView()->selectionModel()->selectedRows(0);

    int row;
    if (selected.size() == 0 ||
        (row = selected.at(0).row()) + dir >=
            this->model_->rowCount(QModelIndex()) ||
        row + dir < 0)
    {
        return;
    }

    this->model_->moveRows(this->model_->index(row, 0), row, selected.size(),
                           this->model_->index(row + dir, 0), row + dir);
    this->tableView_->selectRow(row + dir);
}

}  // namespace chatterino
