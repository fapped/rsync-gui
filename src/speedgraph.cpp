#include "speedgraph.h"

#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace {

constexpr qint64 kMinimumSpanMs = 10000; // a running job does not fill the whole width in its first seconds

// Text over the graph, on a background of its own so that the graph does not make it unreadable.
void drawLabel(QPainter &p, const QRectF &rect, Qt::Alignment alignment, const QString &text, const QColor &color,
               const QColor &background)
{
    const QFontMetricsF metrics(p.font());
    QRectF box = metrics.boundingRect(rect, int(alignment), text).adjusted(-3, 0, 3, 0);
    box.moveTop(qMax(box.top(), rect.top()));
    box.moveLeft(qMax(box.left(), rect.left()));
    p.setPen(Qt::NoPen);
    p.setBrush(background);
    p.drawRoundedRect(box, 2, 2);
    p.setPen(color);
    p.drawText(box, Qt::AlignCenter, text);
}

// "150 MiB/s", "1.5 MiB/s": a decimal only where the scale has one.
QString speedText(qint64 bytesPerSecond)
{
    qint64 unit = 1;
    while (bytesPerSecond / unit >= 1024)
        unit *= 1024;
    const int precision = bytesPerSecond % unit == 0 ? 0 : 1;
    return SpeedGraph::tr("%1/s").arg(QLocale().formattedDataSize(bytesPerSecond, precision));
}

} // namespace

SpeedGraph::SpeedGraph(QWidget *parent)
    : QProgressBar(parent)
{
    setToolTip(tr("Progress, with the speed over the time of the job behind it. "
                  "The top of the graph is the speed in the top left corner."));
    m_repaint.setInterval(500);
    connect(&m_repaint, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void SpeedGraph::start()
{
    m_history.clear();
    m_clock.start();
    m_stoppedAt = -1;
    m_repaint.start();
    update();
}

void SpeedGraph::addSpeed(qint64 bytesPerSecond)
{
    if (!m_clock.isValid() || m_stoppedAt >= 0)
        return;
    m_history.addSample(now(), bytesPerSecond);
    update();
}

void SpeedGraph::pauseSpeed()
{
    if (m_clock.isValid() && m_stoppedAt < 0)
        m_history.pause(now());
}

void SpeedGraph::stop()
{
    if (!m_clock.isValid() || m_stoppedAt >= 0)
        return;
    pauseSpeed();
    m_stoppedAt = m_clock.elapsed();
    m_repaint.stop();
    update();
}

qint64 SpeedGraph::now() const
{
    if (m_stoppedAt >= 0)
        return m_stoppedAt;
    return m_clock.isValid() ? m_clock.elapsed() : 0;
}

QSize SpeedGraph::sizeHint() const
{
    QSize size = QProgressBar::sizeHint();
    size.setHeight(qMax(size.height(), fontMetrics().height() * 2 + 8));
    return size;
}

QSize SpeedGraph::minimumSizeHint() const
{
    QSize size = QProgressBar::minimumSizeHint();
    size.setHeight(sizeHint().height());
    return size;
}

void SpeedGraph::paintEvent(QPaintEvent *event)
{
    if (minimum() >= maximum()) {
        QProgressBar::paintEvent(event); // the busy indicator of the style
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    const QColor highlight = pal.color(QPalette::Highlight);
    QColor border = pal.color(QPalette::WindowText);
    border.setAlphaF(0.3f);

    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(border);
    p.setBrush(pal.color(QPalette::Base));
    p.drawRoundedRect(frame, 3, 3);
    const QRectF area = frame.adjusted(1, 1, -1, -1);
    QPainterPath clip;
    clip.addRoundedRect(area, 2, 2);
    p.setClipPath(clip);

    // The progress, like the chunk of a progress bar.
    const double fraction = qBound(0.0, double(value() - minimum()) / double(maximum() - minimum()), 1.0);
    QColor progressColor = highlight;
    progressColor.setAlphaF(0.22f);
    p.fillRect(QRectF(area.left(), area.top(), area.width() * fraction, area.height()), progressColor);

    // The speed: one filled shape per stretch of known speed.
    const qint64 time = now();
    qint64 bucketMs = 1;
    const QList<double> speeds = m_history.averages(time, &bucketMs);
    const double peak = speeds.isEmpty() ? 0.0 : *std::max_element(speeds.cbegin(), speeds.cend());
    qint64 scale = 0;
    if (peak > 0.0) {
        scale = SpeedHistory::niceScale(peak);
        // A finished job fills the whole width.
        const double span = double(m_stoppedAt >= 0 ? qMax<qint64>(time, 1) : qMax(time, kMinimumSpanMs));
        const auto xAt = [&](qint64 ms) { return area.left() + area.width() * double(ms) / span; };
        const auto yAt = [&](double speed) { return area.bottom() - area.height() * qMin(1.0, speed / double(scale)); };
        QPainterPath line;
        QPainterPath shape;
        for (qsizetype first = 0; first < speeds.size();) {
            if (speeds.at(first) < 0.0) {
                ++first;
                continue;
            }
            qsizetype end = first;
            while (end < speeds.size() && speeds.at(end) >= 0.0)
                ++end;
            const double x0 = xAt(first * bucketMs);
            const double x1 = xAt(qMin(end * bucketMs, time));
            QPolygonF points;
            points << QPointF(x0, yAt(speeds.at(first)));
            for (qsizetype i = first; i < end; ++i)
                points << QPointF(xAt(qMin(i * bucketMs + bucketMs / 2, time)), yAt(speeds.at(i)));
            points << QPointF(x1, yAt(speeds.at(end - 1)));
            line.addPolygon(points);
            points << QPointF(x1, area.bottom()) << QPointF(x0, area.bottom());
            shape.addPolygon(points);
            shape.closeSubpath();
            first = end;
        }
        QColor shapeColor = highlight;
        shapeColor.setAlphaF(0.35f);
        p.fillPath(shape, shapeColor);
        p.strokePath(line, QPen(highlight, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }

    QColor labelBackground = pal.color(QPalette::Base);
    labelBackground.setAlphaF(0.75f);
    const QRectF textArea = area.adjusted(3, 2, -3, -2);
    if (scale > 0) {
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        p.setFont(small);
        QColor scaleColor = pal.color(QPalette::Text);
        scaleColor.setAlphaF(0.7f);
        drawLabel(p, textArea, Qt::AlignLeft | Qt::AlignTop, speedText(scale), scaleColor, labelBackground);
        p.setFont(font());
    }
    if (isTextVisible() && !text().isEmpty())
        drawLabel(p, textArea, Qt::AlignCenter, text(), pal.color(QPalette::Text), labelBackground);
}
