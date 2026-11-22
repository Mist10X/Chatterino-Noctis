#pragma once

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/LinkParser.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include "common/UiLanguage.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "singletons/Paths.hpp"

#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <functional>

namespace chatterino::whisperhistory {

// Account labels have a verified spelling even when the general user cache is
// cold or still contains the protocol login. Never replace it with a fallback.
inline QString senderDisplayName(const TwitchAccount &account,
                                 const QString &resolvedName = {})
{
    if (account.hasVerifiedDisplayName())
        return account.getDisplayName();
    if (!resolvedName.isEmpty() &&
        resolvedName.compare(account.getUserName(), Qt::CaseInsensitive) == 0)
        return resolvedName;
    return account.getUserName();
}

inline void seedVerifiedAccountName(QHash<QString, QString> &knownNames,
                                    const QString &historyAccount,
                                    const TwitchAccount &account)
{
    if (!historyAccount.isEmpty() && historyAccount == account.getUserId() &&
        !account.isAnon() && account.hasVerifiedDisplayName())
        knownNames.insert(historyAccount + QStringLiteral(":") +
                              account.getUserName().toLower(),
                          account.getDisplayName());
}

inline QJsonArray badgeSnapshot(const Message &message)
{
    QJsonArray rows;
    for (const auto &element : message.elements)
    {
        const auto *badge = dynamic_cast<const BadgeElement *>(element.get());
        if (!badge)
            continue;
        const auto emote = badge->getEmote();
        QJsonArray images;
        for (const auto &image : {emote->images.getImage1(), emote->images.getImage2(),
                                  emote->images.getImage3(), emote->images.getImage4()})
        {
            QJsonObject item;
            if (!image->isEmpty())
            {
                item["scale"] = image->scale();
                item["width"] = qRound(image->width() / image->scale());
                item["height"] = qRound(image->height() / image->scale());
                if (!image->url().string.isEmpty())
                {
                    item["url"] = image->url().string;
                }
                else if (auto pixmap = image->pixmapOrLoad();
                         pixmap && pixmap->width() <= 128 && pixmap->height() <= 128)
                {
                    QByteArray bytes;
                    QBuffer buffer(&bytes);
                    buffer.open(QIODevice::WriteOnly);
                    if (pixmap->save(&buffer, "PNG"))
                        item["png"] = QString::fromLatin1(bytes.toBase64());
                }
            }
            images.append(item);
        }
        QJsonObject row{{"name", emote->name.string},
                        {"tooltip", badge->getTooltip()},
                        {"flags", QString::number(static_cast<qint64>(badge->getFlags().value()))},
                        {"images", images}};
        if (dynamic_cast<const FfzBadgeElement *>(badge))
            row["background"] = badge->toJson().value("color");
        rows.append(row);
    }
    return rows;
}

inline void restoreBadges(MessageBuilder &builder, const QJsonArray &rows)
{
    for (const auto &value : rows)
    {
        const auto row = value.toObject();
        bool ok = false;
        const auto raw = row.value("flags").toString().toLongLong(&ok);
        const auto mask = static_cast<qint64>(MessageElementFlag::Badges);
        if (!ok || raw <= 0 || (raw & ~mask) != 0)
            continue;
        const auto flags = static_cast<MessageElementFlag>(raw);
        const auto images = row.value("images").toArray();
        auto restoredImage = [&](qsizetype index) {
            if (index >= images.size())
                return Image::getEmpty();
            const auto image = images[index].toObject();
            const auto scale = image.value("scale").toDouble(1.0);
            if (scale <= 0 || scale > 8)
                return Image::getEmpty();
            const QUrl url(image.value("url").toString());
            if (url.isValid() && !url.host().isEmpty() &&
                (url.scheme() == "https" || url.scheme() == "http"))
                return Image::fromUrl(Url{url.toString()}, scale,
                    QSize(std::clamp(image.value("width").toInt(18), 1, 256),
                          std::clamp(image.value("height").toInt(18), 1, 256)));
            const auto png = image.value("png").toString();
            if (!png.isEmpty() && png.size() <= 100000)
            {
                QPixmap pixmap;
                if (pixmap.loadFromData(QByteArray::fromBase64(png.toLatin1()), "PNG") &&
                    pixmap.width() <= 128 && pixmap.height() <= 128)
                    return Image::fromResourcePixmap(pixmap, scale);
            }
            return Image::getEmpty();
        };
        auto first = restoredImage(0);
        auto second = restoredImage(1);
        auto third = restoredImage(2);
        auto fourth = restoredImage(3);
        if (first->isEmpty() && second->isEmpty() && third->isEmpty() && fourth->isEmpty())
            continue;
        auto emote = std::make_shared<Emote>(Emote{
            .name = EmoteName{row.value("name").toString()},
            .images = ImageSet{first, second, third, fourth},
            .tooltip = Tooltip{row.value("tooltip").toString()}});
        if (row.contains("background"))
            builder.emplace<FfzBadgeElement>(emote, flags, QColor(row.value("background").toString()));
        else
            builder.emplace<BadgeElement>(emote, flags);
    }
}

inline QString legacyKey(const Message &message)
{
    return message.userID + u'/' + message.id + u'/' + message.whisperPeer +
           u'/' + QString::number(message.serverReceivedTime.toMSecsSinceEpoch());
}

inline void refreshLegacyBadges(Channel &channel, const QSet<QString> &legacy,
                                const std::function<MessagePtr(const QString &)> &resolve = {})
{
    QHash<QString, MessagePtr> badgesByUser;
    for (const auto &message : channel.getMessageSnapshot())
    {
        if (message->userID.isEmpty() || !legacy.contains(legacyKey(*message)))
            continue;
        auto found = badgesByUser.constFind(message->userID);
        if (found == badgesByUser.cend())
        {
            if (resolve)
                badgesByUser.insert(message->userID, resolve(message->userID));
            else
            {
                MessageBuilder builder;
                builder.appendGlobalBadges(message->userID);
                badgesByUser.insert(message->userID, builder.release());
            }
            found = badgesByUser.constFind(message->userID);
        }
        QSet<QString> present;
        for (const auto &element : message->elements)
            if (auto *badge = dynamic_cast<BadgeElement *>(element.get()))
                present.insert(badge->getEmote()->name.string + u'/' +
                    QString::number(static_cast<qint64>(badge->getFlags().value())));
        std::vector<std::unique_ptr<MessageElement>> missing;
        if (!*found)
            continue;
        for (const auto &element : (*found)->elements)
        {
            const auto *badge = dynamic_cast<const BadgeElement *>(element.get());
            if (!badge)
                continue;
            const auto key = badge->getEmote()->name.string + u'/' +
                QString::number(static_cast<qint64>(badge->getFlags().value()));
            if (!present.contains(key))
            {
                missing.push_back(badge->clone());
                present.insert(key);
            }
        }
        if (missing.empty())
            continue;
        auto corrected = message->clone();
        auto username = std::ranges::find_if(corrected->elements, [](const auto &element) {
            return element->getFlags().has(MessageElementFlag::Username);
        });
        corrected->elements.insert(username, std::make_move_iterator(missing.begin()),
                                   std::make_move_iterator(missing.end()));
        channel.replaceMessage(message, corrected);
    }
}

inline QString historyPath(const QString &accountId)
{
    static const QRegularExpression validId(QStringLiteral("^[0-9]+$"));
    if (!validId.match(accountId).hasMatch() || accountId == "0")
    {
        return {};
    }
    QDir directory(getApp()->getPaths().settingsDirectory);
    if (!directory.mkpath("whisper-history"))
    {
        return {};
    }
    return directory.filePath("whisper-history/" + accountId + ".jsonl");
}

// Keep enough emote information to display a saved whisper after an account
// loses access to an emote or its provider removes it from the catalog.
inline QJsonObject bodyEmotes(const Message &message)
{
    QJsonObject result;
    for (const auto &element : message.elements)
    {
        auto *part = dynamic_cast<const EmoteElement *>(element.get());
        if (!part || dynamic_cast<const BadgeElement *>(part))
            continue;
        const auto emote = part->getEmote();
        QJsonArray urls;
        for (const auto &image : {emote->images.getImage1(), emote->images.getImage2(),
                                  emote->images.getImage3(), emote->images.getImage4()})
            urls.append(image->url().string);
        result.insert(emote->name.string, urls);
    }
    return result;
}

inline void appendBody(MessageBuilder &builder, const QString &text,
                       const QJsonObject &emotes, const TwitchAccount *self, bool withPreviews)
{
    auto *app = getApp();
    for (const QString &word : text.split(u' '))
    {
        EmotePtr emote;
        const auto urls = emotes.value(word).toArray();
        auto imageUrl = [&](int index) -> Url {
            const QUrl url(index < urls.size() ? urls.at(index).toString() : QString());
            return Url{(url.scheme() == "https" || url.scheme() == "http") && !url.host().isEmpty()
                       ? url.toString() : QString{}};
        };
        if (urls.size() == 4 && !(imageUrl(0).string.isEmpty() && imageUrl(1).string.isEmpty() &&
                                  imageUrl(2).string.isEmpty() && imageUrl(3).string.isEmpty()))
            emote = std::make_shared<Emote>(Emote{.name = EmoteName{word},
                .images = ImageSet{imageUrl(0), imageUrl(1), imageUrl(2), imageUrl(3)}});
        if (!emote && self)
        {
            const auto map = *self->accessEmotes();
            if (auto it = map->find(EmoteNameView{word}); it != map->end())
                emote = it->second;
        }
        if (!emote && app->getBttvEmotes())
            emote = app->getBttvEmotes()->emote(EmoteNameView{word}).value_or(nullptr);
        if (!emote && app->getFfzEmotes())
            emote = app->getFfzEmotes()->emote(EmoteNameView{word}).value_or(nullptr);
        if (emote)
        {
            builder.emplace<EmoteElement>(emote, MessageElementFlag::Emote);
            continue;
        }
        auto addText = [&](QStringView part) {
            if (const auto parsed = linkparser::parse(part))
                builder.addLink(*parsed, part, {}, withPreviews);
            else
                builder.emplace<TextElement>(part.toString(), MessageElementFlag::Text);
        };
        if (app->getEmotes() && app->getEmotes()->getEmojis())
        {
            for (const auto &part : app->getEmotes()->getEmojis()->parse(word))
                std::visit([&](const auto &value) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, EmotePtr>)
                        builder.emplace<EmoteElement>(value, MessageElementFlag::EmojiAll);
                    else
                        addText(value);
                }, part);
        }
        else
            addText(word);
    }
}

