// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "Application.hpp"
#include "common/ChatterinoSetting.hpp"
#include "common/UiLanguage.hpp"
#include "widgets/settingspages/UiComboBox.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/buttons/SignalLabel.hpp"

#include <boost/variant.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QComboBox>
#include <QDebug>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>
#include <utility>

class QScrollArea;

namespace chatterino {

class ColorButton;

class Space : public QLabel
{
    Q_OBJECT
};

class TitleLabel : public QLabel
{
    Q_OBJECT

public:
    TitleLabel(const QString &text)
        : QLabel(uiText(text))
    {
    }
};

class SubtitleLabel : public QLabel
{
    Q_OBJECT

public:
    SubtitleLabel(const QString &text)
        : QLabel(uiText(text))
    {
    }
};

class NavigationLabel : public SignalLabel
{
    Q_OBJECT

public:
    NavigationLabel(const QString &text)
        : SignalLabel()
    {
        this->setText(uiText(text));
    }
};

class DescriptionLabel : public QLabel
{
    Q_OBJECT

public:
    DescriptionLabel(const QString &text)
        : QLabel(uiText(text))
    {
    }
};

class ComboBox : public UiComboBox
{
    Q_OBJECT

public:
    void enableCustomChoice();
    bool isCustomChoice(int index) const;
    void showSettingValue(const boost::variant<int, QString> &value);
    void requestCustomValue(const QString &label,
                            const std::function<void(QString, int)> &apply);

private:
    int presetCount_ = -1;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

class SpinBox : public QSpinBox
{
    Q_OBJECT

public:
    SpinBox(QWidget *parent = nullptr)
        : QSpinBox(parent)
    {
        this->setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

class GeneralPageView;

/// Settings are grouped by topic (appearance, chat, input...), not by the page
/// whose code adds them: while the settings window builds its topic pages,
/// a page sends what it adds next into a topic page and section with this.
/// Outside of that (tests, previews) it does nothing.
void routeSetting(GeneralPageView &from, const QString &topic,
                  const QString &section);

/// The topic pages that routeSetting() sends settings to, by topic name.
/// Set by the settings window while it builds its pages.
class SettingsTopics
{
public:
    static GeneralPageView *find(const QString &topic);
    static bool active();
    static void setActive(
        std::function<GeneralPageView *(const QString &)> lookup);
};

struct DropdownArgs {
    QString value;
    int index;
    QComboBox *combobox;
};

class GeneralPageView : public QWidget
{
    Q_OBJECT
    GeneralPageView(QWidget *parent = nullptr);

public:
    static GeneralPageView *withNavigation(QWidget *parent);
    static GeneralPageView *withSectionPicker(QWidget *parent);
    static GeneralPageView *withoutNavigation(QWidget *parent);

    void addWidget(QWidget *widget, const QStringList &keywords = {});

    void registerWidget(QWidget *widget, const QStringList &keywords,
                        QWidget *parentElement);

    void pushWidget(QWidget *widget);

    void addLayout(QLayout *layout);
    void addStretch();

    TitleLabel *addTitle(const QString &text);
    SubtitleLabel *addSubtitle(const QString &text);

    ComboBox *addDropdown(const QString &text, const QStringList &items,
                          QString toolTipText = {});
    void addNavigationSpacing();

    /// What is added next goes into @a target, at the end of its @a section
    /// (created at the end if it has none). nullptr: back into this view.
    void routeTo(GeneralPageView *target, const QString &section);
    /// Hides sections nothing was put into and ends the page.
    void finishTopic();

    template <typename OnClick>
    QPushButton *makeButton(const QString &text, OnClick onClick)
    {
        if (this->route_ != nullptr)
        {
            return this->route_->makeButton(text, std::move(onClick));
        }
        auto *button = new QPushButton(uiText(text));
        this->currentGroup().widgets.push_back({button, {text}});
        QObject::connect(button, &QPushButton::clicked, onClick);
        return button;
    }

    template <typename OnClick>
    QPushButton *addButton(const QString &text, OnClick onClick)
    {
        if (this->route_ != nullptr)
        {
            return this->route_->addButton(text, std::move(onClick));
        }
        auto button = makeButton(text, onClick);
        auto *layout = new QHBoxLayout();
        layout->addWidget(button);
        layout->addStretch(1);
        this->addLayout(layout);
        return button;
    }

    template <typename T>
    ComboBox *addDropdown(
        const QString &text, const QStringList &items,
        pajlada::Settings::Setting<T> &setting,
        std::function<boost::variant<int, QString>(T)> getValue,
        std::function<T(DropdownArgs)> setValue, bool allowCustomValue = false,
        QString toolTipText = {}, bool listenToActivated = false)
    {
        if (this->route_ != nullptr)
        {
            return this->route_->addDropdown<T>(
                text, items, setting, std::move(getValue), std::move(setValue),
                allowCustomValue, toolTipText, listenToActivated);
        }
        auto *combo = this->addDropdown(text, items, toolTipText);
        if (allowCustomValue)
        {
            combo->enableCustomChoice();
        }

        auto refresh = [&setting, getValue = std::move(getValue), combo] {
            combo->showSettingValue(getValue(setting.getValue()));
        };
        refresh();
        setting.connect([refresh](const T &, auto) { refresh(); },
                        this->managedConnections_);

        auto apply = [combo, &setting, setValue = std::move(setValue)](
                         const QString &value, int index) {
            setting = setValue(DropdownArgs{
                .value = value,
                .index = index,
                .combobox = combo,
            });
            getApp()->getWindows()->forceLayoutChannelViews();
        };
        auto selectPreset = [combo, apply](int index) {
            if (index >= 0 && !combo->isCustomChoice(index))
            {
                apply(combo->sourceText(index), index);
            }
        };
        if (listenToActivated)
        {
            QObject::connect(combo, &QComboBox::activated, combo, selectPreset);
        }
        else
        {
            QObject::connect(combo, &QComboBox::currentIndexChanged, combo,
                             selectPreset);
        }
        if (allowCustomValue)
        {
            QObject::connect(combo, &QComboBox::activated, combo,
                             [combo, text, refresh, apply](int index) {
                if (combo->isCustomChoice(index))
                {
                    // Opening or cancelling the editor leaves the setting intact.
                    refresh();
                    combo->requestCustomValue(text, apply);
                }
            });
        }
        return combo;
    }

    DescriptionLabel *addDescription(const QString &text);

    void addSeparator();
    bool filterElements(const QString &query);

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        (void)event;
    }

private:
    void updateNavigationHighlighting();
    void updateSectionPicker();
    void addToolTip(QWidget &widget, QString text) const;

    struct Widget {
        QWidget *element{};
        QStringList keywords;

        QWidget *parentElement{};
    };

    struct Group {
        QString name;
        QWidget *title{};
        QWidget *navigationLink{};
        Space *space{};
        std::vector<Widget> widgets;
    };

    struct Group;
    /// The section new things go into: the one routed to, else the last.
    Group &currentGroup();
    void placeWidget(QWidget *widget);
    void placeLayout(QLayout *layout);
    /// Where the current section ends in the content (-1: at the very end).
    int insertIndex() const;

    GeneralPageView *route_ = nullptr;
    int insertGroup_ = -1;

    QScrollArea *contentScrollArea_ = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;
    QVBoxLayout *navigationLayout_ = nullptr;
    QComboBox *sectionPicker_ = nullptr;

    std::vector<Group> groups_;
    pajlada::Signals::SignalHolder managedConnections_;
};

}  // namespace chatterino
