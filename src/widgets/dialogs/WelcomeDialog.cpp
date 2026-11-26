// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/WelcomeDialog.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/MenuStyle.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLockFile>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

namespace chatterino {

namespace {

/// Files of a Chatterino profile that carry the user's setup. Logins are not
/// among them: other clients keep them in the Windows credential store.
const QStringList IMPORTED_FILES{
    QStringLiteral("window-layout.json"),
    QStringLiteral("commands.json"),
    QStringLiteral("user-data.json"),
};

QString shownMarker()
{
    return QDir(getApp()->getPaths().miscDirectory).filePath("welcome-shown");
}

/// Other clients' profiles found on this computer (their Settings folders).
QList<std::pair<QString, QString>> otherProfiles()
{
    QList<std::pair<QString, QString>> out;
    const auto appData = qEnvironmentVariable("APPDATA");
    const std::pair<const char *, const char *> known[]{
        {"Chatterino, Chatterino7, Leafyrino", "Chatterino2"},
        {"Chatterino+", "Chatterino by Rish"},
        {"Chatterino", "Chatterino"},
    };
    for (const auto &[name, folder] : known)
    {
        const auto settings =
            QDir(appData).filePath(QString::fromUtf8(folder) + "/Settings");
        if (QFileInfo::exists(QDir(settings).filePath("settings.json")))
        {
            out.append({QString::fromUtf8(name), settings});
        }
    }
    return out;
}

/// Parts of settings.json that belong to this copy and must never be imported
/// from another profile, including legacy integration and credential fields.
const QList<QStringList> OWN_SETTINGS{
    {QStringLiteral("accounts")},
    {QStringLiteral("archive")},
    {QStringLiteral("cache")},
    {QStringLiteral("moltorino"), QStringLiteral("auth")},
    {QStringLiteral("moltorino"), QStringLiteral("botBadge")},
    {QStringLiteral("moltorino"), QStringLiteral("pinnedMessages"),
     QStringLiteral("customAuthToken")},
};

/// Puts @a value (or nothing, when it is undefined) at @a path in @a object.
void setAtPath(QJsonObject &object, const QStringList &path,
               const QJsonValue &value)
{
    if (path.size() == 1)
    {
        if (value.isUndefined())
        {
            object.remove(path.first());
        }
        else
        {
            object.insert(path.first(), value);
        }
        return;
    }
    auto child = object.value(path.first()).toObject();
    setAtPath(child, path.mid(1), value);
    object.insert(path.first(), child);
}

QJsonValue valueAtPath(const QJsonObject &object, const QStringList &path)
{
    const auto value = object.value(path.first());
    if (path.size() == 1 || !value.isObject())
    {
        return path.size() == 1 ? value : QJsonValue(QJsonValue::Undefined);
    }
    return valueAtPath(value.toObject(), path.mid(1));
}

bool readFile(const QString &path, QByteArray &out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    out = file.readAll();
    return file.error() == QFileDevice::NoError;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() &&
           file.commit();
}

bool importProfile(QWidget *parent, const QString &from, bool ask = true,
                   bool restart = true)
{
    const auto fail = [parent](const QString &text) {
        QMessageBox::warning(parent, uiText("Move settings"), text);
        return false;
    };
    const auto own = getApp()->getPaths().settingsDirectory;
    if (QDir(from).absolutePath() == QDir(own).absolutePath())
    {
        return false;
    }

    // 1. everything is read first: nothing changes if a file cannot be read
    QByteArray theirsData;
    if (!readFile(QDir(from).filePath("settings.json"), theirsData))
    {
        return fail(uiText("There is no settings.json in this folder."));
    }
    auto theirs = QJsonDocument::fromJson(theirsData).object();
    if (theirs.isEmpty())
    {
        return fail(uiText("The settings in this folder could not be read."));
    }
    QMap<QString, QByteArray> settings;
    for (const auto &name : IMPORTED_FILES)
    {
        const auto source = QDir(from).filePath(name);
        if (QFileInfo::exists(source) && !readFile(source, settings[name]))
        {
            return fail(uiText("This file could not be read: %1")
                            .arg(QDir::toNativeSeparators(source)));
        }
    }
    if (ask &&
        QMessageBox::question(
            parent, uiText("Move settings"),
            uiText("Tabs with channels, highlights, ignores, commands, filters "
                   "and the rest of the settings are copied from:\n%1\n\nThe "
                   "current settings are kept in a backup folder. Logging in to "
                   "Twitch has to be done again. The program restarts.")
                .arg(QDir::toNativeSeparators(from))) != QMessageBox::Yes)
    {
        return false;
    }

    QLockFile importLock(QDir(own).filePath(".profile-import.lock"));
    if (!importLock.tryLock())
    {
        return fail(uiText("The settings folder could not be locked for import."));
    }
    const auto localSettings = QDir(own).filePath("settings.json");
    QJsonObject mine;
    if (QFileInfo::exists(localSettings))
    {
        QByteArray mineData;
        const bool read = readFile(localSettings, mineData);
        const auto document = QJsonDocument::fromJson(mineData);
        if (!read || !document.isObject())
        {
            return fail(uiText("This file could not be read: %1")
                            .arg(QDir::toNativeSeparators(localSettings)));
        }
        mine = document.object();
    }
    for (const auto &path : OWN_SETTINGS)
    {
        setAtPath(theirs, path, valueAtPath(mine, path));
    }
    // Never import foreign theme selection or theme files. Do not delete
    // existing custom themes from this installation. The destination starts
    // with the embedded Noctis style and the currently required IRC default.
    setAtPath(theirs, {QStringLiteral("appearance"), QStringLiteral("theme")},
              QJsonObject{{"name","Noctis-Dark"},{"darkSystem","Noctis-Dark"},{"lightSystem","Light"}});
    setAtPath(theirs, {QStringLiteral("misc"), QStringLiteral("chatSendProtocol")},
              QStringLiteral("IRC"));
    setAtPath(theirs, {QStringLiteral("misc"), QStringLiteral("noctisThemeIrcDefaultsApplied")}, true);
    setAtPath(theirs, {QStringLiteral("misc"), QStringLiteral("noctisThemeMigrated")}, true);
    settings.insert(QStringLiteral("settings.json"), QJsonDocument(theirs).toJson());

    QTemporaryDir backup(QDir(own).filePath("before-import-" +
        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-XXXXXX"));
    if (!backup.isValid())
    {
        return fail(uiText("The backup folder could not be created, nothing "
                           "was changed."));
    }
    QHash<QString, QString> originals;
    QStringList names{QStringLiteral("settings.json")};
    names += IMPORTED_FILES;
    for (const auto &name : names)
    {
        const auto destination = QDir(own).filePath(name);
        if (QFileInfo::exists(destination))
        {
            const auto saved = backup.filePath(name);
            if (!QFile::copy(destination, saved))
            {
                return fail(uiText("The backup could not be made, nothing was "
                                   "changed."));
            }
            originals.insert(destination, saved);
        }
    }
    backup.setAutoRemove(false);

    QList<QPair<QString, QByteArray>> planned;
    for (auto it = settings.cbegin(); it != settings.cend(); ++it)
    {
        planned.emplaceBack(QDir(own).filePath(it.key()), it.value());
    }
    QStringList committed;
    for (const auto &[destination, contents] : planned)
    {
        if (!writeFile(destination, contents))
        {
            bool restored = true;
            for (auto it = committed.crbegin(); it != committed.crend(); ++it)
            {
                const auto saved = originals.constFind(*it);
                QByteArray data;
                const bool ok = saved == originals.cend()
                                    ? QFile::remove(*it)
                                    : readFile(*saved, data) && writeFile(*it, data);
                restored = restored && ok;
            }
            if (!restored)
            {
                return fail(uiText("Settings could only be partially restored. The backup is in %1.")
                                .arg(QDir::toNativeSeparators(backup.path())));
            }
            return fail(uiText("The settings could not be saved; the previous "
                               "ones were put back."));
        }
        committed.append(destination);
    }
    importLock.unlock();

    // Start again (same arguments, e.g. --portable-dir) without saving the
    // settings in memory over the new ones.
    if (restart)
    {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1));
    }
    std::_Exit(0);
}

void chooseImport(QWidget *parent, QPushButton *button)
{
    auto *menu = new QMenu(parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (const auto &[name, path] : otherProfiles())
    {
        QObject::connect(menu->addAction(name), &QAction::triggered, parent,
                         [parent, path] {
                             importProfile(parent, path);
                         });
    }
    if (!menu->isEmpty())
    {
        menu->addSeparator();
    }
    QObject::connect(
        menu->addAction(uiText("Choose the Settings folder…")),
        &QAction::triggered, parent, [parent] {
            const auto dir = QFileDialog::getExistingDirectory(
                parent, uiText("The Settings folder of the other Chatterino"));
            if (!dir.isEmpty())
            {
                importProfile(parent, dir);
            }
        });
    styleMenu(menu);
    menu->popup(button->mapToGlobal(QPoint(0, button->height())));
}

QWidget *step(float scale, int number, const QString &title, const QString &text,
              const QList<QPushButton *> &buttons)
{
    const auto metric = [scale](int value) {
        return std::max(1, qRound(value * scale));
    };
    auto *fonts = getApp()->getFonts();
    auto *row = new QWidget;
    row->setObjectName("welcomeStep");
    row->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(metric(12), metric(10), metric(12), metric(10));
    layout->setSpacing(metric(12));

    auto *circle = new QLabel(QString::number(number));
    circle->setObjectName("welcomeNumber");
    circle->setAlignment(Qt::AlignCenter);
    circle->setFont(fonts->getFont(FontStyle::ChatMediumBold, scale));
    const int circleSize =
        std::max(metric(26), circle->fontMetrics().height() + metric(6));
    circle->setFixedSize(circleSize, circleSize);
    circle->setStyleSheet(
        QStringLiteral("border-radius: %1px;").arg(circleSize / 2));
    layout->addWidget(circle, 0, Qt::AlignTop);

    auto *texts = new QVBoxLayout;
    texts->setSpacing(metric(4));
    auto *heading = new QLabel(title);
    heading->setObjectName("welcomeTitle");
    heading->setFont(fonts->getFont(FontStyle::ChatMediumBold, scale));
    heading->setWordWrap(true);
    texts->addWidget(heading);
    auto *body = new QLabel(text);
    body->setObjectName("welcomeText");
    body->setFont(fonts->getFont(FontStyle::ChatMedium, scale * .94F));
    body->setTextFormat(Qt::PlainText);
    body->setWordWrap(true);
    texts->addWidget(body);
    layout->addLayout(texts, 1);

    if (!buttons.isEmpty())
    {
        auto *actions = new QVBoxLayout;
        actions->setSpacing(metric(5));
        for (auto *button : buttons)
        {
            button->setCursor(Qt::PointingHandCursor);
            button->setFont(fonts->getFont(FontStyle::ChatMedium, scale * .94F));
            actions->addWidget(button);
        }
        actions->addStretch(1);
        layout->addLayout(actions);
    }
    return row;
}

QColor blend(const QColor &base, const QColor &accent, qreal weight)
{
    return QColor::fromRgbF(base.redF() * (1 - weight) + accent.redF() * weight,
                            base.greenF() * (1 - weight) + accent.greenF() * weight,
                            base.blueF() * (1 - weight) + accent.blueF() * weight);
}

}  // namespace

void showWelcomeDialog(QWidget *parent)
{
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("NoctisWelcomeDialog");
    dialog->setWindowTitle(uiText("Getting started"));
    const auto scale = std::max(1.F, getSettings()->getClampedUiScale());
    const auto metric = [scale](int value) {
        return std::max(1, qRound(value * scale));
    };
    auto *fonts = getApp()->getFonts();
    dialog->setFont(fonts->getFont(FontStyle::ChatMedium, scale));
    const auto *theme = getTheme();
    const auto background = theme->window.background;
    const auto surface = theme->splits.input.background;
    const auto foreground = theme->window.text;
    const auto accent = theme->accent;
    const auto border = theme->splits.header.border;
    const auto muted = blend(background, foreground, .72);
    const auto hover = blend(surface, accent, .12);
    const auto selected = blend(surface, accent, .20);
    dialog->setStyleSheet(QStringLiteral(R"(
QDialog#NoctisWelcomeDialog { background: %1; color: %2; }
QLabel#welcomeHeading, QLabel#welcomeTitle { color: %2; background: transparent; }
QLabel#welcomeText { color: %3; background: transparent; }
QWidget#welcomeStep { background: %4; border: 1px solid %5; border-radius: %9px; }
QLabel#welcomeNumber { background: %8; color: %2; }
QScrollArea#welcomeSteps { background: transparent; border: 0; }
QWidget#welcomeStepsContent { background: %1; }
QDialog#NoctisWelcomeDialog QPushButton {
    background: %6; color: %2; border: 1px solid %12; border-radius: %9px;
    padding: %10px %11px;
}
QDialog#NoctisWelcomeDialog QPushButton:hover { background: %8; border-color: %7; }
QDialog#NoctisWelcomeDialog QPushButton:pressed { background: %8; }
QDialog#NoctisWelcomeDialog QPushButton:focus { border-color: %7; }
QDialog#NoctisWelcomeDialog QPushButton#welcomeDone {
    background: %8; border-color: %7; color: %2;
}
QDialog#NoctisWelcomeDialog QPushButton#welcomeDone:hover { background: %13; }
)")
                              .arg(background.name(), foreground.name(), muted.name(),
                                   surface.name(), border.name(), hover.name(),
                                   accent.name(), selected.name())
                              .arg(metric(8)).arg(metric(6)).arg(metric(12))
                              .arg(blend(surface, foreground, .18).name())
                              .arg(blend(surface, accent, .30).name()));

