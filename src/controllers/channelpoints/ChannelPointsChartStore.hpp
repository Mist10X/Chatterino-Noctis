// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace chatterino {

struct ChannelPointsChartSample {
    QDateTime time;
    qint64 balance = 0;
    // A new observation interval. The renderer must not interpolate across it.
    bool startsSegment = false;
};

class ChannelPointsChartStore
{
public:
    ChannelPointsChartStore() = delete;

    // The account is the owner of the balance, not necessarily the selected
    // chat account. An empty identity is kept separately as unattributed.
    static bool recordSample(const QString &channelId, const QString &accountId,
                             qint64 balance,
                             QDateTime observedAt = QDateTime::currentDateTimeUtc());
    // Explicit end of observation for the channel (all owners). Do not call
    // for a transient request error or ordinary account switching: continuity
    // already expires after 30 minutes without an actual confirmed balance.
    static void markGap(const QString &channelId);
    static QVector<ChannelPointsChartSample> samplesForChannel(
        const QString &channelId);
    static QVector<ChannelPointsChartSample> samplesForAccount(
        const QString &channelId, const QString &accountId);
    static QVector<ChannelPointsChartSample> legacySamplesForChannel(
        const QString &channelId);
};

}  // namespace chatterino
