// SPDX-License-Identifier: MIT

#include "widgets/helper/UsercardRolesPage.hpp"

#include "Application.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/helper/ChannelAvatarLabel.hpp"
#include "widgets/helper/UsercardChrome.hpp"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

using Kind = moddex::RoleKind;
using Result = moddex::ResultStatus;

QString roleCount(Kind kind, const std::optional<qint64> &total)
{
    const auto title = kind == Kind::Moderator ? uiText("Moderators")
                                               : uiText("VIPs");
    if (!total)
    {
        return title;
    }
    return title + QStringLiteral(" (%1)").arg(QLocale().toString(*total));
}

QString dateText(const QDateTime &date)
{
    // The index stores the calendar day in UTC. Local time conversion could
    // shift a grant at UTC midnight to the previous day.
    return activeUiLanguage() == "ru"
               ? date.toUTC().date().toString("dd.MM.yyyy")
               : QLocale(QLocale::English).toString(date.toUTC().date(),
                                                    "MMM d, yyyy");
}

QString roleKey(const moddex::Role &role, bool past)
{
    if (!past)
    {
        return role.channelId;
    }
    // A channel may have granted and revoked the role more than once.
    return role.channelId + u'\n' + role.grantedAt.toString(Qt::ISODateWithMs) +
           u'\n' + role.endedAt.toString(Qt::ISODateWithMs);
}

QLabel *plainLabel(const QString &text, const QString &name, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

}  // namespace

UsercardRolesPage::UsercardRolesPage(QWidget *parent, QUrl baseUrl)
    : QWidget(parent)
    , provider_(baseUrl)
    , lookupAvatars_(baseUrl.host() == QStringLiteral("api.moddex.tv"))
{
    this->setObjectName("UsercardRolesPage");
    this->setMinimumWidth(0);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    auto *modes = new QFrame(this);
    modes->setObjectName("usercardRolesModes");
    auto *modeLayout = new QHBoxLayout(modes);
    modeLayout->setContentsMargins(3, 3, 3, 3);
    modeLayout->setSpacing(3);
    auto *modeGroup = new QButtonGroup(this);
    auto tab = [](QWidget *parent, QHBoxLayout *row, QButtonGroup *group,
                  const QString &name, const QString &caption) {
        auto *button = new QPushButton(caption, parent);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        group->addButton(button);
        row->addWidget(button, 1);
        return button;
    };
    this->activeButton_ = tab(modes, modeLayout, modeGroup, "usercardRolesActive",
                              uiText("Current roles"));
    this->pastButton_ = tab(modes, modeLayout, modeGroup, "usercardRolesPast",
                            uiText("Removed roles"));
    this->activeButton_->setChecked(true);
    layout->addWidget(modes);

    auto *filters = new QWidget(this);
    filters->setObjectName("usercardRolesFilters");
    auto *filterLayout = new QHBoxLayout(filters);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(4);
    auto *filterGroup = new QButtonGroup(this);
    this->modsButton_ = tab(filters, filterLayout, filterGroup,
                            "usercardRolesModerators", uiText("Moderators"));
    this->vipsButton_ = tab(filters, filterLayout, filterGroup,
                            "usercardRolesVips", uiText("VIPs"));
    filterLayout->setStretch(0, 3);
    filterLayout->setStretch(1, 2);
    this->modsButton_->setChecked(true);
    layout->addWidget(filters);

    this->status_ = plainLabel({}, "usercardRolesStatus", this);
    layout->addWidget(this->status_);

    this->scroll_ = new QScrollArea(this);
    this->scroll_->setObjectName("usercardRolesList");
    this->scroll_->setWidgetResizable(true);
    this->scroll_->setFrameShape(QFrame::NoFrame);
    this->scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    this->scroll_->viewport()->setObjectName("usercardRolesViewport");
    auto *rowsHost = new QWidget;
    rowsHost->setObjectName("usercardRolesRows");
    this->rows_ = new QVBoxLayout(rowsHost);
    this->rows_->setContentsMargins(0, 0, 1, 0);
    this->rows_->setSpacing(5);
    this->rows_->setAlignment(Qt::AlignTop);
    this->scroll_->setWidget(rowsHost);
    layout->addWidget(this->scroll_, 1);

    this->moreButton_ = new QPushButton(uiText("More roles"), this);
    this->moreButton_->setObjectName("usercardRolesMore");
    this->retryButton_ = new QPushButton(uiText("Retry role lookup"), this);
    this->retryButton_->setObjectName("usercardRolesRetry");
    for (auto *button : {this->moreButton_, this->retryButton_})
    {
        button->setCursor(Qt::PointingHandCursor);
        layout->addWidget(button);
    }
    this->source_ = plainLabel(uiText("moddex · indexed channels"),
                               "usercardRolesSource", this);
    layout->addWidget(this->source_);

    connect(this->activeButton_, &QPushButton::clicked, this, [this] {
        this->past_ = false;
        this->updateControls();
        this->ensureLoaded();
    });
    connect(this->pastButton_, &QPushButton::clicked, this, [this] {
        this->past_ = true;
        this->updateControls();
        this->ensureLoaded();
    });
    connect(this->modsButton_, &QPushButton::clicked, this, [this] {
        this->kind_ = Kind::Moderator;
        this->updateControls();
    });
    connect(this->vipsButton_, &QPushButton::clicked, this, [this] {
        this->kind_ = Kind::Vip;
        this->updateControls();
    });
    connect(this->moreButton_, &QPushButton::clicked, this, [this] {
        this->requestPage(this->past_, this->kind_);
    });
    connect(this->retryButton_, &QPushButton::clicked, this,
             &UsercardRolesPage::retry);

    this->applyStyle();
    this->updateControls();
}

