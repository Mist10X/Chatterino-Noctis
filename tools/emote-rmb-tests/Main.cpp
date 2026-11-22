// SPDX-License-Identifier: MIT
// Offline native regression checks: actual Split, ChannelView rendering,
// hit-testing, mouse events, context menu and QTextEdit. No providers are started.
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Emote.hpp"
#include "common/UiLanguage.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Resources.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/MenuStyle.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleFactory>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace chatterino;
namespace {

class OfflineApp final : public mock::BaseApplication
{
public:
    OfflineApp()
        : mock::BaseApplication(QStringLiteral("{}"))
        , windows(this->args, this->paths_, this->settings, this->theme,
                  this->fonts)
        , commands(this->paths_)
        , crash(this->paths_)
    {
        // Do not call account load(), provider initialize(), NetworkManager or
        // runGui(). BaseApplication redirects settings to its own QTemporaryDir.
        this->settings.disableSave();
        this->settings.showEmptyInput = true;
    }
    WindowManager *getWindows() override { return &this->windows; }
    AccountController *getAccounts() override { return &this->accounts; }
    HotkeyController *getHotkeys() override { return &this->hotkeys; }
    CommandController *getCommands() override { return &this->commands; }
    EmoteController *getEmotes() override { return &this->emotes; }
    ITwitchIrcServer *getTwitch() override { return &this->twitch; }
    BttvEmotes *getBttvEmotes() override { return &this->bttv; }
    FfzEmotes *getFfzEmotes() override { return &this->ffz; }
    SeventvEmotes *getSeventvEmotes() override { return &this->seventv; }
    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &this->personalEmotes;
    }
    IUserDataController *getUserData() override { return &this->userData; }
    SeventvPaints *getSeventvPaints() override { return &this->paints; }
    SpellChecker *getSpellChecker() override { return &this->spellChecker; }
    CrashHandler *getCrashHandler() override { return &this->crash; }
    WindowManager windows;
    AccountController accounts;
    HotkeyController hotkeys;
    CommandController commands;
    mock::EmoteController emotes;
    mock::MockTwitchIrcServer twitch;
    mock::UserDataController userData;
    SeventvPaints paints;
    BttvEmotes bttv;
    FfzEmotes ffz;
    SeventvEmotes seventv;
    SeventvPersonalEmotes personalEmotes;
    SpellChecker spellChecker;
    CrashHandler crash;
};

class RecordingChannel final : public Channel
{
public:
    RecordingChannel() : Channel(QStringLiteral("offline-emote-rmb"), Type::Misc)
    {
    }
    void sendMessage(const QString &) override { ++this->sent; }
    int sent = 0;
};

QJsonArray checks;
int failures = 0;
QString progressPath;
void progress(const QString &stage)
{
    if (progressPath.isEmpty())
    {
        return;
    }
    QFile log(progressPath);
    if (log.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        log.write(QJsonDocument(QJsonObject{{"stage", stage}})
                      .toJson(QJsonDocument::Compact) + '\n');
        log.flush();
    }
}
void check(bool ok, const QString &name, const QString &actual = {})
{
    progress(name);
    checks.append(QJsonObject{{"name", name}, {"pass", ok}, {"actual", actual}});
    if (!ok)
    {
        ++failures;
    }
}

void pump()
{
    // Dispatch pending paint/layout events without adding a production delay.
    for (int i = 0; i != 4; ++i)
    {
        QApplication::processEvents();
    }
}

// Keep each distinct source pixmap alive: Image::fromResourcePixmap caches by
// its address. No URL, image download or emote-provider data is used.
std::vector<std::unique_ptr<QPixmap>> pixmaps;
EmotePtr emote(const QString &name, QColor color, bool zeroWidth = false,
               EmoteModifierPlacement placement = EmoteModifierPlacement::None)
{
    auto pixmap = std::make_unique<QPixmap>(28, 28);
    pixmap->fill(color);
    auto result = std::make_shared<Emote>(Emote{
        .name = EmoteName{name},
        .images = ImageSet{Image::fromResourcePixmap(*pixmap)},
        .tooltip = Tooltip{name},
        .homePage = {},
        .zeroWidth = zeroWidth,
        .id = {},
        .author = {},
        .baseName = {},
        .modifierPlacement = placement,
    });
    pixmaps.push_back(std::move(pixmap));
    return result;
}

