// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/channelpoints/ChannelPointsChartStore.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace {

using namespace chatterino;
using namespace literals;

const auto CHART_FILE_NAME = u"channel-points-chart.json"_s;
const auto ACCOUNT_DATA_KEY = u"accountSamples"_s;
const auto UNKNOWN_DATA_KEY = u"unattributedSamples"_s;

constexpr qint64 SAME_BALANCE_INTERVAL_MS = 15 * 60 * 1000;
constexpr qint64 MAX_OBSERVATION_GAP_MS = 30 * 60 * 1000;

struct Observation {
    QString accountId;
    QDateTime time;
};

struct ChartDataCache {
    QMutex mutex;
    QJsonObject data;
    QString path;
    QHash<QString, Observation> observations;
    bool loaded = false;
};

ChartDataCache &chartCache()
{
    static ChartDataCache cache;
    return cache;
}

QString chartFilePath()
{
    return combinePath(getApp()->getPaths().miscDirectory, CHART_FILE_NAME);
}

bool ensureChartDataLoaded(ChartDataCache &cache)
{
    const auto path = chartFilePath();
    if (cache.loaded && cache.path == path)
    {
        return true;
    }

    cache.loaded = false;
    cache.path = path;
    cache.data = {};
    cache.observations.clear();
    QFile file(path);
    if (!file.exists())
    {
        cache.loaded = true;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject())
    {
        cache.data = document.object();
        cache.loaded = true;
        return true;
    }
    // Do not overwrite an existing unreadable/corrupt history with a new
    // empty store. Keeping the file makes recovery possible.
    return false;
}

QJsonArray storedSamples(const QJsonObject &data, const QString &channelId,
                         const QString &accountId)
{
    if (accountId.isEmpty())
    {
        return data.value(UNKNOWN_DATA_KEY).toObject().value(channelId).toArray();
    }
    return data.value(ACCOUNT_DATA_KEY).toObject().value(accountId).toObject()
        .value(channelId).toArray();
}

void setStoredSamples(QJsonObject &data, const QString &channelId,
                      const QString &accountId, const QJsonArray &samples)
{
    if (accountId.isEmpty())
    {
        auto unknown = data.value(UNKNOWN_DATA_KEY).toObject();
        unknown.insert(channelId, samples);
        data.insert(UNKNOWN_DATA_KEY, unknown);
        return;
    }
    auto accounts = data.value(ACCOUNT_DATA_KEY).toObject();
    auto channels = accounts.value(accountId).toObject();
    channels.insert(channelId, samples);
    accounts.insert(accountId, channels);
    data.insert(ACCOUNT_DATA_KEY, accounts);
}

QVector<ChannelPointsChartSample> parseSamples(const QJsonArray &samples)
{
    QVector<ChannelPointsChartSample> result;
    result.reserve(samples.size());

    for (const auto &value : samples)
    {
        if (!value.isObject())
        {
            continue;
        }

        const auto object = value.toObject();
        const auto time = QDateTime::fromString(object.value("t"_L1).toString(),
                                                Qt::ISODateWithMs);
        if (!time.isValid())
        {
            continue;
        }

        const auto balance = object.value("balance"_L1).toInteger(-1);
        if (balance < 0)
        {
            continue;
        }

        result.append({time.toUTC(), balance,
                       object.value("startsSegment"_L1).toBool()});
    }

    std::stable_sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                  return left.time < right.time;
              });

    return result;
}

}  // namespace

namespace chatterino {

bool ChannelPointsChartStore::recordSample(const QString &channelId,
                                           const QString &accountId,
                                           qint64 balance,
                                           QDateTime observedAt)
{
    if (channelId.isEmpty() || balance < 0 || !observedAt.isValid())
    {
        return false;
    }

    auto &cache = chartCache();
    QMutexLocker locker(&cache.mutex);

    if (!ensureChartDataLoaded(cache))
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Existing chart history could not be read; preserving it";
        return false;
    }

    observedAt = observedAt.toUTC();
    const auto previousObservation = cache.observations.value(channelId);
    const auto elapsed = previousObservation.time.msecsTo(observedAt);
    const bool startsSegment = !previousObservation.time.isValid() ||
                               previousObservation.accountId != accountId ||
                               elapsed < 0 || elapsed > MAX_OBSERVATION_GAP_MS;

    auto samples = storedSamples(cache.data, channelId, accountId);
    if (!startsSegment && !samples.isEmpty())
    {
        const auto last = samples.last().toObject();
        const auto lastTime = QDateTime::fromString(last.value("t"_L1).toString(),
                                                   Qt::ISODateWithMs);
        if (lastTime.isValid() && lastTime <= observedAt &&
            lastTime.msecsTo(observedAt) < SAME_BALANCE_INTERVAL_MS &&
            last.value("balance"_L1).toInteger(-1) == balance)
        {
            cache.observations.insert(channelId, {accountId, observedAt});
            return false;
        }
    }

    samples.append(QJsonObject{
        {"t"_L1, observedAt.toString(Qt::ISODateWithMs)},
        {"balance"_L1, balance},
        {"startsSegment"_L1, startsSegment || samples.isEmpty()},
    });
    // Keep legacy rows untouched; even a historic zero may have been real.
    // Commit before replacing the cache, so a disk error cannot make the
    // deduplication logic believe an unsaved sample was persisted.
    auto nextData = cache.data;
    setStoredSamples(nextData, channelId, accountId, samples);

    QSaveFile file(chartFilePath());
    if (!file.open(QIODevice::WriteOnly))
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Failed to open channel points chart file for writing:"
            << file.errorString();
        return false;
    }

    const auto data = QJsonDocument(nextData).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit())
    {
        qCWarning(chatterinoTwitch)
            << "[Points] Failed to save channel points chart file:"
            << file.errorString();
        return false;
    }
    cache.data = std::move(nextData);
    cache.observations.insert(channelId, {accountId, observedAt});
    return true;
}

void ChannelPointsChartStore::markGap(const QString &channelId)
{
    if (channelId.isEmpty())
    {
        return;
    }
    auto &cache = chartCache();
    QMutexLocker locker(&cache.mutex);
    ensureChartDataLoaded(cache);
    cache.observations.remove(channelId);
}

QVector<ChannelPointsChartSample> ChannelPointsChartStore::samplesForChannel(
    const QString &channelId)
{
    const auto auth = MoltorinoAuth::resolveCurrentUserToken();
    return samplesForAccount(channelId, auth.userId);
}

QVector<ChannelPointsChartSample> ChannelPointsChartStore::samplesForAccount(
    const QString &channelId, const QString &accountId)
{
    if (channelId.isEmpty() || accountId.isEmpty())
    {
        return {};
    }

    auto &cache = chartCache();
    QMutexLocker locker(&cache.mutex);

    if (!ensureChartDataLoaded(cache))
    {
        return {};
    }
    return parseSamples(storedSamples(cache.data, channelId, accountId));
}

QVector<ChannelPointsChartSample> ChannelPointsChartStore::legacySamplesForChannel(
    const QString &channelId)
{
    if (channelId.isEmpty())
    {
        return {};
    }
    auto &cache = chartCache();
    QMutexLocker locker(&cache.mutex);
    if (!ensureChartDataLoaded(cache))
    {
        return {};
    }
    auto history = parseSamples(cache.data.value(channelId).toArray());
    const auto unattributed = parseSamples(storedSamples(cache.data, channelId, {}));
    history += unattributed;
    std::stable_sort(history.begin(), history.end(), [](const auto &left, const auto &right) {
        return left.time < right.time;
    });
    return history;
}

}  // namespace chatterino
