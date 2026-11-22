// SPDX-License-Identifier: MIT
#pragma once

#include <QComboBox>
#include <QStyleOptionComboBox>

namespace chatterino {

// Localized option labels are presentation only. GeneralPage's existing
// callbacks continue receiving canonical English values and numeric indexes.
class UiComboBox : public QComboBox
{
public:
    explicit UiComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        this->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    }

    QSize sizeHint() const override
    {
        auto hint = QComboBox::sizeHint();
        QStyleOptionComboBox option;
        this->initStyleOption(&option);
        option.rect = QRect(QPoint(), hint);
        // QSS reserves space for padding and the arrow separately. Qt's
        // cached combo size does not always account for both at larger fonts.
        const auto field = this->style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        int textWidth = 0;
        const auto metrics = this->fontMetrics();
        for (int row = 0; row < this->count(); ++row)
        {
            int width = metrics.horizontalAdvance(this->itemText(row));
            if (!this->itemIcon(row).isNull())
            {
                width += this->iconSize().width() + 4;
            }
            textWidth = qMax(textWidth, width);
        }
        // QCommonStyle draws the closed label with a further 2px each side.
        hint.rwidth() += qMax(0, textWidth + 4 - field.width());
        return hint;
    }

    QSize minimumSizeHint() const override
    {
        return this->sizeHint();
    }

    static constexpr int SourceRole = Qt::UserRole + 37;

    QString sourceText(int index) const
    {
        if (index < 0)
        {
            // Editable custom values have no preset row. Preserve the input.
            return this->currentText();
        }
        const auto source = this->itemData(index, SourceRole);
        return source.isValid() ? source.toString() : this->itemText(index);
    }

    void setSourceText(const QString &value)
    {
        for (int i = 0; i < this->count(); ++i)
        {
            if (this->sourceText(i) == value)
            {
                this->setCurrentIndex(i);
                return;
            }
        }
        if (this->isEditable())
        {
            this->setEditText(value);
        }
    }
};

}  // namespace chatterino
