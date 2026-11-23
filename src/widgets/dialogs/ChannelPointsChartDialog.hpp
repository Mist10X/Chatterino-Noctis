#pragma once

#include "providers/moltorino/MoltorinoFeatureFlags.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "widgets/DraggablePopup.hpp"

#    include <pajlada/signals/scoped-connection.hpp>
#    include <QPointer>
#    include <QVector>
#    include <memory>

class QLabel;
class QPaintEvent;
class QResizeEvent;
class QVBoxLayout;

namespace chatterino {

class Button;
class ChannelAvatarLabel;
class ChannelPointsChartView;
class SvgButton;
class TwitchChannel;
class UsercardResizeGrip;

class ChannelPointsChartDialog : public DraggablePopup
{
public:
    ChannelPointsChartDialog(TwitchChannel *channel, QWidget *parent = nullptr);

    static void showDialog(TwitchChannel *channel, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    void showEvent(QShowEvent *event) override;

private:
    void refreshHeader();
    void refreshStyle();
    void reloadChart();
    void applySizeConstraints();
    void scheduleUnpinParentOnClose(QWidget *parent);

    // A pinned chart remains valid even when the tab that opened it is closed.
    std::shared_ptr<TwitchChannel> channel_;
    QVBoxLayout *mainLayout_{};
    QWidget *headerWidget_{};
    QLabel *headerTitleLabel_{};
    QLabel *headerChannelLabel_{};
    QLabel *headerSubtitleLabel_{};
    ChannelAvatarLabel *headerAvatar_{};
    Button *pinButton_{};
    SvgButton *closeButton_{};
    ChannelPointsChartView *chartView_{};
    UsercardResizeGrip *resizeGrip_{};
    bool parentUnpinScheduled_ = false;

    pajlada::Signals::ScopedConnection channelPointsConnection_;

    static QVector<QPointer<ChannelPointsChartDialog>> activeDialogs_;
};

}  // namespace chatterino

#endif
