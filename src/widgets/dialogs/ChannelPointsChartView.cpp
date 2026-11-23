// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/ChannelPointsChartView.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#    include "Application.hpp"
#    include "common/UiLanguage.hpp"
#    include "singletons/Fonts.hpp"
#    include "singletons/Theme.hpp"
#    include "widgets/helper/NoctisTooltip.hpp"

#    include <QComboBox>
#    include <QHBoxLayout>
#    include <QLabel>
#    include <QLinearGradient>
#    include <QListView>
#    include <QLocale>
#    include <QMouseEvent>
#    include <QPainter>
#    include <QPainterPath>
#    include <QPushButton>
#    include <QSignalBlocker>
#    include <QVariantAnimation>
#    include <QVBoxLayout>
#    include <QWheelEvent>
#    include <QtMath>

#    include <algorithm>
#    include <array>
#    include <cmath>
#    include <functional>
#    include <limits>
#    include <optional>

namespace {

using namespace chatterino;
using Sample = ChannelPointsChartSample;
using Samples = QVector<Sample>;

enum class ChartTimeRange {
    All,
    Last24Hours,
    Last7Days,
    Last14Days,
    Last30Days,
    Last90Days,
};

constexpr ChartTimeRange DEFAULT_CHART_TIME_RANGE = ChartTimeRange::Last14Days;

struct ChartTimeRangeOption {
    const char *label;
    ChartTimeRange range;
};

constexpr ChartTimeRangeOption CHART_TIME_RANGE_OPTIONS[] = {
    {"Last 14 days", ChartTimeRange::Last14Days},
    {"Last 24 hours", ChartTimeRange::Last24Hours},
    {"Last 7 days", ChartTimeRange::Last7Days},
    {"Last 30 days", ChartTimeRange::Last30Days},
    {"Last 90 days", ChartTimeRange::Last90Days},
    {"All time", ChartTimeRange::All},
};

QLocale chartLocale()
{
    return QLocale(activeUiLanguage() == "ru" ? QLocale::Russian : QLocale::English);
}

QString fullNumber(qint64 value)
{
    return chartLocale().toString(value);
}

QString signedNumber(qint64 value)
{
    return (value > 0 ? QStringLiteral("+") : QString{}) + fullNumber(value);
}

// Localized, non-scientific tick labels. Increase decimal precision when a
// narrow Y range would otherwise make neighboring tick labels identical.
QString compactNumber(double value, int precision = 1)
{
    static constexpr std::array<double, 5> factors{1.0, 1e3, 1e6, 1e9, 1e12};
    const bool ru = activeUiLanguage() == "ru";
    static const std::array<QString, 5> suffixRu{
        QString(), QStringLiteral("к"), QStringLiteral("млн"),
        QStringLiteral("млрд"), QStringLiteral("трлн")};
    static const std::array<QString, 5> suffixEn{
        QString(), QStringLiteral("K"), QStringLiteral("M"),
        QStringLiteral("B"), QStringLiteral("T")};
    int i = int(factors.size()) - 1;
    while (i > 0 && std::abs(value) < factors[i])
    {
        --i;
    }
    auto result = chartLocale().toString(value / factors[i], 'f',
                                        i == 0 ? 0 : precision);
    if (i != 0)
    {
        auto locale = chartLocale();
        const auto zero = locale.zeroDigit();
        while (result.contains(locale.decimalPoint()) && result.endsWith(zero))
        {
            result.chop(zero.size());
        }
        if (result.endsWith(locale.decimalPoint()))
        {
            result.chop(locale.decimalPoint().size());
        }
    }
    return result + (ru ? suffixRu[i] : suffixEn[i]);
}

QString localTimeText(const QDateTime &time)
{
    return chartLocale().toString(time.toLocalTime(),
                                  QStringLiteral("dd MMM yyyy HH:mm:ss"));
}

QString timeAxisText(qint64 ms, qint64 span)
{
    const auto time = QDateTime::fromMSecsSinceEpoch(ms).toLocalTime();
    constexpr qint64 DAY = 86400000;
    if (span <= 3600000)
    {
        return chartLocale().toString(time, QStringLiteral("HH:mm:ss"));
    }
    if (span <= DAY)
    {
        return chartLocale().toString(time, QStringLiteral("HH:mm"));
    }
    if (span <= 4 * DAY)
    {
        return chartLocale().toString(time, QStringLiteral("d MMM HH:mm"));
    }
    if (span <= 366 * DAY)
    {
        return chartLocale().toString(time, QStringLiteral("d MMM"));
    }
    return chartLocale().toString(time, QStringLiteral("MMM yy"));
}

QColor blendColors(QColor a, QColor b, double weight)
{
    return QColor::fromRgbF(a.redF() * (1 - weight) + b.redF() * weight,
                            a.greenF() * (1 - weight) + b.greenF() * weight,
                            a.blueF() * (1 - weight) + b.blueF() * weight);
}

struct ChartColors {
    QColor surface{"#1b1e29"};
    QColor text{"#e9e6f2"};
    QColor muted{"#9a9cad"};
    QColor border{"#343748"};
    QColor accent{"#9381dc"};
    QColor positive{"#8ac6ae"};
    QColor negative{"#daa5a6"};
};

qint64 timestamp(const Sample &sample)
{
    return sample.time.toMSecsSinceEpoch();
}

bool sameSamples(const Samples &a, const Samples &b)
{
    return a.size() == b.size() &&
           std::equal(a.cbegin(), a.cend(), b.cbegin(),
                      [](const Sample &lhs, const Sample &rhs) {
                          return lhs.time == rhs.time &&
                                 lhs.balance == rhs.balance &&
                                 lhs.startsSegment == rhs.startsSegment;
                      });
}

// Current history carries recorded segment boundaries. Historical, unassigned
// observations can have arbitrarily long periods between recorded changes, so
// too-long intervals in that legacy series are marked unknown conservatively.
bool isGap(const Sample &a, const Sample &b, bool legacy)
{
    constexpr qint64 LEGACY_GAP_MS = 6 * 60 * 60 * 1000LL;
    // A legacy zero may be real, but the previous and next observation may
    // belong to other accounts. Preserve it as an exact isolated sample
    // rather than drawing an apparent spend/recovery from unknown owners.
    return b.startsSegment ||
           (legacy && (timestamp(b) - timestamp(a) > LEGACY_GAP_MS ||
                       ((a.balance == 0) != (b.balance == 0))));
}

Samples sanitized(Samples samples)
{
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [](const Sample &sample) {
                                     return !sample.time.isValid() ||
                                            sample.balance < 0;
                                 }), samples.end());
    std::stable_sort(samples.begin(), samples.end(),
                     [](const auto &a, const auto &b) {
                         return a.time < b.time;
                     });
    return samples;
}

