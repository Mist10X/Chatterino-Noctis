// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/GeneralPageView.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutHelper.hpp"
#include "util/RapidJsonSerializeQString.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/settingspages/CustomWidgets.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QFormLayout>
#include <QInputDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>

#include <algorithm>

namespace chatterino {

GeneralPageView::GeneralPageView(QWidget *parent)
    : QWidget(parent)
    , contentScrollArea_(new QScrollArea)
    , contentLayout_(new QVBoxLayout)
{
    auto *contentWidget = new QWidget;
    contentWidget->setObjectName("settingsScrollBody");
    contentWidget->setLayout(this->contentLayout_);
    this->contentScrollArea_->setWidget(contentWidget);
    this->contentScrollArea_->setObjectName("generalSettingsScrollContent");
    this->contentScrollArea_->setWidgetResizable(true);
}

GeneralPageView *GeneralPageView::withoutNavigation(QWidget *parent)
{
    auto *view = new GeneralPageView(parent);

    view->setLayout(makeLayout<QHBoxLayout>({
        view->contentScrollArea_,
    }));

    return view;
}

GeneralPageView *GeneralPageView::withSectionPicker(QWidget *parent)
{
    auto *view = new GeneralPageView(parent);
    auto *picker = new ComboBox;
    view->sectionPicker_ = picker;
    picker->setObjectName("settingsSectionPicker");
    // The widest translated control in a different section must not enlarge
    // every row in the scroll area. Keep vertical height-for-width negotiation.
    view->contentLayout_->setSizeConstraint(QLayout::SetNoConstraint);
    view->contentScrollArea_->widget()->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    picker->setAccessibleName(uiText("Category"));
    picker->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    picker->setFocusPolicy(Qt::StrongFocus);
    auto *layout = makeLayout<QVBoxLayout>({picker, view->contentScrollArea_});
    layout->setStretch(1, 1);
    view->setLayout(layout);
    QObject::connect(picker, &QComboBox::activated, view, [view, picker](int row) {
        const int index = picker->itemData(row).toInt();
        auto *bar = view->contentScrollArea_->verticalScrollBar();
        bar->setValue(view->groups_.at(index).title->y());
        // The last headings may share the same clamped scroll position.
        picker->setCurrentIndex(row);
    });
    QObject::connect(view->contentScrollArea_->verticalScrollBar(), &QScrollBar::valueChanged,
                     view, [view] { view->updateNavigationHighlighting(); });
    return view;
}

GeneralPageView *GeneralPageView::withNavigation(QWidget *parent)
{
    auto *view = new GeneralPageView(parent);

    auto *navigation =
        wrapLayout(view->navigationLayout_ = makeLayout<QVBoxLayout>({}));
    navigation->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Minimum);
    view->navigationLayout_->setAlignment(Qt::AlignTop);
    view->navigationLayout_->addSpacing(6);

    view->setLayout(makeLayout<QHBoxLayout>({
        view->contentScrollArea_,
        new QSpacerItem(16, 1),
        navigation,
    }));

    QObject::connect(view->contentScrollArea_->verticalScrollBar(),
                     &QScrollBar::valueChanged, view, [view] {
                         view->updateNavigationHighlighting();
                     });

    return view;
}

namespace {

std::function<GeneralPageView *(const QString &)> &activeTopics()
{
    static std::function<GeneralPageView *(const QString &)> lookup;
    return lookup;
}

}  // namespace

GeneralPageView *SettingsTopics::find(const QString &topic)
{
    const auto &lookup = activeTopics();
    return lookup ? lookup(topic) : nullptr;
}

bool SettingsTopics::active()
{
    return static_cast<bool>(activeTopics());
}

void SettingsTopics::setActive(
    std::function<GeneralPageView *(const QString &)> lookup)
{
    activeTopics() = std::move(lookup);
}

void routeSetting(GeneralPageView &from, const QString &topic,
                  const QString &section)
{
    // a page shown on its own (tests, previews) keeps the sections itself
    auto *target = SettingsTopics::find(topic);
    from.routeTo(target != nullptr ? target : &from, section);
}

void GeneralPageView::routeTo(GeneralPageView *target, const QString &section)
{
    this->route_ = target == this ? nullptr : target;
    if (target == nullptr)
    {
        return;
    }
    target->route_ = nullptr;
    const auto name = uiText(section);
    for (size_t i = 0; i < target->groups_.size(); ++i)
    {
        if (target->groups_[i].name == name)
        {
            target->insertGroup_ = static_cast<int>(i);
            return;
        }
    }
    target->addTitle(section);
}

