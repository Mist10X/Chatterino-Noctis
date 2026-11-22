// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/SettingsPage.hpp"
#include "common/UiLanguage.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FunctionEventFilter.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <QDebug>

#include <algorithm>

namespace chatterino {

float SettingsPage::interfaceScale()
{
    // The scale control itself stays usable at both ends of the chat zoom range.
    return 1.F + (std::clamp(getSettings()->getClampedUiScale(), 1.F, 1.3F) -
                  1.F) * .5F;
}

bool filterItemsRec(QObject *object, const QString &query)
{
    bool any{};

    for (auto &&child : object->children())
    {
        auto setOpacity = [&](auto *widget, bool condition) {
            any |= condition;
            widget->greyedOut = !condition;
            widget->update();
        };

        if (auto *checkBox = dynamic_cast<SCheckBox *>(child))
        {
            setOpacity(checkBox,
                       checkBox->text().contains(query, Qt::CaseInsensitive));
        }
        else if (auto *lbl = dynamic_cast<SLabel *>(child))
        {
            setOpacity(lbl, lbl->text().contains(query, Qt::CaseInsensitive));
        }
        else if (auto *comboBox = dynamic_cast<SComboBox *>(child))
        {
            setOpacity(comboBox, [=]() {
                for (int i = 0; i < comboBox->count(); i++)
                {
                    if (comboBox->itemText(i).contains(query,
                                                       Qt::CaseInsensitive))
                    {
                        return true;
                    }
                }
                return false;
            }());
        }
        else if (auto *tabs = dynamic_cast<QTabWidget *>(child))
        {
            for (int i = 0; i < tabs->count(); i++)
            {
                bool tabAny{};

                if (tabs->tabText(i).contains(query, Qt::CaseInsensitive))
                {
                    tabAny = true;
                }
                auto *widget = tabs->widget(i);
                tabAny |= filterItemsRec(widget, query);

                any |= tabAny;
            }
        }
        else
        {
            any |= filterItemsRec(child, query);
        }
    }
    return any;
}

SettingsPage::SettingsPage()
{
}

bool SettingsPage::filterElements(const QString &query)
{
    return filterItemsRec(this, query) || query.isEmpty();
}

SettingsDialogTab *SettingsPage::tab() const
{
    return this->tab_;
}

void SettingsPage::setTab(SettingsDialogTab *tab)
{
    this->tab_ = tab;
}

QCheckBox *SettingsPage::createCheckBox(
    const QString &text, pajlada::Settings::Setting<bool> &setting,
    const QString &toolTipText)
{
    QCheckBox *checkbox = new SCheckBox(uiText(text));
    checkbox->setToolTip(uiText(toolTipText));

    setting.connect(
        [checkbox](const bool &value, auto) {
            checkbox->setChecked(value);
        },
        this->managedConnections_);

    QObject::connect(checkbox, &QCheckBox::toggled, this,
                     [&setting](bool state) {
                         setting = state;
                         getApp()->getWindows()->forceLayoutChannelViews();
                     });

    return checkbox;
}

QComboBox *SettingsPage::createComboBox(
    const QStringList &items, pajlada::Settings::Setting<QString> &setting)
{
    auto *combo = new SComboBox();
    for (const auto &item : items)
    {
        combo->addItem(uiText(item));
        combo->setItemData(combo->count() - 1, item, UiComboBox::SourceRole);
    }

    setting.connect(
        [combo](const QString &value, auto) {
            combo->setSourceText(value);
        },
        this->managedConnections_);

    QObject::connect(combo, &QComboBox::currentIndexChanged,
                     [combo, &setting](int index) {
                         setting = combo->sourceText(index);
                     });

    return combo;
}

QSpinBox *SettingsPage::createSpinBox(pajlada::Settings::Setting<int> &setting,
                                      int min, int max)
{
    QSpinBox *w = new QSpinBox;

    w->setMinimum(min);
    w->setMaximum(max);

    setting.connect(
        [w](const int &value, auto) {
            w->setValue(value);
        },
        this->managedConnections_);
    QObject::connect(w, QOverload<int>::of(&QSpinBox::valueChanged),
                     [&setting](int value) {
                         setting.setValue(value);
                     });

    return w;
}

}  // namespace chatterino
