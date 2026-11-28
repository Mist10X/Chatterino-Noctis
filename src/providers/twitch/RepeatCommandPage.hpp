#pragma once

#include "common/UiLanguage.hpp"
#include "providers/twitch/RepeatCommand.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace chatterino::repeatcommand {

template <typename Rule>
QList<Rule> onlyRules(const QList<Rule> &rules, bool commands)
{
    QList<Rule> result;
    for (const auto &rule : rules)
        if (rule.repeatCommand == commands) result.append(rule);
    return result;
}

template <typename Rule>
QList<Rule> mergeRules(const QList<Rule> &ordinary, const QList<Rule> &commands)
{
    auto result = onlyRules(ordinary, false);
    result.append(onlyRules(commands, true));
    return result;
}

// Edit command rules separately while retaining their original persisted format.
template <typename Rule, typename Save>
QWidget *makePage(QWidget *parent, const QList<Rule> &rules,
                  const QStringList &channels, const QString &preferred, Save save)
{
    auto *page = new QWidget(parent);
    page->setObjectName("RepeatCommandPage");
    auto *layout = new QVBoxLayout(page);
    auto *hint = new QLabel(uiText("Configure !tg and a plain message. Send !tg 5 from your own moderator account to request five messages (1–10). Only one batch runs at a time, through normal Twitch limits."), page);
    hint->setWordWrap(true);
    hint->setObjectName("MutedText");
    layout->addWidget(hint);
    auto *picker = new QComboBox(page);
    picker->setObjectName("RepeatCommandChannel");
    picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    picker->setMinimumContentsLength(12);
    for (const auto &name : channels)
        picker->addItem(name, name);
    auto *scope = new QFormLayout;
    scope->setRowWrapPolicy(QFormLayout::WrapLongRows);
    scope->addRow(uiText("Channel"), picker);
    layout->addLayout(scope);
    auto *splitter = new QSplitter(Qt::Horizontal, page);
    auto *table = new QTableWidget(0, 2, splitter);
    table->setObjectName("RepeatCommandRules");
    table->setHorizontalHeaderLabels({uiText("Command"), uiText("Status")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    auto *editorScroll = new QScrollArea(splitter);
    editorScroll->setObjectName("RepeatCommandEditorScroll");
    editorScroll->setFrameShape(QFrame::NoFrame);
    editorScroll->setWidgetResizable(true);
    auto *editor = new QWidget(editorScroll);
    editor->setObjectName("RepeatCommandEditor");
    editorScroll->setWidget(editor);
    auto *form = new QVBoxLayout(editor);
    form->setSizeConstraint(QLayout::SetMinimumSize);
    auto *group = new QLineEdit(editor);
    group->setObjectName("RepeatCommandGroup");
    group->setPlaceholderText(uiText("Optional group name"));
    group->setMaxLength(80);
    auto *details = new QFormLayout;
    details->setRowWrapPolicy(QFormLayout::WrapLongRows);
    details->addRow(uiText("Group"), group);
    form->addLayout(details);
    auto *enabled = new QCheckBox(uiText("Enable rule"), editor);
    form->addWidget(enabled);
    auto *trigger = new QPlainTextEdit(editor);
    trigger->setObjectName("RepeatCommandTrigger");
    trigger->setPlaceholderText(uiText("One command per line, e.g. !tg. Type !tg 5 in chat."));
    trigger->setTabChangesFocus(true);
    trigger->setMinimumHeight(48);
    trigger->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Ignored);
    form->addWidget(new QLabel(uiText("Repeat commands"), editor));
    form->addWidget(trigger, 1);
    auto *reply = new QPlainTextEdit(editor);
    reply->setObjectName("RepeatCommandReply");
    reply->setTabChangesFocus(true);
    reply->setMinimumHeight(48);
    reply->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Ignored);
    form->addWidget(new QLabel(uiText("Reply text"), editor));
    form->addWidget(reply, 1);
    splitter->setChildrenCollapsible(false);
    splitter->setSizes({260, 540});
    layout->addWidget(splitter, 1);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(uiText("Add command"), page);
    add->setObjectName("RepeatCommandAdd");
    auto *remove = new QPushButton(uiText("Remove selected"), page);
    auto *apply = new QPushButton(uiText("Save rules"), page);
    apply->setObjectName("RepeatCommandSave");
    buttons->addWidget(add); buttons->addWidget(remove); buttons->addStretch(); buttons->addWidget(apply);
    layout->addLayout(buttons);
    auto *status = new QLabel(page);
    status->setWordWrap(true);
    status->setObjectName("RepeatCommandStatus");
    layout->addWidget(status);
    struct Data {
        QList<Rule> rules;
        QList<int> rows;
        int active = -1;
        bool filling = false;
    };
    const auto data = std::make_shared<Data>();
    data->rules = rules;
    const auto update = [=] {
        if (data->filling || data->active < 0) return;
        auto &rule = data->rules[data->active];
        rule.title = group->text().trimmed();
        rule.triggers = trigger->toPlainText().split(u'\n');
        rule.reply = reply->toPlainText().trimmed();
        rule.enabled = enabled->isChecked();
        const auto row = data->rows.indexOf(data->active);
        if (row >= 0 && table->item(row, 0))
        {
            table->item(row,0)->setText(rule.label());
            table->item(row,1)->setText(uiText(rule.enabled ? "Enabled" : "Disabled"));
        }
    };
    const auto select = [=](int row) {
        update();
        data->filling = true;
        data->active = row >= 0 && row < data->rows.size() ? data->rows[row] : -1;
        const bool valid = data->active >= 0;
        const auto rule = valid ? data->rules[data->active] : Rule{};
        editor->setEnabled(valid); remove->setEnabled(valid);
        group->setText(rule.title);
        trigger->setPlainText(rule.triggers.join(u'\n'));
        reply->setPlainText(rule.reply);
        enabled->setChecked(rule.enabled);
        data->filling = false;
    };
    const auto load = [=](int preferredRow = 0) {
        update(); data->active = -1; data->rows.clear();
        const QSignalBlocker blocker(table);
        table->setRowCount(0);
        const auto channel = picker->currentData().toString();
        for (int i = 0; i < data->rules.size(); ++i)
        {
            const auto &rule = data->rules[i];
            if (!rule.repeatCommand || rule.channel != channel) continue;
            data->rows.append(i);
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row,0,new QTableWidgetItem(rule.label()));
            table->setItem(row,1,new QTableWidgetItem(uiText(rule.enabled ? "Enabled" : "Disabled")));
        }
        const int row = table->rowCount() ? std::clamp(preferredRow, 0, table->rowCount()-1) : -1;
        if (row >= 0) table->selectRow(row);
        select(row);
        add->setEnabled(picker->currentIndex() >= 0);
        apply->setEnabled(picker->currentIndex() >= 0);
    };
    QObject::connect(table, &QTableWidget::currentCellChanged, page, [=](int row,int,int,int) { select(row); });
    QObject::connect(picker, &QComboBox::currentIndexChanged, page, [=](int) { load(0); });
    QObject::connect(group, &QLineEdit::textChanged, page, update);
    QObject::connect(trigger, &QPlainTextEdit::textChanged, page, update);
    QObject::connect(reply, &QPlainTextEdit::textChanged, page, update);
    QObject::connect(enabled, &QCheckBox::toggled, page, update);
    QObject::connect(add, &QPushButton::clicked, page, [=] {
        update();
        Rule rule;
        rule.channel = picker->currentData().toString();
        if (rule.channel.isEmpty()) return;
        rule.repeatCommand = true;
        data->rules.append(rule);
        load(table->rowCount());
        trigger->setFocus();
    });
    QObject::connect(remove, &QPushButton::clicked, page, [=] {
        if (data->active < 0) return;
        data->rules.removeAt(data->active);
        data->active = -1;
        load(table->currentRow());
    });
    QObject::connect(apply, &QPushButton::clicked, page, [=] {
        update();
        auto next = data->rules;
        for (auto &rule : next)
        {
            if (!rule.repeatCommand || !channels.contains(rule.channel)) continue;
            for (auto &phrase : rule.triggers) phrase = phrase.trimmed();
            rule.triggers.removeAll(QString{});
            rule.reply = rule.reply.trimmed().replace(u'\n', u' ');
        }
        const auto error = save(next);
        if (error.isEmpty())
        {
            data->rules = std::move(next);
            data->active = -1;
            load(table->currentRow());
            status->setText(uiText("Auto-reply rules saved."));
        }
        else status->setText(error);
    });
    const int index = picker->findData(preferred);
    if (index >= 0) picker->setCurrentIndex(index);
    load(0);
    return page;
}
} // namespace chatterino::repeatcommand