void GeneralPageView::finishTopic()
{
    for (auto &group : this->groups_)
    {
        // a section is its title and the space before the next title
        const bool empty = std::ranges::none_of(group.widgets, [](auto &w) {
            return dynamic_cast<TitleLabel *>(w.element) == nullptr &&
                   dynamic_cast<Space *>(w.element) == nullptr;
        });
        if (empty)
        {
            // Only structural spacer/title entries live here. Keep an empty
            // section out of subsequent searches and the section picker.
            group.widgets.clear();
            group.title->hide();
            if (group.space)
            {
                group.space->hide();
            }
            if (group.navigationLink)
            {
                group.navigationLink->hide();
            }
        }
    }
    this->insertGroup_ = -1;
    this->contentLayout_->addStretch();
    this->updateSectionPicker();
}

GeneralPageView::Group &GeneralPageView::currentGroup()
{
    if (this->insertGroup_ >= 0 &&
        this->insertGroup_ < static_cast<int>(this->groups_.size()))
    {
        return this->groups_[this->insertGroup_];
    }
    return this->groups_.back();
}

int GeneralPageView::insertIndex() const
{
    const auto next = this->insertGroup_ + 1;
    if (this->insertGroup_ < 0 ||
        next >= static_cast<int>(this->groups_.size()))
    {
        return -1;
    }
    // the space before the next title ends this section
    const auto &group = this->groups_[this->insertGroup_];
    QWidget *end = group.space != nullptr
                       ? static_cast<QWidget *>(group.space)
                       : this->groups_[next].title;
    return this->contentLayout_->indexOf(end);
}

void GeneralPageView::placeWidget(QWidget *widget)
{
    const auto at = this->insertIndex();
    if (at < 0)
    {
        this->contentLayout_->addWidget(widget);
    }
    else
    {
        this->contentLayout_->insertWidget(at, widget);
    }
}

void GeneralPageView::placeLayout(QLayout *layout)
{
    const auto at = this->insertIndex();
    if (at < 0)
    {
        this->contentLayout_->addLayout(layout);
    }
    else
    {
        this->contentLayout_->insertLayout(at, layout);
    }
}

void GeneralPageView::addWidget(QWidget *widget, const QStringList &keywords)
{
    if (this->route_ != nullptr)
    {
        this->route_->addWidget(widget, keywords);
        return;
    }
    this->placeWidget(widget);
    if (!this->groups_.empty())
    {
        this->currentGroup().widgets.push_back({
            .element = widget,
            .keywords = keywords,
        });
    }
}

void GeneralPageView::registerWidget(QWidget *widget,
                                     const QStringList &keywords,
                                     QWidget *parentElement)
{
    if (this->route_ != nullptr)
    {
        this->route_->registerWidget(widget, keywords, parentElement);
        return;
    }
    if (!this->groups_.empty())
    {
        this->currentGroup().widgets.push_back({
            .element = widget,
            .keywords = keywords,
            .parentElement = parentElement,
        });
    }
}

void GeneralPageView::pushWidget(QWidget *widget)
{
    if (this->route_ != nullptr)
    {
        this->route_->pushWidget(widget);
        return;
    }
    this->placeWidget(widget);
}

void GeneralPageView::addLayout(QLayout *layout)
{
    if (this->route_ != nullptr)
    {
        this->route_->addLayout(layout);
        return;
    }
    this->placeLayout(layout);
}

void GeneralPageView::addStretch()
{
    if (SettingsTopics::active())
    {
        return;  // topic pages end with finishTopic()
    }
    this->contentLayout_->addStretch();
}

TitleLabel *GeneralPageView::addTitle(const QString &title)
{
    if (this->route_ != nullptr)
    {
        return this->route_->addTitle(title);
    }
    this->insertGroup_ = -1;
    if (!this->groups_.empty())
    {
        this->addWidget(this->groups_.back().space = new Space);
    }

    auto *label = new TitleLabel(uiText(title));
    this->addWidget(label);

    NavigationLabel *navLabel = nullptr;

    if (this->navigationLayout_ != nullptr)
    {
        navLabel = new NavigationLabel(title);
        navLabel->setCursor(Qt::PointingHandCursor);
        this->navigationLayout_->addWidget(navLabel);

        QObject::connect(
            navLabel, &NavigationLabel::leftMouseUp, label, [this, label] {
                this->contentScrollArea_->verticalScrollBar()->setValue(
                    label->y());
            });
    }

    this->groups_.push_back(Group{uiText(title), label, navLabel, nullptr, {}});

    if (this->groups_.size() == 1)
    {
        this->updateNavigationHighlighting();
    }

    return label;
}