inline bool append(const QString &accountId, const Message &message,
                   bool outgoing, const QString &peerDisplayName = {})
{
    const auto path = historyPath(accountId);
    if (path.isEmpty() || message.whisperPeer.isEmpty())
    {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        return false;
    }
    auto timestamp = message.serverReceivedTime.isValid()
                         ? message.serverReceivedTime
                         : QDateTime::currentDateTimeUtc();
    QJsonObject record{
        {"id", message.id},
        {"peer", message.whisperPeer},
        {"peer_name", peerDisplayName},
        {"login", message.loginName},
        {"name", message.displayName},
        {"uid", message.userID},
        {"text", message.messageText},
        {"time", timestamp.toUTC().toString(Qt::ISODateWithMs)},
        {"outgoing", outgoing},
        {"color", message.usernameColor.isValid()
                      ? message.usernameColor.name()
                      : QString()},
        {"badges", badgeSnapshot(message)},
        {"emotes", bodyEmotes(message)},
    };
    const auto line = QByteArray("\n") +
                      QJsonDocument(record).toJson(QJsonDocument::Compact) +
                      QByteArray("\n");
    return file.write(line) == line.size() && file.flush();
}

inline void rememberDisplayName(const QString &accountId, const QString &uid,
                                const QString &login, const QString &name)
{
    if (uid.isEmpty() || name.isEmpty() || name == name.toLower() ||
        name.compare(login, Qt::CaseInsensitive) != 0)
        return;
    QFile file(historyPath(accountId));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        const auto row = QJsonObject{{"uid", uid}, {"login", login}, {"name", name}};
        file.write(QByteArray("\n") +
                   QJsonDocument(row).toJson(QJsonDocument::Compact) + "\n");
    }
}