Samples selectedSamples(const Samples &samples, ChartTimeRange range)
{
    if (samples.isEmpty() || range == ChartTimeRange::All)
    {
        return samples;
    }
    const auto end = samples.last().time;
    const auto start = range == ChartTimeRange::Last24Hours ? end.addSecs(-86400)
                      : range == ChartTimeRange::Last7Days ? end.addDays(-7)
                      : range == ChartTimeRange::Last14Days ? end.addDays(-14)
                      : range == ChartTimeRange::Last30Days ? end.addDays(-30)
                                                          : end.addDays(-90);
    // Never extrapolate a balance from outside the selected period.
    const auto it = std::lower_bound(samples.cbegin(), samples.cend(), start,
                                     [](const Sample &sample, const QDateTime &time) {
                                         return sample.time < time;
                                     });
    Samples visible;
    visible.reserve(std::distance(it, samples.cend()));
    for (auto current = it; current != samples.cend(); ++current)
    {
        visible.push_back(*current);
    }
    return visible;
}

double niceStep(double span, int ticks)
{
    const auto raw = std::max(1.0, span / std::max(1, ticks));
    const auto magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const auto fraction = raw / magnitude;
    return magnitude * (fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10);
}

// An entirely QWidget/QPainter-native plot. It only renders observed
// samples, with geometry cached between hover updates. No third-party
// charting implementation is needed.
class ChartCanvas final : public QWidget
{
public:
    explicit ChartCanvas(QWidget *parent)
        : QWidget(parent)
        , appearance_(this)
    {
        this->setObjectName("ChannelPointsChartCanvas");
        this->setMouseTracking(true);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        this->setAccessibleName(uiText("Channel Points Chart"));
        this->setAccessibleDescription(uiText("Drag or scroll to zoom the time range."));
        appearance_.setDuration(145);
        appearance_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&appearance_, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             opacity_ = value.toReal();
                             this->update();
                         });
    }

    void setScale(float scale)
    {
        scale = std::clamp(scale, .8F, 1.7F);
        if (qFuzzyCompare(this->scale_, scale))
        {
            return;
        }
        this->scale_ = scale;
        invalidateGeometry();
        this->updateGeometry();
    }

    void setColors(ChartColors colors)
    {
        colors_ = std::move(colors);
        this->update();
    }

    void setData(Samples samples, bool legacy, bool reset)
    {
        if (!reset && legacy_ == legacy && sameSamples(samples_, samples))
        {
            return;
        }
        clearHover();
        const bool preserve = !reset && legacy_ == legacy && this->zoomed();
        const auto previousFrom = viewFrom_;
        const auto previousTo = viewTo_;
        legacy_ = legacy;
        samples_ = std::move(samples);
        if (samples_.isEmpty())
        {
            domainFrom_ = domainTo_ = viewFrom_ = viewTo_ = 0;
        }
        else
        {
            domainFrom_ = timestamp(samples_.first());
            domainTo_ = timestamp(samples_.last());
            if (domainFrom_ == domainTo_)
            {
                // A lone observation needs a visible dot, not a zero-width axis.
                domainFrom_ -= 1800000;
                domainTo_ += 1800000;
            }
            viewFrom_ = domainFrom_;
            viewTo_ = domainTo_;
            if (preserve && previousFrom < previousTo &&
                previousTo > domainFrom_ && previousFrom < domainTo_)
            {
                viewFrom_ = std::max(domainFrom_, previousFrom);
                viewTo_ = std::min(domainTo_, previousTo);
            }
        }
        invalidateGeometry();
        // Only opacity is animated: on-screen points always represent the
        // exact observed balance. An interpolated animation could fake values.
        appearance_.stop();
        if (this->isVisible() && !samples_.isEmpty())
        {
            appearance_.setStartValue(.64);
            appearance_.setEndValue(1.0);
            appearance_.start();
        }
        else
        {
            opacity_ = 1.0;
        }
        if (this->rangeChanged)
        {
            this->rangeChanged();
        }
    }

    bool zoomed() const
    {
        return viewFrom_ != domainFrom_ || viewTo_ != domainTo_;
    }

    std::function<void()> rangeChanged;

    QSize sizeHint() const override
    {
        return {qRound(508 * scale_), qRound(310 * scale_)};
    }

    QSize minimumSizeHint() const override
    {
        return {qRound(315 * scale_), qRound(215 * scale_)};
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        clearHover();
        invalidateGeometry();
    }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange)
        {
            invalidateGeometry();
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        ensureGeometry();
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

        drawSummary(p);
        p.setPen(QPen(colors_.border, 1));
        p.setBrush(colors_.surface);
        p.drawRoundedRect(frame_, 8 * scale_, 8 * scale_);

        if (visible_.isEmpty())
        {
            p.setPen(colors_.muted);
            p.setFont(axisFont());
            p.drawText(frame_.adjusted(12 * scale_, 8 * scale_,
                                      -12 * scale_, -8 * scale_),
                       Qt::AlignCenter | Qt::TextWordWrap,
                       samples_.isEmpty()
                           ? uiText("No chart data yet for this channel.")
                           : uiText("No data in the selected time range."));
            drawFooter(p);
            return;
        }

        drawAxes(p);
        p.save();
        p.setClipRect(plot_);
        drawGaps(p);
        p.setOpacity(opacity_);
        auto top = colors_.accent;
        top.setAlpha(50);
        auto bottom = colors_.accent;
        bottom.setAlpha(0);
        QLinearGradient fill(plot_.topLeft(), plot_.bottomLeft());
        fill.setColorAt(0, top);
        fill.setColorAt(1, bottom);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(fillPath_);

        QPen line(colors_.accent, 2.0 * scale_);
        line.setCapStyle(Qt::RoundCap);
        line.setJoinStyle(Qt::RoundJoin);
        p.setPen(line);
        p.setBrush(Qt::NoBrush);
        p.drawPath(linePath_);
        p.setPen(Qt::NoPen);
        p.setBrush(colors_.accent);
        for (const auto &point : dots_)
        {
            p.drawEllipse(point, 2.8 * scale_, 2.8 * scale_);
        }
        p.setOpacity(1.0);

        if (hovered_ >= 0 && hovered_ < visible_.size())
        {
            const auto at = pointFor(visible_[hovered_]);
            auto crosshair = colors_.accent;
            crosshair.setAlpha(145);
            p.setPen(QPen(crosshair, 1.1 * scale_, Qt::DashLine));
            p.drawLine(QPointF(at.x(), plot_.top()),
                       QPointF(at.x(), plot_.bottom()));
            p.setPen(QPen(colors_.surface, 1.5 * scale_));
            p.setBrush(colors_.accent);
            p.drawEllipse(at, 4.5 * scale_, 4.5 * scale_);
        }
        if (dragFrom_.has_value() && dragTo_.has_value())
        {
            const QRectF selection(QPointF(std::min(*dragFrom_, *dragTo_), plot_.top()),
                                   QPointF(std::max(*dragFrom_, *dragTo_), plot_.bottom()));
            auto tint = colors_.accent;
            tint.setAlpha(50);
            p.setPen(QPen(colors_.accent, 1));
            p.setBrush(tint);
            p.drawRect(selection);
        }
        p.restore();
        drawFooter(p);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        ensureGeometry();
        if (dragFrom_)
        {
            dragTo_ = std::clamp(event->position().x(), plot_.left(), plot_.right());
            this->update();
            return;
        }
        updateHover(event->position(), event->globalPosition().toPoint());
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        ensureGeometry();
        if (event->button() == Qt::LeftButton && !visible_.isEmpty() &&
            plot_.contains(event->position()))
        {
            clearHover();
            dragFrom_ = dragTo_ = event->position().x();
            this->update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (dragFrom_ && event->button() == Qt::LeftButton)
        {
            const auto end = std::clamp(event->position().x(), plot_.left(), plot_.right());
            const auto start = *dragFrom_;
            dragFrom_.reset();
            dragTo_.reset();
            if (std::abs(end - start) >= 12 * scale_)
            {
                zoomTo(timeFor(std::min(start, end)), timeFor(std::max(start, end)));
            }
            else
            {
                this->update();
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        ensureGeometry();
        if (samples_.size() < 2 || !plot_.contains(event->position()) ||
            event->angleDelta().y() == 0)
        {
            QWidget::wheelEvent(event);
            return;
        }
        const auto total = domainTo_ - domainFrom_;
        const auto span = viewTo_ - viewFrom_;
        const auto fraction = (event->position().x() - plot_.left()) /
                              std::max(1.0, plot_.width());
        const auto factor = std::pow(1.24, -event->angleDelta().y() / 120.0);
        const auto minimum = std::max<qint64>(1, std::min<qint64>(60000, total));
        const auto newSpan = std::clamp(double(span) * factor, double(minimum), double(total));
        auto start = viewFrom_ + qint64(fraction * (span - newSpan));
        start = std::clamp(start, domainFrom_, domainTo_ - qint64(newSpan));
        zoomTo(start, start + qint64(newSpan));
        event->accept();
    }

    void leaveEvent(QEvent *) override
    {
        clearHover();
    }

    void hideEvent(QHideEvent *) override
    {
        clearHover();
    }

private:
    QFont axisFont() const
    {
        return getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, scale_);
    }

    QFont valueFont() const
    {
        return getApp()->getFonts()->getFont(FontStyle::ChatMediumBold, scale_);
    }

    void invalidateGeometry()
    {
        geometryDirty_ = true;
        // Bounds/state should also be available after a data update before
        // the window becomes visible.
        ensureGeometry();
        this->update();
    }

    void zoomTo(qint64 from, qint64 to)
    {
        const auto minimum = std::max<qint64>(1, std::min<qint64>(60000, domainTo_ - domainFrom_));
        if (to - from < minimum)
        {
            to = std::min(domainTo_, from + minimum);
            from = std::max(domainFrom_, to - minimum);
        }
        viewFrom_ = std::clamp(from, domainFrom_, domainTo_);
        viewTo_ = std::clamp(to, viewFrom_, domainTo_);
        clearHover();
        invalidateGeometry();
        if (this->rangeChanged)
        {
            this->rangeChanged();
        }
    }

    qint64 timeFor(qreal x) const
    {
        return viewFrom_ + qRound64((x - plot_.left()) /
            std::max(1.0, plot_.width()) * double(viewTo_ - viewFrom_));
    }

    qreal xFor(qint64 ms) const
    {
        return plot_.left() + plot_.width() *
            double(ms - viewFrom_) / std::max(1.0, double(viewTo_ - viewFrom_));
    }

    QPointF pointFor(const Sample &sample) const
    {
        return {xFor(timestamp(sample)),
                plot_.bottom() - plot_.height() *
                    (double(sample.balance) - yLow_) / std::max(1.0, yHigh_ - yLow_)};
    }

    void clearHover()
    {
        if (hovered_ != -1 || hoveringGap_ || hoveredSummary_ != -1)
        {
            hovered_ = -1;
            hoveringGap_ = false;
            hoveredSummary_ = -1;
            this->setProperty("hoverBalance", QVariant{});
            noctistooltip::hideText();
            this->update();
        }
    }

    void ensureGeometry()
    {
        if (!geometryDirty_)
        {
            return;
        }
        geometryDirty_ = false;
        linePath_ = QPainterPath{};
        fillPath_ = QPainterPath{};
        dots_.clear();
        gaps_.clear();
        visible_.clear();

        const auto first = std::lower_bound(samples_.cbegin(), samples_.cend(), viewFrom_,
                                            [](const Sample &s, qint64 ms) { return timestamp(s) < ms; });
        const auto last = std::upper_bound(first, samples_.cend(), viewTo_,
                                          [](qint64 ms, const Sample &s) { return ms < timestamp(s); });
        visible_.reserve(std::distance(first, last));
        for (auto it = first; it != last; ++it)
        {
            visible_.append(*it);
        }
        // Unknown intervals are stored separately from paths. This lets us
        // retain their visual extent even when the user zooms into a gap.
        if (samples_.size() > 1)
        {
            auto begin = std::distance(samples_.cbegin(), first);
            begin = std::max<qsizetype>(1, begin);
            for (qsizetype i = begin; i < samples_.size(); ++i)
            {
                const auto a = timestamp(samples_[i - 1]);
                const auto b = timestamp(samples_[i]);
                if (a > viewTo_)
                {
                    break;
                }
                if (b >= viewFrom_ && isGap(samples_[i - 1], samples_[i], legacy_) && a < b)
                {
                    const auto begin = std::max(a, viewFrom_);
                    const auto end = std::min(b, viewTo_);
                    if (begin < end)
                        gaps_.append({begin, end});
                }
            }
        }

        const qreal cardHeight = std::max(qreal(52 * scale_),
            QFontMetricsF(axisFont()).height() + QFontMetricsF(valueFont()).height() + 10 * scale_);
        summary_ = QRectF(0, 0, this->width(), cardHeight);
        const auto footer = footerText();
        const qreal footerHeight = footer.isEmpty() ? 0 :
            std::max(qreal(18 * scale_),
                QFontMetricsF(axisFont()).boundingRect(
                    QRectF(0, 0, std::max(1.0, this->width() - 8.0 * scale_), 1000),
                    Qt::AlignLeft | Qt::TextWordWrap, footer).height() + 6 * scale_);
        frame_ = QRectF(0.5, summary_.bottom() + 7 * scale_,
                        std::max(0.0, this->width() - 1.0),
                        std::max(0.0, this->height() - footerHeight -
                            summary_.bottom() - 7 * scale_ - .5));

        this->setProperty("showingLegacy", legacy_);
        this->setProperty("sampleCount", visible_.size());
        this->setProperty("gapCount", gaps_.size());
        this->setProperty("visibleFrom", viewFrom_);
        this->setProperty("visibleTo", viewTo_);
        this->setProperty("hoverBalance", QVariant{});
        auto setOptionalBalance = [this](const char *name, std::optional<qint64> value) {
            this->setProperty(name, value ? QVariant::fromValue(*value) : QVariant{});
        };
        if (visible_.isEmpty())
        {
            setOptionalBalance("latestBalance", {});
            setOptionalBalance("minimumBalance", {});
            setOptionalBalance("maximumBalance", {});
            setOptionalBalance("netChange", {});
            this->setAccessibleName(uiText("No data in the selected time range."));
            plot_ = frame_.adjusted(8, 8, -8, -8);
            this->setProperty("plotArea", plot_.toAlignedRect());
            return;
        }

        auto [minIt, maxIt] = std::minmax_element(visible_.cbegin(), visible_.cend(),
            [](const auto &a, const auto &b) { return a.balance < b.balance; });
        minBalance_ = minIt->balance;
        maxBalance_ = maxIt->balance;
        const auto min = double(minBalance_);
        const auto max = double(maxBalance_);
        const auto padding = std::max({1.0, (max - min) * .11,
                                       (max == min ? max * .025 : 0.0)});
        const int desiredTicks = std::clamp(qRound(frame_.height() / (43 * scale_)), 2, 5);
        tickStep_ = niceStep(max - min + 2 * padding, desiredTicks);
        yLow_ = std::max(0.0, std::floor((min - padding) / tickStep_) * tickStep_);
        yHigh_ = std::ceil((max + padding) / tickStep_) * tickStep_;
        if (yHigh_ <= yLow_)
        {
            yHigh_ = yLow_ + std::max(1.0, tickStep_);
        }
        axisTicks_.clear();
        const auto axisMetrics = QFontMetricsF(axisFont());
        qreal axisLabelWidth = 0;
        for (int i = 0; i <= 10; ++i)
        {
            const auto val = yLow_ + i * tickStep_;
            if (val > yHigh_ + tickStep_ * .001)
            {
                break;
            }
            // Keep labels distinct even if zoomed into a very small delta
            // around a large balance.
            auto label = tickStep_ < 1000.0 ? chartLocale().toString(val, 'f', 0)
                                          : compactNumber(val, tickStep_ < 10000.0 ? 2 : 1);
            axisLabelWidth = std::max(axisLabelWidth, axisMetrics.horizontalAdvance(label));
            axisTicks_.append({val, std::move(label)});
        }

        const auto timeSpan = viewTo_ - viewFrom_;
        const auto xMargin = std::min(qreal(60 * scale_),
             std::max(axisMetrics.horizontalAdvance(timeAxisText(viewTo_, timeSpan)),
                      axisMetrics.horizontalAdvance(timeAxisText(viewFrom_, timeSpan))) / 2 + 6 * scale_);
        plot_ = frame_.adjusted(std::max(axisLabelWidth + 14 * scale_, xMargin),
                               13 * scale_, -xMargin, -axisMetrics.height() - 13 * scale_);
        if (plot_.width() < 1 || plot_.height() < 1)
        {
            plot_ = QRectF(frame_.center(), QSizeF(1, 1));
        }
        this->setProperty("plotArea", plot_.toAlignedRect());
        setOptionalBalance("latestBalance", visible_.last().balance);
        setOptionalBalance("minimumBalance", minBalance_);
        setOptionalBalance("maximumBalance", maxBalance_);
        const auto net = !legacy_ && visible_.size() > 1
                             ? std::optional<qint64>(visible_.last().balance - visible_.first().balance)
                             : std::nullopt;
        setOptionalBalance("netChange", net);

        QString description = uiText("Last recorded balance") + ": " +
            fullNumber(visible_.last().balance) + "; " +
            uiText("Recorded range") + ": " + fullNumber(minBalance_) +
            " – " + fullNumber(maxBalance_);
        if (net.has_value())
        {
            description += "; " + uiText("Net change") + ": " + signedNumber(*net);
        }
        if (legacy_)
        {
            description += "; " + uiText("Account of older records is unknown.");
        }
        this->setAccessibleName(description);

        // The cached geometry is bounded by horizontal pixels, rather than
        // the total history size. Keep first, last and both extremes in every
        // pixel column so a brief real spend or peak cannot disappear.
        const auto flushSegment = [this](const QVector<QPointF> &points) {
            if (points.isEmpty())
            {
                return;
            }
            if (points.size() == 1)
            {
                dots_.append(points.front());
                return;
            }
            linePath_.moveTo(points.front());
            fillPath_.moveTo(points.front().x(), plot_.bottom());
            fillPath_.lineTo(points.front());
            for (qsizetype i = 1; i < points.size(); ++i)
            {
                linePath_.lineTo(points[i]);
                fillPath_.lineTo(points[i]);
            }
            fillPath_.lineTo(points.back().x(), plot_.bottom());
            fillPath_.closeSubpath();
        };
        QVector<QPointF> points;
        points.reserve(qMax(4, qRound(plot_.width()) * 3));
        qsizetype start = 0;
        while (start < visible_.size())
        {
            qsizetype end = start + 1;
            while (end < visible_.size() && !isGap(visible_[end - 1], visible_[end], legacy_))
            {
                ++end;
            }
            points.clear();
            for (qsizetype index = start; index < end;)
            {
                const auto column = qFloor(xFor(timestamp(visible_[index])));
                auto stop = index + 1;
                auto low = index;
                auto high = index;
                while (stop < end && qFloor(xFor(timestamp(visible_[stop]))) == column)
                {
                    if (visible_[stop].balance < visible_[low].balance) low = stop;
                    if (visible_[stop].balance > visible_[high].balance) high = stop;
                    ++stop;
                }
                std::array<qsizetype, 4> keep{index, low, high, stop - 1};
                std::sort(keep.begin(), keep.end());
                qsizetype previous = -1;
                for (auto k : keep)
                {
                    if (k != previous)
                    {
                        points.push_back(pointFor(visible_[k]));
                        previous = k;
                    }
                }
                index = stop;
            }
            flushSegment(points);
            start = end;
        }
        dots_.append(pointFor(visible_.last()));
    }

    void drawAxes(QPainter &p)
    {
        p.setFont(axisFont());
        const auto fm = p.fontMetrics();
        auto grid = colors_.border;
        grid.setAlpha(100);
        for (const auto &[value, label] : axisTicks_)
        {
            const auto y = plot_.bottom() - plot_.height() *
                (value - yLow_) / std::max(1.0, yHigh_ - yLow_);
            p.setPen(QPen(grid, 1));
            p.drawLine(QPointF(plot_.left(), y), QPointF(plot_.right(), y));
            p.setPen(colors_.muted);
            p.drawText(QRectF(frame_.left() + 4 * scale_, y - fm.height() / 2.0,
                              plot_.left() - frame_.left() - 11 * scale_, fm.height()),
                       Qt::AlignRight | Qt::AlignVCenter, label);
        }
        const qint64 span = viewTo_ - viewFrom_;
        const auto exampleWidth = std::max(fm.horizontalAdvance(timeAxisText(viewTo_, span)),
                                            fm.horizontalAdvance(timeAxisText(viewFrom_, span)));
        const auto ticks = std::clamp(int(plot_.width() /
             std::max(1.0, double(exampleWidth) + 14 * scale_)), 1, 5);
        for (int i = 0; i <= ticks; ++i)
        {
            const auto ms = viewFrom_ + qRound64(double(span) * i / ticks);
            const auto x = plot_.left() + plot_.width() * i / ticks;
            p.setPen(QPen(grid, 1));
            p.drawLine(QPointF(x, plot_.top()), QPointF(x, plot_.bottom()));
            const auto label = timeAxisText(ms, span);
            const auto width = fm.horizontalAdvance(label);
            const auto left = std::clamp(x - width / 2.0, frame_.left() + 4,
                                         std::max(frame_.left() + 4, frame_.right() - width - 4));
            p.setPen(colors_.muted);
            p.drawText(QRectF(left, plot_.bottom() + 5 * scale_, width + 1,
                              fm.height()), Qt::AlignCenter, label);
        }
    }

    void drawGaps(QPainter &p)
    {
        auto tint = colors_.muted;
        tint.setAlpha(20);
        auto boundary = colors_.muted;
        boundary.setAlpha(70);
        for (const auto &[from, to] : gaps_)
        {
            const QRectF region(QPointF(xFor(from), plot_.top()),
                                QPointF(xFor(to), plot_.bottom()));
            if (region.width() < 1.25)
            {
                continue;
            }
            p.fillRect(region, tint);
            p.setPen(QPen(boundary, 1, Qt::DashLine));
            p.drawLine(region.topLeft(), region.bottomLeft());
            p.drawLine(region.topRight(), region.bottomRight());
        }
    }

    QString footerText() const
    {
        QString note;
        if (legacy_)
        {
            note = uiText("Account of older records is unknown.");
        }
        if (!gaps_.isEmpty())
        {
            if (!note.isEmpty())
            {
                note += " · ";
            }
            note += uiText("Shaded intervals: no observations.");
        }
        return note;
    }

    void drawFooter(QPainter &p)
    {
        const auto note = footerText();
        if (note.isEmpty())
        {
            return;
        }
        p.setFont(axisFont());
        p.setPen(colors_.muted);
        p.drawText(QRectF(4 * scale_, frame_.bottom() + 3 * scale_,
                          this->width() - 8 * scale_,
                          this->height() - frame_.bottom() - 3 * scale_),
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, note);
    }

    void drawSummary(QPainter &p)
    {
        const qreal spacing = 6 * scale_;
        const qreal width = (summary_.width() - spacing * 2) / 3;
        if (width < 1)
        {
            return;
        }
        const QString empty = QStringLiteral("—");
        const auto latest = visible_.isEmpty() ? empty : fullNumber(visible_.last().balance);
        const auto delta = !legacy_ && visible_.size() > 1
            ? std::optional<qint64>(visible_.last().balance - visible_.first().balance)
            : std::nullopt;
        const auto change = delta.has_value() ? signedNumber(*delta) : empty;
        const auto range = visible_.isEmpty() ? empty :
            compactNumber(double(minBalance_)) + " – " + compactNumber(double(maxBalance_));
        const std::array<QString, 3> captions{
            uiText("Last recorded balance"), uiText("Net change"), uiText("Recorded range")};
        const std::array<QString, 3> values{latest, change, range};
        for (int i = 0; i < 3; ++i)
        {
            const QRectF box(summary_.left() + i * (width + spacing) + .5, .5,
                             width - 1, summary_.height() - 1);
            p.setPen(QPen(colors_.border, 1));
            p.setBrush(colors_.surface);
            p.drawRoundedRect(box, 7 * scale_, 7 * scale_);
            const qreal inset = 8 * scale_;
            const auto captionRect = box.adjusted(inset, 6 * scale_, -inset, 0);
            p.setPen(colors_.muted);
            auto small = axisFont();
            p.setFont(small);
            if (p.fontMetrics().horizontalAdvance(captions[i]) > captionRect.width())
            {
                // Only the long summary caption shrinks a little when needed.
                const auto size = QFontMetricsF(small).horizontalAdvance(captions[i]);
                small.setPointSizeF(std::max(small.pointSizeF() * .80,
                                            small.pointSizeF() * captionRect.width() / size));
                p.setFont(small);
            }
            p.drawText(captionRect, Qt::AlignLeft | Qt::AlignTop, captions[i]);
            auto large = valueFont();
            p.setFont(large);
            auto value = values[i];
            if (QFontMetricsF(large).horizontalAdvance(value) > captionRect.width())
            {
                if (i == 0 && !visible_.isEmpty())
                {
                    value = compactNumber(double(visible_.last().balance));
                }
                else if (i == 1 && delta.has_value())
                {
                    value = (*delta > 0 ? QStringLiteral("+") : QString{}) + compactNumber(double(*delta));
                }
            }
            if (QFontMetricsF(large).horizontalAdvance(value) > captionRect.width())
            {
                large.setPointSizeF(std::max(large.pointSizeF() * .75,
                    large.pointSizeF() * captionRect.width() / QFontMetricsF(large).horizontalAdvance(value)));
                p.setFont(large);
            }
            p.setPen(i == 1 && delta.value_or(0) != 0
                        ? (*delta > 0 ? colors_.positive : colors_.negative) : colors_.text);
            p.drawText(box.adjusted(inset, 0, -inset, -7 * scale_),
                       Qt::AlignLeft | Qt::AlignBottom, value);
        }
    }

    void updateHover(QPointF pos, QPoint global)
    {
        if (summary_.contains(pos) && !visible_.isEmpty())
        {
            this->unsetCursor();
            const qreal spacing = 6 * scale_;
            const qreal width = (summary_.width() - spacing * 2) / 3;
            const auto index = std::clamp(int(pos.x() / std::max(1.0, width + spacing)), 0, 2);
            if (hoveredSummary_ == index)
            {
                return;
            }
            clearHover();
            hoveredSummary_ = index;
            const auto &first = visible_.first();
            const auto &last = visible_.last();
            QString details;
            if (index == 0)
            {
                // The date of the last actual observation is not otherwise
                // visible, even when the balance fits into the summary tile.
                details = localTimeText(last.time) + "\n" +
                          fullNumber(last.balance) + uiText(" points");
            }
            else if (index == 1 && !legacy_ && visible_.size() > 1)
            {
                details = uiText("Difference between the first and last recorded balance.") +
                          "\n" + fullNumber(first.balance) + " → " + fullNumber(last.balance);
            }
            else if (index == 1 && legacy_)
            {
                details = uiText("Account of older records is unknown.");
            }
            else if (index == 2)
            {
                details = uiText("Recorded range") + "\n" + fullNumber(minBalance_) +
                          " – " + fullNumber(maxBalance_);
            }
            if (!details.isEmpty())
            {
                noctistooltip::showText(global, details, this,
                    QRectF(index * (width + spacing), 0, width, summary_.height()).toAlignedRect());
            }
            return;
        }
        if (!plot_.contains(pos) || visible_.isEmpty())
        {
            clearHover();
            this->unsetCursor();
            return;
        }
        if (hoveredSummary_ != -1)
        {
            clearHover();
        }
        this->setCursor(Qt::CrossCursor);
        const auto time = timeFor(pos.x());
        auto it = std::lower_bound(visible_.cbegin(), visible_.cend(), time,
                                   [](const Sample &s, qint64 ms) { return timestamp(s) < ms; });
        auto index = qsizetype(std::distance(visible_.cbegin(), it));
        if (index >= visible_.size())
        {
            index = visible_.size() - 1;
        }
        if (index > 0 && std::abs(timestamp(visible_[index]) - time) >
                             std::abs(timestamp(visible_[index - 1]) - time))
        {
            --index;
        }
        const auto at = pointFor(visible_[index]);
        auto afterGap = std::upper_bound(gaps_.cbegin(), gaps_.cend(), time,
            [](qint64 value, const auto &gap) { return value < gap.first; });
        const bool inUnknown = afterGap != gaps_.cbegin() &&
            time > std::prev(afterGap)->first && time < std::prev(afterGap)->second;
        if ((inUnknown || visible_.size() == 1) &&
            std::abs(at.x() - pos.x()) > 13 * scale_)
        {
            if (hovered_ != -1 || !hoveringGap_)
            {
                hovered_ = -1;
                hoveringGap_ = true;
                this->setProperty("hoverBalance", QVariant{});
                noctistooltip::showText(global, uiText("No observations in this interval."),
                                      this, plot_.toAlignedRect());
                this->update();
            }
            return;
        }
        if (index == hovered_ && !hoveringGap_)
        {
            return;
        }
        hovered_ = index;
        hoveringGap_ = false;
        const auto &sample = visible_[index];
        this->setProperty("hoverBalance", sample.balance);
        auto text = localTimeText(sample.time) + "\n" +
            fullNumber(sample.balance) + uiText(" points");
        if (index > 0 && !legacy_ && !isGap(visible_[index - 1], sample, false))
        {
            text += "\n" + signedNumber(sample.balance - visible_[index - 1].balance) +
                uiText(" since previous");
        }
        if (legacy_)
        {
            text += "\n" + uiText("Account of older records is unknown.");
        }
        noctistooltip::showText(global, text, this, plot_.toAlignedRect());
        this->update();
    }

    Samples samples_;
    Samples visible_;
    QVector<QPair<qint64, qint64>> gaps_;
    QVector<QPair<double, QString>> axisTicks_;
    QPainterPath linePath_;
    QPainterPath fillPath_;
    QVector<QPointF> dots_;
    QRectF summary_;
    QRectF frame_;
    QRectF plot_;
    ChartColors colors_;
    QVariantAnimation appearance_;
    qint64 domainFrom_ = 0;
    qint64 domainTo_ = 0;
    qint64 viewFrom_ = 0;
    qint64 viewTo_ = 0;
    qint64 minBalance_ = 0;
    qint64 maxBalance_ = 0;
    double yLow_ = 0;
    double yHigh_ = 1;
    double tickStep_ = 1;
    qreal opacity_ = 1;
    float scale_ = 1;
    qsizetype hovered_ = -1;
    int hoveredSummary_ = -1;
    bool legacy_ = false;
    bool geometryDirty_ = true;
    bool hoveringGap_ = false;
    std::optional<qreal> dragFrom_;
    std::optional<qreal> dragTo_;
};

}  // namespace

