// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"
#include "singletons/UpdateCheckPolicy.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Literals.hpp"
#include "common/Mist1XVersion.hpp"
#include "common/Modes.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/UiLanguage.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStringBuilder>
#include <QTimer>
#include <QRandomGenerator>
#include <QtConcurrent>
#include <semver/semver.hpp>

namespace {

using namespace chatterino;
using namespace literals;

/// Releases of Chatterino Noctis are published here (a public repository,
/// so the check needs no login; GitHub allows 60 such requests an hour).
const QString RELEASES_API =
    u"https://api.github.com/repos/Mist10X/chatterino-noctis/releases"_s;
const QString RELEASES_PAGE =
    u"https://github.com/Mist10X/chatterino-noctis/releases"_s;

/// For the update tests only: MIST1X_UPDATE_API points the check at a local
/// copy of the releases API, MIST1X_UPDATE_TEST_INSTALL=1 installs a found
/// update without asking.
QString releasesApi()
{
    const auto test = qEnvironmentVariable("MIST1X_UPDATE_API");
    return test.startsWith(u"http") ? test : RELEASES_API;
}

bool testInstall()
{
    return qEnvironmentVariable("MIST1X_UPDATE_TEST_INSTALL") == u"1";
}
bool isNewer(const QString &online, const QString &current)
{
    semver::version a;
    semver::version b;
    if (!a.from_string_noexcept(online.toStdString()) ||
        !b.from_string_noexcept(current.toStdString()))
    {
        qCWarning(chatterinoUpdate)
            << "Unable to compare versions" << online << current;
        return false;
    }
    return a > b;
}

/// The SHA-256 of @a fileName in a SHA256SUMS.txt ("<hash>  <name>" lines).
QString expectedHash(const QByteArray &sums, const QString &fileName)
{
    for (const auto &line : QString::fromUtf8(sums).split(u'\n'))
    {
        const auto parts = line.simplified().split(u' ');
        auto name = parts.size() >= 2 ? parts.last() : QString();
        name.remove(u'*');
        if (parts.size() >= 2 && name == fileName &&
            parts.first().size() == 64)
        {
            return parts.first().toLower();
        }
    }
    return {};
}

NetworkRequest githubRequest(const QString &url)
{
    return NetworkRequest(url)
        .header("Accept", "application/vnd.github+json")
        .header("User-Agent", "ChatterinoNoctis/" MIST1X_VERSION)
        .followRedirects(true);
}

}  // namespace

