// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Args.hpp"
#include "common/Modes.hpp"
#include "common/network/NetworkManager.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "Test.hpp"

#include <chatterino-test/TestConfig.hpp>
#include <QApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QLocale>
#include <QLoggingCategory>
#include <QNetworkProxyFactory>
#include <QtConcurrent>
#include <QTimer>

#include <chrono>
#include <thread>

using namespace chatterino;

#define SUPPORT_QT_NETWORK_TESTS

#ifndef CHATTERINO_TEST_USE_PUBLIC_HTTPBIN
namespace {
class LocalTestProxyFactory final : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        const auto host = query.peerHostName();
        if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
            QHostAddress(host).isLoopback())
        {
            return {QNetworkProxy(QNetworkProxy::NoProxy)};
        }
        // Keep incidental image, badge and metadata requests on this machine.
        // A local proxy address also avoids resolving the external hostname.
        return {QNetworkProxy(QNetworkProxy::HttpProxy,
                              QStringLiteral("127.0.0.1"), 1)};
    }
};
}  // namespace
#endif

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);

#ifdef SUPPORT_QT_NETWORK_TESTS
    // CMake gtest_discover_tests invokes this binary with --gtest_list_tests.
    // That must complete without waiting for the event loop; otherwise discovery
    // times out and the test target fails to link.
    if (GTEST_FLAG_GET(list_tests))
    {
        return RUN_ALL_TESTS();
    }

    QApplication app(argc, argv);
    // make sure to always debug-log
    QLoggingCategory::setFilterRules("chatterino.*=true");
    // make sure we use the same language when formatting
    QLocale::setDefault(QLocale("en_GB"));

    initResources();

#ifndef CHATTERINO_TEST_USE_PUBLIC_HTTPBIN
    QNetworkProxyFactory::setApplicationProxyFactory(new LocalTestProxyFactory);
#endif
    chatterino::NetworkManager::init();

    Args args;

    // Ensure settings are initialized before any tests are run
    QTemporaryDir settingsDir;
    settingsDir.setAutoRemove(false);  // we'll remove it manually
    chatterino::Settings settings(Modes(args), args, settingsDir.path());

    QTimer::singleShot(0, [&]() {
        auto res = RUN_ALL_TESTS();

        chatterino::NetworkManager::deinit();

        settingsDir.remove();

        // Pick up the last events from the eventloop
        // Using a loop to catch events queueing other events (e.g. deletions)
        for (size_t i = 0; i < 32; i++)
        {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        QApplication::exit(res);
    });

    return QApplication::exec();
#else
    return RUN_ALL_TESTS();
#endif
}
