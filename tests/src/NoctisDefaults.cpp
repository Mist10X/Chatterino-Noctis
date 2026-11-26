#include "mocks/BaseApplication.hpp"
#include "Test.hpp"
#include <QFile>
#include <QTemporaryDir>

using namespace chatterino;

TEST(NoctisDefaults, ExistingProfileForcesEmbeddedThemeAndIrcOnce)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path=dir.filePath("custom.json");
    ASSERT_TRUE(QFile::copy(":/themes/Noctis-Dark.json",path));
    mock::BaseApplication app(QStringLiteral(R"({"appearance":{"theme":{"name":"custom.json"},"uiScale2":1.25},"misc":{"noctisThemeMigrated":true,"chatSendProtocol":"Helix"}})"),true,path);
    app.settings.disableSave();
    EXPECT_EQ(app.theme.themeName.getValue(),"Noctis-Dark");
    EXPECT_EQ(app.settings.chatSendProtocol.getEnum(),ChatSendProtocol::IRC);
    EXPECT_FALSE(app.settings.shouldSendHelixChat());
    EXPECT_TRUE(QFile::exists(path));
}
TEST(NoctisDefaults, LaterIntentionalChoicesAreNotReset)
{
    mock::BaseApplication app(QStringLiteral(R"({"appearance":{"theme":{"name":"Light"}},"misc":{"noctisThemeIrcDefaultsApplied":true,"noctisThemeMigrated":true,"chatSendProtocol":"Helix"}})"),true);
    app.settings.disableSave();
    EXPECT_EQ(app.theme.themeName.getValue(),"Light");
    EXPECT_EQ(app.settings.chatSendProtocol.getEnum(),ChatSendProtocol::Helix);
    EXPECT_TRUE(app.settings.shouldSendHelixChat());
    app.settings.chatSendProtocol=QStringLiteral("Default");
    EXPECT_FALSE(app.settings.shouldSendHelixChat());
}
TEST(NoctisDefaults, NewProfileUsesIrcAndNoctisTheme)
{
    mock::BaseApplication app(true);
    app.settings.disableSave();
    EXPECT_EQ(app.theme.themeName.getValue(),"Noctis-Dark");
    EXPECT_EQ(app.settings.chatSendProtocol.getEnum(),ChatSendProtocol::IRC);
}
