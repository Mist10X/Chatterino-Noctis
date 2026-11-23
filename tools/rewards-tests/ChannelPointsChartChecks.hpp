// SPDX-License-Identifier: MIT
#pragma once

#include "widgets/dialogs/ChannelPointsChartDialog.hpp"
#include "widgets/dialogs/ChannelPointsChartView.hpp"
#include "widgets/helper/UsercardResizeGrip.hpp"
#include "common/UiLanguage.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <algorithm>

namespace chatterino::preview {

template <class Check>
void checkChannelPointsChart(const QString &screenshot, Check &&check)
{
    const auto pump = [](int ms = 230) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    QPointer<ChannelPointsChartDialog> popup = new ChannelPointsChartDialog(nullptr, nullptr);
    popup->setAttribute(Qt::WA_DontShowOnScreen);
    if (auto *pin = popup->findChild<QWidget *>("ChannelPointsChartPin"))
        QMetaObject::invokeMethod(pin, "leftClicked", Qt::DirectConnection);
    popup->show();
    pump(30);
    if (!popup)
    {
        check(false, "chart popup remains open after pinning");
        return;
    }
    auto *view = popup->findChild<ChannelPointsChartView *>("ChannelPointsChartView");
    auto *canvas = popup->findChild<QWidget *>("ChannelPointsChartCanvas");
    auto *range = popup->findChild<QComboBox *>("ChannelPointsChartRangeCombo");
    auto *source = popup->findChild<QComboBox *>("ChannelPointsChartSourceCombo");
    auto *reset = popup->findChild<QPushButton *>("ChannelPointsChartResetButton");
    check(view && canvas && range && source && reset, "chart has native canvas, period, source and reset controls");
    if (!view || !canvas || !range || !source || !reset)
    {
        popup->close();
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc().addSecs(-120);
    const QVector<ChannelPointsChartSample> verified{
        {now.addSecs(-6 * 3600), 120000, true},
        {now.addSecs(-5 * 3600), 124500, false},
        {now.addSecs(-2 * 3600), 135000, true},
        {now.addSecs(-3600), 0, false},
        {now, 141000, false}};
    const QVector<ChannelPointsChartSample> legacy{
        {now.addDays(-3).addSecs(-120), 23000, true},
        {now.addDays(-3).addSecs(-60), 0, false},
        {now.addDays(-3), 25000, false}};
    auto paint = [&] {
        pump();
        return canvas->grab();
    };
    view->setSampleHistories({}, {});
    paint();
    check(canvas->property("sampleCount").toInt() == 0 &&
              !canvas->property("latestBalance").isValid(),
          "empty history does not manufacture zero balance");
    view->setSampleHistories(verified, legacy);
    paint();
    check(canvas->property("sampleCount").toInt() == verified.size() &&
              canvas->property("latestBalance").toLongLong() == 141000 &&
              canvas->property("minimumBalance").toLongLong() == 0,
          "true zero and latest balance stay available");
    check(canvas->property("gapCount").toInt() >= 1,
          "account/session discontinuity remains a chart gap");
    check(source->isVisible() && !canvas->property("showingLegacy").toBool(),
          "verified history takes precedence over unattributed legacy");
    if (source->count() >= 2)
    {
        const int current = source->currentIndex();
        source->setCurrentIndex(current == 0 ? 1 : 0);
        paint();
        check(canvas->property("showingLegacy").toBool() &&
                  canvas->property("sampleCount").toInt() == legacy.size() &&
                  canvas->property("gapCount").toInt() == 2 &&
                  canvas->property("minimumBalance").toLongLong() == 0 &&
                  !canvas->property("netChange").isValid(), "legacy history remains selectable but is not counted as verified change");
        source->setCurrentIndex(current);
        paint();
    }
    view->setSampleHistories({}, {});
    view->setSampleHistories({}, legacy);
    paint();
    check(canvas->property("showingLegacy").toBool() &&
              canvas->property("sampleCount").toInt() == legacy.size(),
          "old recorded data is accessible when verified history is empty");
    view->setSampleHistories(verified, {});
    paint();
    const auto plot = canvas->property("plotArea").toRect();
    check(plot.width() > 100 && plot.height() > 50 &&
              canvas->rect().contains(plot),
          "plot stays inside the canvas");
    const auto left = canvas->property("visibleFrom").toLongLong();
    const auto right = canvas->property("visibleTo").toLongLong();
    if (right > left && plot.isValid())
    {
        const auto eventFor = [&](qint64 time) {
            const auto x = plot.left() + qRound((time - left) * double(plot.width()) / (right - left));
            const QPoint local(std::clamp(x, plot.left(), plot.right()), plot.center().y());
            const auto global = canvas->mapToGlobal(local);
            QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(global),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas, &move);
            paint();
        };
        eventFor(verified[1].time.toMSecsSinceEpoch());
        check(canvas->property("hoverBalance").isValid() &&
                  canvas->property("hoverBalance").toLongLong() == verified[1].balance,
              "hover resolves the recorded balance");
        eventFor(now.addSecs(-3 * 3600 - 1800).toMSecsSinceEpoch());
        check(!canvas->property("hoverBalance").isValid(),
              "hover in a missing interval does not show an interpolated balance");
    }
    else
        check(false, "chart has a finite time range");
    const auto initialRange = range->currentIndex();
    if (range->count() > 1)
    {
        range->setCurrentIndex(initialRange == 0 ? 1 : 0);
        reset->click();
        paint();
        check(range->currentIndex() == initialRange, "reset restores the default time interval");
    }
    if (auto *grip = popup->findChild<QWidget *>("UsercardResizeGrip"))
    {
        popup->resize(popup->minimumSize().expandedTo(QSize(400, 320)));
        pump(20);
        const auto before = popup->geometry();
        const QPoint delta(std::min(35, popup->maximumWidth() - before.width()),
                           std::min(28, popup->maximumHeight() - before.height()));
        const QPoint pos(grip->width() - 2, grip->height() - 2);
        const auto global = grip->mapToGlobal(pos);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), QPointF(global),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(pos + delta), QPointF(global + delta),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos + delta), QPointF(global + delta),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &release);
        pump(40);
        check(delta.x() > 0 && delta.y() > 0 &&
                  popup->width() == before.width() + delta.x() &&
                  popup->height() == before.height() + delta.y(),
              "chart popup resizes through its actual edge grip");
    }
    else check(false, "chart popup has a resize grip");

    // Screenshot and dense chart stress fixture. The view keeps all records
    // and only reduces drawing density if needed.
    QVector<ChannelPointsChartSample> dense;
    for (int i = 0; i < 4000; ++i)
        dense.push_back({now.addSecs((i - 3999) * 60), 125000 + (i % 37) * 5 + i * 4LL,
                         i == 0 || i == 2250});
    view->setSampleHistories(dense, {});
    paint();
    check(canvas->property("sampleCount").toInt() == dense.size() &&
              canvas->property("latestBalance").toLongLong() == dense.back().balance,
          "dense history keeps the full set and correct latest balance");
    const QVector<ChannelPointsChartSample> example{
        {now.addDays(-10), 118000, true}, {now.addDays(-9).addSecs(3600), 118500, false},
        {now.addDays(-7), 124500, true}, {now.addDays(-6), 125200, false},
        {now.addDays(-4), 131000, true}, {now.addDays(-3), 129000, false},
        {now.addDays(-2), 141000, false}, {now.addDays(-1), 146000, true},
        {now.addSecs(-3600), 149000, false}, {now, 150690, false}};
    view->setSampleHistories(example, legacy);
    popup->resize(QSize(qRound(565 * popup->scale()), qRound(425 * popup->scale()))
                      .expandedTo(popup->minimumSize()).boundedTo(popup->maximumSize()));
    paint();
    check(range->width() >= range->minimumWidth() &&
              source->width() >= source->minimumWidth() &&
              reset->isVisible(),
          "period and history selectors retain enough space for unabridged text");
    check(popup->grab().save(screenshot), "chart popup screenshot rendered");
    popup->close();
}

} // namespace chatterino::preview