inline void applyDisplayName(Channel &channel, const QString &uid,
                             const QString &login, const QString &name)
{
    if (uid.isEmpty() || name.isEmpty() ||
        name.compare(login, Qt::CaseInsensitive) != 0)
        return;
    for (const auto &message : channel.getMessageSnapshot())
    {
        if (!message->flags.has(MessageFlag::Whisper) || message->userID != uid ||
            message->loginName.compare(login, Qt::CaseInsensitive) != 0 ||
            (!message->displayName.isEmpty() &&
             (message->displayName.compare(message->loginName, Qt::CaseInsensitive) != 0 ||
              message->displayName != message->displayName.toLower())) ||
            message->displayName == name)
            continue;
        auto corrected = message->clone();
        corrected->displayName = name;
        const auto oldPrefix = (message->displayName.isEmpty()
                                    ? message->loginName : message->displayName) + u' ';
        if (corrected->searchText.startsWith(oldPrefix))
            corrected->searchText.replace(0, oldPrefix.size(), name + u' ');
        for (auto &element : corrected->elements)
        {
            if (element->getFlags().has(MessageElementFlag::Username))
                if (auto *text = dynamic_cast<TextElement *>(element.get()))
                    text->setText(name + u':');
        }
        channel.replaceMessage(message, corrected);
    }
}

