// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/SettingWidget.hpp"

#include "common/QLogging.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/NativeMessaging.hpp"
#include "singletons/Settings.hpp"  // IWYU pragma: keep
#include "util/QMagicEnumTagged.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/ColorButton.hpp"
#include "widgets/helper/SvgWidget.hpp"
#include "widgets/settingspages/CustomWidgets.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"

#include <QBoxLayout>
#include <QCheckBox>
#include <QFontDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <Qt>

#include <algorithm>

namespace chatterino {

SettingWidget::SettingWidget(const QString &mainKeyword)
    : tooltipIcon(new SvgWidget(this))
    , vLayout(new QVBoxLayout(this))
    , hLayout(new QHBoxLayout)
{
    this->vLayout->setContentsMargins(0, 0, 0, 0);

    this->hLayout->setContentsMargins(0, 0, 0, 0);
    this->vLayout->addLayout(this->hLayout);

    this->keywords.append(mainKeyword);
    this->keywords.append(uiText(mainKeyword));
    this->tooltipIcon->setVisible(false);
}

SettingWidget *SettingWidget::checkbox(const QString &label,
                                       BoolSetting &setting)
{
    auto *widget = new SettingWidget(label);

    auto *check = new SCheckBox(uiText(label));

    widget->hLayout->addWidget(check);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addStretch(1);

    // update when setting changes
    setting.connect(
        [check](const bool &value) {
            check->setChecked(value);
        },
        widget->managedConnections);

    // update setting on toggle
    QObject::connect(check, &QCheckBox::toggled, widget,
                     [&setting](bool state) {
                         setting = state;
                     });

    widget->actionWidget = check;
    widget->label = check;

    return widget;
}

SettingWidget *SettingWidget::inverseCheckbox(const QString &label,
                                              BoolSetting &setting)
{
    auto *widget = new SettingWidget(label);

    auto *check = new SCheckBox(uiText(label));

    widget->hLayout->addWidget(check);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addStretch(1);

    // update when setting changes
    setting.connect(
        [check](const bool &value) {
            check->setChecked(!value);
        },
        widget->managedConnections);

    // update setting on toggle
    QObject::connect(check, &QCheckBox::toggled, widget,
                     [&setting](bool state) {
                         setting = !state;
                     });

    widget->actionWidget = check;
    widget->label = check;

    return widget;
}

SettingWidget *SettingWidget::customCheckbox(
    const QString &label, bool initialValue,
    const std::function<void(bool)> &save)
{
    auto *widget = new SettingWidget(label);

    auto *check = new SCheckBox(uiText(label));

    widget->hLayout->addWidget(check);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addStretch(1);

    check->setChecked(initialValue);

    QObject::connect(check, &QCheckBox::toggled, widget, save);

    widget->actionWidget = check;
    widget->label = check;

    return widget;
}

SettingWidget *SettingWidget::intInput(const QString &label,
                                       IntSetting &setting,
                                       IntInputParams params)
{
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) + ":");

    auto *input = new SpinBox;
    if (params.min.has_value())
    {
        input->setMinimum(params.min.value());
    }
    if (params.max.has_value())
    {
        input->setMaximum(params.max.value());
    }
    if (params.singleStep.has_value())
    {
        input->setSingleStep(params.singleStep.value());
    }
    if (params.suffix.has_value())
    {
        input->setSuffix(params.suffix.value());
    }

    widget->hLayout->addWidget(lbl);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addStretch(1);
    widget->hLayout->addWidget(input);

    // update when setting changes
    setting.connect(
        [input](const int &value, const auto &) {
            input->setValue(value);
        },
        widget->managedConnections);

    // update setting on value changed
    QObject::connect(input, QOverload<int>::of(&QSpinBox::valueChanged), widget,
                     [&setting](int newValue) {
                         setting = newValue;
                     });

    widget->actionWidget = input;
    widget->label = lbl;

    return widget;
}

void SettingWidget::layoutDropdown(QWidget *caption, QComboBox *combo)
{
    this->label = caption;
    this->actionWidget = combo;
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *heading = new QHBoxLayout;
    heading->addWidget(caption);
    heading->addWidget(this->tooltipIcon);
    form->setLayout(0, QFormLayout::LabelRole, heading);
    form->setWidget(0, QFormLayout::FieldRole, combo);
    form->setAlignment(combo, Qt::AlignRight);
    this->hLayout->addLayout(form);
}