namespace chatterino {

class ChannelPointsChartView::Private
{
public:
    ChartTimeRange range = DEFAULT_CHART_TIME_RANGE;
    Samples current;
    Samples legacy;
    QComboBox *rangeCombo = nullptr;
    QComboBox *sourceCombo = nullptr;
    QPushButton *resetButton = nullptr;
    QWidget *toolbar = nullptr;
    QLabel *rangeLabel = nullptr;
    ChartCanvas *canvas = nullptr;
    QHBoxLayout *toolbarLayout = nullptr;
    QVBoxLayout *layout = nullptr;
    bool showingLegacy = false;
    bool sourceChosen = false;
    float scale = 1.F;

    void applyFilter(bool reset)
    {
        canvas->setData(selectedSamples(showingLegacy ? legacy : current, range),
                        showingLegacy, reset);
        this->updateResetButton();
    }

    void updateResetButton()
    {
        resetButton->setEnabled(range != DEFAULT_CHART_TIME_RANGE || canvas->zoomed());
    }
};

ChannelPointsChartView::ChannelPointsChartView(QWidget *parent)
    : QWidget(parent)
    , d_(new Private)
{
    d_->layout = new QVBoxLayout(this);
    d_->layout->setContentsMargins(8, 5, 8, 7);
    d_->layout->setSpacing(7);

    d_->toolbar = new QWidget(this);
    d_->toolbar->setObjectName("ChannelPointsChartToolbar");
    d_->toolbarLayout = new QHBoxLayout(d_->toolbar);
    d_->toolbarLayout->setContentsMargins(0, 0, 0, 0);
    d_->toolbarLayout->setSpacing(6);

    d_->rangeLabel = new QLabel(uiText("Range:"), d_->toolbar);
    d_->rangeLabel->setObjectName("ChannelPointsChartRangeLabel");

    d_->rangeCombo = new QComboBox(d_->toolbar);
    d_->rangeCombo->setObjectName("ChannelPointsChartRangeCombo");
    d_->rangeCombo->setAccessibleDescription(
        uiText("Show only points recorded in the selected period"));
    for (const auto &option : CHART_TIME_RANGE_OPTIONS)
    {
        d_->rangeCombo->addItem(uiText(option.label), static_cast<int>(option.range));
    }
    d_->rangeCombo->setCurrentIndex(
        d_->rangeCombo->findData(static_cast<int>(DEFAULT_CHART_TIME_RANGE)));

    d_->sourceCombo = new QComboBox(d_->toolbar);
    d_->sourceCombo->setObjectName("ChannelPointsChartSourceCombo");
    d_->sourceCombo->setAccessibleName(uiText("History"));
    d_->sourceCombo->addItem(uiText("Rewards account"));
    d_->sourceCombo->addItem(uiText("Older data"));
    d_->sourceCombo->setVisible(false);

    for (auto *combo : {d_->rangeCombo, d_->sourceCombo})
    {
        combo->setEditable(false);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        if (auto *view = qobject_cast<QListView *>(combo->view()))
        {
            view->setSpacing(2);
            view->setMouseTracking(true);
        }
    }

    d_->resetButton = new QPushButton(uiText("Reset"), d_->toolbar);
    d_->resetButton->setObjectName("ChannelPointsChartResetButton");
    d_->resetButton->setToolTip(uiText("Reset the time range and chart zoom"));

    d_->toolbarLayout->addWidget(d_->rangeLabel);
    d_->toolbarLayout->addWidget(d_->rangeCombo, 1);
    d_->toolbarLayout->addWidget(d_->sourceCombo, 1);
    d_->toolbarLayout->addWidget(d_->resetButton);
    d_->layout->addWidget(d_->toolbar);

    d_->canvas = new ChartCanvas(this);
    d_->layout->addWidget(d_->canvas, 1);
    d_->canvas->rangeChanged = [this] { d_->updateResetButton(); };

    QObject::connect(d_->rangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     this, [this](int index) {
        if (index >= 0)
        {
            d_->range = static_cast<ChartTimeRange>(d_->rangeCombo->itemData(index).toInt());
            d_->applyFilter(true);
        }
    });
    QObject::connect(d_->sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     this, [this](int index) {
        d_->sourceChosen = true;
        d_->showingLegacy = index == 1;
        d_->applyFilter(true);
    });
    QObject::connect(d_->resetButton, &QPushButton::clicked, this,
                     [this] { this->resetTimeRange(); });

    d_->applyFilter(true);
    this->setChartScale(1.F);
}

ChannelPointsChartView::~ChannelPointsChartView()
{
    delete d_;
}

void ChannelPointsChartView::setSamples(const Samples &samples)
{
    this->setSampleHistories(samples, {});
}

void ChannelPointsChartView::setSampleHistories(const Samples &current,
                                               const Samples &legacy)
{
    d_->current = sanitized(current);
    d_->legacy = sanitized(legacy);
    const auto wasLegacy = d_->showingLegacy;
    if (d_->legacy.isEmpty())
    {
        d_->showingLegacy = false;
        d_->sourceChosen = false;
    }
    else if (!d_->sourceChosen)
    {
        // Never quietly merge or misattribute records that predate account
        // separation. Older history remains available and explicitly labeled.
        d_->showingLegacy = d_->current.isEmpty();
    }
    {
        const QSignalBlocker blocker(d_->sourceCombo);
        d_->sourceCombo->setCurrentIndex(d_->showingLegacy ? 1 : 0);
        d_->sourceCombo->setVisible(!d_->legacy.isEmpty());
    }
    d_->rangeLabel->setVisible(d_->legacy.isEmpty());
    d_->applyFilter(wasLegacy != d_->showingLegacy);
}

void ChannelPointsChartView::reloadFromStore(const QString &channelId)
{
    this->setSampleHistories(ChannelPointsChartStore::samplesForChannel(channelId),
                             ChannelPointsChartStore::legacySamplesForChannel(channelId));
}

void ChannelPointsChartView::resetTimeRange()
{
    const QSignalBlocker blocker(d_->rangeCombo);
    d_->rangeCombo->setCurrentIndex(
        d_->rangeCombo->findData(static_cast<int>(DEFAULT_CHART_TIME_RANGE)));
    d_->range = DEFAULT_CHART_TIME_RANGE;
    d_->applyFilter(true);
}

void ChannelPointsChartView::setChartScale(float scale)
{
    d_->scale = std::clamp(scale, .8F, 1.7F);
    const auto s = d_->scale;
    d_->canvas->setScale(s);
    const auto font = getApp()->getFonts()->getFont(FontStyle::ChatMedium, s);
    d_->rangeCombo->setFont(font);
    d_->sourceCombo->setFont(font);
    d_->resetButton->setFont(font);
    d_->rangeLabel->setFont(font);
    d_->layout->setContentsMargins(qRound(8*s), qRound(5*s), qRound(8*s), qRound(7*s));
    d_->layout->setSpacing(qRound(7*s));
    d_->toolbarLayout->setSpacing(qRound(6*s));
}

void ChannelPointsChartView::applyTheme(const Theme &theme)
{
    auto colors = ChartColors{};
    colors.surface = blendColors(theme.window.background, theme.splits.input.background, .55);
    colors.text = theme.window.text;
    colors.muted = blendColors(theme.window.background, theme.window.text, .70);
    colors.border = blendColors(theme.window.background, theme.splits.header.border, .80);
    colors.accent = theme.accent.isValid() ? theme.accent : theme.tabs.selected.line.regular;
    colors.positive = theme.isLightTheme() ? QColor("#387f69") : QColor("#8ac6ae");
    colors.negative = theme.isLightTheme() ? QColor("#ae5b68") : QColor("#dca5a8");
    d_->canvas->setColors(colors);

    const auto field = theme.splits.input.background;
    const auto hover = theme.tabs.regular.backgrounds.hover;
    const auto selected = blendColors(field, colors.accent, .23);
    d_->toolbar->setStyleSheet(
        QStringLiteral(R"(
        QLabel#ChannelPointsChartRangeLabel { color: %2; background: transparent; }
        QComboBox#ChannelPointsChartRangeCombo, QComboBox#ChannelPointsChartSourceCombo,
        QPushButton#ChannelPointsChartResetButton {
            background: %3; color: %1; border: 1px solid %4;
            border-radius: 6px; padding: 5px 8px; min-height: 20px;
        }
        QComboBox#ChannelPointsChartRangeCombo:hover,
        QComboBox#ChannelPointsChartSourceCombo:hover,
        QPushButton#ChannelPointsChartResetButton:hover:enabled {
            background: %5; border-color: %6;
        }
        QPushButton#ChannelPointsChartResetButton:disabled {
            color: %2; background: transparent;
        }
        QComboBox#ChannelPointsChartRangeCombo::drop-down,
        QComboBox#ChannelPointsChartSourceCombo::drop-down { border: 0; width: 18px; }
        QComboBox QAbstractItemView {
            background: %3; color: %1; border: 1px solid %4;
            border-radius: 6px; outline: none; padding: 3px;
            selection-background-color: %7; selection-color: %1;
        }
        QComboBox QAbstractItemView::item { padding: 5px 8px; min-height: 18px; }
        QComboBox QAbstractItemView::item:hover { background: %5; }
        )").arg(colors.text.name(), colors.muted.name(),
                 field.name(), colors.border.name(), hover.name(),
                 colors.accent.name(), selected.name()));
}

}  // namespace chatterino

#endif