SubtitleLabel *GeneralPageView::addSubtitle(const QString &title)
{
    if (this->route_ != nullptr)
    {
        return this->route_->addSubtitle(title);
    }
    auto *label = new SubtitleLabel(uiText(title));
    this->addWidget(label);

    this->currentGroup().widgets.push_back({label, {title}});

    return label;
}

void ComboBox::enableCustomChoice()
{
    this->presetCount_ = this->count();
    this->addItem(uiText(QStringLiteral("Custom value\u2026")));
}

bool ComboBox::isCustomChoice(int index) const
{
    return this->presetCount_ >= 0 && index == this->count() - 1;
}

void ComboBox::showSettingValue(const boost::variant<int, QString> &value)
{
    const QSignalBlocker blocker(this);
    if (const auto *row = boost::get<int>(&value))
    {
        this->setCurrentIndex(*row);
        return;
    }

    const auto &text = boost::get<QString>(value);
    int row = this->findData(text, SourceRole);
    if (row < 0)
    {
        // Keep one row for a previously saved or newly entered custom value.
        if (this->presetCount_ >= 0 && this->count() > this->presetCount_ + 1)
        {
            this->removeItem(this->presetCount_);
        }
        row = this->presetCount_ >= 0 ? this->presetCount_ : this->count();
        this->insertItem(row, text);
        this->setItemData(row, text, SourceRole);
    }
    this->setCurrentIndex(row);
}

void ComboBox::requestCustomValue(const QString &label,
                                  const std::function<void(QString, int)> &apply)
{
    bool ok = false;
    const auto value = QInputDialog::getText(
        this, label, uiText(QStringLiteral("Custom value\u2026")), QLineEdit::Normal,
        this->currentText(), &ok);
    if (ok && !value.trimmed().isEmpty())
    {
        // Some settings use the preset index for named sentinel values.
        int row = this->findData(value, SourceRole);
        if (row < 0)
        {
            row = this->findText(value, Qt::MatchExactly);
        }
        if (row >= 0 && !this->isCustomChoice(row))
        {
            apply(this->sourceText(row), row);
        }
        else
        {
            apply(value, -1);
        }
    }
}

ComboBox *GeneralPageView::addDropdown(const QString &text,
                                       const QStringList &list,
                                       QString toolTipText)
{
    if (this->route_ != nullptr)
    {
        return this->route_->addDropdown(text, list, toolTipText);
    }
    auto *layout = new QFormLayout;
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *combo = new ComboBox;
    combo->setFocusPolicy(Qt::StrongFocus);
    for (const auto &item : list)
    {
        combo->addItem(uiText(item));
        combo->setItemData(combo->count() - 1, item, UiComboBox::SourceRole);
    }

    auto *label = new SettingCaption(uiText(text) + ":");
    layout->addRow(label, combo);
    layout->setAlignment(combo, Qt::AlignRight);

    this->addToolTip(*label, toolTipText);
    this->addLayout(layout);

    this->currentGroup().widgets.push_back({combo, {text}});
    this->currentGroup().widgets.push_back({label, {text}});

    return combo;
}

void GeneralPageView::addNavigationSpacing()
{
    if (this->route_ != nullptr || SettingsTopics::active())
    {
        return;
    }
    assert(this->navigationLayout_ != nullptr &&
           "addNavigationSpacing used without navigation");

    this->navigationLayout_->addSpacing(24);
}

DescriptionLabel *GeneralPageView::addDescription(const QString &text)
{
    if (this->route_ != nullptr)
    {
        return this->route_->addDescription(text);
    }
    auto *label = new DescriptionLabel(text);

    label->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                   Qt::LinksAccessibleByKeyboard);
    label->setOpenExternalLinks(true);
    label->setWordWrap(true);

    this->addWidget(label);

    this->currentGroup().widgets.push_back({label, {text}});

    return label;
}

void GeneralPageView::addSeparator()
{
    if (this->route_ != nullptr)
    {
        this->route_->addSeparator();
        return;
    }
    this->addWidget(new Line(false));
}