namespace chatterino {

Updates::Updates(const Modes &modes_, const Paths &paths_, Settings &settings)
    : paths(paths_)
    , modes(modes_)
    , currentVersion_(QStringLiteral(MIST1X_VERSION))
    , updateGuideLink_(RELEASES_PAGE)
{
    qCDebug(chatterinoUpdate) << "init UpdateManager";
    this->nextCheck_.setSingleShot(true);
    QObject::connect(&this->nextCheck_, &QTimer::timeout, &this->nextCheck_, [this] {
        this->checkForUpdates();
    });

    // "beta" also takes test versions (pre-releases) into account
    settings.betaUpdates.connect(
        [this] {
            this->checkForUpdates();
        },
        this->managedConnections, false);
}

bool Updates::isDowngradeOf(const QString &online, const QString &current)
{
    semver::version onlineVersion;
    if (!onlineVersion.from_string_noexcept(online.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse online version"
                                    << online << "into a proper semver string";
        return false;
    }

    semver::version currentVersion;
    if (!currentVersion.from_string_noexcept(current.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse current version"
                                    << current << "into a proper semver string";
        return false;
    }

    return onlineVersion < currentVersion;
}

void Updates::deleteOldFiles()
{
    std::ignore = QtConcurrent::run([dir{this->paths.miscDirectory}] {
        for (const auto *name : {"Update.exe", "update.zip"})
        {
            auto path = combinePath(dir, name);
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
    });
}

const QString &Updates::getCurrentVersion() const
{
    return this->currentVersion_;
}

const QString &Updates::getOnlineVersion() const
{
    return this->onlineVersion_;
}

const QString &Updates::getReleaseNotes() const
{
    return this->releaseNotes_;
}

const QString &Updates::getReleasePage() const
{
    return this->releasePage_;
}

bool Updates::isInstalledCopy()
{
#ifdef Q_OS_WIN
    // the setup leaves its uninstaller next to the program
    return QFile::exists(
        QDir(QCoreApplication::applicationDirPath()).filePath("unins000.exe"));
#else
    return false;
#endif
}

void Updates::startPeriodicChecks()
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getArgs().isolationReport ||
        ((app->getArgs().uiSnapshot || app->getArgs().dontSaveSettings) &&
         releasesApi() == RELEASES_API))
    {
        return;  // offline stands, previews and one-off runs
    }
    if (this->periodicStarted_)
        return;
    this->periodicStarted_ = true;
    if (!this->checkInFlight_ && !this->nextCheck_.isActive())
        this->scheduleCheck_(updatecheck::firstDelay + std::chrono::milliseconds(QRandomGenerator::global()->bounded(3000)));
}

void Updates::updateFailed_(Status status, const QString &reason)
{
    qCWarning(chatterinoUpdate) << "Update failed:" << reason;
    this->setStatus_(status);
    const auto page =
        this->releasePage_.isEmpty() ? RELEASES_PAGE : this->releasePage_;
    postToThread([reason, page] {
        auto *box = new QMessageBox(
            QMessageBox::Warning, uiText("Chatterino Noctis update"),
            uiText("The update could not be installed:") + "\n" + reason);
        box->setAttribute(Qt::WA_DeleteOnClose);
        auto *open = box->addButton(uiText("Open the download page"),
                                    QMessageBox::AcceptRole);
        box->addButton(QMessageBox::Close);
        QObject::connect(open, &QPushButton::clicked, [page] {
            QDesktopServices::openUrl(QUrl(page));
        });
        box->show();
        box->raise();
    });
}

void Updates::installUpdates()
{
    if (this->confirmedPrerelease_ && !getSettings()->betaUpdates)
    {
        this->checkForUpdates();
        return;
    }
    if (this->status_ != UpdateAvailable && this->status_ != DownloadFailed &&
        this->status_ != WriteFileFailed && this->status_ != RunUpdaterFailed)
    {
        return;
    }

    // A portable copy updates itself by unpacking the new archive over it;
    // the page has it.
    if (!Updates::isInstalledCopy() || this->updateExe_.isEmpty() ||
        this->updateSums_.isEmpty())
    {
        QDesktopServices::openUrl(QUrl(
            this->releasePage_.isEmpty() ? RELEASES_PAGE : this->releasePage_));
        return;
    }

    this->setStatus_(Downloading);
    const auto setupName = QUrl(this->updateExe_).fileName();

    // First the checksums, then the setup: only a file whose SHA-256 matches
    // the one published with the release is started.
    githubRequest(this->updateSums_)
        .timeout(60000)
        .onError([this](const NetworkResult &result) {
            this->updateFailed_(DownloadFailed, result.formatError());
        })
        .onSuccess([this, setupName](const NetworkResult &sums) {
            const auto expected = expectedHash(sums.getData(), setupName);
            if (expected.isEmpty())
            {
                this->updateFailed_(DownloadFailed,
                                    uiText("The release has no checksum for "
                                           "the setup."));
                return;
            }
            githubRequest(this->updateExe_)
                .timeout(15 * 60 * 1000)
                .onError([this](const NetworkResult &result) {
                    this->updateFailed_(DownloadFailed, result.formatError());
                })
                .onSuccess([this, expected](const NetworkResult &setup) {
                    const auto &data = setup.getData();
                    const auto got =
                        QCryptographicHash::hash(data,
                                                 QCryptographicHash::Sha256)
                            .toHex();
                    if (QString::fromLatin1(got) != expected)
                    {
                        this->updateFailed_(
                            DownloadFailed,
                            uiText("The downloaded file is damaged (its "
                                   "checksum does not match)."));
                        return;
                    }

                    const auto filePath =
                        combinePath(this->paths.miscDirectory, "Update.exe");
                    QFile file(filePath);
                    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly) ||
                        file.write(data) != data.size())
                    {
                        this->updateFailed_(
                            WriteFileFailed,
                            uiText("The file could not be saved. An antivirus "
                                   "may be blocking it."));
                        return;
                    }
                    file.close();

                    // Everything saved now: the setup may close this copy
                    // before it is done saving on exit.
                    if (auto *app = dynamic_cast<Application *>(tryGetApp());
                        app != nullptr && app->getWindows() != nullptr)
                    {
                        app->getWindows()->save();
                    }
                    getSettings()->requestSave();

                    // The setup shows only its progress, closes this copy if
                    // it is still running, keeps the settings and starts the
                    // new version (AUTOUPDATE, see the release installer).
                    const bool started = QProcess::startDetached(
                        filePath, {"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                   "/CLOSEAPPLICATIONS", "/AUTOUPDATE=1"});
                    if (!started)
                    {
                        this->updateFailed_(
                            RunUpdaterFailed,
                            uiText("The setup could not be started. An "
                                   "antivirus may be blocking it."));
                        return;
                    }
                    QApplication::exit(0);
                })
                .execute();
        })
        .execute();
}

void Updates::checkForUpdates()
{
    using namespace std::chrono;
    if (isAppAboutToQuit() || this->checkInFlight_)
        return;
    if (this->status_ == Downloading)
    {
        this->scheduleCheck_(minutes(1));
        return;
    }

    if (!this->rateBlockedUntil_.hasExpired())
    {
        this->scheduleCheck_(milliseconds(this->rateBlockedUntil_.remainingTime()) + seconds(1));
        return;
    }
    if (this->lastRequest_.isValid() && this->lastRequest_.elapsed() < duration_cast<milliseconds>(updatecheck::requestSpacing).count())
    {
        this->scheduleCheck_(duration_cast<milliseconds>(updatecheck::requestSpacing) - milliseconds(this->lastRequest_.elapsed()));
        return;
    }

    this->nextCheck_.stop();
    this->checkInFlight_ = true;
    this->lastRequest_.start();
    this->currentCheckWithTests_ = getSettings()->betaUpdates;
    if (!this->currentCheckWithTests_ && this->confirmedPrerelease_)
    {
        this->confirmedRelease_ = false;
        this->onlineVersion_.clear();
        this->updateExe_.clear();
        this->updatePortable_.clear();
        this->updateSums_.clear();
    }
    if (!this->confirmedRelease_ || !isNewer(this->onlineVersion_, this->currentVersion_))
        this->setStatus_(Searching);
    this->requestRelease_(this->currentCheckWithTests_, false);
}

void Updates::scheduleCheck_(std::chrono::milliseconds delay)
{
    if (!isAppAboutToQuit())
        this->nextCheck_.start(std::max(delay, std::chrono::milliseconds(100)));
}

void Updates::checkFinished_(bool success, bool rateLimited)
{
    using namespace std::chrono;
    this->checkInFlight_ = false;
    if (success)
        this->consecutiveFailures_ = 0;
    else
        this->consecutiveFailures_ = std::min(this->consecutiveFailures_ + 1, 4U);

    if (rateLimited)
        this->rateBlockedUntil_ = QDeadlineTimer(updatecheck::rateLimitDelay);

    if (!success && this->status_ != Downloading &&
        (!this->confirmedRelease_ || !isNewer(this->onlineVersion_, this->currentVersion_)))
        this->setStatus_(SearchFailed);

    if (this->currentCheckWithTests_ != getSettings()->betaUpdates && !rateLimited)
    {
        this->scheduleCheck_(milliseconds(100));
        return;
    }
    const auto delay = success ? duration_cast<milliseconds>(updatecheck::regularDelay)
                               : updatecheck::retryDelay(this->consecutiveFailures_, rateLimited);
    // Avoid synchronizing every installation on the same shared VPN/IP.
    this->scheduleCheck_(delay + milliseconds(QRandomGenerator::global()->bounded(30000)));
}

void Updates::requestRelease_(bool withTests, bool latestFallback)
{
    auto onSuccess = [this, withTests, latestFallback](const NetworkResult &result) {
        if (this->status_ == Downloading || withTests != getSettings()->betaUpdates)
        {
            this->checkFinished_(true);
            return;
        }
        if (result.status() == 304)
        {
            // 304 is meaningful only if we have validated metadata for this
            // exact release channel; never treat a missing body as no update.
            const bool valid = this->confirmedRelease_ &&
                this->confirmedWithTests_ == withTests && !this->listEtag_.isEmpty();
            if (valid)
                this->setStatus_(isNewer(this->onlineVersion_, this->currentVersion_)
                                    ? UpdateAvailable : NoUpdateAvailable);
            this->checkFinished_(valid);
            return;
        }
        const auto value = result.parseJsonValue();
        const auto selected = updatecheck::bestRelease(value, withTests);
        if (!selected)
        {
            if (!withTests && !latestFallback && value.isArray())
            {
                this->listEtag_.clear();
                this->requestRelease_(withTests, true);
                return;
            }
            qCDebug(chatterinoUpdate) << "No eligible release in response";
            this->checkFinished_(false);
            return;
        }
        const auto &release = selected->metadata;
        const auto &version = selected->version;

        const auto setupName =
            u"ChatterinoNoctis-Setup-" % version % u".exe";
        const auto portableName =
            u"ChatterinoNoctis-" % version % u"-portable.zip";
        const auto legacySetupName =
            u"ChatterinoByMist1X-Setup-" % version % u".exe";
        const auto legacyPortableName =
            u"ChatterinoByMist1X-" % version % u"-portable.zip";
        QString setup;
        QString portable;
        QString sums;
        for (const auto &value : release["assets"].toArray())
        {
            const auto asset = value.toObject();
            const auto name = asset["name"].toString();
            const auto url = asset["browser_download_url"].toString();
            if (name == setupName)
            {
                setup = url;
            }
            else if (name == portableName)
            {
                portable = url;
            }
            else if (name == legacySetupName && setup.isEmpty())
            {
                setup = url;
            }
            else if (name == legacyPortableName && portable.isEmpty())
            {
                portable = url;
            }
            else if (name == u"SHA256SUMS.txt")
            {
                sums = url;
            }
        }

        const bool changed = this->onlineVersion_ != version || this->updateExe_ != setup ||
            this->updatePortable_ != portable || this->updateSums_ != sums ||
            this->releaseNotes_ != release["body"].toString().trimmed() ||
            this->releasePage_ != release["html_url"].toString();
        this->onlineVersion_ = version;
        this->updateExe_ = setup;
        this->updatePortable_ = portable;
        this->updateSums_ = sums;
        this->releaseNotes_ = release["body"].toString().trimmed();
        this->releasePage_ = release["html_url"].toString();
        this->isDowngrade_ = false;
        this->confirmedRelease_ = true;
        this->confirmedPrerelease_ = selected->prerelease;
        this->confirmedWithTests_ = withTests;
        this->listEtag_ = latestFallback ? QByteArray{} : result.etag();

        qCDebug(chatterinoUpdate)
            << "Newest release" << version << "current" << this->currentVersion_;
        const bool newer = isNewer(version, this->currentVersion_);
        this->setStatus_(newer ? UpdateAvailable : NoUpdateAvailable, changed);
        this->checkFinished_(true);
        if (newer && testInstall())
        {
            this->installUpdates();
        }
    };

    auto request = githubRequest(latestFallback ? QString(releasesApi() % u"/latest")
                                                : QString(releasesApi() % u"?per_page=30"));
    if (!latestFallback && this->confirmedRelease_ && this->confirmedWithTests_ == withTests && !this->listEtag_.isEmpty())
        request = std::move(request).header("If-None-Match", this->listEtag_);
    std::move(request)
        .caller(&this->nextCheck_)
        .timeout(30000)
        .onSuccess(onSuccess)
        .onError([this](const NetworkResult &result) {
            qCDebug(chatterinoUpdate)
                << "Update check failed:" << result.formatError();
            this->checkFinished_(false, result.status() == 403 || result.status() == 429);
        })
        .execute();
}

Updates::Status Updates::getStatus() const
{
    return this->status_;
}

QString Updates::portableUpdaterPath(const Paths &paths)
{
    return combinePath(paths.rootAppDataDirectory,
                       "updater.1/ChatterinoUpdater.exe");
}

bool Updates::shouldShowUpdateButton() const
{
    switch (this->getStatus())
    {
        // not on a failed check: no internet is not worth a red button
        case UpdateAvailable:
        case Downloading:
        case DownloadFailed:
        case WriteFileFailed:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isError() const
{
    switch (this->getStatus())
    {
        case SearchFailed:
        case DownloadFailed:
        case WriteFileFailed:
        case MissingPortableUpdater:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isDowngrade() const
{
    return this->isDowngrade_;
}

QString Updates::buildUpdateAvailableText() const
{
    return uiText("Version %1 is available (you have %2).")
        .arg(this->getOnlineVersion(), this->getCurrentVersion());
}

void Updates::setStatus_(Status status, bool forceNotify)
{
    if (this->status_ != status || forceNotify)
    {
        this->status_ = status;
        postToThread([this, status] {
            this->statusUpdated.invoke(status);
        });
    }
}

}  // namespace chatterino
