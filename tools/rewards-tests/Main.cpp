// SPDX-License-Identifier: MIT
// Actual IRC parser and chat rendering; synthetic data, no provider startup.
#include "controllers/accounts/AccountController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Resources.hpp"
#include "singletons/WindowManager.hpp"
#include "util/VectorMessageSink.hpp"
#include "widgets/helper/ChannelView.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QImage>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

using namespace chatterino;
class OfflineApp : public mock::BaseApplication
{
public:
    explicit OfflineApp(const QString &customTheme = {})
        : mock::BaseApplication(QStringLiteral("{}"), false, customTheme)
        , windows(this->args, this->paths_, this->settings, this->theme,
                  this->fonts)
    {
        this->settings.disableSave();
    }
    IUserDataController *getUserData() override
    {
        return &this->users;
    }
    SeventvPaints *getSeventvPaints() override
    {
        return &this->paints;
    }
    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }
    SeventvPaints paints;
    mock::EmoteController emotes;
    WindowManager *getWindows() override
    {
        return &this->windows;
    }
    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }
    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }
    PubSub *getTwitchPubSub() override
    {
        return &this->pubSub;
    }
    ILogging *getChatLogger() override
    {
        return &this->logging;
    }
    struct NoLog : ILogging {
        void addMessage(const QString &, MessagePtr, const QString &,
                        const QString &) override
        {
        }
        void closeChannel(const QString &, const QString &) override
        {
        }
    } logging;
    PubSub pubSub{QStringLiteral("wss://127.0.0.1:9")};
    mock::MockTwitchIrcServer twitch;
    WindowManager windows;
    HotkeyController hotkeys;
    AccountController accounts;
    mock::UserDataController users;
};