std::size_t UsercardRolesPage::index(bool past, Kind kind)
{
    return (past ? 2 : 0) + (kind == Kind::Vip ? 1 : 0);
}

std::size_t UsercardRolesPage::selectedIndex() const
{
    return index(this->past_, this->kind_);
}

void UsercardRolesPage::setTarget(const QString &userId, const QString &)
{
    if (userId == this->userId_)
    {
        this->ensureLoaded();
        return;
    }
    this->userId_ = userId;
    this->resetLists();
    this->ensureLoaded();
}

void UsercardRolesPage::setActive(bool active)
{
    this->active_ = active;
    if (active)
    {
        this->ensureLoaded();
    }
}

void UsercardRolesPage::setNetworkEnabled(bool enabled)
{
    if (this->networkEnabled_ == enabled)
    {
        return;
    }
    this->networkEnabled_ = enabled;
    this->resetLists();
    this->ensureLoaded();
}

void UsercardRolesPage::setScale(float scale)
{
    const auto changed = this->scale_ != scale;
    this->scale_ = scale;
    this->applyStyle();
    if (changed)
    {
        this->renderedIndex_ = -1;
        this->renderRows();
    }
}

void UsercardRolesPage::resetLists()
{
    // Results for a previously selected card identity are discarded.
    ++this->generation_;
    this->lists_ = {};
    this->past_ = false;
    this->kind_ = Kind::Moderator;
    this->renderedIndex_ = -1;
    this->updateControls();
}

void UsercardRolesPage::ensureLoaded()
{
    if (!this->active_ || !this->networkEnabled_ || this->userId_.isEmpty())
    {
        return;
    }
    if (!this->past_)
    {
        if (this->lists_[0].state == State::Idle)
        {
            this->requestCurrent();
        }
        return;
    }
    for (const auto kind : {Kind::Moderator, Kind::Vip})
    {
        if (this->lists_[index(true, kind)].state == State::Idle)
        {
            this->requestPage(true, kind);
        }
    }
}

void UsercardRolesPage::requestCurrent()
{
    auto &mods = this->lists_[0];
    auto &vips = this->lists_[1];
    if (!this->active_ || !this->networkEnabled_ || this->userId_.isEmpty() ||
        mods.state == State::Loading || vips.state == State::Loading)
    {
        return;
    }
    mods.state = State::Loading;
    vips.state = State::Loading;
    this->updateControls();
    const auto generation = this->generation_;
    this->provider_.requestCurrent(
        this->userId_, this,
        [this, generation](Result result, moddex::CurrentRoles roles) {
            if (generation != this->generation_)
            {
                return;
            }
            if (result == Result::Unavailable)
            {
                this->unavailable();
                return;
            }
            this->finishPage(0, result, std::move(roles.moderators));
            this->finishPage(1, result, std::move(roles.vips));
            this->updateControls();
        });
}

void UsercardRolesPage::requestPage(bool past, Kind kind)
{
    const auto i = index(past, kind);
    auto &state = this->lists_[i];
    if (!this->active_ || !this->networkEnabled_ || this->userId_.isEmpty() ||
        state.state == State::Loading || state.state == State::Unavailable ||
        (state.loaded && !state.page.hasMore))
    {
        return;
    }
    state.state = State::Loading;
    this->updateControls();
    const auto generation = this->generation_;
    this->provider_.requestPage(
        this->userId_, kind, past, state.page.cursor, this,
        [this, generation, i](Result result, moddex::RolePage page) {
            if (generation != this->generation_)
            {
                return;
            }
            if (result == Result::Unavailable)
            {
                this->unavailable();
                return;
            }
            this->finishPage(i, result, std::move(page));
            this->updateControls();
        });
}

