// SPDX-License-Identifier: MIT
// Offline real-widget verification. Never loads accounts or starts providers.
#include "common/UiLanguage.hpp"
#include "common/WindowDescriptors.hpp"
#include "singletons/UpdateCheckPolicy.hpp"
#include "singletons/Updates.hpp"
#include "common/network/NetworkManager.hpp"
#include "messages/LimitedQueue.hpp"
#include "messages/MessageBuilder.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/MenuStyle.hpp"
#include "widgets/helper/UpdateBanner.hpp"
#include "widgets/dialogs/ReplyThreadPopup.hpp"
#include "messages/MessageThread.hpp"
#include "widgets/Scrollbar.hpp"
#include "common/Version.hpp"
#include "common/Mist1XVersion.hpp"
#include "controllers/tray/TrayController.hpp"
#include "widgets/Window.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/buttons/TitlebarButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/Label.hpp"
#include <QLayout>
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/filters/lang/Tokenizer.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/ffz/FfzRoomBatch.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/WhisperHistory.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Resources.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/EditHotkeyDialog.hpp"
#include "widgets/dialogs/ChannelFilterEditorDialog.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/settingspages/CommandPage.hpp"
#include "widgets/settingspages/FiltersPage.hpp"
#include "widgets/settingspages/GeneralPage.hpp"
#include "widgets/settingspages/HighlightingPage.hpp"
#include "widgets/settingspages/IgnoresPage.hpp"
#include "widgets/settingspages/KeyboardSettingsPage.hpp"
#include "widgets/settingspages/NoctisPage.hpp"
#include "widgets/settingspages/ModerationPage.hpp"
#include "widgets/settingspages/MoltorinoPage.hpp"
#include "widgets/settingspages/AccountsPage.hpp"
#include "widgets/settingspages/SettingsPage.hpp"
#include "widgets/settingspages/ExternalToolsPage.hpp"
#include "widgets/settingspages/AboutPage.hpp"
#include "widgets/settingspages/NicknamesPage.hpp"
#include "widgets/settingspages/TechnorinoPage.hpp"
#include "widgets/settingspages/UiComboBox.hpp"
#include "widgets/helper/SettingsDialogTab.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QInputDialog>
#include <QHeaderView>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QCheckBox>
#include <QStyleOptionComboBox>
#include <QScreen>
#ifdef USEWINSDK
#include <Windows.h>
#endif
#include <QMenu>
#include <QSystemTrayIcon>
#include <QLineEdit>
#include <QLocale>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QPushButton>
#include <QDialog>
#include <QTextEdit>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>