#include "common/UiLanguage.hpp"
#include "common/network/NetworkManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "widgets/AccountSwitchPopup.hpp"
#include "widgets/settingspages/AccountsPage.hpp"
#include "widgets/dialogs/ChannelPointsDialog.hpp"
#include "widgets/dialogs/WelcomeDialog.hpp"
#include "widgets/helper/ChannelPointsLabel.hpp"
#include "widgets/helper/UsercardResizeGrip.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTableView>
#include <QPushButton>
#include <QMessageBox>
#include <QTemporaryDir>
#include "ChannelPointsChartChecks.hpp"
#include "ChannelPointsStoreChecks.hpp"
namespace chatterino {
class ChannelPointsDialogTestAccess
{
public:
    static void seed(ChannelPointsDialog &d,
                     const QVector<GqlChannelPointReward> &r)
    {
        d.initialFetchDone_ = true;
        d.rewards_ = r;
        d.rebuildContent();
    }
    static void detail(ChannelPointsDialog &d, const GqlChannelPointReward &r)
    {
        d.openRewardDetail(r);
    }
    static void back(ChannelPointsDialog &d)
    {
        d.showRewardsView();
    }
};
}  // namespace chatterino
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication qt(argc, argv);
    const bool chartOnly = argc == 3 && QByteArray(argv[2]) == "--chart-only";
    if (argc != 2 && !chartOnly)
        return 20;
    initResources();
    initializeUiLanguage("ru");
    const auto chartTheme = chartOnly ? qEnvironmentVariable("NOCTIS_CHART_THEME") : QString{};
    OfflineApp app(chartTheme);
    if (qEnvironmentVariableIsSet("NOCTIS_PICKER_UI_SCALE"))
        app.settings.uiScale.setValue(qEnvironmentVariable("NOCTIS_PICKER_UI_SCALE").toFloat());
    if (chartOnly)
    {
        if (!chartTheme.isEmpty())
            app.theme.themeName.setValue(QFileInfo(chartTheme).fileName());
        QJsonArray checks;
        int failures = 0;
        const auto check = [&](bool ok, const QString &name) {
            checks.append(QJsonObject{{"name", name}, {"pass", ok}});
            failures += !ok;
        };
        preview::checkChannelPointsStore(check);
        const auto reportPath = QString::fromLocal8Bit(argv[1]);
        preview::checkChannelPointsChart(QFileInfo(reportPath).dir().filePath("chart.png"), check);
        QFile out(reportPath);
        if (!out.open(QIODevice::WriteOnly)) return 21;
        out.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failed", failures},
                                            {"passed", checks.size() - failures}}).toJson());
        markAppAboutToQuit();
        return failures ? 1 : 0;
    }
    auto channel = std::make_shared<TwitchChannel>("offline-rewards");
    channel->setChannelPointBalance(1000);
    // Real popups own themselves and can close on focus loss. Keep the
    // fixture on the heap, just as the normal showDialog entry point does.
    QPointer<ChannelPointsDialog> catalog = new ChannelPointsDialog(channel.get(), nullptr);
    auto &dialog = *catalog;
    QVector<GqlChannelPointReward> rewards;
    for (int i = 1; i <= 8; ++i)
    {
        GqlChannelPointReward r;
        r.id = QString::number(i);
        r.title = QStringList{
            "Выбрать музыку",
            "Обнять чат",
            "Сообщение стримеру",
            "Показать кота",
            "Пить воду",
            "Заказать игру",
            "Приветствие",
            "Длинное название награды для проверки переноса"}[i - 1];
        r.prompt = i == 1 ? "любимая песня" : "Описание награды";
        r.cost = i * 250;
        r.isEnabled = true;
        r.isInStock = true;
        r.backgroundColor = QStringList{"#c53030", "#308e56", "#426bbe", "#a970ff"}[(i - 1) % 4];
        rewards.push_back(r);
    }
    auto hidden = rewards[0];
    hidden.id = "disabled";
    hidden.isEnabled = false;
    rewards.push_back(hidden);
    ChannelPointsDialogTestAccess::seed(dialog, rewards);

    dialog.show();
    qt.processEvents();
    auto cards = [&] {
        return dialog.findChildren<QPushButton *>(
            "ChannelPointsRewardCardButton");
    };
    QJsonArray checks;
    int failures = 0;
    auto check = [&](bool ok, QString name) {
        checks.append(QJsonObject{{"name", name}, {"pass", ok}});
        failures += !ok;
    };
    check(ChannelPointsLabel::formatBalance(150694) == "150,6к" &&
              ChannelPointsLabel::formatBalance(999) == "999" &&
              ChannelPointsLabel::formatBalance(1000) == "1к" &&
              ChannelPointsLabel::formatBalance(1'240'000) == "1,2м" &&
              ChannelPointsLabel::formatBalance(-1) == "...",
          "Russian compact balance truncates safely");
    initializeUiLanguage("en");
    check(ChannelPointsLabel::formatBalance(150694) == "150.6k" &&
              ChannelPointsLabel::formatBalance(1'240'000) == "1.2m",
          "English compact balance uses English decimal and suffix");
    initializeUiLanguage("ru");
    {
        NetworkManager::init();
        QTemporaryDir icons;
        QImage red(12, 12, QImage::Format_RGB32);
        red.fill(Qt::red);
        QImage green(12, 12, QImage::Format_RGB32);
        green.fill(Qt::green);
        const auto redPath = icons.filePath("red.png");
        const auto greenPath = icons.filePath("green.png");
        red.save(redPath);
        green.save(greenPath);
        ChannelPointsLabel pill;
        pill.setFont(app.getFonts()->getFont(FontStyle::ChatMediumBold, 1.2F));
        pill.setText(ChannelPointsLabel::formatBalance(150694));
        pill.resize(pill.sizeHint());
        auto hasColor = [&](QColor color) {
            const auto pixels = pill.grab().toImage();
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x)
                    if (pixels.pixelColor(x, y) == color)
                        return true;
            return false;
        };
        auto awaitColor = [&](QColor color) {
            QElapsedTimer elapsed;
            elapsed.start();
            while (!hasColor(color) && elapsed.elapsed() < 3000)
            {
                QEventLoop events;
                QTimer::singleShot(20, &events, &QEventLoop::quit);
                events.exec();
            }
            return hasColor(color);
        };
        pill.setIconUrl(QUrl::fromLocalFile(redPath).toString());
        check(awaitColor(Qt::red), "custom red channel points icon is painted");
        pill.setIconUrl(QUrl::fromLocalFile(greenPath).toString());
        check(awaitColor(Qt::green) && !hasColor(Qt::red),
              "switching channels paints the new custom icon");
        pill.setIconUrl({});
        check(!hasColor(Qt::green), "channel without icon falls back to default coin");
        // The catalog is an auto-closing popup; use a child control so
        // exercising a visible pill does not deactivate its parent.
        pill.setParent(&dialog);
        pill.show();
        qt.processEvents();
        auto *animation = pill.findChild<QVariantAnimation *>();
        pill.setBalance(150694, "channel-a:account-a");
        check(animation && animation->state() == QAbstractAnimation::Stopped,
              "first balance is displayed immediately");
        pill.setBalance(151694, "channel-a:account-a");
        check(animation && animation->state() == QAbstractAnimation::Running,
              "change within same channel and account animates");
        pill.setBalance(1275, "channel-b:account-a");
        check(animation && animation->state() == QAbstractAnimation::Stopped &&
                  pill.text() == "1,2к",
              "switching channels snaps to correct balance without animating");
        pill.setBalance(1336, "channel-b:account-b");
        check(animation && animation->state() == QAbstractAnimation::Stopped &&
                  pill.text() == "1,3к",
              "switching accounts snaps to correct balance");
        pill.setBalance(1400, "channel-b:account-b");
        pill.setStatusText("...");
        check(animation && animation->state() == QAbstractAnimation::Stopped &&
                  pill.text() == "...",
              "loading cancels active points animation");
        NetworkManager::deinit();
    }
    check(cards().size() == 8, "enabled supported rewards only");
    check(cards().front()->accessibleName().startsWith(rewards[0].title),
          "ascending price");
    check(!dialog.findChild<QLineEdit *>("ChannelPointsRewardSearch") &&
              !dialog.findChild<QComboBox *>("ChannelPointsRewardSort") &&
              !dialog.findChild<QCheckBox *>("ChannelPointsAffordableOnly"),
          "filter controls removed");
    auto catalogSize = dialog.size();
    check(catalogSize.width() == qRound(368 * dialog.scale()) &&
              catalogSize.height() == qRound(436 * dialog.scale()),
          "larger initial catalog");
    auto *avatar = dialog.findChild<QLabel *>("ChannelIdentityAvatar");
    auto *channelLabel = dialog.findChild<QLabel *>("ChannelPointsHeaderChannel");
    auto *heading = dialog.findChild<QLabel *>("ChannelPointsHeaderTitle");
    check(avatar && avatar->isVisible() && channelLabel && heading &&
              channelLabel->text() == channel->getName() &&
              heading->geometry().right() < avatar->geometry().left() &&
              avatar->geometry().right() < channelLabel->geometry().left(),
          "channel avatar between heading and channel name");
    auto *grip = static_cast<UsercardResizeGrip *>(
        dialog.findChild<QWidget *>("UsercardResizeGrip"));
    const auto drag = [&](QPoint point, QPoint delta) {
        const auto global = grip->mapToGlobal(point);
        QMouseEvent press(QEvent::MouseButtonPress, point, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &press);
        QMouseEvent move(QEvent::MouseMove, point + delta, global + delta,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, point + delta, global + delta,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &release);
    };
    check(grip && dialog.minimumWidth() < dialog.maximumWidth() &&
              dialog.minimumHeight() < dialog.maximumHeight(),
          "production popup has free resize constraints");
    if (!grip)
        return 22;
    drag(QPoint(grip->width() - 2, grip->height() - 2), QPoint(45, 60));
    check(dialog.size() == catalogSize + QSize(45, 60), "catalog corner drag resizes both axes");
    catalogSize = dialog.size();
    channel->setChannelPointBalance(500);
    qt.processEvents();
    check(cards().size() == 8, "balance does not hide rewards");
    bool shortage = false;
    for (auto *c : cards())
        shortage |= c->toolTip().contains("Не хватает");
    check(shortage, "shortage tooltip");
    ChannelPointsDialogTestAccess::detail(dialog, rewards[0]);
    check(dialog.findChild<QPushButton *>("ChannelPointsUtilityButton")
              ->isHidden(),
          "refresh hidden in detail");
    qt.processEvents();
    QEventLoop settle;
    QTimer::singleShot(90, &settle, &QEventLoop::quit);
    settle.exec();
    auto *area = dialog.findChild<QScrollArea *>("ChannelPointsScrollArea");
    auto *button = dialog.findChild<QPushButton *>("ChannelPointsRedeemButton");
    auto *prompt = dialog.findChild<QLabel *>("ChannelPointsRewardPromptLabel");
    check(area->verticalScrollBar()->maximum() == 0 && button->height() >= button->sizeHint().height()
              && prompt->height() >= prompt->fontMetrics().height(),
          "description and action fit without scrolling");
    auto *balanceAfter = dialog.findChild<QLabel *>("ChannelPointsRewardRemainder");
    check(balanceAfter && balanceAfter->text().endsWith("250"), "projected points balance");
    const auto detailFont = app.getFonts()->getFont(FontStyle::ChatMedium, dialog.scale() * 1.06F);
    check(prompt && balanceAfter && prompt->font().family() == detailFont.family() &&
              balanceAfter->font().family() == detailFont.family() &&
              prompt->fontMetrics().height() == QFontMetrics(detailFont).height() &&
              balanceAfter->fontMetrics().height() == QFontMetrics(detailFont).height(),
          "description and balance use larger shared chat font");
    check(dialog.height() < catalogSize.height(), "detail fits selected reward");
    check(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + "-detail.png"), "detail screenshot");
    const auto detailSize = dialog.size();
    drag(QPoint(grip->width() - 2, grip->height() - 2), QPoint(30, 70));
    QTimer::singleShot(90, &settle, &QEventLoop::quit);
    settle.exec();
    check(dialog.size() == detailSize + QSize(30, 70), "detail manual size survives layout refresh");
    channel->setChannelPointBalance(100);
    qt.processEvents();
    QTimer::singleShot(90, &settle, &QEventLoop::quit);
    settle.exec();
    check(dialog.height() == detailSize.height() + 70, "detail manual height survives balance refresh");
    check(!dialog.findChild<QPushButton *>("ChannelPointsRedeemButton")->isEnabled(),
          "balance change disables unaffordable reward");
    ChannelPointsDialogTestAccess::back(dialog);
    check(cards().size() == 8, "back restores catalog");
    check(dialog.height() == catalogSize.height(), "back restores list size");
    ChannelPointsDialogTestAccess::detail(dialog, rewards[0]);
    check(dialog.height() == detailSize.height() + 70, "reopening detail restores manual height");
    ChannelPointsDialogTestAccess::back(dialog);
    auto bits = rewards[0];
    bits.id = "bits";
    bits.title = "Gigantify";
    bits.isAutomatic = true;
    bits.rewardType = "SEND_GIGANTIFIED_EMOTE";
    bits.pricingType = "BITS";
    bits.cost = 1;
    app.settings.enableGigantifyEmotes = true;
    rewards.push_back(bits);
    ChannelPointsDialogTestAccess::seed(dialog, rewards);
    check(cards().back()->accessibleName().startsWith("Gigantify"),
          "Bits remain separate");
    ChannelPointsDialogTestAccess::detail(dialog, bits);
    check(!dialog.findChild<QLabel *>("ChannelPointsRewardRemainder"), "Bits never subtracted from points");
    ChannelPointsDialogTestAccess::back(dialog);
    channel->setChannelPointBalance(-1);
    check(cards().size() == 9, "unknown balance keeps all rewards");
    qt.processEvents();
    check(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + ".png"),
          "catalog screenshot");
    dialog.resize(330, 510);
    qt.processEvents();
    check(dialog.grab().save(QString::fromLocal8Bit(argv[1]) + "-compact.png"),
          "compact screenshot");
    std::weak_ptr<TwitchChannel> originalChannel = channel;
    channel.reset();
    check(!originalChannel.expired(), "reward catalog keeps its channel alive after the source split changes");
    dialog.close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(catalog.isNull() && originalChannel.expired(), "closing the reward catalog releases its channel");
    qt.processEvents();
    // A disposable offline welcome dialog with the Noctis palette. No
    // account import, login or production service is started by this fixture.
    auto *theme = app.getThemes();
    theme->window.background = QColor("#101015");
    theme->window.text = QColor("#e0dfe7");
    theme->splits.input.background = QColor("#18171f");
    theme->splits.header.border = QColor("#1b1a23");
    theme->accent = QColor("#8573c0");
    qt.setWindowIcon(QIcon(":/icon.ico"));
    for (const auto &language : {QStringLiteral("ru"), QStringLiteral("en")})
    {
        initializeUiLanguage(language);
        QWidget parent;
        showWelcomeDialog(&parent);
        qt.processEvents();
        auto *welcome = parent.findChild<QDialog *>("NoctisWelcomeDialog");
        check(welcome && welcome->isVisible() &&
                  welcome->styleSheet().contains("#101015") &&
                  welcome->styleSheet().contains("#8573c0"),
              "welcome uses Noctis theme palette " + language);
        if (!welcome)
        {
            continue;
        }
        auto *start = welcome->findChild<QPushButton *>("welcomeDone");
        auto *steps = welcome->findChild<QScrollArea *>("welcomeSteps");
        auto *welcomeHeading = welcome->findChild<QLabel *>("welcomeHeading");
        check(welcomeHeading && welcomeHeading->text() == uiText("Welcome to Chatterino Noctis") &&
                  start && start->text() == uiText("Start") &&
                  steps && steps->widget()->findChildren<QWidget *>("welcomeStep").size() == 4,
              "welcome preserves localized headings and all setup steps " + language);
        check(welcome->grab().save(QString::fromLocal8Bit(argv[1]) +
                                  "-welcome-" + language + ".png"),
              "welcome screenshot " + language);
        if (steps)
        {
            const auto rows = steps->widget()->findChildren<QWidget *>("welcomeStep");
            steps->verticalScrollBar()->setValue(steps->verticalScrollBar()->maximum());
            qt.processEvents();
            check(!rows.isEmpty() &&
                      rows.last()->mapTo(steps->viewport(), rows.last()->rect().bottomLeft()).y() <
                          steps->viewport()->height(),
                  "welcome last step is reachable on a small screen " + language);
        }
        if (start)
        {
            start->click();
            qt.processEvents();
            check(!parent.findChild<QDialog *>("NoctisWelcomeDialog") ||
                      !parent.findChild<QDialog *>("NoctisWelcomeDialog")->isVisible(),
                  "welcome start button closes dialog " + language);
        }
    }
    {
        auto &accounts = app.getAccounts()->twitch;
        auto first = std::make_shared<TwitchAccount>("case_fixture", "", "", "900020");
        auto second = std::make_shared<TwitchAccount>("second_fixture", "", "", "900021");
        first->setDisplayName("900020", "case_fixture", "Case_Fixture");
        accounts.accounts.insert(first);
        accounts.accounts.insert(second);
        for (const auto &language : {QStringLiteral("ru"), QStringLiteral("en")})
        {
            initializeUiLanguage(language);
            AccountSwitchPopup popup;
            popup.refresh();
            popup.show();
            qt.processEvents();
            auto *list = popup.findChild<QListWidget *>("TwitchAccountList");
            auto *manage = popup.findChild<QPushButton *>("ManageAccountsButton");
            check(list && list->count() == 3 && list->item(0)->text() == uiText("Anonymous") &&
                      list->item(0)->data(Qt::UserRole).toString().isEmpty(),
                  "account switch uses localized anonymous row " + language);
            if (!list || list->count() != 3)
            {
                continue;
            }
            check(list->item(1)->text() == "Case_Fixture" &&
                      list->item(1)->data(Qt::UserRole).toString() == "case_fixture",
                  "account switch preserves case separately from login " + language);
            second->setDisplayName("900021", "second_fixture", "Second_Fixture");
            accounts.userListUpdated.invoke();
            check(list->item(2)->text() == "Second_Fixture",
                  "account switch refreshes a background account name " + language);
            check(manage && popup.height() < qRound(145 * popup.scale()) &&
                      list->visualItemRect(list->item(2)).bottom() < list->viewport()->height() &&
                      manage->geometry().top() - list->viewport()->mapTo(
                          &popup, list->visualItemRect(list->item(2)).bottomLeft()).y() <
                          qRound(24 * popup.scale()),
                  "three accounts fit a compact popup " + language);
            list->setCurrentRow(1);
            check(popup.grab().save(QString::fromLocal8Bit(argv[1]) +
                                    "-accounts-" + language + ".png"),
                  "account switch screenshot " + language);
            // Offline manager has not been loaded, so this only changes the
            // synthetic currentUsername setting (no OAuth or IRC activity).
            list->itemClicked(list->item(1));
            check(accounts.currentUsername.getValue() == "case_fixture",
                  "account click selects original login " + language);
            list->itemClicked(list->item(0));
            check(accounts.currentUsername.getValue().isEmpty(),
                  "anonymous click selects empty login " + language);
            popup.hide();
        }
        {
            initializeUiLanguage("ru");
            AccountsPage page;
            page.resize(600, 420);
            page.show();
            qt.processEvents();
            auto *table = page.findChild<QTableView *>("NoctisAccountsTable");
            check(table && !table->showGrid() && table->model()->rowCount() == 3 &&
                      table->model()->index(1, 0).data().toString() == "Case_Fixture" &&
                      table->model()->index(2, 0).data().toString() == "Second_Fixture",
                  "settings accounts use theme and verified names");
            check(page.grab().save(QString::fromLocal8Bit(argv[1]) + "-account-settings.png"),
                  "account settings screenshot");
        }
        for (int i = 0; i < 10; ++i)
        {
            accounts.accounts.insert(std::make_shared<TwitchAccount>(
                QStringLiteral("extra_fixture_%1").arg(i), "", "",
                QString::number(900030 + i)));
        }
        AccountSwitchPopup longListPopup;
        longListPopup.refresh();
        longListPopup.show();
        qt.processEvents();
        auto *longList = longListPopup.findChild<QListWidget *>("TwitchAccountList");
        check(longList && longList->count() == 13 &&
                  longList->verticalScrollBar()->maximum() > 0,
              "long accounts list scrolls");
        if (longList && longList->count() > 0)
        {
            auto *last = longList->item(longList->count() - 1);
            longList->scrollToItem(last, QAbstractItemView::PositionAtBottom);
            qt.processEvents();
            check(longList->visualItemRect(last).bottom() < longList->viewport()->height(),
                  "last account remains reachable");
        }
        longListPopup.hide();
    }
    {
        // A directory at the last theme target makes QSaveFile fail on every
        // platform, after both settings and an earlier theme have committed.
        QTemporaryDir incoming;
        const QDir profile(incoming.path());
        const QDir local(app.getPaths().settingsDirectory);
        const QDir localThemes(app.getPaths().themesDirectory);
        const auto put = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        const auto read = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        const auto runImport = [&] {
            QString warning;
            QTimer::singleShot(0, &qt, [&warning] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                {
                    warning = box->text();
                    box->accept();
                }
            });
            importSettingsForTesting(profile.filePath("Settings"));
            return warning;
        };
        const auto backups = [&] {
            return local.entryList({QStringLiteral("before-import-*")}, QDir::Dirs);
        };
        const QByteArray oldSettings = R"({"fixture":"previous"})";
        const auto target = local.filePath("settings.json");
        const bool setup = incoming.isValid() && profile.mkdir("Settings") &&
            profile.mkdir("Themes") && localThemes.mkpath(".") &&
            localThemes.mkdir("zz-conflict.json") &&
            put(profile.filePath("Themes/aa-new.json"), "{}") &&
            put(profile.filePath("Themes/zz-conflict.json"), "{}") &&
            put(profile.filePath("Settings/settings.json"), R"({"fixture":"incoming"})") &&
            put(profile.filePath("Settings/user-data.json"), "{}") &&
            put(target, "invalid json");
        check(setup, "prepare isolated import failure");
        if (setup)
        {
            const auto before = backups().size();
            check(runImport() == uiText("This file could not be read: %1")
                                  .arg(QDir::toNativeSeparators(target)) &&
                      read(target) == "invalid json" && backups().size() == before,
                  "import preserves an unreadable settings document");
            check(put(target, oldSettings), "seed previous valid settings");
            for (int attempt = 1; attempt <= 2; ++attempt)
            {
                const auto warning = runImport();
                check(warning == uiText("The settings could not be saved; the previous "
                                        "ones were put back.") &&
                          read(target) == oldSettings && !local.exists("user-data.json") &&
                          !localThemes.exists("aa-new.json") &&
                          QFileInfo(localThemes.filePath("zz-conflict.json")).isDir() &&
                          backups().size() == before + attempt,
                      QStringLiteral("failed theme rolls back the profile with a unique backup (%1)")
                          .arg(attempt));
            }
        }
    }
    QFile out(QString::fromLocal8Bit(argv[1]));
    if (!out.open(QIODevice::WriteOnly))
        return 21;
    out.write(QJsonDocument(QJsonObject{{"checks", checks},
                                        {"failed", failures},
                                        {"passed", checks.size() - failures}})
                  .toJson());
    markAppAboutToQuit();
    return failures ? 1 : 0;
}
