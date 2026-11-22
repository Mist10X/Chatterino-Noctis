// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/UiLanguage.hpp"
#include "widgets/settingspages/HighlightingPage.hpp"

#include "Application.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "controllers/highlights/BadgeHighlightModel.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightBlacklistModel.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightModel.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/highlights/UserHighlightModel.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/BadgePickerDialog.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/color/ColorItemDelegate.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QFileDialog>
#include <QHeaderView>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>

namespace chatterino {

namespace {

QList<DisplayBadge> availableBadges = {
    {"Broadcaster", "broadcaster"},
    {"Admin", "admin"},
    {"Staff", "staff"},
    {"Moderator", "moderator"},
    {"Lead Moderator", "lead_moderator"},
    {"Verified", "partner"},
    {"VIP", "vip"},
    {"Founder", "founder"},
    {"Subscriber", "subscriber"},
    {"Predicted Blue", "predictions/blue-1,predictions/blue-2"},
    {"Predicted Pink", "predictions/pink-2,predictions/pink-1"},
};
}

HighlightingPage::HighlightingPage()
{
    LayoutCreator<HighlightingPage> layoutCreator(this);

    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();
    {
        auto tabs = layout.emplace<QTabWidget>();
        {
            auto highlights = tabs.appendTab(new QVBoxLayout, uiText("Messages"));
            {
                highlights.emplace<QLabel>(
                    uiText("Play notification sounds and highlight messages based on "
                    "certain patterns.\n"
                    "Message highlights are prioritized over badge highlights "
                    "and user highlights."))->setWordWrap(true);

                auto *view =
                    highlights
                        .emplace<EditableModelView>(
                            (new HighlightModel(nullptr))
                                ->initialized(
                                    &getSettings()->highlightedMessages))
                        .getElement();
                view->addRegexHelpLink();
                view->setTitles({uiText("Pattern"), uiText("Show in\nMentions"),
                                 uiText("Flash\ntaskbar"), uiText("Enable\nregex"),
                                 uiText("Case-\nsensitive"), uiText("Play\nsound"),
                                 uiText("Custom\nsound"), uiText("Color")});
                view->getTableView()->setItemDelegateForColumn(
                    HighlightModel::Column::Color, new ColorItemDelegate(view));
                view->setReadableColumns({0}, true);

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->highlightedMessages.append(HighlightPhrase{
                        "my phrase", true, true, false, false, false, "",
                        *ColorProvider::instance().color(
                            ColorType::SelfHighlight)});
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Messages);
                                 });
            }

            auto pingUsers = tabs.appendTab(new QVBoxLayout, uiText("Users"));
            {
                pingUsers.emplace<QLabel>(
                    uiText("Play notification sounds and highlight messages from "
                    "certain users.\n"
                    "User highlights are prioritized over badge highlights, "
                    "but under message highlights."))->setWordWrap(true);
                EditableModelView *view =
                    pingUsers
                        .emplace<EditableModelView>(
                            (new UserHighlightModel(nullptr))
                                ->initialized(&getSettings()->highlightedUsers))
                        .getElement();

                view->addRegexHelpLink();
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::UseRegex);
                view->getTableView()->horizontalHeader()->hideSection(
                    HighlightModel::Column::CaseSensitive);