    auto *layout = new QVBoxLayout(dialog);
    const int margin = metric(16);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(metric(10));
    auto *header = new QHBoxLayout;
    header->setSpacing(metric(12));
    const auto appIcon = dialog->windowIcon();
    if (!appIcon.isNull())
    {
        auto *icon = new QLabel;
        const int iconSize = metric(36);
        icon->setPixmap(appIcon.pixmap(QSize(iconSize, iconSize),
                                       dialog->devicePixelRatioF()));
        icon->setAlignment(Qt::AlignCenter);
        icon->setFixedSize(iconSize, iconSize);
        header->addWidget(icon, 0, Qt::AlignVCenter);
    }
    auto *headerTexts = new QVBoxLayout;
    headerTexts->setSpacing(metric(4));
    auto *heading = new QLabel(uiText("Welcome to Chatterino Noctis"));
    heading->setObjectName("welcomeHeading");
    auto headingFont = fonts->getFont(FontStyle::ChatLarge, scale);
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    heading->setWordWrap(true);
    headerTexts->addWidget(heading);
    auto *intro = new QLabel(uiText("A few steps to set it up. All of them can "
                                    "be done later from the settings."));
    intro->setObjectName("welcomeText");
    intro->setWordWrap(true);
    headerTexts->addWidget(intro);
    header->addLayout(headerTexts, 1);
    layout->addLayout(header);