template <typename T>
SettingWidget *SettingWidget::dropdown(const QString &label,
                                       EnumStringSetting<T> &setting)
{
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) % ":");
    auto *combo = new ComboBox;
    combo->setFocusPolicy(Qt::StrongFocus);

    for (const auto value : magic_enum::enum_values<T>())
    {
        combo->addItem(uiText(qmagicenum::enumDisplayNameString(value)),
                       QVariant(static_cast<std::underlying_type_t<T>>(value)));
    }

    widget->layoutDropdown(lbl, combo);

    setting.connect(
        [&setting, combo](const QString &value) {
            auto enumValue =
                qmagicenum::enumCast<T>(value, qmagicenum::CASE_INSENSITIVE)
                    .value_or(setting.defaultValue);

            auto i = magic_enum::enum_index(enumValue).value_or(0);

            combo->setCurrentIndex(i);
        },
        widget->managedConnections);

    QObject::connect(
        combo, &QComboBox::currentTextChanged,
        [label, combo, &setting](const auto &newText) {
            bool ok = true;
            auto enumValue = combo->currentData().toInt(&ok);
            if (!ok)
            {
                qCWarning(chatterinoWidget)
                    << "Combo" << label << " with value" << newText
                    << "did not contain an intable UserRole data";
                return;
            }

            setting = qmagicenum::enumNameString(static_cast<T>(enumValue));
        });

    return widget;
}

template SettingWidget *SettingWidget::dropdown<SoundBackend>(
    const QString &label, EnumStringSetting<SoundBackend> &setting);
template SettingWidget *SettingWidget::dropdown<EmoteTooltipScale>(
    const QString &label, EnumStringSetting<EmoteTooltipScale> &setting);
template SettingWidget *SettingWidget::dropdown<StreamLinkPreferredQuality>(
    const QString &label,
    EnumStringSetting<StreamLinkPreferredQuality> &setting);
template SettingWidget *SettingWidget::dropdown<ChatSendProtocol>(
    const QString &label, EnumStringSetting<ChatSendProtocol> &setting);
template SettingWidget *SettingWidget::dropdown<RecentMessagesApi>(
    const QString &label, EnumStringSetting<RecentMessagesApi> &setting);
template SettingWidget *SettingWidget::dropdown<TabStyle>(
    const QString &label, EnumStringSetting<TabStyle> &setting);
template SettingWidget *SettingWidget::dropdown<TabAvatarStyle>(
    const QString &label, EnumStringSetting<TabAvatarStyle> &setting);
template SettingWidget *SettingWidget::dropdown<TimestampSize>(
    const QString &label, EnumStringSetting<TimestampSize> &setting);
template SettingWidget *SettingWidget::dropdown<ShowModerationState>(
    const QString &label, EnumStringSetting<ShowModerationState> &setting);
template SettingWidget *SettingWidget::dropdown<EmojiStyle>(
    const QString &label, EnumStringSetting<EmojiStyle> &setting);
template SettingWidget *SettingWidget::dropdown<SplitMpsCorner>(
    const QString &label, EnumStringSetting<SplitMpsCorner> &setting);
template SettingWidget *SettingWidget::dropdown<SplitMpsWindow>(
    const QString &label, EnumStringSetting<SplitMpsWindow> &setting);
template SettingWidget *SettingWidget::dropdown<BrowserManifestFormat>(
    const QString &label, EnumStringSetting<BrowserManifestFormat> &setting);
template SettingWidget *SettingWidget::dropdown<TwitchReadConnectionMode>(
    const QString &label, EnumStringSetting<TwitchReadConnectionMode> &setting);
template SettingWidget *SettingWidget::dropdown<KickConnectionPreference>(
    const QString &label, EnumStringSetting<KickConnectionPreference> &setting);

