#pragma once

#include "speedhistory.h"

#include <QElapsedTimer>
#include <QProgressBar>
#include <QTimer>

// The progress bar of the window with a live graph of the speed over the time of the job behind the progress.
// The x axis is the time since start() (while running at least 10 seconds, after stop() the whole job), the
// y axis goes from 0 to the scale shown in the top left corner. Without a range (busy indicator) it is an
// ordinary progress bar.
class SpeedGraph : public QProgressBar
{
    Q_OBJECT

public:
    explicit SpeedGraph(QWidget *parent = nullptr);

    // Starts a new graph (the previous one stays visible until then).
    void start();
    void addSpeed(qint64 bytesPerSecond);
    // No speed is known until the next addSpeed() (e.g. a phase that reports none).
    void pauseSpeed();
    // Stops the clock; the graph of the finished job stays.
    void stop();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qint64 now() const;

    SpeedHistory m_history;
    QElapsedTimer m_clock;
    qint64 m_stoppedAt = -1; // the time of stop(), -1 while running
    QTimer m_repaint;        // the graph moves on also without new samples
};
