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
    const auto otherChannel = QStringLiteral("901000004");
    const auto priorChannel = QStringLiteral("901000005");
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
    auto priorRow = row(time.addSecs(360), 77);
    priorRow.insert("startsSegment", true);
    const QJsonArray priorRows{priorRow};

    // Refuse to overwrite any existing path, including another fixture.
    if (QFile::exists(path))
    {
        check(false, QStringLiteral("chart fixture owns a new isolated file"));
        return;
    }
    QSaveFile file(path);
    // A previous-session identified sample deliberately has no observation
    // in this process. A restart must still create a history boundary.
    const auto json = QJsonDocument(QJsonObject{
        {channel, legacyRows},
        {"accountSamples", QJsonObject{
            {first, QJsonObject{{priorChannel, priorRows}}}}}}).toJson();
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
    check(Store::recordSample(priorChannel, first, 77, t0),
          QStringLiteral("first process observation creates a restart boundary"));
    const auto prior = Store::samplesForAccount(priorChannel, first);
    check(prior.size() == 2 && prior[0].startsSegment && prior[1].startsSegment,
          QStringLiteral("restart does not interpolate even with a recent unchanged stored balance"));
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
    check(!Store::recordSample(channel, first, 0, t0.addSecs(903)),
          QStringLiteral("short account switch does not defeat per-owner deduplication"));
    check(Store::recordSample(channel, first, 1, t0.addSecs(904)),
          QStringLiteral("changed balance after short account switch is recorded"));
    check(!Store::recordSample(channel, second, 50, t0.addSecs(905)),
          QStringLiteral("each account maintains its own unchanged-balance interval"));
    check(!Store::recordSample(channel, first, -1, t0.addSecs(905)) &&
              !Store::recordSample(channel, first, 10, QDateTime{}),
          QStringLiteral("negative and invalidly timed observations do not become samples"));
    check(Store::recordSample(channel, first, 2, t0.addSecs(906)),
          QStringLiteral("a single invalid observation does not interrupt a short interval"));

    // Deduplicated *confirmed* observations keep the series continuous, but
    // the passage of time and invalid or other-owner observations cannot.
    check(Store::recordSample(otherChannel, first, 50, t0) &&
              !Store::recordSample(otherChannel, first, 50, t0.addSecs(600)) &&
              Store::recordSample(otherChannel, first, 51, t0.addSecs(2300)),
          QStringLiteral("deduplication still advances the last confirmed observation"));
    const auto continuous = Store::samplesForAccount(otherChannel, first);
    check(continuous.size() == 2 && continuous[0].startsSegment &&
              !continuous[1].startsSegment,
          QStringLiteral("deduplicated confirmation safely spans a long stored-sample interval"));
    check(Store::recordSample(channel, second, 51, t0.addSecs(1800)) &&
              Store::recordSample(channel, second, 52, t0.addSecs(2700)),
          QStringLiteral("second account remains independently observed"));
    check(!Store::recordSample(channel, first, -1, t0.addSecs(2500)),
          QStringLiteral("invalid balance cannot advance the observation clock"));
    check(Store::recordSample(channel, first, 3, t0.addSecs(2707)),
          QStringLiteral("over 30 minutes without own valid observation creates a real gap"));

    Store::markGap(channel);
    check(Store::recordSample(channel, first, 3, t0.addSecs(2708)) &&
              Store::recordSample(channel, second, 52, t0.addSecs(2709)),
          QStringLiteral("explicit channel stop breaks continuity for all its observed owners"));
    check(!Store::recordSample(otherChannel, first, 51, t0.addSecs(2710)) &&
              Store::recordSample(otherChannel, first, 52, t0.addSecs(2711)),
          QStringLiteral("explicit channel stop does not disturb another channel"));
    const auto channelSeparated = Store::samplesForAccount(otherChannel, first);
    check(channelSeparated.size() == 3 && !channelSeparated.last().startsSegment,
          QStringLiteral("continuity is scoped to channel as well as account"));

    const auto a = Store::samplesForAccount(channel, first);
    const auto b = Store::samplesForAccount(channel, second);
    check(a.size() == 7 && b.size() == 4 && a[2].balance == 0 &&
              a[5].balance == 3 && b[0].balance == 50,
          QStringLiteral("account histories remain independent and keep true zero"));
    check(a.size() == 7 && a[0].startsSegment && !a[1].startsSegment &&
              !a[2].startsSegment && !a[3].startsSegment &&
              !a[4].startsSegment && a[5].startsSegment && a[6].startsSegment &&
              b.size() == 4 && b[0].startsSegment && !b[1].startsSegment &&
              !b[2].startsSegment && b[3].startsSegment,
          QStringLiteral("gap flags separate real idle periods and explicit stops, not ordinary account switches"));

    check(Store::recordSample(channel, {}, 400, t0.addSecs(2711)),
          QStringLiteral("unidentified token balance stored separately"));
    check(Store::recordSample(channel, first, 4, t0.addSecs(2712)) &&
              !Store::recordSample(channel, {}, 400, t0.addSecs(2713)),
          QStringLiteral("unattributed observations and identified accounts do not interrupt each other"));
    const auto combinedLegacy = Store::legacySamplesForChannel(channel);
    check(combinedLegacy.size() == 3 && combinedLegacy.last().balance == 400 &&
              combinedLegacy.last().startsSegment &&
              Store::samplesForAccount(channel, first).size() == 8 &&
              !Store::samplesForAccount(channel, first).last().startsSegment &&
              Store::samplesForAccount(channel, {}).isEmpty(),
          QStringLiteral("unknown token never leaks into a named account history"));

    QFile saved(path);
    const auto stored = saved.open(QIODevice::ReadOnly)
                            ? QJsonDocument::fromJson(saved.readAll()).object()
                            : QJsonObject{};
    check(stored.value(channel).toArray() == legacyRows,
          QStringLiteral("all historical rows are preserved exactly in the file"));
    check(stored.value("accountSamples").toObject().value(first).toObject()
                  .value(channel).toArray().size() == 8 &&
              stored.value("accountSamples").toObject().value(second).toObject()
                  .value(channel).toArray().size() == 4,
          QStringLiteral("on-disk schema separates authenticated identities"));
    check(stored.value("accountSamples").toObject().value(first).toObject()
                  .value(priorChannel).toArray().at(0).toObject() == priorRow,
          QStringLiteral("existing identified sample and boundary retained verbatim"));
}

}  // namespace chatterino::preview
