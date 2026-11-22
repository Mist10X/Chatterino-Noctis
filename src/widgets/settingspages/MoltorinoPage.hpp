#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QLabel;
class QPushButton;

namespace chatterino {

class GeneralPageView;

class MoltorinoPage : public SettingsPage
{
    Q_OBJECT

public:
    MoltorinoPage();

    bool filterElements(const QString &query) override;

private:
    void openAuthDialog();
    void refreshAuthAccounts();
    void updateAuthSummary();
    void updateAuthInstructions(const QString &text, bool isError = false);
    void updateAuthStatus(const QString &text, bool isValid,
                          bool isError = false);
    GeneralPageView *settingsView_{};

    QLabel *authStatusLabel_{};
    QLabel *authInstructionsLabel_{};
    QPushButton *addAuthAccountButton_{};
    QPushButton *refreshAuthAccountsButton_{};
    bool authRefreshInFlight_{false};
    int authRefreshGeneration_{0};

};

}  // namespace chatterino