void UsercardRolesPage::retry()
{
    const auto &state = this->lists_[this->selectedIndex()];
    if (!this->past_ && !state.loaded)
    {
        this->requestCurrent();
    }
    else
    {
        this->requestPage(this->past_, this->kind_);
    }
}

void UsercardRolesPage::unavailable()
{
    // The service deliberately uses the same response for unknown and
    // non-public identities. Drop loaded rows and don't try another lookup.
    ++this->generation_;
    this->lists_ = {};
    for (auto &state : this->lists_)
    {
        state.state = State::Unavailable;
    }
    this->renderedIndex_ = -1;
    this->updateControls();
}

void UsercardRolesPage::finishPage(std::size_t i, Result result,
                                    moddex::RolePage page)
{
    auto &state = this->lists_[i];
    if (result != Result::Ok)
    {
        state.state = State::Error;
        return;
    }
    auto &current = state.page;
    const bool past = i >= 2;
    QSet<QString> seen;
    for (const auto &role : current.items)
    {
        seen.insert(roleKey(role, past));
    }
    for (auto &role : page.items)
    {
        auto key = roleKey(role, past);
        if (!seen.contains(key))
        {
            seen.insert(std::move(key));
            current.items.push_back(std::move(role));
        }
    }
    if (page.total)
    {
        current.total = page.total;
    }
    current.hasMore = page.hasMore;
    current.cursor = std::move(page.cursor);
    state.loaded = true;
    state.state = State::Ready;
}

void UsercardRolesPage::updateControls()
{
    this->activeButton_->setChecked(!this->past_);
    this->pastButton_->setChecked(this->past_);
    this->modsButton_->setChecked(this->kind_ == Kind::Moderator);
    this->vipsButton_->setChecked(this->kind_ == Kind::Vip);
    this->modsButton_->setText(roleCount(
        Kind::Moderator, this->lists_[index(this->past_, Kind::Moderator)].page.total));
    this->vipsButton_->setText(roleCount(
        Kind::Vip, this->lists_[index(this->past_, Kind::Vip)].page.total));

    const auto &state = this->lists_[this->selectedIndex()];
    QString status;
    if (!this->networkEnabled_)
    {
        status = uiText("Role lookup is unavailable");
    }
    else if (this->userId_.isEmpty())
    {
        status = uiText("Waiting for Twitch user ID…");
    }
    else if (state.state == State::Loading)
    {
        status = uiText("Loading roles…");
    }
    else if (state.state == State::Unavailable)
    {
        status = uiText("No public role data for this user");
    }
    else if (state.state == State::Error)
    {
        status = uiText("Could not load roles. Try again.");
    }
    else if (state.state == State::Ready && state.page.items.empty())
    {
        status = this->past_ ? uiText("No indexed removed roles of this type")
                             : uiText("No indexed current roles of this type");
    }
    this->status_->setText(status);
    this->status_->setVisible(!status.isEmpty());
    this->source_->setText(this->past_ ? uiText("moddex · removed roles since August 2026")
                                        : uiText("moddex · indexed channels"));
    this->moreButton_->setVisible(state.state == State::Ready && state.page.hasMore);
    this->retryButton_->setVisible(state.state == State::Error);
    this->renderRows();
}

void UsercardRolesPage::renderRows()
{
    const auto i = this->selectedIndex();
    const auto &roles = this->lists_[i].page.items;
    if (static_cast<int>(i) != this->renderedIndex_ ||
        roles.size() < this->renderedCount_)
    {
        while (auto *item = this->rows_->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        this->renderedIndex_ = static_cast<int>(i);
        this->renderedCount_ = 0;
        this->scroll_->verticalScrollBar()->setValue(0);
    }
    for (; this->renderedCount_ < roles.size(); ++this->renderedCount_)
    {
        this->addRow(roles[this->renderedCount_]);
    }
}

void UsercardRolesPage::addRow(const moddex::Role &role)
{
    auto *row = new QFrame(this->scroll_->widget());
    row->setObjectName("usercardRoleRow");
    auto *outer = new QHBoxLayout(row);
    outer->setContentsMargins(qRound(8 * this->scale_), qRound(6 * this->scale_),
                              qRound(8 * this->scale_), qRound(6 * this->scale_));
    outer->setSpacing(qRound(7 * this->scale_));

    auto *details = new QWidget(row);
    auto *lines = new QVBoxLayout(details);
    lines->setContentsMargins(0, 0, 0, 0);
    lines->setSpacing(qRound(2 * this->scale_));
    auto *channel = plainLabel(role.displayName.isEmpty() ? role.login : role.displayName,
                               "usercardRoleChannel", details);
    channel->setProperty("channelLogin", role.login);
    channel->setFont(getApp()->getFonts()->getFont(FontStyle::ChatMedium, this->scale_));
    auto *avatar = new ChannelAvatarLabel(channel, row);
    avatar->setChannel(role.login, this->lookupAvatars_ && this->networkEnabled_);
    outer->addWidget(avatar, 0, Qt::AlignTop);
    outer->addWidget(details, 1);
    lines->addWidget(channel);

    QStringList dates;
    if (role.grantedAt.isValid())
    {
        dates.append(uiText("Granted: %1").arg(dateText(role.grantedAt)));
    }
    if (this->past_ && role.endedAt.isValid())
    {
        dates.append(uiText("Removed by %1").arg(dateText(role.endedAt)));
    }
    if (!dates.isEmpty())
    {
        auto *date = plainLabel(dates.join(QStringLiteral(" · ")),
                                "usercardRoleDate", details);
        date->setFont(getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall,
                                                   this->scale_));
        lines->addWidget(date);
    }
    this->rows_->addWidget(row);
}

