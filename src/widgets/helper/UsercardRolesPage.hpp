// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/ModdexRoles.hpp"

#include <QUrl>
#include <QWidget>

#include <array>
#include <cstddef>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace chatterino {

/// Public assigned roles of a Twitch account.
class UsercardRolesPage final : public QWidget
{
    Q_OBJECT

public:
    explicit UsercardRolesPage(
        QWidget *parent = nullptr,
        QUrl baseUrl = QUrl(QStringLiteral("https://api.moddex.tv")));

    void setTarget(const QString &userId, const QString &login);
    void setActive(bool active);
    void setScale(float scale);
    void setNetworkEnabled(bool enabled);

private:
    enum class State { Idle, Loading, Ready, Unavailable, Error };

    struct ListState {
        moddex::RolePage page;
        State state = State::Idle;
        bool loaded = false;
    };

    static std::size_t index(bool past, moddex::RoleKind kind);
    std::size_t selectedIndex() const;

    void resetLists();
    void ensureLoaded();
    void requestCurrent();
    void requestPage(bool past, moddex::RoleKind kind);
    void retry();
    void unavailable();
    void finishPage(std::size_t index, moddex::ResultStatus result,
                    moddex::RolePage page);
    void updateControls();
    void renderRows();
    void addRow(const moddex::Role &role);
    void applyStyle();

    moddex::RoleProvider provider_;
    const bool lookupAvatars_;
    std::array<ListState, 4> lists_;
    QString userId_;
    std::size_t generation_ = 0;
    bool active_ = false;
    bool networkEnabled_ = true;
    bool past_ = false;
    moddex::RoleKind kind_ = moddex::RoleKind::Moderator;
    float scale_ = 1.F;
    int renderedIndex_ = -1;
    std::size_t renderedCount_ = 0;

    QPushButton *activeButton_;
    QPushButton *pastButton_;
    QPushButton *modsButton_;
    QPushButton *vipsButton_;
    QPushButton *moreButton_;
    QPushButton *retryButton_;
    QLabel *status_;
    QLabel *source_;
    QScrollArea *scroll_;
    QVBoxLayout *rows_;
};

}  // namespace chatterino
