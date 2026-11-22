// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

namespace chatterino {

class GeneralPageView;

class NoctisPage : public SettingsPage
{
    Q_OBJECT

public:
    NoctisPage();

    bool filterElements(const QString &query) override;

private:
    void initLayout(GeneralPageView &layout);

    GeneralPageView *view_{};
};

}  // namespace chatterino