void UsercardRolesPage::applyStyle()
{
    const auto colors = usercardChrome::palette(getTheme()->isLightTheme());
    auto *fonts = getApp()->getFonts();
    this->setFont(fonts->getFont(FontStyle::ChatMedium, this->scale_));
    const auto small = fonts->getFont(FontStyle::ChatMediumSmall, this->scale_);
    for (auto *button : {this->activeButton_, this->pastButton_, this->modsButton_,
                          this->vipsButton_, this->moreButton_, this->retryButton_})
    {
        button->setFont(small);
    }
    this->status_->setFont(small);
    this->source_->setFont(small);

    this->layout()->setContentsMargins(qRound(8 * this->scale_),
                                       qRound(8 * this->scale_),
                                       qRound(8 * this->scale_),
                                       qRound(8 * this->scale_));
    this->layout()->setSpacing(qRound(7 * this->scale_));
    this->rows_->setSpacing(qRound(5 * this->scale_));
    this->setStyleSheet(
        QStringLiteral(
            "#UsercardRolesPage { background: transparent; color:%1; }"
            "#UsercardRolesPage QLabel { background:transparent; color:%1; }"
            "QFrame#usercardRolesModes { background:%2; border:1px solid %3;"
            " border-radius:8px; }"
            "#usercardRolesModes QPushButton { background:transparent; color:%4;"
            " border:0; padding:5px 4px; border-radius:5px; }"
            "#usercardRolesModes QPushButton:hover { color:%1; }"
            "#usercardRolesModes QPushButton:checked { background:%5; color:%6; }"
            "#usercardRolesFilters QPushButton { background:transparent; color:%4;"
            " border:0; border-bottom:1px solid %3; padding:5px 4px; }"
            "#usercardRolesFilters QPushButton:hover { color:%1; }"
            "#usercardRolesFilters QPushButton:checked { color:%6;"
            " border-bottom-color:%6; }"
            "#usercardRolesList, #usercardRolesViewport, #usercardRolesRows"
            " { background:transparent; border:0; }"
            "#usercardRolesList QScrollBar:vertical { background:transparent;"
            " width:7px; margin:2px 0; border:0; }"
            "#usercardRolesList QScrollBar::handle:vertical { background:%7;"
            " min-height:24px; border-radius:3px; }"
            "#usercardRolesList QScrollBar::handle:vertical:hover { background:%6; }"
            "#usercardRolesList QScrollBar::add-line:vertical,"
            "#usercardRolesList QScrollBar::sub-line:vertical { height:0; }"
            "#usercardRolesList QScrollBar::add-page:vertical,"
            "#usercardRolesList QScrollBar::sub-page:vertical { background:none; }"
            "QFrame#usercardRoleRow { background:%2; border:1px solid %3;"
            " border-radius:7px; }"
            "QLabel#usercardRoleDate, QLabel#usercardRolesStatus,"
            " QLabel#usercardRolesSource { color:%4; }"
            "QPushButton#usercardRolesMore, QPushButton#usercardRolesRetry"
            " { background:%2; color:%6; border:1px solid %3; border-radius:6px;"
            " padding:5px 8px; }"
            "QPushButton#usercardRolesMore:hover, QPushButton#usercardRolesRetry:hover"
            " { background:%5; border-color:%6; }")
            .arg(colors.foreground, colors.panel, colors.border, colors.muted,
                 colors.cover, colors.accent, colors.scroll));
}

}  // namespace chatterino
