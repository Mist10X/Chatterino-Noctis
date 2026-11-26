#pragma once
#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QPointer>
#include <QScopedValueRollback>

namespace chatterino::automationdialog {
inline QDialog *&current()
{
    static QDialog *editor = nullptr;
    return editor;
}
inline int run(QDialog &dialog)
{
    QScopedValueRollback<QDialog *> active(current(), &dialog);
    QList<QPointer<QWidget>> locked;
    if (auto *parent = qobject_cast<QDialog *>(dialog.parentWidget()))
    {
        for (auto *child : parent->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
        {
            if (!child->isWindow() && child->isEnabled())
            {
                locked.append(child);
                child->setEnabled(false);
            }
        }
        QObject::connect(parent, &QDialog::finished, &dialog, &QDialog::reject);
    }
    QEventLoop loop;
    QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    dialog.setWindowModality(Qt::NonModal);
    dialog.setModal(false);
    dialog.show();
    dialog.raise();
    dialog.activateWindow();
    if (dialog.isVisible()) loop.exec();
    for (const auto &child : locked)
        if (child) child->setEnabled(true);
    return dialog.result();
}
inline bool raiseExisting(const QString &name)
{
    for (auto *widget : QApplication::topLevelWidgets())
    {
        if (widget->objectName() == name && widget->isVisible())
        {
            widget->raise();
            widget->activateWindow();
            return true;
        }
    }
    return false;
}
} // namespace chatterino::automationdialog