struct Page
{
    QVector<QJsonObject> records;
    qint64 before = 0;
};

inline Page readPage(const QString &path, qint64 before)
{
    constexpr int pageRows = 100;
    constexpr qint64 blockSize = 32768;
    Page page;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return page;
    auto end = std::min(before, file.size());
    auto pos = end;
    int scanned = 0;
    const auto readLineAt = [&](qint64 start) {
        if (end > start && file.seek(start))
        {
            ++scanned;
            auto row = QJsonDocument::fromJson(file.read(end - start)).object();
            if (!row.isEmpty())
                page.records.append(std::move(row));
        }
    };
    // Walk backwards in blocks, retaining only the selected page. 'before'
    // is an exclusive byte offset, so later appends never shift older pages.
    while (pos > 0)
    {
        auto start = std::max<qint64>(0, pos - blockSize);
        if (!file.seek(start))
            break;
        const auto bytes = file.read(pos - start);
        for (qsizetype i = bytes.size(); i-- > 0;)
        {
            if (bytes[i] != '\n')
                continue;
            readLineAt(start + i + 1);
            end = start + i;
            if (scanned >= pageRows || (scanned > 0 && before - end >= 512 * 1024))
            {
                page.before = end;
                std::reverse(page.records.begin(), page.records.end());
                return page;
            }
        }
        pos = start;
    }
    readLineAt(0);
    std::reverse(page.records.begin(), page.records.end());
    return page;
}

struct Cursor {
    QString account;
    quint64 generation = 0;
    qint64 before = 0;
    bool loading = false;
    QHash<QString, QString> knownNames;
};

inline Cursor &cursor()
{
    static Cursor value;
    return value;
}

