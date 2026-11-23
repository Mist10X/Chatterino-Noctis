// SPDX-License-Identifier: MIT
#pragma once

#include "Application.hpp"
#include "controllers/channelpoints/ChannelPointsChartStore.hpp"
#include "singletons/Paths.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

namespace chatterino::preview {

// Called only by the reward test runner after creating its disposable
// mock::BaseApplication profile, before opening any channel chart.
template <typename Check>
void checkChannelPointsStore(Check &&check)
{
    const auto path = QDir(getApp()->getPaths().miscDirectory)
                          .filePath("channel-points-chart.json");
    const auto channel = QStringLiteral("901000001");
    const auto first = QStringLiteral("901000002");
    const auto second = QStringLiteral("901000003");
    const auto time = QDateTime::fromString("2026-11-01T12:00:00.000Z",
                                            Qt::ISODateWithMs);
    const auto row = [](const QDateTime &date, QJsonValue balance) {
        return QJsonObject{{"t", date.toString(Qt::ISODateWithMs)},
                           {"balance", balance}};
    };
    const QJsonArray legacyRows{row(time, 100), row(time.addSecs(60), 0),
                                row(time.addSecs(120), -1),
                                row(time.addSecs(180), 1.5),
                                row(time.addSecs(240), "0"),
                                QJsonObject{{"t", time.toString(Qt::ISODateWithMs)}}};

    // Refuse to overwrite any existing path, including another fixture.
    if (QFile::exists(path))
    {
        check(false, QStringLiteral("chart fixture owns a new isolated file"));
        return;
    }
    QSaveFile file(path);
    const auto json = QJsonDocument(QJsonObject{{channel, legacyRows}}).toJson();
    const bool seeded = file.open(QIODevice::WriteOnly) &&
                        file.write(json) == json.size() && file.commit();
    check(seeded, QStringLiteral("chart fixture writes old channel-only schema"));
    if (!seeded)
    {
        return;
    }

    using Store = ChannelPointsChartStore;
    const auto legacy = Store::legacySamplesForChannel(channel);
    check(legacy.size() == 2 && legacy[0].balance == 100 &&
              legacy[1].balance == 0,
          QStringLiteral("chart legacy preserves real zero but rejects invalid balances"));
    check(Store::samplesForAccount(channel, first).isEmpty(),
          QStringLiteral("legacy rows never become an identified account's balance"));

    const auto t0 = time.addSecs(600);
    check(Store::recordSample(channel, first, 150, t0),
          QStringLiteral("first scoped balance recorded"));
    check(!Store::recordSample(channel, first, 150, t0.addSecs(60)),
          QStringLiteral("unchanged balance is deduplicated within 15 minutes"));
    check(Store::recordSample(channel, first, 150, t0.addSecs(900)),
          QStringLiteral("unchanged confirmed balance makes a periodic observation"));
    check(Store::recordSample(channel, first, 0, t0.addSecs(901)),
          QStringLiteral("true zero balance remains a normal scoped observation"));
    check(Store::recordSample(channel, second, 50, t0.addSecs(902)),
          QStringLiteral("another account records its own balance"));
    check(Store::recordSample(channel, first, 0, t0.addSecs(903)),
          QStringLiteral("returning to an account records even an unchanged balance"));

    Store::markGap(channel);
    check(Store::recordSample(channel, first, 0, t0.addSecs(904)),
          QStringLiteral("an unavailable interval starts another observation segment"));
    check(Store::recordSample(channel, first, 20, t0.addSecs(904 + 31 * 60)),
          QStringLiteral("long unobserved interval is recorded without interpolation"));
    check(!Store::recordSample(channel, first, -1, t0.addSecs(4000)),
          QStringLiteral("unknown balance is never persisted as zero"));

    const auto a = Store::samplesForAccount(channel, first);
    const auto b = Store::samplesForAccount(channel, second);
    check(a.size() == 6 && b.size() == 1 && a[2].balance == 0 &&
              a[5].balance == 20 && b[0].balance == 50,
          QStringLiteral("account histories remain independent and keep true zero"));
    check(a.size() == 6 && a[0].startsSegment && !a[1].startsSegment &&
              !a[2].startsSegment && a[3].startsSegment &&
              a[4].startsSegment && a[5].startsSegment,
          QStringLiteral("gap flags separate restart, switch, failure and idle intervals"));

    check(Store::recordSample(channel, {}, 400, t0.addSecs(4100)),
          QStringLiteral("unidentified token balance stored separately"));
    const auto combinedLegacy = Store::legacySamplesForChannel(channel);
    check(combinedLegacy.size() == 3 && combinedLegacy.last().balance == 400 &&
              combinedLegacy.last().startsSegment &&
              Store::samplesForAccount(channel, first).size() == 6 &&
              Store::samplesForAccount(channel, {}).isEmpty(),
          QStringLiteral("unknown token never leaks into a named account history"));

    QFile saved(path);
    const auto stored = saved.open(QIODevice::ReadOnly)
                            ? QJsonDocument::fromJson(saved.readAll()).object()
                            : QJsonObject{};
    check(stored.value(channel).toArray() == legacyRows,
          QStringLiteral("all historical rows are preserved exactly in the file"));
    check(stored.value("accountSamples").toObject().value(first).toObject()
                  .value(channel).toArray().size() == 6 &&
              stored.value("accountSamples").toObject().value(second).toObject()
                  .value(channel).toArray().size() == 1,
          QStringLiteral("on-disk schema separates authenticated identities"));
}

}  // namespace chatterino::preview
