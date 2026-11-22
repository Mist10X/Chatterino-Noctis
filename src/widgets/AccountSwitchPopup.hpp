// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <QWidget>

class QPushButton;

namespace chatterino {

class AccountSwitchWidget;
class KickAccountSwitchWidget;
class MicroNotebook;

class AccountSwitchPopup : public BaseWindow
{
    Q_OBJECT

public:
    AccountSwitchPopup(QWidget *parent = nullptr);

    void refresh();

protected:
    void paintEvent(QPaintEvent *event) override;

    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;

private:
    void fitContents();
    QPushButton *manageAccountsButton_ = nullptr;
    MicroNotebook *notebook_ = nullptr;
    struct {
        AccountSwitchWidget *accountSwitchWidget = nullptr;
        KickAccountSwitchWidget *kickAccountSwitcher = nullptr;
    } ui_;
};

}  // namespace chatterino