using namespace chatterino;
namespace {
class OfflineApp final : public mock::BaseApplication
{
public:
    class TestSound final : public ISoundController
    {
    public:
        void play(const QUrl &sound) override { last = sound; ++plays; }
        QUrl last;
        int plays = 0;
    };
    explicit OfflineApp(bool legacyFontStartup = false, const QString &customThemeFile = {})
        : mock::BaseApplication(legacyFontStartup
              ? QStringLiteral(R"({"appearance":{"currentFontFamily":"Mist Montserrat","currentFontSize":10,"currentFontWeight":600,"uiScale2":1.3}})")
              : QStringLiteral("{}"), false, customThemeFile)
        , windows(this->args, this->paths_, this->settings, this->theme, this->fonts)
        , commands(this->paths_)
        , crash(this->paths_)
    {
        this->settings.disableSave();
        this->paths_.settingsDirectory = this->settingsDir.path();
    }
    WindowManager *getWindows() override { return &this->windows; }
    AccountController *getAccounts() override { return &this->accounts; }
    HotkeyController *getHotkeys() override { return &this->hotkeys; }
    NotificationController *getNotifications() override
    {
        if (!this->notifications)
            this->notifications = std::make_unique<NotificationController>();
        return this->notifications.get();
    }
    CommandController *getCommands() override { return &this->commands; }
    EmoteController *getEmotes() override { return &this->emotes; }
    ISoundController *getSound() override { return &this->sound; }
    ITwitchIrcServer *getTwitch() override { return &this->twitch; }
    BttvEmotes *getBttvEmotes() override { return &this->bttv; }
    FfzEmotes *getFfzEmotes() override { return &this->ffz; }
    SeventvEmotes *getSeventvEmotes() override { return &this->seventv; }
    IUserDataController *getUserData() override { return &this->userData; }
    SeventvPaints *getSeventvPaints() override { return &this->paints; }
    SpellChecker *getSpellChecker() override { return &this->spellChecker; }
    CrashHandler *getCrashHandler() override { return &this->crash; }
    WindowManager windows;
    AccountController accounts;
    HotkeyController hotkeys;
    std::unique_ptr<NotificationController> notifications;
    CommandController commands;
    mock::EmoteController emotes;
    TestSound sound;
    mock::MockTwitchIrcServer twitch;
    mock::UserDataController userData;
    SeventvPaints paints;
    BttvEmotes bttv;
    FfzEmotes ffz;
    SeventvEmotes seventv;
    SpellChecker spellChecker;
    CrashHandler crash;
};
QJsonArray checks;
int failures = 0;
void check(bool value, const QString &name)
{
    checks.append(QJsonObject{{"name",name},{"pass",value}});
    if (!value)++failures;
}
void checkChatReadability(Split &split, float scale)
{
    const auto &view = split.getChannelView();
    const auto body = getApp()->getFonts()->getFont(FontStyle::ChatMedium, view.scale());
    const auto timestamp = getApp()->getFonts()->getFont(FontStyle::TimestampMedium, view.scale());
    const auto *edit = split.getInput().findChild<QTextEdit *>();
    const auto *header = split.findChild<SplitHeader *>();
    const auto label = QStringLiteral(" at %1x").arg(scale);
    const auto lineHeight = QFontMetrics(body).height();
    const bool chatReadable = body.pointSizeF() >= 7.5 && timestamp.pointSizeF() >= 7.5 &&
                              view.height() >= lineHeight;
    check(chatReadable,
          "readable message and timestamp with room for a line" + label);
    if (!chatReadable)
        qWarning() << "Unreadable chat at zoom" << scale << "viewport" << view.size()
                   << "font line height" << lineHeight << "split" << split.size()
                   << "composer" << split.getInput().size();
    check(edit && edit->font().family() == body.family() &&
              edit->font().pointSizeF() == body.pointSizeF() &&
              edit->viewport()->height() >= edit->fontMetrics().height(),
          "composer displays a full line of the current chat font" + label);
    check(header && header->height() >= header->fontMetrics().height(),
          "channel header has room for its text" + label);
}
void checkTrayLanguage(QApplication &qt, OfflineApp &app, const QString &output,
                      const QString &language)
{
    TrayController tray(app.windows);
    auto *icon = tray.findChild<QSystemTrayIcon *>();
    check(icon && icon->toolTip() == "Chatterino Noctis", language + " real tray tooltip");
    if (!icon || !icon->contextMenu())
    {
        check(false, language + " real tray menu exists");
        return;
    }
    check(icon->icon().pixmap(256).toImage() == qt.windowIcon().pixmap(256).toImage(),
          language + " tray artwork matches application icon");
    auto *menu = icon->contextMenu();
    const auto actions = menu->actions();
    check(actions.size() >= 2 &&
              actions.first()->text() == uiText("Open Chatterino Noctis") &&
              actions.last()->text() == uiText("Quit Chatterino Noctis"),
          language + " real tray open and quit branding");
    menu->setAttribute(Qt::WA_DontShowOnScreen, true);
    check(menu->grab().save(output + "/" + language + "-tray-menu.png"),
          language + " real tray menu rendered");
}

void checkCustomScrollSpeed(QComboBox &combo, OfflineApp &app)
{
    const auto original = app.settings.mouseScrollMultiplier.getValue();
    const int customRow = combo.findText(uiText("Custom value…"));
    check(customRow >= 0, "scroll speed offers explicit custom input");
    if (customRow >= 0)
    {
        const auto originalText = combo.currentText();
        for (const bool confirm : {false, true})
        {
            QTimer::singleShot(0, [confirm] {
                for (auto *window : QApplication::topLevelWidgets())
                {
                    if (auto *dialog = qobject_cast<QInputDialog *>(window);
                        dialog && dialog->isVisible())
                    {
                        dialog->setTextValue("1.37");
                        if (confirm)
                        {
                            dialog->accept();
                        }
                        else
                        {
                            dialog->reject();
                        }
                        break;
                    }
                }
            });
            combo.setCurrentIndex(customRow);
            combo.activated(customRow);
            QApplication::processEvents();
            if (!confirm)
            {
                check(app.settings.mouseScrollMultiplier.getValue() == original &&
                          combo.currentText() == originalText,
                      "cancelling custom scroll speed preserves the preset");
            }
        }
        check(qFuzzyCompare(app.settings.mouseScrollMultiplier.getValue(), 1.37F) &&
                  combo.currentText() == "1.37x" && !combo.lineEdit(),
              "custom scroll speed is stored and shown without an inline editor");
        app.settings.mouseScrollMultiplier.setValue(original);
    }
}

bool isSettingsTopic(const SettingsDialogTab *tab)
{
    return QStringList{uiText("Appearance"), uiText("Chat"),
                       uiText("Chat input"), uiText("Emotes and badges"),
                       uiText("User cards"), uiText("Links and search"),
                       uiText("Translation"), uiText("Streamer Mode"),
                       uiText("Moderation tools"), uiText("System settings"),
                       uiText("YouTube and Kick"), uiText("Fun"),
                       uiText("Sign-in for extra features")}.contains(tab->name());
}

void checkSettingsSelectors(QWidget &widget, OfflineApp &app, const QString &output)
{
    int inspected = 0;
    bool checkedCustomSpeed = false;
    for (auto *tab : widget.findChildren<SettingsDialogTab *>())
    {
        // Topic pages contain the preset selectors. Open each page
        // through its normal keyboard path to measure the real layout.
        if (!isSettingsTopic(tab))
        {
            continue;
        }
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(tab, &enter);
        QApplication::processEvents();
        for (auto *combo : widget.findChildren<QComboBox *>())
        {
            if (!combo->isVisibleTo(&widget) || combo->currentText().isEmpty())
            {
                continue;
            }
            QStyleOptionComboBox opt;
            opt.initFrom(combo);
            opt.editable = combo->isEditable();
            const int available = combo->style()->subControlRect(
                QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, combo).width();
            const auto label = tab->name() + " / " + combo->currentText();
            check(combo->fontMetrics().horizontalAdvance(combo->currentText()) <= available,
                  "selected option fits at 130 percent: " + label);
            QString widest;
            int widestWidth = 0;
            for (int row = 0; row < combo->count(); ++row)
            {
                const auto text = combo->itemText(row);
                const int width = combo->fontMetrics().horizontalAdvance(text);
                if (width > widestWidth)
                {
                    widest = text;
                    widestWidth = width;
                }
            }
            check(widestWidth <= available,
                  "longest preset fits at 130 percent: " + tab->name() + " / " + widest);
            if (combo->findData("Default", UiComboBox::SourceRole) >= 0 ||
                combo->findData("No limit", UiComboBox::SourceRole) >= 0)
            {
                check(!combo->isEditable() && !combo->lineEdit(),
                      "preset has no text editor: " + label);
            }
            if (combo->findData("0.75x", UiComboBox::SourceRole) >= 0 &&
                combo->findData("1.5x", UiComboBox::SourceRole) >= 0 &&
                combo->findData("1.25x", UiComboBox::SourceRole) < 0 &&
                !checkedCustomSpeed && tab->name() == uiText("Chat"))
            {
                checkCustomScrollSpeed(*combo, app);
                for (auto *scroll : widget.findChildren<QScrollArea *>())
                {
                    if (scroll->widget() && scroll->widget()->isAncestorOf(combo))
                    {
                        scroll->ensureWidgetVisible(combo, 0, 100);
                        QApplication::processEvents();
                        check(widget.grab().save(output + "/ru-chat-presets-130.png"),
                              "rendered scrolling presets in the Chat settings");
                        break;
                    }
                }
                checkedCustomSpeed = true;
            }
            ++inspected;
        }
        for (auto *scroll : widget.findChildren<QScrollArea *>("generalSettingsScrollContent"))
        {
            if (scroll->isVisibleTo(&widget))
            {
                check(scroll->horizontalScrollBar()->maximum() == 0,
                      "compact topic fits horizontally: " + tab->name());
            }
        }
    }
    check(inspected > 25, "checked selectors across the compact settings pages");
    check(checkedCustomSpeed, "located scroll speed on its settings page");
}

void checkInitialSettingsHighlightFit(QWidget &widget, const QString &output)
{
    const auto tabs = widget.findChildren<SettingsDialogTab *>();
    const auto highlights = std::ranges::find_if(tabs, [](auto *tab) {
        return tab->name() == uiText("Highlights");
    });
    if (highlights == tabs.end())
    {
        check(false, "default settings finds Highlights");
        return;
    }
    const auto initialSize = widget.size();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(*highlights, &enter);
    QEventLoop settle;
    QTimer::singleShot(30, &settle, &QEventLoop::quit);
    settle.exec();
    auto *page = (*highlights)->createdPage();
    auto *subtabs = page ? page->findChild<QTabWidget *>() : nullptr;
    auto *table = subtabs ? subtabs->currentWidget()->findChild<QTableView *>() : nullptr;
    auto *helpLink = subtabs ? subtabs->currentWidget()->findChild<QLabel *>("settingsRegexHelp") : nullptr;
    check(table && helpLink, "default Highlights table and help link exist");
    if (!table || !helpLink)
    {
        return;
    }
    const auto available = widget.screen()->availableGeometry().size();
    check(widget.size() == initialSize && widget.width() <= available.width() &&
              widget.height() <= available.height(),
          "initial Highlights page keeps the screen-bounded dialog size");
    if (available.width() >= 1280)
    {
        check(table->horizontalHeader()->length() <= table->viewport()->width() &&
                  table->horizontalScrollBar()->maximum() == 0,
              "all highlight columns fit at initial 1.3x settings size on a wide screen");
        check(helpLink->heightForWidth(helpLink->width()) <=
                  helpLink->fontMetrics().height() + 6,
              "regex help fits on one line at initial 1.3x settings size");
    }
    else
    {
        check(table->horizontalHeader()->length() <= table->viewport()->width() ||
                  table->horizontalScrollBar()->maximum() > 0,
              "highlight controls remain accessible on a small screen");
    }
    check(widget.grab().save(output + "/settings-highlights-initial-130.png"),
          "rendered initial Highlights layout without a manual resize");
}

void checkSettingsZoomMatrix(QWidget &widget, OfflineApp &app, const QString &output)
{
    const auto tabs = widget.findChildren<SettingsDialogTab *>();
    const auto open = [](SettingsDialogTab *tab) {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(tab, &enter);
        QApplication::processEvents();
    };
    const auto appearance = std::ranges::find_if(tabs, [](auto *tab) {
        return tab->name() == uiText("Appearance");
    });
    if (appearance == tabs.end())
    {
        check(false, "settings matrix finds Appearance");
        return;
    }
    open(*appearance);
    QList<float> scales{1.25F, 1.3F};
    for (auto *combo : widget.findChildren<QComboBox *>())
    {
        if (!combo->isVisibleTo(&widget) ||
            combo->findData("2.66x", UiComboBox::SourceRole) < 0)
            continue;
        for (int row = 0; row < combo->count(); ++row)
        {
            auto entry = combo->itemData(row, UiComboBox::SourceRole).toString();
            if (entry == "Default")
                scales.append(1.F);
            else if (entry.endsWith('x'))
                scales.append(entry.chopped(1).toFloat());
        }
        break;
    }
    std::ranges::sort(scales);
    check(scales.size() >= 18 && scales.front() == .5F && scales.back() == 4.F,
          "matrix covers every Zoom preset and custom fractional zoom");
    auto *search = widget.findChild<QLineEdit *>("settingsSearch");
    auto *ok = widget.findChild<QPushButton *>("settingsOk");
    auto *sidebar = widget.findChild<QScrollArea *>("settingsSidebar");
    if (!search || !ok || !sidebar)
    {
        check(false, "settings matrix finds shell");
        return;
    }
    QJsonArray measurements;
    QFile tableProgress(output + "/settings-table-progress.log");
    tableProgress.open(QIODevice::WriteOnly | QIODevice::Text);
    for (float scale : scales)
    {
        app.settings.uiScale.setValue(scale);
        open(*appearance);
        int combos = 0, buttons = 0, scrollablePages = 0, topicPages = 0;
        QStringList clipped;
        for (auto *tab : tabs)
        {
            if (!isSettingsTopic(tab))
                continue;
            open(tab);
            auto *page = tab->createdPage();
            ++topicPages;
            auto *scroll = page->findChild<QScrollArea *>("generalSettingsScrollContent");
            if (scroll && scroll->verticalScrollBar()->maximum() > 0)
            {
                auto *bar = scroll->verticalScrollBar();
                bar->setValue(bar->maximum());
                scrollablePages += bar->value() == bar->maximum();
                bar->setValue(0);
            }
            for (auto *combo : page->findChildren<QComboBox *>())
            {
                if (!combo->isVisibleTo(&widget) || !combo->count())
                    continue;
                QStyleOptionComboBox opt;
                opt.initFrom(combo);
                opt.editable = combo->isEditable();
                const auto field = combo->style()->subControlRect(QStyle::CC_ComboBox,
                    &opt, QStyle::SC_ComboBoxEditField, combo);
                ++combos;
                for (int row = 0; row < combo->count(); ++row)
                {
                    if (combo->fontMetrics().horizontalAdvance(combo->itemText(row)) > field.width())
                    {
                        clipped.append(tab->name() + " / " + combo->itemText(row));
                        break;
                    }
                }
                if (combo->height() < combo->fontMetrics().height())
                    clipped.append(tab->name() + " / " + combo->currentText());
            }
            for (auto *button : page->findChildren<QPushButton *>())
            {
                if (!button->isVisibleTo(&widget) || button->text().isEmpty())
                    continue;
                ++buttons;
                if (button->fontMetrics().horizontalAdvance(button->text()) > button->width() ||
                    button->fontMetrics().height() > button->height())
                    clipped.append(tab->name() + " / " + button->text());
            }
        }
        int tables = 0;
        const auto checkTables = [&](QWidget *page) {
            for (auto *table : page->findChildren<QTableView *>())
            {
                auto *header = table->horizontalHeader();
                if (!table->isVisibleTo(&widget) || !header->isVisibleTo(&widget) ||
                    header->objectName() != "settingsReadableHeader")
                    continue;
                ++tables;
                for (int column = 0; column < header->count(); ++column)
                {
                    if (header->isSectionHidden(column))
                        continue;
                    const auto text = table->model()->headerData(column, Qt::Horizontal).toString();
                    const auto lines = text.split('\n');
                    for (const auto &line : lines)
                    {
                        if (header->fontMetrics().horizontalAdvance(line) + 10 > header->sectionSize(column))
                            clipped.append(text);
                    }
                    if (header->fontMetrics().height() +
                        (static_cast<int>(lines.size()) - 1) * header->fontMetrics().lineSpacing() > header->height())
                        clipped.append(text);
                }
                if (!header->isSectionHidden(0) && header->sectionSize(0) < 150)
                    clipped.append(QStringLiteral("collapsed first column"));
                for (int row = 0; row < table->model()->rowCount(); ++row)
                {
                    const auto cell = table->model()->index(row, 0);
                    if ((cell.flags() & Qt::ItemIsEditable) ||
                        !cell.data(Qt::CheckStateRole).isValid())
                        continue;
                    const auto words = cell.data().toString().simplified().split(' ');
                    for (const auto &word : words)
                    {
                        // These built-in options have an indicator at the left.
                        if (table->fontMetrics().horizontalAdvance(word) + 26 > header->sectionSize(0))
                            clipped.append(cell.data().toString());
                    }
                }
                if (header->length() > table->viewport()->width())
                {
                    // An initial selection can still have Qt's queued scrollbar
                    // layout pending. Let the real event loop settle and check
                    // the final viewport instead of measuring mid-layout.
                    QEventLoop settle;
                    QTimer::singleShot(15, &settle, &QEventLoop::quit);
                    settle.exec();
                    auto *bar = table->horizontalScrollBar();
                    if (header->length() > table->viewport()->width())
                    {
                        bar->setValue(bar->maximum());
                        if (!bar->isVisibleTo(&widget) || bar->maximum() <= 0 ||
                            bar->value() != bar->maximum())
                            clipped.append(QStringLiteral("table cannot scroll horizontally: %1 (%2 vs %3, range %4)")
                                .arg(table->model()->headerData(0, Qt::Horizontal).toString())
                                .arg(header->length()).arg(table->viewport()->width()).arg(bar->maximum()));
                        bar->setValue(0);
                    }
                }
            }
        };
        for (auto *tab : tabs)
        {
            const auto name = tab->name();
            if (name != uiText("Highlights") && name != uiText("Ignores") &&
                name != uiText("Nicknames") && name != uiText("Live Notifications") &&
                name != uiText("Moderation buttons and logs"))
                continue;
            tableProgress.write(QString("%1x / %2\n").arg(scale).arg(name).toUtf8());
            tableProgress.flush();
            open(tab);
            auto *page = tab->createdPage();
            if (auto *subtabs = page->findChild<QTabWidget *>())
            {
                const int original = subtabs->currentIndex();
                for (int i = 0; i < subtabs->count(); ++i)
                {
                    subtabs->setCurrentIndex(i);
                    QApplication::processEvents();
                    QApplication::processEvents();
                    checkTables(subtabs->widget(i));
                }
                subtabs->setCurrentIndex(0);
                QApplication::processEvents();
                QApplication::processEvents();
                if (name == uiText("Highlights") && scale == 1.3F)
                {
                    check(widget.grab().save(output + "/settings-highlights-130.png"), "rendered highlights table");
                }
                subtabs->setCurrentIndex(original);
            }
            else
            {
                QApplication::processEvents();
                checkTables(page);
            }
        }
        const auto font = app.fonts.getFont(FontStyle::ChatMedium,
            1.F + (std::clamp(scale, 1.F, 1.3F) - 1.F) * .5F);
        auto *bar = sidebar->verticalScrollBar();
        bar->setValue(bar->maximum());
        const bool scrollable = bar->value() == bar->maximum() && bar->maximum() > 0;
        open(*appearance);
        auto captions = sidebar->findChildren<QLabel *>("settingsCaption");
        const bool distinctCaptions = captions.size() == 4 && std::ranges::all_of(captions,
            [search](auto *caption) {
                return caption->font().pointSizeF() < search->font().pointSizeF() &&
                    caption->font().weight() >= QFont::Bold && caption->wordWrap();
            });
        check(topicPages >= 12 && combos > 25 && buttons > 0 && scrollablePages > 0 &&
                  tables >= 9 && distinctCaptions && scrollable && search->font().pointSizeF() == font.pointSizeF() &&
                  ok->font().pointSizeF() == font.pointSizeF() && clipped.isEmpty(),
              QString("readable settings at %1x: %2").arg(scale).arg(clipped.join("; ")));
        measurements.append(QJsonObject{{"zoom", scale}, {"dpiScale", widget.devicePixelRatioF()},
            {"width", widget.width()}, {"height", widget.height()}, {"textPt", search->font().pointSizeF()},
            {"searchHeight", search->height()}, {"buttonHeight", ok->height()},
            {"sidebarWidth", sidebar->width()}, {"sidebarScroll", bar->maximum()},
            {"pages", topicPages}, {"scrollablePages", scrollablePages},
            {"combos", combos}, {"buttons", buttons}, {"tables", tables}, {"captions", distinctCaptions},
            {"clipped", QJsonArray::fromStringList(clipped)}});
        if (scale == .5F || scale == 4.F)
            check(widget.grab().save(output + (scale == .5F ? "/settings-min.png" : "/settings-max.png")),
                  "settings matrix rendered an endpoint");
    }
    QFile result(output + "/settings-zoom-matrix.json");
    check(result.open(QIODevice::WriteOnly) &&
              result.write(QJsonDocument(measurements).toJson()) > 0, "saved settings zoom metrics");
}

QJsonArray texts(QWidget &root)
{
    QJsonArray result;
    for (auto *child : root.findChildren<QWidget *>())
    {
        QString text;
        if (auto *label = qobject_cast<QLabel *>(child))text=label->text();
        else if (auto *button = qobject_cast<QAbstractButton *>(child))text=button->text();
        else if (auto *edit=qobject_cast<QLineEdit *>(child))text=edit->placeholderText();
        if (!text.isEmpty())result.append(QJsonObject{{"class",child->metaObject()->className()},{"text",text},{"tooltip",child->toolTip()}});
        if (auto *combo=qobject_cast<QComboBox *>(child))
        {
            for(int i=0;i<combo->count();++i)
                result.append(QJsonObject{{"class","QComboBox"},{"text",combo->itemText(i)},{"source",combo->itemData(i,UiComboBox::SourceRole).toString()}});
        }
    }
    return result;
}
}