inline QSet<QString> applyPage(const Page &page, Channel &channel, QHash<QString, QString> &knownNames)
{
    QSet<QString> legacy;
    QSet<QString> seen;
    for (const auto &existing : channel.getMessageSnapshot())
        if (!existing->id.isEmpty())
            seen.insert(existing->id);
    std::vector<MessagePtr> messages;
    const auto identityKey = [](const QJsonObject &row) {
        const auto uid = row.value("uid").toString();
        const auto login = row.value("login").toString();
        return uid.isEmpty() || login.isEmpty() ? QString{}
            : uid + QStringLiteral(":") + login.toLower();
    };
    // Pages arrive newest first. Retain the latest verified spelling for each identity.
    for (auto it = page.records.crbegin(); it != page.records.crend(); ++it)
    {
        const auto &row = *it;
        const auto key = identityKey(row);
        const auto login = row.value("login").toString();
        const auto name = row.value("name").toString();
        if (!key.isEmpty() && !knownNames.contains(key) && name != name.toLower() &&
            name.compare(login, Qt::CaseInsensitive) == 0)
        {
            knownNames.insert(key, name);
        }
    }
    for (const auto &record : page.records)
    {
        const auto peer = record.value("peer").toString();
        const auto login = record.value("login").toString();
        const auto text = record.value("text").toString();
        const auto id = record.value("id").toString();
        const auto timestamp = QDateTime::fromString(
            record.value("time").toString(), Qt::ISODateWithMs);
        if (peer.isEmpty() || login.isEmpty() || !timestamp.isValid() ||
            (!id.isEmpty() && seen.contains(id)))
        {
            continue;
        }
        if (!id.isEmpty())
        {
            seen.insert(id);
        }
        const bool outgoing = record.value("outgoing").toBool();
        auto name = record.value("name").toString();
        if (name.isEmpty() || (name.compare(login, Qt::CaseInsensitive) == 0 &&
                               name == name.toLower()))
        {
            name = knownNames.value(identityKey(record), name);
        }
        if (name.isEmpty())
        {
            name = login;
        }
        const QColor color(record.value("color").toString());
        MessageBuilder builder;
        builder->id = id;
        builder->whisperPeer = peer;
        builder->loginName = login;
        builder->displayName = name;
        builder->userID = record.value("uid").toString();
        builder->usernameColor = color;
        builder->messageText = text;
        builder->searchText = name + " " + peer + " " + text;
        builder->serverReceivedTime = timestamp;
        builder->flags.set(MessageFlag::Whisper);
        builder->flags.set(MessageFlag::DoNotLog);
        builder->flags.set(MessageFlag::DoNotTriggerNotification);
        builder.emplace<TimestampElement>(timestamp.toLocalTime().time());
        if (record.contains("badges"))
            restoreBadges(builder, record.value("badges").toArray());
        else
            legacy.insert(legacyKey(builder.message()));
        builder.emplace<TextElement>(
            name + u':', MessageElementFlag::Username,
            color.isValid() ? MessageColor(color)
                            : MessageColor(MessageColor::Text),
            FontStyle::ChatMediumBold)->setLink({Link::UserInfo, login});
        auto *accounts = getApp()->getAccounts();
        appendBody(builder, text, record.value("emotes").toObject(),
                   accounts ? accounts->twitch.getCurrent().get() : nullptr, false);
        auto message = builder.release();
        auto peerName = record.value("peer_name").toString();
        if (peerName.isEmpty())
            peerName = peer;
        const auto title = outgoing ? uiText("Whisper to %1").arg(peerName)
                                    : uiText("Whisper");
        MessageBuilder::makeWhisperCard(
            *message, title + " · " +
                          timestamp.toLocalTime().date().toString("dd.MM.yyyy"),
            outgoing ? QString() : peer);
        messages.push_back(std::move(message));
    }
    channel.addMessagesAtStart(messages);
    return legacy;
}

// Used by offline fixtures, and only reads one page.
inline QSet<QString> restore(const QString &account, Channel &channel)
{
    const auto path = historyPath(account);
    QHash<QString, QString> names;
    channel.clearMessages();
    channel.setMessageLimit(0);
    return applyPage(readPage(path, QFileInfo(path).size()), channel, names);
}

inline bool hasOlder(const QString &account)
{
    const auto &state = cursor();
    return state.account == account && !state.loading && state.before > 0;
}

inline void loadPage(const QString &account, const ChannelPtr &channel, bool first,
                     std::function<void(QSet<QString>)> done = {})
{
    auto &state = cursor();
    const auto path = historyPath(account);
    if (first)
    {
        state = Cursor{.account = account, .generation = state.generation + 1};
        channel->clearMessages();
        channel->setMessageLimit(0);
        state.before = path.isEmpty() ? 0 : QFileInfo(path).size();
    }
    if (!hasOlder(account))
        return;
    state.loading = true;
    auto *watcher = new QFutureWatcher<Page>(QCoreApplication::instance());
    QObject::connect(watcher, &QFutureWatcher<Page>::finished, watcher,
        [watcher, weak = std::weak_ptr(channel), generation = state.generation, done = std::move(done)] {
            watcher->deleteLater();
            auto channel = weak.lock();
            auto &state = cursor();
            if (!channel || state.generation != generation)
                return;
            const auto page = watcher->result();
            state.before = page.before;
            state.loading = false;
            if (auto *accounts = getApp()->getAccounts())
                seedVerifiedAccountName(state.knownNames, state.account,
                                        *accounts->twitch.getCurrent());
            auto legacy = applyPage(page, *channel, state.knownNames);
            if (done)
                done(std::move(legacy));
            else if (!legacy.empty())
                refreshLegacyBadges(*channel, legacy);
        });
    watcher->setFuture(QtConcurrent::run(readPage, path, state.before));
}

}  // namespace chatterino::whisperhistory