bool GeneralPageView::filterElements(const QString &query)
{
    bool any{};

    for (auto &&group : this->groups_)
    {
        if (this->sectionPicker_ && group.widgets.empty())
        {
            group.title->hide();
            if (group.space)
            {
                group.space->hide();
            }
            continue;
        }
        bool descriptionMatches{};
        for (auto &&widget : group.widgets)
        {
            if (auto *x = dynamic_cast<DescriptionLabel *>(widget.element); x)
            {
                if (x->text().contains(query, Qt::CaseInsensitive))
                {
                    descriptionMatches = true;
                    break;
                }
            }
        }

        if (group.name.contains(query, Qt::CaseInsensitive) ||
            descriptionMatches)
        {
            for (auto &&widget : group.widgets)
            {
                widget.element->show();
                if (widget.parentElement != nullptr)
                {
                    widget.parentElement->show();
                }
            }

            group.title->show();
            if (group.navigationLink != nullptr)
            {
                group.navigationLink->show();
            }
            any = true;
        }

        else
        {
            auto groupAny = false;

            QWidget *currentSubtitle = nullptr;
            bool currentSubtitleVisible = false;
            bool currentSubtitleSearched = false;

            for (auto &&widget : group.widgets)
            {
                if (auto *x = dynamic_cast<SubtitleLabel *>(widget.element))
                {
                    currentSubtitleSearched = false;
                    if (currentSubtitle)
                    {
                        currentSubtitle->setVisible(currentSubtitleVisible);
                    }

                    currentSubtitleVisible = false;
                    currentSubtitle = widget.element;

                    if (x->text().contains(query, Qt::CaseInsensitive))
                    {
                        currentSubtitleSearched = true;
                    }
                    continue;
                }

                if (auto *x = dynamic_cast<Line *>(widget.element))
                {
                    x->hide();
                }

                for (auto &&keyword : widget.keywords)
                {
                    if (keyword.contains(query, Qt::CaseInsensitive) ||
                        uiText(keyword).contains(query, Qt::CaseInsensitive) ||
                        currentSubtitleSearched)
                    {
                        currentSubtitleVisible = true;
                        widget.element->show();
                        if (widget.parentElement != nullptr)
                        {
                            widget.parentElement->show();
                        }
                        groupAny = true;
                        break;
                    }

                    widget.element->hide();
                    if (widget.parentElement != nullptr)
                    {
                        widget.parentElement->hide();
                    }
                }
            }

            if (currentSubtitle)
            {
                currentSubtitle->setVisible(currentSubtitleVisible);
            }

            if (group.space)
            {
                group.space->setVisible(groupAny);
            }

            group.title->setVisible(groupAny);
            if (group.navigationLink != nullptr)
            {
                group.navigationLink->setVisible(groupAny);
            }
            any |= groupAny;
        }
    }

    this->updateSectionPicker();
    return any;
}

void GeneralPageView::updateSectionPicker()
{
    if (!this->sectionPicker_)
    {
        return;
    }
    this->sectionPicker_->clear();
    for (size_t i = 0; i < this->groups_.size(); ++i)
    {
        const auto &group = this->groups_[i];
        if (!group.title->isHidden())
        {
            this->sectionPicker_->addItem(group.name, static_cast<int>(i));
        }
    }
    this->sectionPicker_->setVisible(this->sectionPicker_->count() > 1);
    this->updateNavigationHighlighting();
}

void GeneralPageView::updateNavigationHighlighting()
{
    if (this->sectionPicker_)
    {
        const auto *bar = this->contentScrollArea_->verticalScrollBar();
        if (bar->value() == bar->minimum())
        {
            this->sectionPicker_->setCurrentIndex(this->sectionPicker_->count() ? 0 : -1);
            return;
        }
        int active = 0;
        for (int row = 1; row < this->sectionPicker_->count(); ++row)
        {
            const int index = this->sectionPicker_->itemData(row).toInt();
            if (this->groups_[index].title->y() > bar->value())
            {
                break;
            }
            active = row;
        }
        this->sectionPicker_->setCurrentIndex(active);
        return;
    }
    if (this->navigationLayout_ == nullptr)
    {
        return;
    }

    auto scrollY = this->contentScrollArea_->verticalScrollBar()->value();
    auto first = true;

    for (auto &&group : this->groups_)
    {
        if (first && (group.title->geometry().bottom() > scrollY ||
                      &group == &this->groups_.back()))
        {
            first = false;
            group.navigationLink->setStyleSheet(
                QStringLiteral("color: %1").arg(getTheme()->accent.name()));
        }
        else
        {
            group.navigationLink->setStyleSheet("");
        }
    }
}

void GeneralPageView::addToolTip(QWidget &widget, QString text) const
{
    if (text.isEmpty())
    {
        return;
    }

    text = uiText(text);

    widget.setToolTip(text);
}

}  // namespace chatterino