std::optional<QPoint> visiblePixel(ChannelView &view, QColor color)
{
    pump();
    view.performLayout();
    const auto pixmap = view.grab();
    const auto image = pixmap.toImage();
    // Pick an interior pixel of an actual rendered emote. This goes through
    // normal production layout/hit-testing rather than a hardcoded coordinate.
    for (int y = 2; y + 2 < image.height(); ++y)
    {
        for (int x = 2; x + 2 < image.width(); ++x)
        {
            if (image.pixelColor(x, y).rgb() == color.rgb() &&
                image.pixelColor(x + 2, y + 2).rgb() == color.rgb())
            {
                return QPoint(qRound(x / pixmap.devicePixelRatio()),
                              qRound(y / pixmap.devicePixelRatio()));
            }
        }
    }
    return std::nullopt;
}

void rightClick(ChannelView &view, QPoint point,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    progress(QStringLiteral("rightClick constructing press"));
    const QPointF local(point);
    const QPointF global(view.mapToGlobal(point));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::RightButton,
                      Qt::RightButton, modifiers);
    progress(QStringLiteral("rightClick sending press"));
    QApplication::sendEvent(&view, &press);
    progress(QStringLiteral("rightClick press returned"));
    QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                        Qt::RightButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(&view, &release);
    progress(QStringLiteral("rightClick release returned"));
    pump();
    progress(QStringLiteral("rightClick pump returned"));
}

void closeMenus()
{
    for (auto *widget : QApplication::allWidgets())
    {
        if (auto *menu = qobject_cast<QMenu *>(widget))
        {
            menu->close();
        }
    }
    pump();
}

QAction *menuAction(QMenu *menu, const QString &title)
{
    if (menu)
    {
        for (auto *action : menu->actions())
        {
            if (action->text() == title)
            {
                return action;
            }
        }
    }
    return nullptr;
}

bool visibleMenu()
{
    const auto widgets = QApplication::allWidgets();
    return std::ranges::any_of(widgets, [](auto *widget) {
        return qobject_cast<QMenu *>(widget) && widget->isVisible();
    });
}

// Windows sends a separate context-menu event after the mouse release.
// Deliver it to the widget under the cursor after the reply layout changes.
bool contextMenuOpens(QWidget &widget, QContextMenuEvent::Reason reason,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    bool opened = false;
    QTimer closeOnOpen;
    closeOnOpen.setSingleShot(true);
    QObject::connect(&closeOnOpen, &QTimer::timeout, [&] {
        opened = visibleMenu();
        closeMenus();
    });
    const QPoint local = widget.rect().center();
    QContextMenuEvent event(reason, local, widget.mapToGlobal(local), modifiers);
    closeOnOpen.start(0);
    QApplication::sendEvent(&widget, &event);
    closeOnOpen.stop();
    pump();
    return opened;
}

