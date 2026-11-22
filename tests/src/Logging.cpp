// SPDX-License-Identifier: MIT
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "singletons/Logging.hpp"
#include "singletons/helper/LoggingChannel.hpp"
#include "Test.hpp"

#include <QFile>
#include <QTemporaryDir>

namespace chatterino {

TEST(Logging, ServerDateAndStreamFollowLogDirectory)
{
    mock::BaseApplication app;
    QTemporaryDir oldDir;
    QTemporaryDir newDir;
    ASSERT_TRUE(oldDir.isValid() && newDir.isValid());
    app.settings.enableLogging = true;
    app.settings.onlyLogListedChannels = false;
    app.settings.tryUseTwitchTimestamps = true;
    app.settings.separatelyStoreStreamLogs = true;
    app.settings.logPath = oldDir.path();
    Logging logging(app.settings);
    const auto send = [&](const QString &text) {
        auto message = std::make_shared<Message>();
        message->messageText = text;
        message->serverReceivedTime = QDateTime(QDate(2024, 12, 31), QTime(23, 59, 59));
        logging.addMessage("fixture", message, "twitch", "123");
    };
    send("before-move");
    app.settings.logPath = newDir.path();
    send("after-move");
    logging.closeChannel("fixture", "twitch");

    const auto contents = [](const QTemporaryDir &dir, const QString &suffix) {
        QFile file(dir.filePath("Twitch/Channels/fixture/fixture-" + suffix + ".log"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    for (const auto &suffix : {QStringLiteral("2024-12-31"), QStringLiteral("123")})
    {
        EXPECT_TRUE(contents(oldDir, suffix).contains("before-move"));
        EXPECT_FALSE(contents(oldDir, suffix).contains("after-move"));
        EXPECT_TRUE(contents(newDir, suffix).contains("after-move"));
    }
}

}  // namespace chatterino