int main(int argc, char **argv)
{
    const bool fontStartup = argc > 2 && QByteArray(argv[2])=="--native-font-hidden";
    const bool captionOnly = fontStartup || (argc > 2 && QByteArray(argv[2])=="--native-caption-hidden");
    qputenv("QT_QPA_PLATFORM",captionOnly?"windows:fontengine=gdi":"offscreen");
    QApplication qt(argc,argv);
    qt.setApplicationName("Chatterino-Noctis-localization-offline-tests");
    qt.setStyle(QStyleFactory::create("Fusion"));
    // The offscreen QPA backend doesn't enumerate Windows' fonts. Seed only
    // this synthetic process; production font selection is unchanged.
    const auto fontId = QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    qt.setFont(QFont("Segoe UI", 9));
    check(fontId >= 0, "offscreen Windows font loaded");
    const auto output = argc > 1 ? QDir::fromNativeSeparators(QString::fromLocal8Bit(argv[1])) : QString();
    if (!QDir::isAbsolutePath(output) || !QDir().mkpath(output))return 21;
    {
        using namespace updatecheck;
        using namespace std::chrono_literals;
        const QJsonArray releases{
            QJsonObject{{"tag_name", "v1.9.9"}},
            QJsonObject{{"tag_name", "v2.1.0-beta.1"}, {"prerelease", true}},
            QJsonObject{{"tag_name", "v9.0.0"}, {"draft", true}},
            QJsonObject{{"tag_name", "v2.0.0"}},
            QJsonObject{{"tag_name", "not-a-version"}},
            QJsonObject{{"tag_name", "v3.0.0-rc.1"}},
        };
        const auto stable = bestRelease(releases, false);
        const auto beta = bestRelease(releases, true);
        check(stable && stable->version == "2.0.0", "stable update selects highest valid non-draft non-prerelease even if not first");
        check(beta && beta->version == "3.0.0-rc.1" && beta->prerelease, "beta update selects highest permitted version");
        check(!bestRelease(QJsonObject{{"message", "API error"}}, false), "invalid release metadata is not a successful no-update");
        check(!bestRelease(QJsonArray{QJsonObject{{"tag_name", "v2.0.0-beta.1"}}}, false), "stable channel skips beta even without prerelease flag");
        check(retryDelay(1,false)==1min && retryDelay(2,false)==2min && retryDelay(3,false)==5min && retryDelay(4,false)==10min, "update retry backoff and regular cadence");
        check(retryDelay(1,true)==1h && firstDelay==4s && regularDelay==10min && requestSpacing==15s, "update rate-limit and startup cadence");
        const auto layoutPath = output + "/window-state-fixture.json";
        QFile layoutFile(layoutPath);
        check(layoutFile.open(QIODevice::WriteOnly|QIODevice::Truncate), "window state fixture file opened");
        layoutFile.write(R"({"windows":[{"type":"main","state":"minimized","restoreMaximized":true,"x":10,"y":20,"width":800,"height":600,"nativeNormalGeometry":{"x":3520,"y":20,"width":960,"height":900},"tabs":[]},{"type":"popup","state":"maximized","tabs":[]},{"type":"popup","state":"fullscreen","tabs":[]}]})");
        layoutFile.close();
        const auto restored = WindowLayout::loadFromFile(layoutPath);
        check(restored && restored->windows_.size() == 3 &&
            restored->windows_[0].state_ == WindowDescriptor::State::Minimized && restored->windows_[0].restoreMaximized_ &&
            restored->windows_[0].geometry_ == QRect(10,20,800,600) &&
            restored->windows_[1].state_ == WindowDescriptor::State::Maximized && restored->windows_[2].state_ == WindowDescriptor::State::FullScreen,
            "window layout preserves minimized restore target, geometry, maximize, fullscreen");
        check(restored && restored->windows_[0].nativeNormalGeometry_ == QRect(3520,20,960,900) &&
              !restored->windows_[1].nativeNormalGeometry_.isValid(),
              "native restore rect is separate from logical geometry and optional for legacy files");
    }
    auto progress = [&](const QString &stage) {
        QFile log(output+"/ui-startup.jsonl");log.open(QIODevice::WriteOnly|QIODevice::Append);
        log.write(QJsonDocument(QJsonObject{{"stage",stage}}).toJson(QJsonDocument::Compact)+"\n");log.close();
    };
    progress("resources");
    initResources();
    qt.setWindowIcon(QIcon(":/icon.ico"));
    qt.setApplicationDisplayName("Chatterino Noctis");
    if (argc > 2 && QByteArray(argv[2]).startsWith("--theme-migration-"))
    {
        const auto mode = QByteArray(argv[2]);
        const bool stock = mode == "--theme-migration-stock";
        const bool custom = mode == "--theme-migration-custom" ||
                            mode == "--theme-migration-custom-mist" ||
                            mode == "--theme-migration-custom-noctis";
        const auto oldName = mode == "--theme-migration-custom"
                                 ? QStringLiteral("fixture-custom.json")
                                 : mode == "--theme-migration-custom-noctis"
                                       ? QStringLiteral("Noctis-Dark.json")
                                       : QStringLiteral("Mist-Dark.json");
        QTemporaryDir themeDir;
        const auto customFile = themeDir.filePath(oldName);
        const auto source = stock ? qEnvironmentVariable("NOCTIS_LEGACY_THEME_FIXTURE")
                                  : QStringLiteral(":/themes/Noctis-Dark.json");
        check((!custom && !stock) || QFile::copy(source, customFile),
              "isolated custom theme source copied");
        const auto seed = QJsonDocument(QJsonObject{{"appearance", QJsonObject{
            {"theme", QJsonObject{{"name", oldName}}}}}}).toJson(QJsonDocument::Compact);
        mock::BaseApplication migrationApp(QString::fromUtf8(seed), true,
                                          (custom || stock) ? customFile : QString());
        migrationApp.settings.disableSave();
        const auto selected = migrationApp.theme.themeName.getValue();
        check(selected == (custom ? oldName : QStringLiteral("Noctis-Dark")),
              custom ? "existing custom theme is preserved" : "old default migrates to built-in Noctis Dark");
        QFile report(output + "/theme-migration-results.json");
        report.open(QIODevice::WriteOnly | QIODevice::Truncate);
        report.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures},
                                                {"selectedTheme", selected}}).toJson());
        return failures ? 1 : 0;
    }
    progress("mock application");
    const auto previewTheme = qEnvironmentVariable("NOCTIS_PREVIEW_THEME");
    const auto candidateThemeFile = QDir(QCoreApplication::applicationDirPath()).filePath("Themes/" + previewTheme);
    const auto themeFile = QFileInfo::exists(candidateThemeFile) && !previewTheme.isEmpty()
                               ? candidateThemeFile : QString();
    OfflineApp app(fontStartup, themeFile);
    const auto publicThemes = app.theme.availableThemes();
    check(std::any_of(publicThemes.begin(), publicThemes.end(), [&](const auto &entry) {
        return entry.second.toString() == "Noctis-Dark";
    }), "Noctis Dark is built into the public client");
    check(app.theme.themeName.getValue() == "Noctis-Dark", "new profiles use Noctis Dark");
    if (!previewTheme.isEmpty())
    {
        const auto available = app.theme.availableThemes();
        check(std::any_of(available.begin(), available.end(), [&](const auto &entry) {
            return entry.second.toString() == previewTheme;
        }), "requested preview theme exists in isolated fixture");
        app.theme.themeName.setValue(previewTheme);
    }
    installMenuStyle();
    if (argc > 2 && QByteArray(argv[2]) == "--reply-thread")
    {
        Split split(nullptr);
        auto source = std::make_shared<Channel>("thread-fixture", Channel::Type::None);
        split.setChannel(IndirectChannel(source));
        ReplyThreadPopup popup(true, &split);
        auto thread = std::make_shared<MessageThread>(makeSystemMessage("Первое сообщение в ветке"));
        auto reply = makeSystemMessage("Ответ на первое сообщение");
        thread->addToThread(reply);
        popup.setThread(thread, source);
        popup.setAttribute(Qt::WA_DontShowOnScreen);
        if (auto *pin = popup.findChild<QWidget *>("ReplyThreadPin"))
            QMetaObject::invokeMethod(pin, "leftClicked", Qt::DirectConnection);
        popup.show();
        QApplication::processEvents();
        auto *grip = popup.findChild<QWidget *>("UsercardResizeGrip");
        bool resized = false;
        if (grip)
        {
            const auto before = popup.geometry();
            const QPoint edge(grip->width() - 2, grip->height() - 2);
            const QPoint global = grip->mapToGlobal(edge), delta(72, 48);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(edge), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(grip, &press);
            QMouseEvent move(QEvent::MouseMove, QPointF(edge + delta), QPointF(global + delta), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(grip, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(edge + delta), QPointF(global + delta), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(grip, &release);
            resized = popup.width() == before.width() + delta.x() && popup.height() == before.height() + delta.y();
        }
        check(resized, "public reply thread resizes by dragging real corner");
        check(popup.minimumWidth() < popup.maximumWidth(), "reply popup is not fixed width");
        check(popup.grab().save(output + "/reply-thread.png"), "reply thread renders");
        popup.hide();
        QFile report(output + "/reply-thread-results.json");
        report.open(QIODevice::WriteOnly | QIODevice::Truncate);
        report.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures}}).toJson());
        return failures ? 1 : 0;
    }
    if (argc > 2 && QByteArray(argv[2]) == "--update-prompt")
    {
        auto notes = QStringLiteral("# Chatterino Noctis 2.0\n\n");
        for (int i = 0; i < 12; ++i)
            notes += QStringLiteral("- Новый дизайн и более удобная работа с чатом: "
                                    "изменение %1, сохранение настроек и истории.\n").arg(i + 1);
        notes += QStringLiteral("- Последний пункт — <img src=https://example.invalid/never.png>.");
        int actions = 0;
        const auto available = QApplication::primaryScreen()->availableGeometry();
        for (const auto language : {QStringLiteral("ru"), QStringLiteral("en")})
        {
            initializeUiLanguage(language);
            for (const float scale : {0.5F, 1.3F, 4.F})
            {
                app.settings.uiScale.setValue(scale);
                const auto title = uiText("Version %1 is available (you have %2).").arg("2.0.0", "1.3.9");
                QPointer<QDialog> dialog(createUpdatePrompt(nullptr, title, notes,
                    false, false, [&]{ ++actions; }));
                dialog->show();
                qt.processEvents();
                auto *text = dialog->findChild<QTextEdit *>("UpdateNotes");
                auto *primary = dialog->findChild<QPushButton *>("UpdatePrimary");
                auto *later = dialog->findChild<QPushButton *>("UpdateLater");
                check(text && text->isReadOnly() &&
                      text->toPlainText().startsWith("Chatterino Noctis 2.0") &&
                      text->toPlainText().contains("•  Новый дизайн") &&
                      text->toPlainText().contains("Последний пункт") &&
                      text->toPlainText().contains("<img src="),
                      "release text is complete, formatted and inert");
                check(text && text->verticalScrollBar()->maximum() > 0 &&
                      text->horizontalScrollBar()->maximum() == 0 &&
                      text->verticalScrollBar()->value() == 0 &&
                      dialog->width() <= available.width() &&
                      dialog->height() <= available.height(),
                      QStringLiteral("long notes scroll from the beginning without overflowing the desktop (%1/%2, %3x%4, scroll %5/%6)")
                          .arg(language).arg(scale).arg(dialog->width()).arg(dialog->height())
                          .arg(text ? text->verticalScrollBar()->value() : -1)
                          .arg(text ? text->verticalScrollBar()->maximum() : -1));
                check(primary && later &&
                      primary->text() == uiText("Open the download page") &&
                      later->text() == uiText("Later") &&
                      primary->isVisible() && later->isVisible(),
                      "portable actions remain accessible and localized");
                if (language == "ru" && scale == 1.3F)
                    dialog->grab().save(output + "/update-prompt.png");
                const auto previous = actions;
                later->click();
                qt.processEvents();
                check(actions == previous, "Later does not start an update");
            }
        }
        auto *installed = createUpdatePrompt(nullptr, "2.0.0", "Short notes",
                                             true, false, [&] { ++actions; });
        installed->show();
        qt.processEvents();
        auto *primary = installed->findChild<QPushButton *>("UpdatePrimary");
        check(primary && primary->text() == uiText("Update now"), "installed update button");
        primary->click();
        qt.processEvents();
        check(actions == 1, "update action invoked once");
        auto *downloading = createUpdatePrompt(nullptr, "2.0.0", notes, true, true,
                                               [&] { ++actions; });
        downloading->show();
        qt.processEvents();
        check(!downloading->findChild<QPushButton *>("UpdatePrimary") &&
              !downloading->findChild<QTextEdit *>("UpdateNotes"),
              "download in progress cannot initiate another update");
        downloading->close();
        qt.processEvents();
        QFile report(output + "/update-prompt-results.json");
        report.open(QIODevice::WriteOnly | QIODevice::Truncate);
        report.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures}}).toJson());
        markAppAboutToQuit();
        return failures ? 1 : 0;
    }
    if (argc > 2 && QByteArray(argv[2]) == "--branding")
    {
        for (const auto &language : {QStringLiteral("en"), QStringLiteral("ru")})
        {
            initializeUiLanguage(language);
            const bool russian = language == "ru";
            check(uiText("Chatterino Noctis Settings") ==
                      (russian ? QStringLiteral("Настройки Chatterino Noctis")
                               : QStringLiteral("Chatterino Noctis Settings")),
                  language + " settings caption");
            check(uiText("Open Chatterino Noctis") ==
                      (russian ? QStringLiteral("Открыть Chatterino Noctis")
                               : QStringLiteral("Open Chatterino Noctis")),
                  language + " localized tray action");
            checkTrayLanguage(qt, app, output, language);
        }
        QFile report(output + "/ui-results.json");
        if (!report.open(QIODevice::WriteOnly))
            return 1;
        report.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures}}).toJson());
        markAppAboutToQuit();
        return failures ? 1 : 0;
    }
    if (argc > 2 && QByteArray(argv[2]) == "--settings-zoom-matrix")
    {
        initializeUiLanguage("ru");
        app.settings.chatFontFamily.setValue(QStringLiteral("Noctis Montserrat"));
        app.settings.chatFontSize.setValue(10);
        app.settings.uiScale.setValue(1.3F);
        SettingsDialog::showDialog(nullptr);
        for (auto *window : QApplication::topLevelWidgets())
        {
            if (auto *settings = dynamic_cast<SettingsDialog *>(window))
            {
                std::unique_ptr<SettingsDialog> owner(settings);
                qt.processEvents();
                checkInitialSettingsHighlightFit(*settings, output);
                // Still exercise the deliberately compact, manually resized
                // dialog throughout the zoom matrix.
                settings->resize(900, 640);
                qt.processEvents();
                checkSettingsZoomMatrix(*settings, app, output);
                QFile result(output + "/settings-zoom-results.json");
                if (!result.open(QIODevice::WriteOnly))
                    return 1;
                result.write(QJsonDocument(checks).toJson());
                result.close();
                settings->hide();
                QApplication::processEvents();
                markAppAboutToQuit();
                progress("matrix closing settings");
                owner.reset();
                progress("matrix settings destroyed");
                return failures ? 1 : 0;
            }
        }
        return 1;
    }
    if (!captionOnly)
    {
        const auto previousLanguage = activeUiLanguage();
        initializeUiLanguage("ru");
        QLineEdit field;
        field.setText(QStringLiteral("Пример"));
        field.selectAll();
        std::unique_ptr<QMenu> menu(field.createStandardContextMenu());
        auto *child = menu->addMenu(QStringLiteral("&Actions"));
        auto *toggle = child->addAction(QStringLiteral("A && B"));
        toggle->setCheckable(true);
        menu->ensurePolished();
        child->ensurePolished();
        check(menu->testAttribute(Qt::WA_TranslucentBackground) &&
                  child->testAttribute(Qt::WA_TranslucentBackground) &&
                  !menu->styleSheet().isEmpty() &&
                  menu->styleSheet() == child->styleSheet(),
              "standard edit menu and nested menu share the rounded Noctis surface");
        const auto expectedFont = app.fonts.getFont(FontStyle::UiMedium, 1.F);
        check(menu->font().family() == expectedFont.family() &&
                  menu->font().pointSizeF() == expectedFont.pointSizeF(),
              "context menu follows the chat font");
        menu->popup(QPoint(10, 10));
        menu->hide();
        const auto previousScale = app.settings.uiScale.getValue();
        app.settings.uiScale.setValue(1.3F);
        child->menuAction()->setVisible(false);
        menu->popup(QPoint(10, 10));
        qt.processEvents();
        const int rowHeight = menu->actionGeometry(menu->actions().front()).height();
        const auto scaledFont = app.fonts.getFont(FontStyle::UiMedium, 1.3F);
        check(menu->font().pointSizeF() == scaledFont.pointSizeF(),
              "compact menu keeps full UI type size at 130 percent");
        check(rowHeight >= menu->fontMetrics().height() + 4 && rowHeight <= 27,
              "standard input menu has compact rows at 130 percent");
        check(menu->grab().save(output + "/input-menu-130.png"),
              "standard input menu rendered at 130 percent");
        menu->hide();
        child->menuAction()->setVisible(true);
        app.settings.uiScale.setValue(previousScale);
        toggle->trigger();
        check(toggle->isChecked() && toggle->text() == "A && B" &&
                  child->menuAction()->text() == "&Actions",
              "styling preserves check actions, mnemonic and literal ampersand");
        const auto originalTheme = app.theme.themeName.getValue();
        const auto originalSheet = menu->styleSheet();
        app.theme.themeName.setValue(app.theme.isLightTheme() ? "Dark" : "Light");
        menu->popup(QPoint(10, 10));
        check(menu->styleSheet() != originalSheet,
              "a persistent menu picks up a changed theme on reopening");
        menu->hide();
        app.theme.themeName.setValue(originalTheme);
        initializeUiLanguage(previousLanguage);
    }
    {
        const auto root = QJsonDocument::fromJson(R"({"rooms":[{"twitch_id":10,"set":1},{"twitch_id":"20","set":2},{"twitch_id":30,"set":3}],"sets":{"1":{"title":"first"},"2":{"title":"second"}}})").object();
        const auto first = ffz::detail::roomFromBatch(root, "10");
        const auto second = ffz::detail::roomFromBatch(root, "20");
        check(first && second && first->value("sets").toObject().keys() == QStringList{"1"} && second->value("sets").toObject().keys() == QStringList{"2"}, "FFZ batch isolates numeric/string room IDs and their emote sets");
        check(!ffz::detail::roomFromBatch(root, "30") && !ffz::detail::roomFromBatch(root, "40"), "FFZ partial batch preserves old cache for missing room/set");
    }
    {
        Channel merged("restart-fixture", Channel::Type::Twitch);
        auto make = [](const QString &id, qint64 time) -> MessagePtr {
            auto message = std::make_shared<Message>();
            message->id = id;
            message->serverReceivedTime = QDateTime::fromMSecsSinceEpoch(time);
            return message;
        };
        merged.fillInMissingMessages({make("cached", 1000), make("cached", 1000)});
        merged.fillInMissingMessages({make("cached", 1000), make("gap", 2000), make("gap", 2000)});
        const auto snap = merged.getMessageSnapshot();
        check(snap.size() == 2 && snap[0]->id == "cached" && snap[1]->id == "gap",
              "live and server history merge cached IDs without a duplicate row");
    }
    if (qEnvironmentVariableIsSet("NOCTIS_WINDOW_STATE_FIXTURE"))
    {
        const auto mode = qEnvironmentVariable("NOCTIS_WINDOW_STATE_FIXTURE");
        const auto path = app.getPaths().settingsDirectory + "/window-layout.json";
        const auto nativeFixture = qEnvironmentVariable("NOCTIS_WINDOW_NATIVE_RESTORE");
        QPoint origin(20, 30);
        if (!nativeFixture.isEmpty())
        {
            QScreen *secondary = nullptr;
            for (auto *screen : QApplication::screens())
                if (screen != QApplication::primaryScreen()) { secondary = screen; break; }
            if (!secondary || QApplication::platformName() != "windows") return 48;
            origin = secondary->availableGeometry().topLeft() + QPoint(30, 40);
        }
        QJsonObject descriptor{{"type", "main"}, {"x", origin.x()}, {"y", origin.y()},
                               {"width", 600}, {"height", 450}, {"tabs", QJsonArray{}}};
        if (mode != "normal")
            descriptor["state"] = mode.startsWith("minimized") ? QStringLiteral("minimized") : mode;
        if (mode == "minimized-maximized")
            descriptor["restoreMaximized"] = true;
        if (nativeFixture == "restore")
        {
            check(QFile::copy(output + "/native-window-seed.json", path), "saved native fixture copied to new isolated profile");
        }
        else
        {
            QFile fixture(path);
            check(fixture.open(QIODevice::WriteOnly|QIODevice::Truncate), "window runtime fixture opened");
            fixture.write(QJsonDocument(QJsonObject{{"windows", QJsonArray{descriptor}}}).toJson());
            fixture.close();
        }
        const auto originalLayout = WindowLayout::loadFromFile(path);
        app.windows.initialize();
        QCoreApplication::processEvents();
        auto &window = app.windows.getMainWindow();
        const bool minimized = mode.startsWith("minimized");
        const bool maximized = mode.endsWith("maximized");
        check(window.isVisible() && window.isMinimized() == minimized &&
              window.windowState().testFlag(Qt::WindowMaximized) == maximized,
              "actual restored main window visibility and state " + mode);
        app.windows.save();
        const auto saved = WindowLayout::loadFromFile(path);
        check(saved && !saved->windows_.empty() &&
              saved->windows_[0].state_ == (minimized ? WindowDescriptor::State::Minimized : maximized ? WindowDescriptor::State::Maximized : WindowDescriptor::State::None) &&
              (!minimized || saved->windows_[0].restoreMaximized_ == maximized), "actual main window state remains correct when saved " + mode);
#ifdef USEWINSDK
        if (!nativeFixture.isEmpty())
        {
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            const auto hwnd = reinterpret_cast<HWND>(window.winId());
            const bool gotPlacement = GetWindowPlacement(hwnd, &placement);
            const auto &rect = placement.rcNormalPosition;
            const QRect actual(rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top);
            RECT visibleRect{};
            GetWindowRect(hwnd, &visibleRect);
            const QRect visible(visibleRect.left, visibleRect.top, visibleRect.right-visibleRect.left, visibleRect.bottom-visibleRect.top);
            check(gotPlacement && saved && saved->windows_[0].nativeNormalGeometry_ == actual,
                  "saved placement equals native Windows physical rectangle");
            if (nativeFixture == "restore")
                check(originalLayout && (mode == "normal" ? originalLayout->windows_[0].nativeVisibleGeometry_ == visible : originalLayout->windows_[0].nativeNormalGeometry_ == actual),
                      "new process restores exact physical coordinates and dimensions");
            else
            {
                // The offline mock does not implement the automatic Whisper
                // tab's decode path. Keep the actual saved geometry/state,
                // but use the same empty-tab layout as the seed phase.
                QFile from(path);
                const bool opened = from.open(QIODevice::ReadOnly);
                auto root = QJsonDocument::fromJson(from.readAll()).object();
                auto windows = root.value("windows").toArray();
                auto main = windows[0].toObject();
                main["tabs"] = QJsonArray{};
                if (mode == "normal")
                {
                    // A snapped window may keep an old unsnapped right-half
                    // placement even while physically displayed on the left.
                    auto stale = main.value("nativeNormalGeometry").toObject();
                    stale["x"] = stale.value("x").toInt() + 700;
                    main["nativeNormalGeometry"] = stale;
                }
                windows[0] = main;
                root["windows"] = windows;
                QFile to(output + "/native-window-seed.json");
                const bool written = to.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    to.write(QJsonDocument(root).toJson()) > 0;
                check(opened && written, "persist native fixture for second process");
            }
        }
#endif
        QFile report(output+"/window-runtime-results.json"); report.open(QIODevice::WriteOnly|QIODevice::Truncate);
        report.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures}, {"mode", mode}}).toJson());
        report.close();
        markAppAboutToQuit();
        return failures ? 47 : 0;
    }
    if (qEnvironmentVariableIsSet("NOCTIS_UPDATE_HTTP_FIXTURE"))
    {
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "local update HTTP fixture listens");
        qputenv("MIST1X_UPDATE_API", QStringLiteral("http://127.0.0.1:%1/releases").arg(server.serverPort()).toUtf8());
        qunsetenv("MIST1X_UPDATE_TEST_INSTALL");
        int requests = 0;
        QStringList paths;
        bool conditional = false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection())
            {
                auto input = std::make_shared<QByteArray>();
                auto receive = [&, socket, input] {
                    *input += socket->readAll();
                    if (!input->contains("\r\n\r\n")) return;
                    ++requests;
                    paths.push_back(QString::fromUtf8(input->split('\n').front()));
                    conditional |= input->toLower().contains("if-none-match: \"fixture-v1\"");
                    QByteArray status = "200 OK", etag = "\"fixture-v1\"";
                    QByteArray body = R"([{"tag_name":"v1.3.8"},{"tag_name":"v99.0.0-beta.1","prerelease":true},{"tag_name":"v99.0.0","draft":false}])";
                    if (requests == 2) { status = "304 Not Modified"; body.clear(); }
                    else if (requests == 3) { body = R"([{"tag_name":"v99.0.1"}])"; etag = "\"fixture-v2\""; }
                    else if (requests >= 4) { status = "429 Too Many Requests"; body = R"({"message":"rate limited"})"; }
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nETag: " + etag +
                                  "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    input->clear();
                    socket->disconnectFromHost();
                };
                QObject::connect(socket, &QTcpSocket::readyRead, socket, receive);
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                if (socket->bytesAvailable()) receive();
            }
        });
        NetworkManager::init();
        auto &updates = app.getUpdates();
        check(updates.getCurrentVersion() == MIST1X_VERSION,
              "update controller uses the exact compiled release version");
        auto wait = [](auto &&predicate, int timeout) {
            QElapsedTimer elapsed; elapsed.start();
            while (!predicate() && elapsed.elapsed() < timeout)
            {
                QEventLoop loop;
                QTimer::singleShot(30, &loop, &QEventLoop::quit);
                loop.exec();
            }
            return predicate();
        };
        auto pause = [](int ms) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); };
        for (int i = 0; i < 5; ++i) updates.checkForUpdates();
        check(wait([&]{ return updates.getStatus() == Updates::UpdateAvailable; }, 5000) && updates.getOnlineVersion() == "99.0.0" && requests == 1,
              "HTTP fixture chooses highest stable and coalesces concurrent checks");
        for (int i = 0; i < 5; ++i) updates.checkForUpdates();
        check(wait([&]{ return requests >= 2; }, 18000), "HTTP fixture next request scheduled despite throttle");
        pause(500);
        check(conditional && updates.getStatus() == Updates::UpdateAvailable && updates.getOnlineVersion() == "99.0.0" && requests == 2,
              "conditional 304 retains confirmed update and does not create duplicate request");
        updates.checkForUpdates();
        check(wait([&]{return updates.getOnlineVersion()=="99.0.1";},18000) && updates.getStatus()==Updates::UpdateAvailable,
              "new release discovered while prior update still visible");
        updates.checkForUpdates();
        check(wait([&]{return requests>=4;},18000), "rate-limit response received");
        pause(500);
        updates.checkForUpdates();
        pause(1000);
        check(requests==4 && updates.getOnlineVersion()=="99.0.1" && updates.getStatus()==Updates::UpdateAvailable,
              "rate limit keeps update visible and prevents immediate repeat");
        markAppAboutToQuit();
        NetworkManager::deinit();
        QFile report(output+"/update-http-results.json"); report.open(QIODevice::WriteOnly|QIODevice::Truncate);
        report.write(QJsonDocument(QJsonObject{{"checks", checks}, {"failures", failures}, {"requests", requests}}).toJson());
        report.close();
        return failures ? 46 : 0;
    }
    {
        TwitchAccount account("case_fixture", "", "", "900010");
        check(account.getDisplayName() == "case_fixture",
              "account display falls back to stored login before profile");
        check(account.setDisplayName("900010", "case_fixture", "Case_Fixture") &&
                  account.getDisplayName() == "Case_Fixture" &&
                  account.getUserName() == "case_fixture" &&
                  account.getUserId() == "900010",
              "confirmed account spelling preserves login and UID");
        check(!account.setDisplayName("900011", "case_fixture", "CASE_FIXTURE") &&
                  !account.setDisplayName("900010", "other_login", "Other_Login") &&
                  account.getDisplayName() == "Case_Fixture",
              "different profile identity cannot overwrite account spelling");
        check(account.setDisplayName("900010", "case_fixture", "別の名前") &&
                  account.getDisplayName() == "case_fixture",
              "localized account name leaves the login label unambiguous");
        check(account.setDisplayName("900010", "CASE_FIXTURE", "Case_Fixture") &&
                  account.setUserName("renamed_fixture") &&
                  account.getDisplayName() == "renamed_fixture" &&
                  !account.setDisplayName("900010", "case_fixture", "Case_Fixture"),
              "account rename invalidates previous spelling and rejects stale profile");
        check(account.setDisplayName("900010", "renamed_fixture", "Renamed_Fixture") &&
                  account.getDisplayName() == "Renamed_Fixture" &&
                  account.getUserName() == "renamed_fixture",
              "renamed account accepts its new verified display spelling");
        check(whisperhistory::senderDisplayName(account) == "Renamed_Fixture" &&
                  whisperhistory::senderDisplayName(account, "renamed_fixture") == "Renamed_Fixture" &&
                  whisperhistory::senderDisplayName(account, "Wrong_User") == "Renamed_Fixture",
              "outgoing whispers prefer verified nickname to cold or stale cache");
        QHash<QString, QString> names;
        whisperhistory::seedVerifiedAccountName(names, "900011", account);
        check(names.isEmpty(), "whisper history spelling cannot leak across accounts");
        whisperhistory::seedVerifiedAccountName(names, "900010", account);
        check(names.value("900010:renamed_fixture") == "Renamed_Fixture",
              "own verified spelling seeds paged whisper history");
    }
    {
        auto whisper = [](const QString &id, const QString &peer,
                          const QString &text) {
            MessageBuilder builder;
            builder->id = id;
            builder->whisperPeer = peer;
            builder->loginName = peer;
            builder->displayName = peer;
            builder->userID = peer == "alice" ? "700001" : "700002";
            builder->messageText = text;
            builder->serverReceivedTime = QDateTime::currentDateTimeUtc();
            builder->flags.set(MessageFlag::Whisper);
            return builder.release();
        };
        auto first = whisper("whisper-fixture-1", "alice", "Привет");
        auto second = whisper("whisper-fixture-2", "bob", "Hello");
        auto proper = whisper("whisper-fixture-case", "alice", "Case");
        proper->displayName = "Alice";
        QPixmap badgePixmap(18, 18);
        badgePixmap.fill(QColor("#8868d0"));
        const auto badge = std::make_shared<Emote>(Emote{
            .name = EmoteName{"fixture-badge"},
            .images = ImageSet{Image::fromResourcePixmap(badgePixmap)},
            .tooltip = Tooltip{"Fixture badge"}});
        first->elements.push_back(std::make_unique<BadgeElement>(
            badge, MessageElementFlag::BadgeFolhinha));
        first->elements.push_back(std::make_unique<FfzBadgeElement>(
            badge, MessageElementFlag::BadgeFfz, QColor("#5485e3")));
        check(whisperhistory::append("900001", *first, false) &&
                  whisperhistory::append("900001", *second, true) &&
                  whisperhistory::append("900001", *first, false),
              "whispers saved to synthetic profile");
        check(whisperhistory::append("900002", *second, false),
              "whispers second account saved separately");
        check(whisperhistory::append("900001", *proper, false),
              "canonical spelling saved after earlier lowercase whisper");
        auto channel = std::make_shared<Channel>("/whispers", Channel::Type::TwitchWhispers);
        whisperhistory::restore("900001", *channel);
        const auto messages = channel->getMessageSnapshot();
        auto countBadges = [](const MessagePtr &message) {
            return std::ranges::count_if(message->elements, [](const auto &element) {
                return dynamic_cast<const BadgeElement *>(element.get()) != nullptr;
            });
        };
        check(countBadges(messages.front()) == 2 &&
                  whisperhistory::badgeSnapshot(*messages.front()) ==
                      whisperhistory::badgeSnapshot(*first),
              "whisper restores local badge image, high-bit flags and FFZ background");
        check(messages.size() == 3 && messages.front()->messageText == "Привет" &&
                  messages.front()->displayName == "Alice" &&
                  messages[1]->whisperPeer == "bob",
              "whisper restore preserves body, peer and deduplicates");
        whisperhistory::applyDisplayName(*channel, "700002", "bob", "Bob");
        whisperhistory::rememberDisplayName("900001", "700002", "bob", "Bob");
        check(channel->getMessageSnapshot()[1]->displayName == "Bob",
              "resolved whisper spelling updates only the matching UID/login");
        whisperhistory::restore("900001", *channel);
        check(channel->getMessageSnapshot().size() == 3 &&
                  channel->getMessageSnapshot()[1]->displayName == "Bob" &&
                  countBadges(channel->getMessageSnapshot().front()) == 2,
              "display-name metadata survives restart without adding messages");
        QHash<QString, QString> ownNames{{"700002:bob", "BOB"}};
        auto restored = std::make_shared<Channel>("/whispers", Channel::Type::TwitchWhispers);
        const auto path = whisperhistory::historyPath("900002");
        whisperhistory::applyPage(whisperhistory::readPage(path, QFileInfo(path).size()),
                                  *restored, ownNames);
        check(restored->getMessageSnapshot().size() == 1 &&
                  restored->getMessageSnapshot()[0]->displayName == "BOB" &&
                  restored->getMessageSnapshot()[0]->messageText == "Hello",
              "verified spelling repairs a lowercase stored whisper without changing its body");
        ChannelView view(nullptr, ChannelView::Context::None, 1000);
        view.setWhisperPeer("alice");
        view.setChannel(channel);
        check(view.getMessagesSnapshot().size() == 2,
              "whispers can filter by conversation");
        whisperhistory::restore("900002", *channel);
        const auto other = channel->getMessageSnapshot();
        check(other.size() == 1 && other.front()->whisperPeer == "bob" &&
                  other.front()->displayName == "bob" &&
                  view.getMessagesSnapshot().empty(),
              "whisper accounts stay isolated");
        view.setWhisperPeer({});
        view.setChannel(channel);
        check(view.getMessagesSnapshot().size() == 1,
              "whispers can show all conversations again");
        QFile pages(whisperhistory::historyPath("900004"));
        if (pages.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            for (int i = 0; i < 125; ++i)
                pages.write(QJsonDocument(QJsonObject{{"id", QString::number(i)}})
                                .toJson(QJsonDocument::Compact) + "\n");
            pages.close();
            auto recent = whisperhistory::readPage(pages.fileName(), pages.size());
            auto older = whisperhistory::readPage(pages.fileName(), recent.before);
            check(recent.records.size() == 100 && older.records.size() == 25 &&
                      recent.records.front()["id"] == "25" && older.records.back()["id"] == "24" &&
                      older.before == 0, "whisper pages complete and non-overlapping");
        }
        MessageBuilder formatted;
        whisperhistory::appendBody(formatted, "Kappa https://example.org/test",
            QJsonObject{{"Kappa", QJsonArray{"https://example.org/emote.png", "", "", ""}}},
            nullptr, false);
        const auto formattedMessage = formatted.release();
        check(std::ranges::any_of(formattedMessage->elements, [](const auto &e) {
                  return dynamic_cast<EmoteElement *>(e.get()) != nullptr;
              }) && std::ranges::any_of(formattedMessage->elements, [](const auto &e) {
                  return dynamic_cast<LinkElement *>(e.get()) != nullptr;
              }), "saved whisper has clickable link and persisted emote");
        // Simulate a history line written before badge snapshots existed.
        QFile old(whisperhistory::historyPath("900003"));
        if (old.open(QIODevice::WriteOnly))
        {
            old.write(QJsonDocument(QJsonObject{
                {"id", "legacy"}, {"peer", "alice"}, {"login", "alice"},
                {"uid", "700001"}, {"name", "Alice"}, {"text", "Old"},
                {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}
            }).toJson(QJsonDocument::Compact) + "\n");
            old.close();
        }
        auto legacy = whisperhistory::restore("900003", *channel);
        const auto resolve = [&](const QString &uid) -> MessagePtr {
            MessageBuilder source;
            if (uid == "700001")
                source.emplace<BadgeElement>(badge, MessageElementFlag::BadgeChatterino);
            return source.release();
        };
        whisperhistory::refreshLegacyBadges(*channel, legacy, resolve);
        whisperhistory::refreshLegacyBadges(*channel, legacy, resolve);
        check(legacy.size() == 1 && channel->getMessageSnapshot().size() == 1 &&
                  countBadges(channel->getMessageSnapshot().front()) == 1,
              "legacy whisper acquires available global badge without duplicates");
    }
    progress("bounded queue checks");
    LimitedQueue<int> bounded(1000);
    for (int i = 0; i < 5010; ++i)
        bounded.pushBack(i);
    check(bounded.size() == 1000 && bounded.first() == 4010 && bounded.last() == 5009,
          "ordinary queue retains newest 1000 messages");
    progress("font state");
    const auto originalFont=qt.font();
    const auto originalChatFont=app.settings.chatFontFamily.getValue();
    const auto originalChatSize=app.settings.chatFontSize.getValue();
    const auto originalScale=app.settings.uiScale.getValue();
    const auto originalLocale=QLocale().name();
    const auto originalWeight=app.settings.chatFontWeight.getValue();
    if (fontStartup)
    {
        check(originalChatFont=="Noctis Montserrat" && originalChatSize==10 && originalWeight==600,"font startup synthetic profile selected original family/size/weight");
        check(QFontInfo(app.fonts.getFont(FontStyle::ChatMedium,1.3f)).family()=="Noctis Montserrat","font startup resolved bundled face before any settings change");
    }
    app.settings.chatFontFamily.setValue("Noctis Montserrat");
    app.settings.chatFontSize.setValue(10);
    app.settings.chatFontWeight.setValue(600);
    qt.processEvents();
    check(QFontDatabase::families().contains("Noctis Montserrat"),"selected missing font registered privately from resources");
    check(QFile::exists(":/licenses/montserrat-OFL.txt"),"bundled font OFL available");
    QJsonArray resolvedFonts;
    for (const auto style : {FontStyle::ChatMedium,FontStyle::ChatMediumBold,FontStyle::TimestampMedium})
    {
        const QFont font=app.fonts.getFont(style,1.3f);const QFontInfo actual(font);
        check(actual.family()=="Noctis Montserrat","actual chat font resolves original family "+QString::number(int(style)));
        resolvedFonts.append(QJsonObject{{"style",int(style)},{"requested",font.family()},{"resolved",actual.family()},{"weight",actual.weight()},{"pointSize",actual.pointSizeF()}});
    }
    const auto chatFont=app.fonts.getFont(FontStyle::ChatMedium,1.3f);
    check(chatFont.weight()==600 && chatFont.pointSize()==13,"selected weight600 and scaled size13 retained");
    // The interface follows the chat font (one typeface for tabs, headers and chat).
    check(app.fonts.getFont(FontStyle::UiTabs,1).family()=="Noctis Montserrat","UI font follows the chat font");
    {
        QWidget form;
        QLabel label(&form);
        QLineEdit edit(&form);
        auto expected = app.fonts.getFont(FontStyle::UiMedium, 1.3F);
        check(qFuzzyCompare(expected.pointSizeF(), 11.7),
              "UI text retains fractional point sizes");
        check(QFontInfo(label.font()).family() == "Noctis Montserrat" &&
                  QFontInfo(edit.font()).family() == "Noctis Montserrat",
              "ordinary labels and fields inherit the resolved chat family");
        const auto mono = app.fonts.getFont(FontStyle::ChatMediumMono, 1.F);
        check(QFontInfo(mono).family() != QFontInfo(chatFont).family(),
              "technical monospace text keeps its separate font");
        const auto themeName = app.theme.themeName.getValue();
        for (const auto *name : {"Light", "Dark"})
        {
            app.theme.themeName.setValue(name);
            qt.processEvents();
            check(label.palette().windowText().color() == app.theme.window.text &&
                      edit.palette().base().color() == app.theme.messages.backgrounds.regular &&
                      edit.palette().highlightedText().color() == app.theme.tabs.selected.text,
                  QString("existing controls follow active palette: ") + name);
        }
        app.theme.themeName.setValue(themeName);
        app.settings.chatFontFamily.setValue("Segoe UI");
        qt.processEvents();
        check(label.font().family() == "Segoe UI" && edit.font().family() == "Segoe UI",
              "an open form follows font selection");
        app.settings.chatFontFamily.setValue("Noctis Montserrat");
        qt.processEvents();
    }
    check(app.settings.uiScale.getValue()==originalScale,"font repair does not change UI scale");
    QImage fontExample(950,165,QImage::Format_ARGB32_Premultiplied);fontExample.fill(QColor("#121216"));
    QPainter fontPainter(&fontExample);fontPainter.setFont(chatFont);fontPainter.setPen(QColor("#eeeaf4"));
    fontPainter.drawText(12,35,QString::fromUtf8("StreamElements: Заказывай и получай кешбэк 15%"));
    fontPainter.drawText(12,75,QString::fromUtf8("Grisha_Cement: Бери это архив"));
    fontPainter.drawText(12,115,QString::fromUtf8("dngminion: та их уже скупили"));fontPainter.end();
    check(fontExample.save(output+"/resolved-chat-font.png"),"actual Fonts chat font rendered without visible windows");
    QFile resolution(output+"/font-resolution.json");resolution.open(QIODevice::WriteOnly);resolution.write(QJsonDocument(QJsonObject{{"fonts",resolvedFonts},{"startup",fontStartup},{"uiFontFollowsChat",app.fonts.getFont(FontStyle::UiTabs,1).family()=="Noctis Montserrat"}}).toJson());
    app.settings.chatFontFamily.setValue(originalChatFont);app.settings.chatFontSize.setValue(originalChatSize);app.settings.chatFontWeight.setValue(originalWeight);qt.processEvents();
    check(app.settings.chatFontFamily.getValue()==originalChatFont && app.settings.chatFontSize.getValue()==originalChatSize && app.settings.chatFontWeight.getValue()==originalWeight,"synthetic font settings restored");
    QJsonArray pages;
    std::vector<std::unique_ptr<Window>> brandingWindows;
    for (const auto &language : {QString("ru"),QString("en")})
    {
        progress("language "+language);
        initializeUiLanguage(language);
        check(Version::instance().fullVersion().startsWith("Chatterino Noctis "), language+" real version display branding");
        check(!Version::instance().fullVersion().contains("Nightly"),language+" display version omits Nightly");
        check(Version::instance().isNightly() == bool(LEAFYRINO_EXPECT_NIGHTLY_BUILD),
              language+" actual nightly build metadata retained");
        check(uiText("Chatterino Noctis")=="Chatterino Noctis", language+" localized display branding");
        check(uiText("Chatterino Noctis Settings").contains("Chatterino Noctis"), language+" settings caption branding");
        check(qt.windowIcon().availableSizes().contains(QSize(256,256)), language+" embedded ICO contains 256px");
        // The supplied ICO and high-resolution PNG use independent resampling.
        // Compare them at the same size in premultiplied color space.
        const auto ico = qt.windowIcon().pixmap(QSize(256,256),1.0)
                             .toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage sourceIcon(":/icon.png");
        const auto png = sourceIcon.scaled(ico.size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        check(sourceIcon.size()==QSize(1024,1024) && sourceIcon.hasAlphaChannel(),
              language+" full-resolution PNG");
        qint64 difference = 0;
        if (ico.size() == png.size())
        {
            for (int y = 0; y < ico.height(); ++y)
            {
                const auto *a = ico.constScanLine(y);
                const auto *b = png.constScanLine(y);
                for (int x = 0; x < ico.width() * 4; ++x)
                {
                    difference += qAbs(int(a[x]) - int(b[x]));
                }
            }
        }
        check(!ico.isNull() && ico.size()==png.size() &&
                  difference <= qint64(ico.width()) * ico.height() * 4 * 2,
              language+" ICO PNG rendered artwork matches");
        for (const int size : {16,24,32,48,64,128,256})
        {
            const auto image=qt.windowIcon().pixmap(QSize(size,size),1.0).toImage();
            check(image.hasAlphaChannel() && image.pixelColor(0,0).alpha()<=2 && image.pixelColor(image.width()-1,image.height()-1).alpha()<=2,language+" icon transparent corners "+QString::number(size));
        }
        {
            auto mainOwner = std::make_unique<Window>(WindowType::Main, nullptr);
            auto &main = *mainOwner;
            main.setAttribute(Qt::WA_DontShowOnScreen, true);
            auto checkFrame = [&](const QString &phase) {
                if (!main.hasCustomWindowFrame())return;
                int captionButtons = 0;
                for (auto *widget : main.findChildren<QWidget *>())
                {
                    auto *button = dynamic_cast<TitleBarButton *>(widget);
                    if (!button)continue;
                    const auto style = button->getButtonStyle();
                    if (style != TitleBarButtonStyle::Minimize && style != TitleBarButtonStyle::Maximize && style != TitleBarButtonStyle::Unmaximize && style != TitleBarButtonStyle::Close)continue;
                    ++captionButtons;
                    check(button->minimumHeight()==int(30*button->scale()) && button->scale()>=1.F &&
                              button->scale()<=std::max(1.F, main.scale()) &&
                              button->maximumHeight()==button->minimumHeight(),language+" caption button fixed height "+phase);
                    check(button->minimumWidth()>0 && button->maximumWidth()==button->minimumWidth(),language+" caption button fixed width "+phase);
                }
                check(captionButtons==3,language+" three caption buttons "+phase);
            };
            checkFrame("before native show");
            main.resize(900,600);main.show();qt.processEvents();
            auto checkHeader = [&](const QString &phase) {
                if (!main.hasCustomWindowFrame())return;
                const int height=main.layout()->itemAt(0)->geometry().height();
                QFile metrics(output+"/caption-metrics.jsonl");metrics.open(QIODevice::WriteOnly|QIODevice::Append);
                metrics.write(QJsonDocument(QJsonObject{{"language",language},{"phase",phase},{"height",height},{"scale",main.scale()},{"width",main.width()},{"windowHeight",main.height()}}).toJson(QJsonDocument::Compact)+"\n");
                check(height>=30 && height<=qRound(32*std::max(1.F, main.scale())),language+" compact titlebar "+phase);
                auto *caption = main.layout()->itemAt(0)->layout();
                bool labelsFit = caption != nullptr;
                for (int i = 0; caption && i < caption->count(); ++i)
                {
                    auto *button = dynamic_cast<LabelButton *>(caption->itemAt(i)->widget());
                    if (button && button->isVisible())
                        labelsFit &= button->width() - 2 * button->padding().width() >=
                                    button->fontMetrics().horizontalAdvance(button->text());
                }
                check(labelsFit,language+" full caption action and account labels "+phase);
                auto *title = caption ? dynamic_cast<Label *>(caption->itemAt(0)->widget()) : nullptr;
                check(title && title->getFontStyle() == FontStyle::UiTabs &&
                          title->accessibleName() == main.windowTitle(),
                      language+" branded caption uses the tab typeface "+phase);
                if (title && title->width() >= qRound(38 * title->scale()))
                {
                    const auto capture = title->grab();
                    const auto image = capture.toImage();
                    const auto dpr = capture.devicePixelRatio();
                    const auto region = QRect(qRound(8 * title->scale() * dpr),
                        qRound((title->height() - 16 * title->scale()) / 2 * dpr),
                        qRound(16 * title->scale() * dpr), qRound(16 * title->scale() * dpr));
                    int colored = 0;
                    const auto bounds = region.intersected(image.rect());
                    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                        for (int x = bounds.left(); x <= bounds.right(); ++x)
                        {
                            const auto pixel = image.pixelColor(x, y);
                            colored += pixel.alpha() > 100 && pixel.blue() > pixel.red() + 20 &&
                                       pixel.blue() > pixel.green() + 20;
                        }
                    check(colored >= 8, language+" Noctis caption icon is rendered "+phase);
                }
            };
            checkHeader("first content");
            checkFrame("after first layout");
            main.showMinimized();qt.processEvents();main.showNormal();qt.processEvents();
            checkHeader("minimize restore");checkFrame("minimize restore");
            main.resize(250,600);qt.processEvents();checkHeader("narrow");checkFrame("narrow");
            main.resize(900,600);qt.processEvents();checkHeader("wide again");checkFrame("wide again");
            main.hide();main.show();qt.processEvents();checkHeader("hide restore");
            constexpr float navigationScales[]{0.5F, 0.6F, 0.7F, 0.8F, 0.9F,
                1.F, 1.2F, 1.25F, 1.3F, 1.4F, 1.6F, 1.8F, 2.F, 2.33F, 2.66F, 3.F, 3.5F, 4.F};
            for (const float scale : navigationScales)
            {
                app.settings.uiScale.setValue(scale);qt.processEvents();qt.processEvents();
                checkHeader("UI scale "+QString::number(scale));checkFrame("UI scale "+QString::number(scale));
            }
            app.settings.uiScale.setValue(originalScale);qt.processEvents();qt.processEvents();
            checkHeader("original UI scale restored");
            check(main.windowTitle()==Version::instance().fullVersion(),language+" actual main window caption");
            check(main.windowIcon().cacheKey()==qt.windowIcon().cacheKey(),language+" main window inherits application icon");
            check(main.grab().save(output+"/"+language+"-main.png"),language+" actual main window rendered");
            if (language == "ru" && !captionOnly)
            {
                const auto previousMockWhispers = app.twitch.whispersChannel;
                app.twitch.whispersChannel = std::make_shared<Channel>(
                    "/whispers", Channel::Type::TwitchWhispers);
                auto &notebook = main.getNotebook();
                const int originalPages = notebook.getPageCount();
                notebook.ensureWhispersTab();
                auto *whispers = dynamic_cast<SplitContainer *>(
                    notebook.getPageAt(notebook.getPageCount() - 1));
                check(whispers && whispers->isPermanentWhispersTab() &&
                          notebook.getPageCount() == originalPages + 1,
                      "permanent whispers tab created automatically");
                if (whispers)
                {
                    notebook.ensureWhispersTab();
                    check(notebook.getPageCount() == originalPages + 1,
                          "permanent whispers tab is not duplicated");
                    notebook.select(whispers, false);
                    notebook.removeCurrentPage();
                    check(notebook.getPageCount() == originalPages + 1 &&
                              !notebook.canClosePage(whispers),
                          "permanent whispers tab cannot be closed by shortcut");
                    const auto splits = whispers->getSplits();
                    if (!splits.empty())
                    {
                        auto *split = splits.front();
                        split->setChannel(IndirectChannel(Channel::getEmpty()));
                        check(split->getChannel()->getType() ==
                                  Channel::Type::TwitchWhispers,
                              "permanent whispers split cannot be replaced");
                        whispers->deleteSplit(split);
                        check(whispers->getSplits().size() == 1,
                              "permanent whispers split cannot be removed");
                        auto *otherSplit = whispers->appendNewSplit(false);
                        check(whispers->getSplits().size() == 1 &&
                                  otherSplit->parentWidget() != whispers &&
                                  notebook.getPageCount() == originalPages + 2,
                              "adding a split keeps the permanent whispers page dedicated");
                    }
                    notebook.onlyShowLiveTabsAction->trigger();
                    check(whispers->getTab()->isVisible(),
                          "permanent whispers tab remains visible in live-only mode");
                    const auto originalTabInfo =
                        app.settings.informOnTabVisibilityToggle.getValue();
                    app.settings.informOnTabVisibilityToggle.setValue(false);
                    notebook.hideAllTabsAction->trigger();
                    check(whispers->getTab()->isVisible(),
                          "permanent whispers tab remains visible when channel tabs are hidden");
                    notebook.showAllTabsAction->trigger();
                    app.settings.informOnTabVisibilityToggle.setValue(originalTabInfo);
                    auto *ordinaryPage = notebook.addPage(false);
                    const bool savedCloseButton = app.settings.showTabCloseButton.getValue();
                    app.settings.showTabCloseButton.setValue(false);
                    notebook.select(whispers, false);
                    notebook.select(ordinaryPage, false);
                    notebook.select(whispers, false);
                    notebook.select(ordinaryPage, false);
                    auto *activeTab = ordinaryPage->getTab();
                    const auto startFrame = activeTab->grab().toImage();
                    const auto startGeometry = activeTab->geometry();
                    check(notebook.getSelectedPage() == ordinaryPage &&
                              ordinaryPage->isVisible() && activeTab->isSelected(),
                          "rapid tab switches activate the final chat immediately");
                    QEventLoop settleTabs;
                    QTimer::singleShot(170, &settleTabs, &QEventLoop::quit);
                    settleTabs.exec();
                    check(startFrame != activeTab->grab().toImage() &&
                              startGeometry == activeTab->geometry() &&
                              notebook.getSelectedPage() == ordinaryPage,
                          "selection accent animates in place with tab close buttons hidden");
                    app.settings.showTabCloseButton.setValue(savedCloseButton);
                    auto *tabMenu = ordinaryPage->getTab()->findChild<QMenu *>(
                        QString(), Qt::FindDirectChildrenOnly);
                    check(tabMenu, "channel tab has a context menu");
                    if (tabMenu)
                    {
                        app.settings.uiScale.setValue(1.3F);
                        qt.processEvents();
                        tabMenu->popup(QPoint(10, 10));
                        qt.processEvents();
                        check(tabMenu->font().pointSizeF() ==
                                  app.fonts.getFont(FontStyle::UiMedium, 1.3F).pointSizeF() &&
                                  tabMenu->grab().save(output + "/channel-tab-menu.png"),
                              "Russian tab menu rendered at full UI type size at 130 percent");
                        tabMenu->hide();
                        app.settings.uiScale.setValue(originalScale);
                    }
                    auto *testSplit = ordinaryPage->appendNewSplit(false);
                    auto fixtureChat = std::make_shared<Channel>("fixture_channel", Channel::Type::Misc);
                    testSplit->setChannel(IndirectChannel(fixtureChat));
                    ordinaryPage->refreshTab();
                    qt.processEvents();
                    auto *header = testSplit->findChild<SplitHeader *>();
                    if (header)
                    {
                        auto menu = header->mainMenuForTesting();
                        check(menu->actions().size() < 13 &&
                                  menu->grab().save(output + "/split-menu.png"),
                              "split menu has a compact root list");
                    }
                    const auto originalCache = app.paths_.twitchProfileAvatars;
                    app.paths_.twitchProfileAvatars = output + "/avatars";
                    QDir().mkpath(app.paths_.twitchProfileAvatars);
                    QImage avatar(32, 32, QImage::Format_RGB32);
                    avatar.fill(Qt::blue);
                    avatar.save(app.paths_.twitchProfileAvatars + "/fixture.png");
                    const auto savedTabStyle = app.settings.tabStyle.getValue();
                    const auto savedAvatarStyle = app.settings.tabAvatarStyle.getValue();
                    app.settings.tabStyle.setValue(QStringLiteral("compact"));
                    app.settings.tabAvatarStyle.setValue(QStringLiteral("avataronly"));
                    activeTab->setDefaultTitle("fixture_channel");
                    activeTab->setAvatarLogin("fixture");
                    notebook.select(whispers, false);
                    const auto settle = [](int delay) {
                        QEventLoop loop;
                        QTimer::singleShot(delay, &loop, &QEventLoop::quit);
                        loop.exec();
                    };
                    settle(180);
                    const int closedWidth = activeTab->width();
                    notebook.select(ordinaryPage, false);
                    settle(70);
                    const int openingWidth = activeTab->width();
                    settle(130);
                    check(closedWidth < openingWidth && openingWidth < activeTab->width(),
                          "channel tab reveals its name through intermediate widths");
                    app.settings.uiScale.setValue(1.F);
                    settle(200);
                    const auto normalTab = activeTab->grab().toImage();
                    const auto normalSize = activeTab->size();
                    app.settings.uiScale.setValue(0.5F);
                    settle(200);
                    check(activeTab->size() == normalSize && activeTab->height() >= 28 &&
                              normalTab == activeTab->grab().toImage(),
                          "half-scale channel tab retains 1x avatar, name and click target");

                    // Keep the user's avatar preference intact when switching
                    // tab style, and exercise the live notebook layout.
                    const auto longLogin = QStringLiteral("wwwwwwwwwwwwwwwwwwwwwwwww");
                    avatar.save(app.paths_.twitchProfileAvatars + "/" + longLogin + ".png");
                    activeTab->setDefaultTitle(longLogin);
                    activeTab->setAvatarLogin(longLogin);
                    auto *secondPage = notebook.addPage(false);
                    auto *secondTab = secondPage->getTab();
                    secondTab->setDefaultTitle(QStringLiteral("fixture"));
                    secondTab->setAvatarLogin(QStringLiteral("fixture"));
                    app.settings.tabStyle.setValue(QStringLiteral("normal"));
                    settle(180);
                    const int fullFirst = activeTab->minimumTabWidth();
                    const int fullSecond = secondTab->minimumTabWidth();
                    activeTab->grab();
                    secondTab->grab();
                    check(fullFirst > activeTab->height() &&
                              fullSecond > secondTab->height() &&
                              activeTab->toolTip().isEmpty() &&
                              secondTab->toolTip().isEmpty() &&
                              app.settings.tabAvatarStyle.getEnum() == TabAvatarStyle::AvatarOnly,
                          "normal tabs show whole active and inactive channel names despite stored AvatarOnly");
                    notebook.select(secondPage, false);
                    settle(70);
                    check(activeTab->minimumTabWidth() == fullFirst &&
                              secondTab->minimumTabWidth() == fullSecond,
                          "normal channel tab widths stay fixed while the selection animates");
                    settle(170);
                    check(activeTab->minimumTabWidth() == fullFirst &&
                              secondTab->minimumTabWidth() == fullSecond,
                          "normal tab names remain expanded after switching channels");
                    app.settings.tabAvatarStyle.setValue(QStringLiteral("off"));
                    settle(180);
                    check(activeTab->minimumTabWidth() > activeTab->height() &&
                              activeTab->minimumTabWidth() < fullFirst,
                          "normal tabs respect the explicit setting to hide avatars");
                    app.settings.tabStyle.setValue(QStringLiteral("compact"));
                    app.settings.tabAvatarStyle.setValue(QStringLiteral("avatarandname"));
                    settle(180);
                    check(activeTab->minimumTabWidth() > activeTab->height(),
                          "compact style retains the explicit avatar-and-name option");
                    app.settings.tabAvatarStyle.setValue(QStringLiteral("avataronly"));
                    settle(180);
                    check(activeTab->minimumTabWidth() == activeTab->height(),
                          "switching back to compact restores collapsed inactive tabs");
                    notebook.select(ordinaryPage, false);
                    notebook.removePage(secondPage);
                    activeTab->setDefaultTitle(QStringLiteral("fixture_channel"));
                    activeTab->setAvatarLogin(QStringLiteral("fixture"));
                    app.settings.tabStyle.setValue(savedTabStyle);
                    app.settings.tabAvatarStyle.setValue(savedAvatarStyle);
                    settle(180);

                    std::vector<SplitContainer *> wrapPages;
                    for (int i = 0; i < 14; ++i)
                    {
                        auto *page = notebook.addPage(false);
                        page->getTab()->setCustomTitle(QStringLiteral("wrap channel %1").arg(i));
                        wrapPages.push_back(page);
                    }
                    settle(300);
                    bool accessible = true;
                    for (auto *page : wrapPages)
                    {
                        auto *tab = page->getTab();
                        int index = -1;
                        accessible &= tab->isVisible() &&
                            notebook.tabAt(tab->geometry().bottomLeft() + QPoint(2, 0), index) == page;
                    }
                    check(accessible && wrapPages.back()->getTab()->y() >= activeTab->y() + activeTab->height(),
                          "half-scale wrapped rows use their full visible click and drag targets");
                    for (auto *page : wrapPages)
                        notebook.removePage(page);
                    fixtureChat->addMessage(makeSystemMessage(QStringLiteral("Всем привет!")), MessageContext::Repost);
                    fixtureChat->addMessage(makeSystemMessage(QStringLiteral("Добрый вечер!")), MessageContext::Repost);
                    for (const float scale : navigationScales)
                    {
                        app.settings.uiScale.setValue(scale);
                        settle(35);
                        int index = -1;
                        const auto size = activeTab->size();
                        const auto expectedHeight = int(NOTEBOOK_TAB_HEIGHT * std::max(1.F, scale));
                        checkChatReadability(*testSplit, scale);
                        check(size.height() == expectedHeight && size.width() >= size.height() &&
                                  notebook.tabAt(activeTab->geometry().center(), index) == ordinaryPage,
                              QStringLiteral("channel tab readable geometry and hit target at %1x").arg(scale));
                        if (scale == 0.5F || scale == 1.3F || scale == 4.F)
                        {
                            check(activeTab->grab().save(output + QStringLiteral("/channel-tab-%1x.png").arg(scale)),
                                  QStringLiteral("channel tab rendered at %1x").arg(scale));
                            const auto frame = main.grab();
                            const int sample = qRound(3 * frame.devicePixelRatio());
                            if (main.hasCustomWindowFrame())
                                check(frame.toImage().pixelColor(sample, sample) == app.theme.window.background,
                                      QStringLiteral("caption surface uses the active theme at %1x").arg(scale));
                            check(frame.save(output + QStringLiteral("/navigation-and-chat-%1x.png").arg(scale)),
                                  QStringLiteral("channel navigation and local chat rendered at %1x").arg(scale));
                        }
                    }
                    if (qEnvironmentVariableIsSet("NOCTIS_CHECK_ZOOM_STEPS"))
                    {
                        const auto priorSize = main.size();
                        main.resize(2560, 1400);
                        for (int step = 2; step <= 100; ++step)
                        {
                            const auto zoom = step / 10.F;
                            app.settings.setClampedUiScale(zoom);
                            settle(20);
                            checkChatReadability(*testSplit, zoom);
                            check(activeTab->height() == int(NOTEBOOK_TAB_HEIGHT * std::clamp(zoom, 1.F, 4.F)),
                                  QString("channel tab geometry at shortcut zoom %1x").arg(zoom));
                            int index = -1;
                            check(notebook.tabAt(activeTab->geometry().center(), index) == ordinaryPage,
                                  QString("channel tab hit target at shortcut zoom %1x").arg(zoom));
                        }
                        main.resize(priorSize);
                    }
                    app.settings.uiScale.setValue(originalScale);
                    app.paths_.twitchProfileAvatars = originalCache;
                    notebook.removePage(ordinaryPage);
                }
                app.twitch.whispersChannel = previousMockWhispers;
            }
            main.hide();
            brandingWindows.push_back(std::move(mainOwner));
        }
        if (captionOnly)continue; // no tray/dialog/provider work in native hidden mode
        checkTrayLanguage(qt, app, output, language);
        {
            QWidget gallery;
            gallery.setAttribute(Qt::WA_DontShowOnScreen,true);
            auto *row = new QHBoxLayout(&gallery);
            for (const int size : {16,24,32,48,64,128,256})
            {
                auto *column = new QVBoxLayout;
                auto *label = new QLabel;
                label->setPixmap(qt.windowIcon().pixmap(QSize(size,size),qt.devicePixelRatio()));
                column->addWidget(label,0,Qt::AlignCenter);
                column->addWidget(new QLabel(QString::number(size)),0,Qt::AlignCenter);
                row->addLayout(column);
            }
            gallery.adjustSize();
            for (const QString background : {QString("#101017"),QString("#ffffff")})
            {
                gallery.setAutoFillBackground(true);auto p=gallery.palette();p.setColor(QPalette::Window,QColor(background));gallery.setPalette(p);
                const QString suffix=background=="#101017"?"dark":"light";
                check(gallery.grab().save(output+"/"+language+"-icons-"+suffix+".png"),language+" transparent icons on "+suffix);
            }
            check(gallery.grab().save(output+"/"+language+"-icon-sizes.png"),language+" icon sizes rendered at actual DPR");
        }
        progress("basic checks "+language);
        check(uiText("Settings")==(language=="ru"?QString::fromUtf8("Настройки"):QString("Settings")),language+" settings");
        const QString duplicateNotice = QStringLiteral("Your message was not sent because it is identical to the previous one you sent, less than 30 seconds ago.");
        check(uiText(duplicateNotice)==(language=="ru"?QString::fromUtf8("Сообщение не отправлено: вы уже отправляли такое же сообщение менее 30 секунд назад."):duplicateNotice), language+" duplicate notice translation");
        check(uiText("Slow mode is enabled. You can send another message in %1 s.").arg(17).contains("17"),language+" slow-mode duration preserved");
        check(qt.font()==originalFont,language+" application font unchanged");
        check(QLocale().name()==originalLocale,language+" data locale unchanged");
        const auto nickname=QStringLiteral("General");
        const auto content=QStringLiteral("Save #Always Kappa Привет");
        QLabel userLabel(nickname);
        QLineEdit input(content);
        check(userLabel.text()==nickname && input.text()==content,language+" user data unchanged");
        check(uiText("User %1").arg(nickname).endsWith(nickname),language+" substituted nickname unchanged");
        check(uiText("https://example.test/Settings")=="https://example.test/Settings",language+" URL unchanged");
        UiComboBox options;
        options.addItem(uiText("Left"));options.setItemData(0,"Left",UiComboBox::SourceRole);
        options.addItem(uiText("Right"));options.setItemData(1,"Right",UiComboBox::SourceRole);
        options.setSourceText("Right");
        check(options.currentIndex()==1 && options.sourceText(1)=="Right",language+" raw option stable");
        if (language=="ru")check(options.currentText()==QString::fromUtf8("Справа"),"Russian option display");
        UiComboBox custom;
        custom.setEditable(true);
        custom.addItem(uiText("Left"));
        custom.setItemData(0,"Left",UiComboBox::SourceRole);
        custom.setCurrentIndex(-1);
        custom.setEditText("My custom value");
        check(custom.sourceText(-1)=="My custom value", language+" custom editable value unchanged");
        std::vector<std::pair<QString,std::function<QWidget *()>>> factories{
            {"settings",[]()->QWidget *{
                SettingsDialog::showDialog(nullptr);
                for (auto *window : QApplication::topLevelWidgets())
                    if (dynamic_cast<SettingsDialog *>(window))return window;
                return nullptr;
            }},
            {"general",[]{return new GeneralPage;}},
            {"noctis",[]{return new NoctisPage;}},
            {"technorino",[]{return new TechnorinoPage;}},
            {"moltorino",[]{return new MoltorinoPage;}},
            {"accounts",[]{return new AccountsPage;}},
            {"external-tools",[]{return new ExternalToolsPage;}},
            {"about",[]{return new AboutPage;}},
            {"commands",[]{return new CommandPage;}},
            {"highlights",[]{return new HighlightingPage;}},
            {"ignores",[]{return new IgnoresPage;}},
            {"filters",[]{return new FiltersPage;}},
            {"hotkeys",[]{return new KeyboardSettingsPage;}},
            {"moderation",[]{return new ModerationPage;}},
            {"nicknames",[]{return new NicknamesPage;}},
            {"edit-hotkey",[]{return new EditHotkeyDialog(nullptr,nullptr);}},
            {"filter-editor",[]{return new ChannelFilterEditorDialog(nullptr);}},
        };
        // Settings pages schedule short layout timers that capture their views.
        // Keep pages alive while the fixture pumps those timers, as the real
        // SettingsDialog does; don't destroy a page between event-loop turns.
        std::vector<std::unique_ptr<QWidget>> ownedPages;
        for(const auto &[name,factory]:factories)
        {
            QFile progress(output+"/ui-progress.jsonl");
            progress.open(QIODevice::WriteOnly|QIODevice::Append);
            progress.write(QJsonDocument(QJsonObject{{"language",language},{"page",name}}).toJson(QJsonDocument::Compact)+"\n");
            progress.close();
            std::unique_ptr<QWidget> widget(factory());
            check(widget != nullptr, language+" "+name+" created");
            if (!widget)continue;
            if (name == "highlights")
            {
                auto *slider = widget->findChild<QSlider *>("NotificationSoundVolume");
                auto *value = widget->findChild<QLabel *>("NotificationSoundVolumeValue");
                auto *previewSound = widget->findChild<QPushButton *>("NotificationSoundPreview");
                check(slider && value && previewSound && slider->minimum() == 0 && slider->maximum() == 100,
                      language + " notification volume control present");
                if (slider && value && previewSound)
                {
                    const int previous = app.settings.soundVolume.getValue();
                    const auto oldSound = app.settings.pathHighlightSound.getValue();
                    slider->setValue(35);
                    check(app.settings.soundVolume.getValue() == 35 && value->text() == "35%",
                          language + " volume slider updates persisted setting");
                    app.settings.soundVolume.setValue(0);
                    check(slider->value() == 0 && value->text() == "0%", language + " volume supports mute");
                    app.settings.soundVolume.setValue(100);
                    check(slider->value() == 100 && value->text() == "100%", language + " volume supports original level");
                    const int before = app.sound.plays;
                    app.settings.pathHighlightSound.setValue(QString());
                    previewSound->click();
                    check(app.sound.plays == before + 1 && app.sound.last.isEmpty(), language + " preview uses builtin ping");
                    const auto custom = output + "/fixture.wav";
                    app.settings.pathHighlightSound.setValue(custom);
                    previewSound->click();
                    check(app.sound.plays == before + 2 && app.sound.last == QUrl::fromLocalFile(custom),
                          language + " preview uses selected sound");
                    app.settings.pathHighlightSound.setValue(oldSound);
                    app.settings.soundVolume.setValue(previous);
                }
            }
            if (name == "filters")
            {
                auto *table = widget->findChild<QTableView *>();
                check(table != nullptr, language + " filter status table present");
                if (table)
                {
                    auto &records = app.settings.filterRecords;
                    const int row = records.append(std::make_shared<FilterRecord>(
                        "test", QStringLiteral("message.content contains \"hello\"")));
                    auto *model = table->model();
                    const auto expression = model->index(row, 1);
                    const auto status = model->index(row, 2);
                    check(model->index(row, 0).flags().testFlag(Qt::ItemIsEditable) &&
                              expression.flags().testFlag(Qt::ItemIsEditable) &&
                              !status.flags().testFlag(Qt::ItemIsEditable) &&
                              !model->setData(status, QStringLiteral("ignored"), Qt::EditRole) &&
                              status.data() == uiText("Valid"),
                          language + " only filter name and expression accept edits");
                    check(model->setData(expression, QStringLiteral("("), Qt::EditRole) &&
                              !status.flags().testFlag(Qt::ItemIsEditable) &&
                              status.data() == uiText("Show errors") &&
                              table->horizontalHeader()->sectionResizeMode(2) ==
                                  QHeaderView::ResizeToContents,
                          language + " invalid filter status stays read-only and sizes to its text");
                    records.removeAt(row);
                }
            }
            if (name=="filter-editor")
            {
                check(static_cast<ChannelFilterEditorDialog *>(widget.get())->getFilter()==QStringLiteral("(message.content contains \"hello\")"),
                      language+" filter expression remains canonical");
                for (auto *combo : widget->findChildren<QComboBox *>())
                {
                    if (combo->findData("message text") < 0)continue;
                    for (int i = 0; i < combo->count(); ++i)
                    {
                        combo->setCurrentIndex(i);
                        const auto key = filters::VALID_IDENTIFIERS_MAP.key(combo->itemData(i).toString());
                        check(static_cast<ChannelFilterEditorDialog *>(widget.get())->getFilter() ==
                            QString("(%1 contains \"hello\")").arg(key),language+" canonical variable "+key);
                    }
                    break;
                }
            }
            widget->setAttribute(Qt::WA_DontShowOnScreen,true);
            if (name == "settings")
            {
                check(widget->width() <= 940 && widget->height() <= 680,
                      language+" settings opens at a compact size");
            }
            else
            {
                widget->resize(915, 650);
            }
            widget->show();
            qt.processEvents();
            if (name == "settings")
            {
                QComboBox *section = nullptr;
                QScrollArea *area = nullptr;
                for (auto *combo : widget->findChildren<QComboBox *>("settingsSectionPicker"))
                {
                    if (combo->isVisibleTo(widget.get())) section = combo;
                }
                for (auto *scroll : widget->findChildren<QScrollArea *>("generalSettingsScrollContent"))
                {
                    if (scroll->isVisibleTo(widget.get())) area = scroll;
                }
                check(section && section->count() > 1 &&
                          section->currentText() == uiText("Language and theme"),
                      language+" settings section matches the visible opening heading");
                check(area && area->horizontalScrollBar()->maximum() == 0,
                      language+" appearance controls fit the compact viewport");
                if (section && area)
                {
                    const int last = section->count() - 1;
                    section->setCurrentIndex(last);
                    section->activated(last);
                    qt.processEvents();
                    check(area->verticalScrollBar()->value() > 0 && section->currentIndex() == last,
                          language+" last settings section opens and stays selected");
                    section->setCurrentIndex(0);
                    section->activated(0);
                    area->verticalScrollBar()->setValue(0);
                    qt.processEvents();
                }
            }
            if (name=="general")
            {
                auto *general=static_cast<GeneralPage *>(widget.get());
                check(general->filterElements(language=="ru"?QString::fromUtf8("шрифт"):QString("font")),language+" settings search");
                general->filterElements("");
                qt.processEvents();
                QComboBox *picker=nullptr;
                for(auto *combo:widget->findChildren<QComboBox *>())
                    if(combo->findData("ru")>=0 && combo->findData("en")>=0)picker=combo;
                check(picker!=nullptr,language+" language picker present");
                if(picker)
                {
                    app.settings.saveSnapshot();
                    picker->setCurrentIndex(picker->findData("en"));
                    check(app.settings.uiLanguage.getValue()=="en",language+" language code stored");
                    app.settings.restoreSnapshot();
                    picker->setCurrentIndex(picker->findData(language));
                    qt.processEvents();
                    QFile bounds(output+"/picker-bounds-"+language+".json");
                    bounds.open(QIODevice::WriteOnly);
                    const auto topLeft = picker->mapTo(widget.get(), QPoint(0,0));
                    bounds.write(QJsonDocument(QJsonObject{{"x",topLeft.x()},{"width",picker->width()},
                        {"root_width",widget->width()}}).toJson());
                }
            }
            if (name == "edit-hotkey")
            {
                auto *category = widget->findChild<QComboBox *>("categoryPicker");
                auto *action = widget->findChild<QComboBox *>("actionPicker");
                check(category && action, language + " hotkey selectors present");
                if (category && action)
                {
                    const int original = category->currentIndex();
                    QSet<int> widths;
                    bool fits = true;
                    for (int i = 0; i < category->count(); ++i)
                    {
                        category->setCurrentIndex(i);
                        QStyleOptionComboBox opt;
                        opt.initFrom(action);
                        opt.rect = QRect(QPoint(), action->sizeHint());
                        const int field = action->style()->subControlRect(
                            QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField,
                            action).width();
                        widths.insert(field);
                        for (int j = 0; j < action->count(); ++j)
                        {
                            fits &= action->fontMetrics().horizontalAdvance(
                                        action->itemText(j)) <= field;
                        }
                    }
                    check(fits && widths.size() > 1 && !action->isEditable(),
                          language + " hotkey action field tracks translated choices after category changes");
                    category->setCurrentIndex(original);
                }
            }
            if (name=="about")
            {
                auto *brand=widget->findChild<QLabel *>("BrandDisplayName");
                auto *attribution=widget->findChild<QLabel *>("BrandAttribution");
                check(brand && brand->text().contains("Chatterino Noctis"),language+" about brand heading");
                check(attribution && attribution->text().contains("Leafyrino") && attribution->text().contains("Chatterino7"),language+" upstream attribution preserved");
            }
            if(name=="settings" || name=="general" || name=="edit-hotkey" || name=="about")
                check(widget->grab().save(output+"/"+language+"-"+name+".png"),language+" "+name+" rendered");
            pages.append(QJsonObject{{"language",language},{"page",name},{"texts",texts(*widget)},{"width",widget->width()},{"height",widget->height()}});
            if (name == "settings" && language == "ru")
            {
                widget->resize(900, 640);
                qt.processEvents();
                check(widget->grab().save(output + "/ru-settings-900.png"),
                      "compact settings rendered at desktop opening size");
                const auto previousScale = app.settings.uiScale.getValue();
                app.settings.uiScale.setValue(1.3F);
                qt.processEvents();
                const auto *search = widget->findChild<QLineEdit *>("settingsSearch");
                const auto expected = app.fonts.getFont(FontStyle::ChatMedium, SettingsPage::interfaceScale());
                check(search && search->font().pointSizeF() == expected.pointSizeF() &&
                          search->font().weight() >= QFont::Medium,
                      "settings uses scaled chat text with readable stroke weight");
                check(widget->grab().save(output + "/ru-settings-130.png"),
                      "settings rendered at 130 percent");
                checkSettingsSelectors(*widget, app, output);
                app.settings.uiScale.setValue(1.F);
                qt.processEvents();
                const auto *ok = widget->findChild<QPushButton *>("settingsOk");
                const auto *sidebar = widget->findChild<QScrollArea *>("settingsSidebar");
                const auto *tab = widget->findChild<SettingsDialogTab *>();
                if (search && ok && tab && sidebar)
                {
                    const auto bodySize = search->font().pointSizeF();
                    const auto editHeight = search->height();
                    const auto buttonHeight = ok->height();
                    const auto tabHeight = tab->height();
                    const auto sidebarWidth = sidebar->width();
                    const auto windowSize = widget->size();
                    app.settings.uiScale.setValue(.5F);
                    qt.processEvents();
                    check(search->font().pointSizeF() == bodySize &&
                              ok->font().pointSizeF() == bodySize &&
                              search->height() == editHeight && ok->height() == buttonHeight &&
                              tab->height() == tabHeight && sidebar->width() == sidebarWidth &&
                              widget->size() == windowSize,
                          "settings keep usable text and controls at half zoom");
                    check(widget->grab().save(output + "/ru-settings-050.png"),
                          "settings rendered at 50 percent");
                }
                else
                {
                    check(false, "settings half zoom coverage finds the actual controls");
                }
                app.settings.uiScale.setValue(previousScale);
                qt.processEvents();
            }
            widget->hide();
            ownedPages.push_back(std::move(widget));
        }
        QEventLoop pendingLayouts;
        QTimer::singleShot(25, &pendingLayouts, &QEventLoop::quit);
        pendingLayouts.exec();
        ownedPages.clear();
    }
    check(app.settings.chatFontFamily.getValue()==originalChatFont && app.settings.chatFontSize.getValue()==originalChatSize && app.settings.uiScale.getValue()==originalScale,"persisted fonts and scale unchanged");
    initializeUiLanguage("unknown");
    check(activeUiLanguage()=="en" && uiText("Settings")=="Settings","unknown language falls back to English");
    QJsonObject result{{"checks",checks},{"failures",failures},{"pages",pages},{"font",originalFont.toString()},{"devicePixelRatio",qt.devicePixelRatio()},{"version",Version::instance().fullVersion()},{"scope","synthetic offline widgets only"}};
    QFile file(output+"/ui-results.json");file.open(QIODevice::WriteOnly);file.write(QJsonDocument(result).toJson());file.close();
    markAppAboutToQuit();
    return failures?1:0;
}