                view->setTitles({uiText("Username"), uiText("Show in\nMentions"),
                                 uiText("Flash\ntaskbar"), uiText("Enable\nregex"),
                                 uiText("Case-\nsensitive"), uiText("Play\nsound"),
                                 uiText("Custom\nsound"), uiText("Color")});
                view->getTableView()->setItemDelegateForColumn(
                    UserHighlightModel::Column::Color,
                    new ColorItemDelegate(view));
                view->setReadableColumns({0}, true);

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->highlightedUsers.append(HighlightPhrase{
                        "highlighted user", true, true, false, false, false, "",
                        *ColorProvider::instance().color(
                            ColorType::SelfHighlight)});
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Users);
                                 });
            }

            auto badgeHighlights = tabs.appendTab(new QVBoxLayout, uiText("Badges"));
            {
                badgeHighlights.emplace<QLabel>(
                    uiText("Play notification sounds and highlight messages based on "
                    "user badges.\n"
                    "Badge highlights are prioritized under user and message "
                    "highlights."))->setWordWrap(true);
                auto *view = badgeHighlights
                                 .emplace<EditableModelView>(
                                     (new BadgeHighlightModel(nullptr))
                                         ->initialized(
                                             &getSettings()->highlightedBadges))
                                 .getElement();
                view->setTitles({uiText("Name"), uiText("Show In\nMentions"), uiText("Flash\ntaskbar"),
                                 uiText("Play\nsound"), uiText("Custom\nsound"), uiText("Color")});
                view->getTableView()->setItemDelegateForColumn(
                    BadgeHighlightModel::Column::Color,
                    new ColorItemDelegate(view));
                view->setReadableColumns({0}, true);

                std::ignore = view->addButtonPressed.connect([this] {
                    auto d = std::make_shared<BadgePickerDialog>(
                        availableBadges, this);

                    d->setWindowTitle(uiText("Choose badge"));
                    if (d->exec() == QDialog::Accepted)
                    {
                        auto s = d->getSelection();
                        if (!s)
                        {
                            return;
                        }
                        getSettings()->highlightedBadges.append(
                            HighlightBadge{s->badgeName(), s->displayName(),
                                           false, false, false, "",
                                           *ColorProvider::instance().color(
                                               ColorType::SelfHighlight)});
                    }
                });

                QObject::connect(view->getTableView(), &QTableView::clicked,
                                 [this, view](const QModelIndex &clicked) {
                                     this->tableCellClicked(
                                         clicked, view, HighlightTab::Badges);
                                 });
            }

            auto disabledUsers =
                tabs.appendTab(new QVBoxLayout, uiText("Blacklisted Users"));
            {
                disabledUsers.emplace<QLabel>(
                    uiText("Disable notification sounds and highlights from certain "
                    "users (e.g. bots)."))->setWordWrap(true);
                EditableModelView *view =
                    disabledUsers
                        .emplace<EditableModelView>(
                            (new HighlightBlacklistModel(nullptr))
                                ->initialized(&getSettings()->blacklistedUsers))
                        .getElement();

                view->addRegexHelpLink();
                view->setTitles({uiText("Username"), uiText("Enable\nregex")});
                view->setReadableColumns({0}, true);

                std::ignore = view->addButtonPressed.connect([] {
                    getSettings()->blacklistedUsers.append(
                        HighlightBlacklistUser{"blacklisted user", false});
                });
            }
        }

        auto customSound = layout.emplace<QHBoxLayout>().withoutMargin();
        {
            auto label = customSound.append(this->createLabel<QString>(
                [](const auto &value) {
                    if (value.isEmpty())
                    {
                        return uiText(QString("Default sound: Chatterino Ping"));
                    }

                    auto url = QUrl::fromLocalFile(value);
                    return uiText(QString("Default sound: <a href=\"%1\"><span "
                                   "style=\"color: white\">%2</span></a>"))
                        .arg(url.toString(QUrl::FullyEncoded),
                             shortenString(url.fileName(), 50));
                },
                getSettings()->pathHighlightSound));
            label->setToolTip(
                uiText("This sound will play for all highlight phrases that have "
                "sound enabled and don't have a custom sound set."));
            label->setTextFormat(Qt::RichText);
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                           Qt::LinksAccessibleByKeyboard);
            label->setOpenExternalLinks(true);
            customSound->setStretchFactor(label.getElement(), 1);

            auto clearSound = customSound.emplace<QPushButton>(uiText("Clear"));
            auto selectFile = customSound.emplace<QPushButton>(uiText("Change..."));

            QObject::connect(selectFile.getElement(), &QPushButton::clicked,
                             this, [this]() mutable {
                                 auto fileName = QFileDialog::getOpenFileName(
                                     this, tr("Open Sound"), "",
                                     tr("Audio Files (*.mp3 *.wav)"));

                                 getSettings()->pathHighlightSound = fileName;
                             });
            QObject::connect(clearSound.getElement(), &QPushButton::clicked,
                             this, [=]() mutable {
                                 getSettings()->pathHighlightSound = QString();
                             });

            getSettings()->pathHighlightSound.connect(
                [clearSound = clearSound.getElement()](const auto &value) {
                    if (value.isEmpty())
                    {
                        clearSound->hide();
                    }
                    else
                    {
                        clearSound->show();
                    }
                },
                this->managedConnections_);
        }

        auto volumeRow = layout.emplace<QHBoxLayout>().withoutMargin();
        auto volumeCaption = volumeRow.emplace<QLabel>(uiText("Sound volume"));
        auto *volume = volumeRow.emplace<QSlider>(Qt::Horizontal).getElement();
        volume->setObjectName("NotificationSoundVolume");
        volumeCaption->setBuddy(volume);
        volume->setAccessibleName(volumeCaption->text());
        volume->setRange(0, 100);
        volume->setPageStep(10);
        volumeRow->setStretchFactor(volume, 1);
        auto *percentage = volumeRow.emplace<QLabel>().getElement();
        percentage->setObjectName("NotificationSoundVolumeValue");
        percentage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        percentage->setMinimumWidth(percentage->fontMetrics().horizontalAdvance("100%") + 8);
        auto *preview = volumeRow.emplace<QPushButton>(uiText("Preview sound")).getElement();
        preview->setObjectName("NotificationSoundPreview");
        getSettings()->soundVolume.connect([volume, percentage](int value) {
            const QSignalBlocker blocker(volume);
            volume->setValue(value);
            percentage->setText(QString::number(volume->value()) + u'%');
        }, this->managedConnections_);
        QObject::connect(volume, &QSlider::valueChanged, this, [](int value) {
            getSettings()->soundVolume = value;
        });
        QObject::connect(preview, &QPushButton::clicked, this, [] {
            const auto path = getSettings()->pathHighlightSound.getValue();
            getApp()->getSound()->play(path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path));
        });

        layout.append(createCheckBox(
            uiText("Play highlight sound even when Chatterino Noctis is focused"),
            getSettings()->highlightAlwaysPlaySound));
        layout.append(createCheckBox(
            uiText("Flash taskbar only stops highlighting when Chatterino Noctis is focused"),
            getSettings()->longAlerts));
    }

    this->disabledUsersChangedTimer_.setSingleShot(true);
}

