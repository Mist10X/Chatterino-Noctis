// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QKeySequence>
#include <QList>
#include <QWidget>

#include <span>

class QAbstractTableModel;
class QTableView;
class QHBoxLayout;

namespace chatterino {

class EditableModelView : public QWidget
{
public:
    EditableModelView(QAbstractTableModel *model, bool movable = true);

    void setTitles(std::initializer_list<QString> titles);
    void setReadableColumns(std::initializer_list<int> textColumns,
                            bool wrapRows = false);
    void setValidationRegexp(QRegularExpression regexp);

    QTableView *getTableView();
    QAbstractTableModel *getModel();

    pajlada::Signals::NoArgSignal addButtonPressed;

    void addCustomButton(QWidget *widget);
    void addRegexHelpLink();

    bool filterSearchResults(const QString &query,
                             std::span<const int> columnSelect);
    void filterSearchResultsHotkey(const QKeySequence &keySequenceQuery);

private:
    bool eventFilter(QObject *object, QEvent *event) override;
    void updateColumnLayout();

    QTableView *tableView_{};
    QAbstractTableModel *model_{};
    QHBoxLayout *buttons_{};
    QList<int> textColumns_;
    bool columnLayoutPending_ = false;

    void moveRow(int dir);
};

}  // namespace chatterino
