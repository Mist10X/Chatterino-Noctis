#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "controllers/channelpoints/ChannelPointsChartStore.hpp"

#    include <QWidget>

namespace chatterino {

class Theme;

class ChannelPointsChartView : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelPointsChartView(QWidget *parent = nullptr);

    void setSamples(const QVector<ChannelPointsChartSample> &samples);
    void setSampleHistories(const QVector<ChannelPointsChartSample> &current,
                            const QVector<ChannelPointsChartSample> &legacy);
    void reloadFromStore(const QString &channelId);
    void applyTheme(const Theme &theme);
    void setChartScale(float scale);
    void resetTimeRange();

    ~ChannelPointsChartView() override;

private:
    class Private;
    Private *d_;
};

}  // namespace chatterino

#endif
