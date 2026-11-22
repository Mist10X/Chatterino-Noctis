// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/InstallCounter.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace chatterino {

namespace {

const QString COUNTER =
    QStringLiteral("https://abacus.jasoncameron.dev/hit/chatterino-by-mist1x/"
                   "installs");

QString countedMarker()
{
    return QDir(getApp()->getPaths().miscDirectory).filePath("install-counted");
}

}  // namespace

void countInstallOnce()
{
    auto *app = tryGetApp();
    if (app == nullptr || app->getArgs().dontSaveSettings ||
        app->getArgs().uiSnapshot || app->getArgs().isolationReport ||
        qEnvironmentVariableIsSet("MIST1X_NO_INSTALL_COUNT") ||
        !getSettings()->countInstallation ||
        QFileInfo::exists(countedMarker()))
    {
        return;
    }
    NetworkRequest(COUNTER)
        .timeout(20000)
        .onSuccess([](const NetworkResult &) {
            QFile marker(countedMarker());
            if (marker.open(QIODevice::WriteOnly))
            {
                marker.write("1");
            }
        })
        .execute();
}

}  // namespace chatterino