template <typename T>
SettingWidget *SettingWidget::dropdown(const QString &label,
                                       EnumSetting<T> &setting)
{
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) % ":");
    auto *combo = new ComboBox;
    combo->setFocusPolicy(Qt::StrongFocus);

    for (const auto value : magic_enum::enum_values<T>())
    {
        combo->addItem(uiText(qmagicenum::enumDisplayNameString(value)),
                       QVariant(static_cast<std::underlying_type_t<T>>(value)));
    }

    widget->layoutDropdown(lbl, combo);

    setting.connect(
        [combo, label](const auto &value) {
            std::optional<int> foundRow;

            for (auto row = 0; row < combo->model()->rowCount(); ++row)
            {
                auto index = combo->model()->index(row, 0);
                auto rowEnumValue = index.data(Qt::UserRole);
                if (rowEnumValue == value)
                {
                    foundRow = row;
                    break;
                }
            }

            if (foundRow)
            {
                combo->setCurrentIndex(*foundRow);
            }
            else
            {
                qCWarning(chatterinoWidget)
                    << "Did not find a correct combo box row for" << label
                    << " with value" << value;
            }
        },
        widget->managedConnections);

    QObject::connect(combo, &QComboBox::currentTextChanged,
                     [label, combo, &setting](const auto &newText) {
                         bool ok = true;
                         auto enumValue = combo->currentData().toInt(&ok);
                         if (!ok)
                         {
                             qCWarning(chatterinoWidget)
                                 << "Combo" << label << " with value" << newText
                                 << "did not contain an intable UserRole data";
                             return;
                         }

                         setting.setValue(enumValue);
                     });

    return widget;
}

template SettingWidget *SettingWidget::dropdown<LastMessageLineStyle>(
    const QString &label, EnumSetting<LastMessageLineStyle> &setting);
template SettingWidget *SettingWidget::dropdown<ThumbnailPreviewMode>(
    const QString &label, EnumSetting<ThumbnailPreviewMode> &setting);
template SettingWidget *SettingWidget::dropdown<StreamerModeSetting>(
    const QString &label, EnumSetting<StreamerModeSetting> &setting);

SettingWidget *SettingWidget::dropdown(
    const QString &label, QStringSetting &setting,
    const std::vector<std::pair<QString, QVariant>> &items)
{
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) % ":");
    auto *combo = new ComboBox;
    combo->setFocusPolicy(Qt::StrongFocus);

    for (const auto &[itemText, itemData] : items)
    {
        combo->addItem(itemData.toString() == QStringLiteral("System")
                           ? uiText(itemText)
                           : itemText,
                       itemData);
    }

    widget->layoutDropdown(lbl, combo);

    setting.connect(
        [combo, label](const auto &value) {
            std::optional<int> foundRow;

            for (auto row = 0; row < combo->model()->rowCount(); ++row)
            {
                auto index = combo->model()->index(row, 0);
                auto rowEnumValue = index.data(Qt::UserRole);
                if (rowEnumValue == value)
                {
                    foundRow = row;
                    break;
                }
            }

            if (foundRow)
            {
                combo->setCurrentIndex(*foundRow);
            }
            else
            {
                qCWarning(chatterinoWidget)
                    << "Did not find a correct combo box row for" << label
                    << " with value" << value;
            }
        },
        widget->managedConnections);

    QObject::connect(combo, &QComboBox::currentTextChanged,
                     [label, combo, &setting](const auto & /*newText*/) {
                         auto stringValue = combo->currentData().toString();

                         setting.setValue(stringValue);
                     });

    return widget;
}

SettingWidget *SettingWidget::colorButton(const QString &label,
                                          QStringSetting &setting)
{
    QColor color(setting.getValue());
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) + ":");

    auto *colorButton = new ColorButton(color);
    // keep the row height: the button only grows sideways
    colorButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    widget->hLayout->addWidget(lbl);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addStretch(1);
    widget->hLayout->addWidget(colorButton);

    // update when setting changes
    setting.connect(
        [colorButton](const QString &value, const auto &) {
            colorButton->setColor(QColor(value));
        },
        widget->managedConnections);

    QObject::connect(colorButton, &ColorButton::clicked, [widget, &setting]() {
        auto *dialog = new ColorPickerDialog(QColor(setting), widget);
        // colorButton & setting are never deleted and the signal is deleted
        // once the dialog is closed
        QObject::connect(
            dialog, &ColorPickerDialog::colorConfirmed, widget,
            [&setting](auto selected) {
                if (selected.isValid())
                {
                    setting.setValue(selected.name(QColor::HexArgb));
                }
            });
        dialog->show();
    });

    widget->actionWidget = colorButton;
    widget->label = lbl;

    return widget;
}

SettingWidget *SettingWidget::lineEdit(const QString &label,
                                       QStringSetting &setting,
                                       const QString &placeholderText)
{
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) + ":");

    auto *edit = new QLineEdit;
    edit->setText(setting);
    if (!placeholderText.isEmpty())
    {
        edit->setPlaceholderText(placeholderText);
    }

    widget->hLayout->addWidget(lbl);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addWidget(edit);

    // Update the setting when the widget changes.
    QObject::connect(edit, &QLineEdit::textChanged,
                     [&setting](const QString &newValue) {
                         setting = newValue;
                     });

    // Update the widget to reflect the new setting value if the setting changes
    // This _will_ fire every time the widget changes, so we are being conservative
    // with the `setText` call to ensure the user doesn't get their cursor bounced around.
    setting.connect(
        [edit](const QString &value) {
            if (edit->text() != value)
            {
                edit->setText(value);
            }
        },
        widget->managedConnections, false);

    widget->actionWidget = edit;
    widget->label = lbl;

    return widget;
}