void draft(Split &split, QTextEdit &edit, const QString &text,
           int position = -1, int selectionEnd = -1)
{
    progress(QStringLiteral("draft setting text"));
    split.getInput().setInputText(text);
    progress(QStringLiteral("draft text set"));
    auto cursor = edit.textCursor();
    cursor.setPosition(position < 0 ? text.size() : position);
    if (selectionEnd >= 0)
    {
        cursor.setPosition(selectionEnd, QTextCursor::KeepAnchor);
    }
    edit.setTextCursor(cursor);
    progress(QStringLiteral("draft cursor set"));
    pump();
    progress(QStringLiteral("draft pump returned"));
}

}  // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication qt(argc, argv);
    qt.setApplicationName(QStringLiteral("Leafyrino-offline-emote-rmb-tests"));
    if (argc < 2)
    {
        return 20;
    }
    const auto reportPath = QFileInfo(QString::fromLocal8Bit(argv[1]));
    if (!reportPath.isAbsolute())
    {
        return 21;
    }
    progressPath = reportPath.absoluteFilePath() + QStringLiteral(".progress.jsonl");
    { QFile log(progressPath); log.open(QIODevice::WriteOnly | QIODevice::Truncate); }
    progress(QStringLiteral("QApplication created; guard passed"));
    qt.setStyle(QStyleFactory::create("Fusion"));
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    qt.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    progress(QStringLiteral("initializing resources"));
    initResources();
    progress(QStringLiteral("resources initialized"));
    QElapsedTimer elapsed;
    elapsed.start();
    {
        progress(QStringLiteral("constructing offline application"));
        OfflineApp app;
        installMenuStyle();
        initializeUiLanguage(QStringLiteral("ru"));
        progress(QStringLiteral("offline application constructed"));
        QWidget host;
        QVBoxLayout layout(&host);
        progress(QStringLiteral("constructing split"));
        Split split(&host);
        progress(QStringLiteral("split constructed"));
        layout.addWidget(&split);
        host.resize(720, 380);
        auto channel = std::make_shared<RecordingChannel>();
        progress(QStringLiteral("setting synthetic channel"));
        split.setChannel(IndirectChannel(channel));
        progress(QStringLiteral("synthetic channel set"));
        host.show();
        progress(QStringLiteral("host shown"));
        pump();
        progress(QStringLiteral("initial events dispatched"));
        auto *edit = split.getInput().findChild<QTextEdit *>();
        check(edit != nullptr, QStringLiteral("real split input exists"));
        int menuCount = 0;
        QPointer<QMenu> lastMenu;
        auto connection = app.windows.channelViewContextMenuRequested.connect(
            [&](const ChannelView &, const auto &, const auto *, QMenu &menu) {
                ++menuCount;
                lastMenu = &menu;
            });

        const QColor marker(17, 234, 85);
        const auto smile = emote(QStringLiteral("RmbSmile"), marker);
        MessageBuilder builder;
        builder.emplace<EmoteElement>(smile, MessageElementFlag::Emote);
        channel->addMessage(builder.release(), MessageContext::Repost);
        auto &view = split.getChannelView();
        const auto point = visiblePixel(view, marker);
        check(point.has_value(), QStringLiteral("emote rendered and hit location found"));
        if (point && edit)
        {
            draft(split, *edit, {});
            rightClick(view, *point);
            check(edit->toPlainText() == "RmbSmile " && menuCount == 0 &&
                      !visibleMenu(),
                  QStringLiteral("RMB inserts into empty input without menu"),
                  edit->toPlainText());

            draft(split, *edit, QStringLiteral("draft"));
            rightClick(view, *point);
            check(edit->toPlainText() == "draft RmbSmile ",
                  QStringLiteral("draft preserved with separator at end"),
                  edit->toPlainText());

            draft(split, *edit, QStringLiteral("hello world"), 6);
            rightClick(view, *point);
            edit->insertPlainText(QStringLiteral("X"));
            check(edit->toPlainText() == "hello RmbSmile Xworld",
                  QStringLiteral("middle cursor and existing separator preserved"),
                  edit->toPlainText());

            draft(split, *edit, QStringLiteral("one replace two"), 4, 11);
            rightClick(view, *point);
            check(edit->toPlainText() == "one RmbSmile two",
                  QStringLiteral("selection replaced without losing surrounding draft"),
                  edit->toPlainText());

            draft(split, *edit, {});
            rightClick(view, *point);
            rightClick(view, *point);
            check(edit->toPlainText() == "RmbSmile RmbSmile ",
                  QStringLiteral("repeated RMB inserts separated codes"),
                  edit->toPlainText());

            draft(split, *edit, QStringLiteral("untouched"), 3);
            const auto beforeShiftMenus = menuCount;
            rightClick(view, *point, Qt::ShiftModifier);
            check(edit->toPlainText() == "untouched" &&
                      edit->textCursor().position() == 3 &&
                      menuCount == beforeShiftMenus + 1 && visibleMenu(),
                  QStringLiteral("Shift RMB keeps original menu and draft/cursor"));
            check(lastMenu && !menuAction(lastMenu, uiText("&Open")) &&
                      !menuAction(lastMenu, uiText("&Copy")) &&
                      menuAction(lastMenu, uiText("Copy &message")) &&
                      !menuAction(lastMenu, uiText("Copy &full message")),
                  QStringLiteral("local emote omits empty links and identical clipboard actions"));
            closeMenus();

            draft(split, *edit, QStringLiteral("readonly"));
            edit->setReadOnly(true);
            const auto beforeReadonlyMenus = menuCount;
            rightClick(view, *point);
            check(edit->toPlainText() == "readonly" &&
                      menuCount == beforeReadonlyMenus + 1,
                  QStringLiteral("read-only input is not changed"));
            closeMenus();
            edit->setReadOnly(false);

            edit->setEnabled(false);
            const auto beforeDisabledMenus = menuCount;
            rightClick(view, *point);
            check(edit->toPlainText() == "readonly" &&
                      menuCount == beforeDisabledMenus + 1,
                  QStringLiteral("disabled input is not changed"));
            closeMenus();
            edit->setEnabled(true);
        }

        // Use a new in-memory channel per shape, exercising normal setChannel.
        const auto show = [&](const std::function<void(MessageBuilder &)> &add) {
            auto next = std::make_shared<RecordingChannel>();
            MessageBuilder message;
            add(message);
            next->addMessage(message.release(), MessageContext::Repost);
            split.setChannel(IndirectChannel(next));
            pump();
            return next;
        };

        // Populate all image frames before displaying this message so URLs are
        // only used as menu data. No provider, fetch or browser is started.
        const auto menuEmote = std::make_shared<Emote>(*smile);
        const auto imageURL = [](int size) {
            return QStringLiteral("file:///fixture/emote-%1.png").arg(size);
        };
        const auto fixtureImage = [&](int size) {
            QPixmap frame(28 * size, 28 * size);
            frame.fill(marker);
            auto image = Image::fromUrl(Url{imageURL(size)}, 1.0 / size, frame.size());
            detail::assignFrames(image, {{frame, 1}});
            return image;
        };
        const auto small = fixtureImage(1);
        menuEmote->images = ImageSet{small, small, fixtureImage(4), fixtureImage(8)};
        menuEmote->homePage = Url{QStringLiteral("https://7tv.app/emotes/fixture")};
        pump();
        const auto menuChannel = show([&](MessageBuilder &message) {
            message.emplace<TextElement>(QStringLiteral("Viewer:"),
                                         MessageElementFlag::Username);
            message.emplace<EmoteElement>(menuEmote, MessageElementFlag::Emote);
        });
        const auto menuPoint = visiblePixel(view, marker);
        check(menuPoint.has_value(), QStringLiteral("linked emote visible in real message"));
        if (menuPoint)
        {
            rightClick(view, *menuPoint, Qt::ShiftModifier);
            auto *open = menuAction(lastMenu, uiText("&Open"));
            auto *copy = menuAction(lastMenu, uiText("&Copy"));
            auto *openMenu = open ? open->menu() : nullptr;
            auto *copyMenu = copy ? copy->menu() : nullptr;
            check(openMenu && copyMenu && menuAction(lastMenu, uiText("Copy &message")) &&
                      menuAction(lastMenu, uiText("Copy &full message")),
                  QStringLiteral("image menus and distinct message copy formats are available"));
            const QString page = QStringLiteral("&Страница смайлика");
            check(menuAction(openMenu, QStringLiteral("Изображение · &1×")) &&
                      !menuAction(openMenu, QStringLiteral("Изображение · &2×")) &&
                      menuAction(openMenu, QStringLiteral("Изображение · &4×")) &&
                      menuAction(openMenu, QStringLiteral("Изображение · &8×")) &&
                      menuAction(openMenu, page) && menuAction(copyMenu, page) &&
                      menuAction(openMenu, QStringLiteral("&ChatVault")) &&
                      menuAction(openMenu, QStringLiteral("Lur&kology")),
                  QStringLiteral("Russian image sizes, unique URLs, provider page and external sites"));
            auto *copyLargest = menuAction(copyMenu, QStringLiteral("Ссылка на изображение · &8×"));
            if (copyLargest)
            {
                copyLargest->trigger();
            }
            check(copyLargest && QApplication::clipboard()->text() == imageURL(8),
                  QStringLiteral("largest image action copies the original exact URL"));
            if (lastMenu && openMenu)
            {
                lastMenu->setActiveAction(open);
                openMenu->popup(lastMenu->mapToGlobal(
                    QPoint(lastMenu->width(), lastMenu->actionGeometry(open).top())));
                pump();
                const auto styled = [&](QMenu *menu) {
                    return menu->testAttribute(Qt::WA_TranslucentBackground) &&
                           menu->styleSheet().contains(QStringLiteral("border-radius")) &&
                           menu->font().family() == app.fonts.getFont(
                               FontStyle::ChatMediumSmall, view.scale()).family();
                };
                check(lastMenu->isVisible() && openMenu->isVisible() &&
                          styled(lastMenu) && styled(openMenu),
                      QStringLiteral("root and nested image popup use translucent rounded chat style"));
                const QPixmap root = lastMenu->grab();
                const QPixmap nested = openMenu->grab();
                const auto rootSize = root.deviceIndependentSize().toSize();
                const auto subSize = nested.deviceIndependentSize().toSize();
                const int top = lastMenu->actionGeometry(open).top();
                const auto dpr = root.devicePixelRatio();
                QPixmap cascade(QSize(rootSize.width() + subSize.width(),
                                      std::max(rootSize.height(), top + subSize.height())) * dpr);
                cascade.setDevicePixelRatio(dpr);
                cascade.fill(Qt::transparent);
                QPainter painter(&cascade);
                painter.drawPixmap(0, 0, root);
                painter.drawPixmap(rootSize.width(), top, nested);
                painter.end();
                check(cascade.save(reportPath.absoluteFilePath() + ".menu.png"),
                      QStringLiteral("saved real chat menu and submenu render"));
            }
            closeMenus();
        }

        auto layeredChannel = show([&](MessageBuilder &message) {
            const auto overlay = emote(QStringLiteral("RmbOverlay"), marker, true);
            const auto modifier = emote(QStringLiteral("RmbModifier"), marker,
                                        false, EmoteModifierPlacement::Suffix);
            auto *layered = message.emplace<LayeredEmoteElement>(
                std::vector<LayeredEmoteElement::Emote>{
                    {smile, MessageElementFlag::Emote},
                    {overlay, MessageElementFlag::Emote}},
                MessageElementFlag::Emote);
            layered->addModifier(modifier);
        });
        const auto layeredPoint = visiblePixel(view, marker);
        check(layeredPoint.has_value(), QStringLiteral("layered emote rendered"));
        if (edit && layeredPoint)
        {
            draft(split, *edit, {});
            rightClick(view, *layeredPoint);
            check(edit->toPlainText() == "RmbSmile RmbOverlay RmbModifier ",
                  QStringLiteral("layered emote includes zero-width and modifier tokens"),
                  edit->toPlainText());
        }

        auto badgeChannel = show([&](MessageBuilder &message) {
            message.emplace<BadgeElement>(smile, MessageElementFlag::BadgeSubscription);
        });
        const auto badgePoint = visiblePixel(view, marker);
        check(badgePoint.has_value(), QStringLiteral("badge rendered"));
        if (edit && badgePoint)
        {
            draft(split, *edit, QStringLiteral("badge stays"));
            const auto before = menuCount;
            rightClick(view, *badgePoint);
            check(edit->toPlainText() == "badge stays" && menuCount == before + 1,
                  QStringLiteral("badge does not insert and keeps original menu"));
            closeMenus();
        }

        auto regexChannel = show([&](MessageBuilder &message) {
            message.emplace<EmoteElement>(
                emote(QStringLiteral("\\:-?\\)"), marker), MessageElementFlag::Emote);
        });
        const auto regexPoint = visiblePixel(view, marker);
        if (edit && regexPoint)
        {
            draft(split, *edit, {});
            rightClick(view, *regexPoint);
            check(edit->toPlainText() == ":) ",
                  QStringLiteral("Twitch regex emote becomes usable canonical code"),
                  edit->toPlainText());
        }
        else
        {
            check(false, QStringLiteral("Twitch regex emote rendered"));
        }

        ChannelView standalone(nullptr, ChannelView::Context::UserCard);
        standalone.resize(500, 180);
        standalone.setChannel(channel);
        standalone.show();
        const auto standalonePoint = visiblePixel(standalone, marker);
        check(standalonePoint.has_value(), QStringLiteral("standalone usercard emote rendered"));
        if (edit && standalonePoint)
        {
            draft(split, *edit, QStringLiteral("other input"));
            const auto before = menuCount;
            rightClick(standalone, *standalonePoint);
            check(edit->toPlainText() == "other input" && menuCount == before + 1,
                  QStringLiteral("no-Split usercard keeps menu and cannot change unrelated input"));
            closeMenus();
        }
        check(channel->sent == 0 && layeredChannel->sent == 0 &&
                  badgeChannel->sent == 0 && regexChannel->sent == 0,
              QStringLiteral("all clicks leave message send count at zero"));
        auto local = std::make_shared<TwitchChannel>("fixture-reply-room");
        split.setChannel(IndirectChannel(local));
        app.settings.usernameRightClickModifier = Qt::ShiftModifier;
        app.settings.usernameRightClickModifierBehavior = UsernameRightClickBehavior::Reply;
        MessageBuilder replyable;
        replyable.message().id = "fixture-reply-id";
        replyable.message().loginName = "viewer";
        replyable.message().displayName = "Viewer";
        // The reply preview displays the IRC text; the chat renders its elements.
        replyable.message().messageText = smile->name.string;
        replyable.message().serverReceivedTime = QDateTime::currentDateTime();
        replyable.emplace<EmoteElement>(smile, MessageElementFlag::Emote);
        local->addMessage(replyable.release(), MessageContext::Repost);
        pump();
        const auto replyPoint = visiblePixel(view, marker);
        check(replyPoint.has_value(), QStringLiteral("replyable row rendered"));
        if (edit && replyPoint)
        {
            draft(split, *edit, QStringLiteral("draft"));
            const auto before = menuCount;
            rightClick(view, *replyPoint, Qt::ShiftModifier);
            check(edit->toPlainText().startsWith("@Viewer") &&
                      edit->toPlainText().contains("draft") &&
                      menuCount == before && !visibleMenu(),
                  QStringLiteral("Shift RMB on message content starts reply without menu"),
                  edit->toPlainText());
            auto *preview = split.getInput().findChild<MessageView *>();
            check(preview && preview->isVisible(),
                  QStringLiteral("reply preview is displayed"));
            if (preview && preview->isVisible())
            {
                check(!contextMenuOpens(*preview, QContextMenuEvent::Mouse,
                                        Qt::ShiftModifier) &&
                          !contextMenuOpens(*preview, QContextMenuEvent::Mouse),
                      QStringLiteral("late native context menu event cannot open reply preview menu"));
                const QPointF local = preview->rect().center();
                const QPointF global = preview->mapToGlobal(local.toPoint());
                QMouseEvent press(QEvent::MouseButtonPress, local, global,
                                  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                                    Qt::RightButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(preview, &press);
                QApplication::sendEvent(preview, &release);
                check(contextMenuOpens(*preview, QContextMenuEvent::Mouse),
                      QStringLiteral("deliberate right click on reply preview still opens copy menu"));
                check(contextMenuOpens(*preview, QContextMenuEvent::Keyboard),
                      QStringLiteral("keyboard context menu remains available"));
                check(edit->toPlainText().startsWith("@Viewer") &&
                          edit->toPlainText().contains("draft"),
                      QStringLiteral("context menu events preserve the reply and draft"));
            }
        }
        connection.disconnect();
        markAppAboutToQuit();
    }
    pixmaps.clear();
    QJsonObject report{{"checks", checks}, {"passed", checks.size() - failures},
                       {"failed", failures}, {"elapsed_ms", elapsed.elapsed()},
                       {"mode", "offscreen-real-widgets-no-network"}};
    QFile output(reportPath.absoluteFilePath());
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return 22;
    }
    output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    return failures == 0 ? 0 : 1;
}