void HighlightingPage::openSoundDialog(const QModelIndex &clicked,
                                       EditableModelView *view, int soundColumn)
{
    auto fileUrl = QFileDialog::getOpenFileUrl(this, tr("Open Sound"), QUrl(),
                                               tr("Audio Files (*.mp3 *.wav)"));
    view->getModel()->setData(clicked, fileUrl, Qt::UserRole);
    view->getModel()->setData(clicked, fileUrl.fileName(), Qt::DisplayRole);
}

void HighlightingPage::openColorDialog(const QModelIndex &clicked,
                                       EditableModelView *view,
                                       HighlightTab tab)
{
    auto initial =
        view->getModel()->data(clicked, Qt::DecorationRole).value<QColor>();

    auto *dialog = new ColorPickerDialog(initial, this);

    QObject::connect(dialog, &ColorPickerDialog::colorConfirmed, this,
                     [=](auto selected) {
                         if (selected.isValid())
                         {
                             view->getModel()->setData(clicked, selected,
                                                       Qt::DecorationRole);
                         }
                     });
    dialog->show();
}

void HighlightingPage::tableCellClicked(const QModelIndex &clicked,
                                        EditableModelView *view,
                                        HighlightTab tab)
{
    if (!clicked.flags().testFlag(Qt::ItemIsEnabled))
    {
        return;
    }

    switch (tab)
    {
        case HighlightTab::Messages:
        case HighlightTab::Users: {
            using Column = HighlightModel::Column;

            if (clicked.column() == Column::SoundPath)
            {
                this->openSoundDialog(clicked, view, Column::SoundPath);
            }
            else if (clicked.column() == Column::Color)
            {
                this->openColorDialog(clicked, view, tab);
            }
        }
        break;

        case HighlightTab::Badges: {
            using Column = BadgeHighlightModel::Column;
            if (clicked.column() == Column::SoundPath)
            {
                this->openSoundDialog(clicked, view, Column::SoundPath);
            }
            else if (clicked.column() == Column::Color)
            {
                this->openColorDialog(clicked, view, tab);
            }
        }
        break;

        case HighlightTab::Blacklist:
            break;
    }
}

}  // namespace chatterino
