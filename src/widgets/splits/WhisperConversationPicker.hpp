#pragma once

#include <QHash>
#include <QPushButton>
#include <QVariantAnimation>

#include <functional>

class QFrame;
class QListWidget;
class QListWidgetItem;

namespace chatterino {

class Split;

class WhisperConversationPicker final : public QPushButton
{
public:
    WhisperConversationPicker(Split *split,
                              std::function<void(const QString &)> selected);
    void reset();
    void addPeer(const QString &login, const QString &displayName = {});
    void selectPeer(const QString &login);
    const QString &peer() const;
    void refreshStyle();

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setOpen(bool open);
    void dismiss();
    int rowHeight() const;
    void positionPanel();
    void choose(QListWidgetItem *item);

    Split *const split_;
    QFrame *const panel_;
    QListWidget *const list_;
    std::function<void(const QString &)> selected_;
    QHash<QString, QListWidgetItem *> peers_;
    QString peer_;
    QVariantAnimation reveal_;
    qreal progress_ = 0.0;
    int visibleRows_ = 1;
    bool open_ = false;
};

}  // namespace chatterino
