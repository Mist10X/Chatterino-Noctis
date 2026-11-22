// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QDeadlineTimer>
#include <QByteArray>

#include <memory>
#include <vector>

namespace chatterino {

class Paths;
class Settings;
class Modes;

class Updates
{
    const Paths &paths;
    const Modes &modes;

public:
    Updates(const Modes &modes_, const Paths &paths_, Settings &settings);

    enum Status {
        None,
        Searching,
        UpdateAvailable,
        NoUpdateAvailable,
        SearchFailed,
        Downloading,
        DownloadFailed,
        WriteFileFailed,
        MissingPortableUpdater,
        RunUpdaterFailed,
    };

    static bool isDowngradeOf(const QString &online, const QString &current);

    void deleteOldFiles();

    /// Asks GitHub for the newest release of Chatterino Noctis.
    void checkForUpdates();
    /// Checks shortly after startup and periodically, with failure backoff.
    void startPeriodicChecks();
    /// Installed with the setup (not a portable copy): the update can be
    /// downloaded and installed from inside the program.
    static bool isInstalledCopy();
    const QString &getReleaseNotes() const;
    const QString &getReleasePage() const;
    const QString &getCurrentVersion() const;
    const QString &getOnlineVersion() const;
    void installUpdates();
    Status getStatus() const;

    static QString portableUpdaterPath(const Paths &paths);

    bool shouldShowUpdateButton() const;
    bool isError() const;
    bool isDowngrade() const;

    /// Generates the string that the update dialog will show.
    QString buildUpdateAvailableText() const;

    pajlada::Signals::Signal<Status> statusUpdated;

private:
    QString currentVersion_;
    QString onlineVersion_;
    Status status_ = None;
    bool isDowngrade_{};

    QString updateExe_;
    QString updatePortable_;
    QString updateGuideLink_;
    QString updateSums_;
    QString releaseNotes_;
    QString releasePage_;
    QTimer nextCheck_;
    QElapsedTimer lastRequest_;
    QDeadlineTimer rateBlockedUntil_;
    QByteArray listEtag_;
    bool periodicStarted_ = false;
    bool checkInFlight_ = false;
    bool currentCheckWithTests_ = false;
    bool confirmedRelease_ = false;
    bool confirmedPrerelease_ = false;
    bool confirmedWithTests_ = false;
    unsigned consecutiveFailures_ = 0;

    void checkFinished_(bool success, bool rateLimited = false);
    void requestRelease_(bool withTests, bool latestFallback);
    void scheduleCheck_(std::chrono::milliseconds delay);

    void updateFailed_(Status status, const QString &reason);

    void setStatus_(Status status, bool forceNotify = false);

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino
