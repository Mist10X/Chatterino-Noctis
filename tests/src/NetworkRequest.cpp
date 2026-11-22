// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/network/NetworkRequest.hpp"

#include "common/network/NetworkManager.hpp"
#include "common/network/NetworkResult.hpp"
#include "NetworkHelpers.hpp"
#include "Test.hpp"
#include "util/QMagicEnum.hpp"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QSemaphore>

#include <array>
#include <chrono>
#include <thread>

using namespace chatterino;

namespace {

QString getStatusURL(int code)
{
    return QString("%1/status/%2").arg(HTTPBIN_BASE_URL).arg(code);
}

QString getDelayURL(int delay)
{
    return QString("%1/delay/%2").arg(HTTPBIN_BASE_URL).arg(delay);
}

QString getHttpbinUrl(QStringView path)
{
    return QString("%1/%2").arg(HTTPBIN_BASE_URL, path);
}

QHttpMultiPart *makeMultipart()
{
    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QVariant("form-data; name=\"text\""));
    part.setBody("my text");

    multipart->append(part);
    return multipart;
}

}  // namespace

TEST(NetworkRequest, Success)
{
    const std::vector<int> codes{200, 201, 202, 203, 204, 205, 206};

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    for (const auto code : codes)
    {
        auto url = getStatusURL(code);
        RequestWaiter waiter;

        NetworkRequest(url)
            .onSuccess([code, &waiter, url](const NetworkResult &result) {
                EXPECT_EQ(result.status(), code);
                waiter.requestDone();
            })
            .onError([&](const NetworkResult & /*result*/) {
                // The codes should *not* throw an error
                EXPECT_TRUE(false);
                waiter.requestDone();
            })
            .execute();

        waiter.waitForRequest();
    }

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());
}

TEST(NetworkRequest, FinallyCallbackOnSuccess)
{
    const std::vector<int> codes{200, 201, 202, 203, 204, 205, 206};

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    for (const auto code : codes)
    {
        auto url = getStatusURL(code);
        RequestWaiter waiter;

        bool finallyCalled = false;

        NetworkRequest(url)
            .finally([&waiter, &finallyCalled] {
                finallyCalled = true;
                waiter.requestDone();
            })
            .execute();

        waiter.waitForRequest();

        EXPECT_TRUE(finallyCalled);
    }
}

TEST(NetworkRequest, Error)
{
    const std::vector<int> codes{
        400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410,
        411, 412, 413, 414, 418, 500, 501, 502, 503, 504,
    };

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    for (const auto code : codes)
    {
        auto url = getStatusURL(code);
        RequestWaiter waiter;

        NetworkRequest(url)
            .onSuccess([&waiter, url](const NetworkResult & /*result*/) {
                // The codes should throw an error
                EXPECT_TRUE(false);
                waiter.requestDone();
            })
            .onError([code, &waiter, url](const NetworkResult &result) {
                EXPECT_EQ(result.status(), code);

                waiter.requestDone();
            })
            .execute();

        waiter.waitForRequest();
    }

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());
}

TEST(NetworkRequest, FinallyCallbackOnError)
{
    const std::vector<int> codes{
        400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 410,
        411, 412, 413, 414, 418, 500, 501, 502, 503, 504,
    };

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    for (const auto code : codes)
    {
        auto url = getStatusURL(code);
        RequestWaiter waiter;

        bool finallyCalled = false;

        NetworkRequest(url)
            .finally([&waiter, &finallyCalled] {
                finallyCalled = true;
                waiter.requestDone();
            })
            .execute();

        waiter.waitForRequest();

        EXPECT_TRUE(finallyCalled);
    }
}

TEST(NetworkRequest, TimeoutTimingOut)
{
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    auto url = getDelayURL(5);
    RequestWaiter waiter;

    NetworkRequest(url)
        .timeout(1000)
        .onSuccess([&waiter](const NetworkResult & /*result*/) {
            // The timeout should throw an error
            EXPECT_TRUE(false);
            waiter.requestDone();
        })
        .onError([&waiter, url](const NetworkResult &result) {
            qDebug() << QTime::currentTime().toString()
                     << "timeout request finish error";
            EXPECT_EQ(result.error(),
                      NetworkResult::NetworkError::TimeoutError);
            EXPECT_EQ(result.status(), std::nullopt);

            waiter.requestDone();
        })
        .execute();

    waiter.waitForRequest();

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());
}

TEST(NetworkRequest, TimeoutNotTimingOut)
{
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    auto url = getDelayURL(1);
    RequestWaiter waiter;

    NetworkRequest(url)
        .timeout(3000)
        .onSuccess([&waiter, url](const NetworkResult &result) {
            EXPECT_EQ(result.status(), 200);

            waiter.requestDone();
        })
        .onError([&waiter, url](const NetworkResult & /*result*/) {
            // The timeout should *not* throw an error
            EXPECT_TRUE(false);
            waiter.requestDone();
        })
        .execute();

    waiter.waitForRequest();

    EXPECT_TRUE(NetworkManager::workerThread->isRunning());
}

TEST(NetworkRequest, FinallyCallbackOnTimeout)
{
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    auto url = getDelayURL(5);

    RequestWaiter waiter;
    bool finallyCalled = false;
    bool onSuccessCalled = false;
    bool onErrorCalled = false;

    NetworkRequest(url)
        .timeout(1000)
        .onSuccess([&](const NetworkResult & /*result*/) {
            onSuccessCalled = true;
        })
        .onError([&](const NetworkResult &result) {
            onErrorCalled = true;
            EXPECT_EQ(result.error(),
                      NetworkResult::NetworkError::TimeoutError);
            EXPECT_EQ(result.status(), std::nullopt);
        })
        .finally([&] {
            finallyCalled = true;
            waiter.requestDone();
        })
        .execute();

    waiter.waitForRequest();

    EXPECT_TRUE(finallyCalled);
    EXPECT_TRUE(onErrorCalled);
    EXPECT_FALSE(onSuccessCalled);
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());
}

TEST(NetworkRequest, BatchedTimeouts)
{
    ASSERT_TRUE(NetworkManager::workerThread->isRunning());
    auto gate = std::make_shared<QSemaphore>();
    ASSERT_TRUE(QMetaObject::invokeMethod(NetworkManager::accessManager,
        [gate] { gate->acquire(); }, Qt::QueuedConnection));

    // Hold the worker queue until the short deadlines have expired. A local
    // data URL would succeed immediately if the budget started at dispatch
    // from the worker rather than when execute() queued each request.
    struct State {
        RequestWaiter waiter;
        unsigned successes = 0;
        unsigned timeouts = 0;
        unsigned completions = 0;
    };
    std::array<State, 10> states;
    for (size_t i = 0; i < states.size(); ++i)
    {
        auto &state = states[i];
        NetworkRequest(QUrl("data:text/plain,ready"))
            .timeout(i == states.size() - 1 ? 10000 : 50)
            .onSuccess([&state](const NetworkResult &result) {
                EXPECT_EQ(result.getData(), QByteArray("ready"));
                ++state.successes;
            })
            .onError([&state](const NetworkResult &result) {
                EXPECT_EQ(result.error(), QNetworkReply::TimeoutError);
                EXPECT_EQ(result.status(), std::nullopt);
                ++state.timeouts;
            })
            .finally([&state] {
                ++state.completions;
                state.waiter.requestDone();
            })
            .execute();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gate->release();
    for (size_t i = 0; i < states.size(); ++i)
    {
        auto &state = states[i];
        state.waiter.waitForRequest();
        const bool longDeadline = i == states.size() - 1;
        EXPECT_EQ(state.successes, longDeadline ? 1U : 0U);
        EXPECT_EQ(state.timeouts, longDeadline ? 0U : 1U);
        EXPECT_EQ(state.completions, 1U);
    }
}

TEST(NetworkRequest, HttpGetHeaders)
{
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    RequestWaiter waiter;
    NetworkRequest(getHttpbinUrl(u"headers"), NetworkRequestType::Get)
        .timeout(1000)
        .onSuccess([&](const NetworkResult &result) {
            // make sure we don't attempt to send a body
            auto body = QString::fromUtf8(result.getData());
            EXPECT_FALSE(body.contains("content-length", Qt::CaseInsensitive));
            waiter.requestDone();
        })
        .execute();

    waiter.waitForRequest();
}

TEST(NetworkRequest, HttpBody)
{
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    for (auto ty : std::array{
             NetworkRequestType::Post,
             NetworkRequestType::Put,
             NetworkRequestType::Patch,
         })
    {
        auto path = qmagicenum::enumNameString(ty).toLower();
        RequestWaiter waiter;
        bool success = false;
        NetworkRequest(getHttpbinUrl(path), ty)
            .timeout(1000)
            .payload("foobar")
            .header("Content-Type", "text/plain")
            .onSuccess([&](const NetworkResult &result) {
                EXPECT_EQ(result.getData(), "foobar") << path;
                success = true;
            })
            .finally([&] {
                waiter.requestDone();
            })
            .execute();

        waiter.waitForRequest();
        ASSERT_TRUE(success) << path;
    }
}

TEST(NetworkRequest, HttpBodyMultipart)
{
    EXPECT_TRUE(NetworkManager::workerThread->isRunning());

    for (auto ty : std::array{
             NetworkRequestType::Post,
             NetworkRequestType::Put,
             NetworkRequestType::Patch,
         })
    {
        auto path = qmagicenum::enumNameString(ty).toLower();
        RequestWaiter waiter;
        bool success = false;
        NetworkRequest(getHttpbinUrl(path), ty)
            .timeout(1000)
            .multiPart(makeMultipart())
            .onSuccess([&](const NetworkResult &result) {
                auto body = QString::fromUtf8(result.getData());
                EXPECT_TRUE(body.contains("Content-Disposition: form-data; "
                                          "name=\"text\"\r\n\r\nmy text",
                                          Qt::CaseInsensitive))
                    << "Path:" << path << "Body:" << body;
                success = true;
            })
            .finally([&] {
                waiter.requestDone();
            })
            .execute();

        waiter.waitForRequest();
        ASSERT_TRUE(success) << path;
    }
}
