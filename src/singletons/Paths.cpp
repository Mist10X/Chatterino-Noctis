// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Paths.hpp"

#include "common/Args.hpp"
#include "common/Isolation.hpp"
#include "common/Modes.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <cassert>
#include <mutex>

using namespace std::literals;

namespace chatterino {

Paths::Paths(const Args &args, const Modes &modes)
{
    this->initAppFilePathHash();

    this->initRootDirectory(args, modes);
    this->initSubDirectories();
}

bool Paths::createFolder(const QString &folderPath)
{
    return QDir().mkpath(folderPath);
}

QString Paths::cacheDirectory() const
{
    static const auto pathSetting = [this] {
        QStringSetting cachePathSetting("/cache/path");

        cachePathSetting.connect([](const auto &newPath) {
            if (!newPath.isEmpty())
            {
                QDir().mkpath(newPath);
            }
        });

        return cachePathSetting;
    }();

    auto path = pathSetting.getValue();

    if (path.isEmpty())
    {
        return this->cacheDirectory_;
    }

    // An older version or an imported profile may name any folder (even
    // Documents): the cache then lives in its own folder inside it, so the
    // clean-up never touches anything else.
    static std::mutex mutex;
    static QString checkedPath;
    static QString checkedResult;
    std::lock_guard lock(mutex);
    if (path != checkedPath)
    {
        checkedPath = path;
        checkedResult = path;
        if (!this->isOwnCacheFolder(path))
        {
            // Existing profiles may still store their cache under the old
            // folder name. Reuse that cache, while new folders use Noctis.
            const auto legacy =
                QDir(path).filePath(QStringLiteral("Chatterino by Mist1X cache"));
            const auto renamed =
                QDir(path).filePath(QStringLiteral("Chatterino Noctis cache"));
            checkedResult = !QFileInfo::exists(renamed) && QFileInfo(legacy).isDir()
                                ? legacy : renamed;
            QDir().mkpath(checkedResult);
        }
    }
    return checkedResult;
}

bool Paths::isOwnCacheFolder(const QString &path) const
{
    const auto clean = QDir::cleanPath(QDir(path).absolutePath());
    const auto root = QDir::cleanPath(this->rootAppDataDirectory);
    const auto inside = [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) == 0 ||
               a.startsWith(b + u'/', Qt::CaseInsensitive);
    };
    if (clean.isEmpty() || inside(root, clean))
    {
        return false;  // the program folder itself or a parent of it
    }
    if (clean.compare(QDir::cleanPath(this->cacheDirectory_),
                      Qt::CaseInsensitive) == 0)
    {
        return true;
    }
    const auto name = QFileInfo(clean).fileName();
    return name.compare(QStringLiteral("Chatterino Noctis cache"),
                        Qt::CaseInsensitive) == 0 ||
           name.compare(QStringLiteral("Chatterino by Mist1X cache"),
                        Qt::CaseInsensitive) == 0;
}

QString Paths::messageLogsIn(const QString &configured) const
{
    return configured.isEmpty() ? this->messageLogDirectory : configured;
}

QString Paths::cacheFilePath(const QString &fileName) const
{
    return combinePath(this->cacheDirectory(), fileName);
}

void Paths::initAppFilePathHash()
{
    auto path = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
    // Preserve the browser extension queue identity across the executable rename.
    if (QFileInfo(path).fileName().compare(QStringLiteral("Chatterino Noctis.exe"),
                                           Qt::CaseInsensitive) == 0)
    {
        path = QFileInfo(path).dir().filePath(QStringLiteral("Leafyrino.exe"));
    }
#endif
    this->applicationFilePathHash =
        QCryptographicHash::hash(
            path.toUtf8(),
            QCryptographicHash::Sha224)
            .toBase64()
            .mid(0, 32)
            .replace("+", "-")
            .replace("/", "x");
}

void Paths::initRootDirectory(const Args &args, const Modes &modes)
{
    // Installed Linux packages have a read-only program directory. Portable
    // copies retain their profile beside the executable or in --portable-dir.
    static_assert(isolation::FORCE_PORTABLE);
    assert(modes.isPortable);
    (void)modes;

    QString root = QCoreApplication::applicationDirPath();
    if (args.portableDirectory.has_value())
    {
        root = args.portableDirectory.value();
        if (!QFileInfo(root).isDir())
        {
            throw std::runtime_error(
                "The profile directory \""s +
                QDir::toNativeSeparators(root).toStdString() +
                "\" does not exist. Chatterino Noctis will not create or use "
                "another one.");
        }
    }
#ifdef Q_OS_LINUX
    else if (qEnvironmentVariableIsSet("APPIMAGE") || root == QStringLiteral("/usr/bin") ||
             root == QStringLiteral("/usr/local/bin") || root == QStringLiteral("/app/bin") ||
             root.startsWith(QStringLiteral("/nix/store/")))
    {
        const auto dataRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (dataRoot.isEmpty())
        {
            throw std::runtime_error("Could not locate a writable user data directory.");
        }
        root = QDir(dataRoot).filePath(QStringLiteral("chatterino-noctis"));
    }
#endif

    const auto reason =
        isolation::rejectProfileRoot(root, isolation::foreignProfileRoots());
    if (!reason.isEmpty())
    {
        throw std::runtime_error(
            "Chatterino Noctis refused to use this profile directory. "s +
            reason.toStdString());
    }

    this->rootAppDataDirectory = QDir::cleanPath(root);
}

void Paths::initSubDirectories()
{
    assert(!this->rootAppDataDirectory.isEmpty());

    auto makePath = [&](const QString &name) -> QString {
        auto path = combinePath(this->rootAppDataDirectory, name);

        if (!QDir().mkpath(path))
        {
            throw std::runtime_error("Could not create directory \""s +
                                     path.toStdString() + "\"");
        }

        return path;
    };

    makePath("");
    this->settingsDirectory = makePath("Settings");
    for (const auto &dir : {this->rootAppDataDirectory, this->settingsDirectory})
    {
        const auto error = isolation::probeWritable(dir);
        if (!error.isEmpty())
        {
            throw std::runtime_error(error.toStdString());
        }
    }
    this->cacheDirectory_ = makePath("Cache");
    this->messageLogDirectory = makePath("Logs");
    this->miscDirectory = makePath("Misc");
    this->twitchProfileAvatars =
        makePath(combinePath("ProfileAvatars", "twitch"));
    this->pluginsDirectory = makePath("Plugins");
    this->themesDirectory = makePath("Themes");
    this->crashdumpDirectory = makePath("Crashes");
    this->dictionariesDirectory = makePath("Dictionaries");
#ifdef Q_OS_WIN
    this->ipcDirectory = makePath("IPC");
#else

    this->ipcDirectory = "/tmp";
#endif
}

}  // namespace chatterino