SettingWidget *SettingWidget::fontButton(const QString &label,
                                         QStringSetting &familySetting,
                                         std::function<QFont()> currentFont,
                                         std::function<void(QFont)> onChange)
{
    auto *widget = new SettingWidget(label);

    auto *lbl = new SettingCaption(uiText(label) + ":");

    auto *button = new SPushButton(currentFont().family());

    widget->hLayout->addWidget(lbl);
    widget->hLayout->addWidget(widget->tooltipIcon);
    widget->hLayout->addStretch(1);
    widget->hLayout->addWidget(button);

    familySetting.connect(
        [button, currentFont](const auto &) {
            button->setText(currentFont().family());
        },
        widget->managedConnections);

    QObject::connect(button, &QPushButton::clicked,
                     [widget, currentFont{std::move(currentFont)},
                      onChange{std::move(onChange)}]() {
                         bool ok = false;
                         auto font =
                             QFontDialog::getFont(&ok, currentFont(), widget);
                         if (ok)
                         {
                             onChange(font);
                         }
                     });

    widget->actionWidget = button;
    widget->label = lbl;

    return widget;
}

SettingWidget *SettingWidget::setTooltip(QString tooltip)
{
    assert(!tooltip.isEmpty());
    tooltip = uiText(tooltip);

    // A hint icon is useful only if the hint contributes information.
    // Some older settings used the visible caption itself as the hint.
    if (this->keywords.size() >= 2 &&
        (tooltip.simplified() == this->keywords[0].simplified() ||
         tooltip.simplified() == this->keywords[1].simplified()))
    {
        return this;
    }

    if (this->label != nullptr)
    {
        this->label->setToolTip(tooltip);
    }

    if (this->actionWidget != nullptr)
    {
        this->actionWidget->setToolTip(tooltip);
    }

    this->tooltipIcon->setVisible(true);
    this->tooltipIcon->load(u":/settings/hint.svg"_qs);
    this->tooltipIcon->setToolTip(tooltip);
    this->tooltipIcon->setFixedSize(12, 18);

    this->keywords.append(tooltip);

    return this;
}

SettingWidget *SettingWidget::setDescription(const QString &text)
{
    auto *lbl = new QLabel(uiText(text));
    lbl->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                 Qt::LinksAccessibleByKeyboard);
    lbl->setOpenExternalLinks(true);
    lbl->setWordWrap(true);
    lbl->setObjectName("description");

    this->vLayout->insertWidget(0, lbl);

    this->keywords.append(text);

    return this;
}

SettingWidget *SettingWidget::addKeywords(const QStringList &newKeywords)
{
    this->keywords.append(newKeywords);

    return this;
}

SettingWidget *SettingWidget::conditionallyEnabledBy(BoolSetting &setting)
{
    setting.connect(
        [this](const auto &value, const auto &) {
            this->actionWidget->setEnabled(value);
        },
        this->managedConnections);

    return this;
}

SettingWidget *SettingWidget::conditionallyEnabledBy(
    QStringSetting &setting, const QString &expectedValue)
{
    setting.connect(
        [this, expectedValue](const auto &value, const auto &) {
            this->actionWidget->setEnabled(value == expectedValue);
        },
        this->managedConnections);

    return this;
}

void SettingWidget::addTo(GeneralPageView &view)
{
    view.pushWidget(this);

    this->registerWidget(view);
}

void SettingWidget::addTo(GeneralPageView &view, QFormLayout *formLayout)
{
    this->registerWidget(view);

    formLayout->addRow(this->label, this->actionWidget);
}

void SettingWidget::addToLayout(QLayout *layout)
{
    if (this->label == this->actionWidget)
    {
        layout->addWidget(this->actionWidget);
        return;
    }

    assert(false && "unimplemented");
}

void SettingWidget::registerWidget(GeneralPageView &view)
{
    if (this->label != nullptr)
    {
        view.registerWidget(this->label, this->keywords, this);
    }
    view.registerWidget(this->actionWidget, this->keywords, this);
}

}  // namespace chatterino