    auto *scroll = new QScrollArea(dialog);
    scroll->setObjectName("welcomeSteps");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->verticalScrollBar()->setStyleSheet(QStringLiteral(R"(
QScrollBar:vertical { background: %1; width: %2px; margin: 0; }
QScrollBar::handle:vertical { background: %3; border-radius: %4px; min-height: %5px; }
QScrollBar::handle:vertical:hover { background: %6; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)")
        .arg(background.name()).arg(metric(6))
        .arg(blend(surface, foreground, .22).name()).arg(metric(3))
        .arg(metric(20)).arg(blend(surface, accent, .52).name()));
    auto *steps = new QWidget;
    steps->setObjectName("welcomeStepsContent");
    auto *stepsLayout = new QVBoxLayout(steps);
    stepsLayout->setContentsMargins(0, 0, 0, 0);
    stepsLayout->setSpacing(metric(7));
    stepsLayout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(steps);
    layout->addWidget(scroll, 1);

    auto *window = parent;
    auto settingsButton = [dialog, window](const QString &text,
                                           SettingsDialogPreference tab) {
        auto *button = new QPushButton(text);
        QObject::connect(button, &QPushButton::clicked, dialog,
                         [window, tab] {
                             getApp()->getWindows()->showSettingsDialog(window,
                                                                        tab);
                         });
        return button;
    };
    auto linkButton = [](const QString &text, const QString &url) {
        auto *button = new QPushButton(text);
        QObject::connect(button, &QPushButton::clicked, [url] {
            QDesktopServices::openUrl(QUrl(url));
        });
        return button;
    };

    auto *importButton = new QPushButton(uiText("Move…"));
    QObject::connect(importButton, &QPushButton::clicked, dialog,
                     [dialog, importButton] {
                         chooseImport(dialog, importButton);
                     });
    stepsLayout->addWidget(step(
        scale, 1, uiText("Settings from another Chatterino"),
        uiText("If you used Chatterino, Chatterino7, Leafyrino or Chatterino+: "
               "tabs with channels, highlights, ignores, commands and filters. "
               "Do this first, then log in."),
        {importButton}));

    stepsLayout->addWidget(step(
        scale, 2, uiText("Log in to Twitch"),
        uiText("Without it the chat can only be read. Settings → Accounts → "
               "Add new account."),
        {settingsButton(uiText("Log in"), SettingsDialogPreference::Accounts)}));
    stepsLayout->addWidget(step(
        scale, 3, uiText("Open a channel"),
        uiText("Click the empty panel or «+» next to the tabs and type the "
               "channel name. Several channels fit in one tab."),
        {}));
    stepsLayout->addWidget(step(
        scale, 4, uiText("Channel points, pins, polls and predictions"),
        uiText("They need a second login: Settings → Sign-in for extra "
               "features. The code is confirmed on twitch.tv/activate."),
        {settingsButton(uiText("Set up"),
                        SettingsDialogPreference::Moltorino)}));
    stepsLayout->addStretch(1);
    auto *footer = new QHBoxLayout;
    footer->setSpacing(metric(8));
    auto *hint = new QLabel(uiText("The settings are sorted by topic; the "
                                   "search at the top finds any of them."));
    hint->setObjectName("welcomeText");
    hint->setWordWrap(true);
    footer->addWidget(hint, 1);
    const auto guide =
        QDir(QCoreApplication::applicationDirPath()).filePath("Руководство.html");
    if (QFileInfo::exists(guide))
    {
        auto *guideButton = linkButton(uiText("Guide"),
                                       QUrl::fromLocalFile(guide).toString());
        guideButton->setCursor(Qt::PointingHandCursor);
        footer->addWidget(guideButton);
    }
    auto *done = new QPushButton(uiText("Start"));
    done->setObjectName("welcomeDone");
    done->setCursor(Qt::PointingHandCursor);
    done->setFont(fonts->getFont(FontStyle::ChatMediumBold, scale));
    QObject::connect(done, &QPushButton::clicked, dialog, &QDialog::accept);
    footer->addWidget(done);
    layout->addLayout(footer);

    // Fit the onboarding content on the current display, with a scrolling
    // middle section for a large chat font or a small screen.
    const auto available = dialog->screen()->availableGeometry().size();
    const int width =
        std::min(metric(580), std::max(1, available.width() - metric(32)));
    dialog->setMinimumWidth(std::min(metric(440), width));
    dialog->ensurePolished();
    const int contentWidth = std::max(1, width - 2 * margin);
    const auto naturalHeight = [contentWidth](QLayout *area) {
        const auto measured = area->heightForWidth(contentWidth);
        return measured >= 0 ? measured : area->sizeHint().height();
    };
    const int height = 2 * margin + naturalHeight(header) +
                       naturalHeight(stepsLayout) + naturalHeight(footer) +
                       2 * layout->spacing();
    dialog->resize(width,
                   std::min(std::max(height, layout->minimumSize().height()),
                            std::max(1, available.height() - metric(48))));
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void importSettingsForTesting(const QString &from)
{
    importProfile(nullptr, from, false, false);
}

void showWelcomeDialogIfNew(QWidget *parent)
{
    if (QFileInfo::exists(shownMarker()))
    {
        return;
    }
    QFile marker(shownMarker());
    if (marker.open(QIODevice::WriteOnly))
    {
        marker.write("1");
    }
    // updating from an older version: already set up
    if (!getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        return;
    }
    showWelcomeDialog(parent);
}

}  // namespace chatterino
