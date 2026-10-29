#include "speedhistory.h"

#include <algorithm>
#include <cmath>

SpeedHistory::SpeedHistory(int maxBuckets, qint64 bucketMs, qint64 staleMs)
    : m_bucketMs(bucketMs)
    , m_initialBucketMs(bucketMs)
    , m_maxBuckets(qMax(2, maxBuckets))
    , m_staleMs(staleMs)
{
}

void SpeedHistory::clear()
{
    m_buckets.clear();
    m_bucketMs = m_initialBucketMs;
    m_sampleMs = -1;
}

void SpeedHistory::addSample(qint64 msecs, qint64 bytesPerSecond)
{
    msecs = qMax<qint64>(0, msecs);
    if (m_sampleMs >= 0) {
        msecs = qMax(msecs, m_sampleMs);
        addInterval(m_buckets, m_bucketMs, m_maxBuckets, m_sampleMs, heldUntil(msecs), m_sampleSpeed);
    }
    m_sampleMs = msecs;
    m_sampleSpeed = double(qMax<qint64>(0, bytesPerSecond));
}

void SpeedHistory::pause(qint64 msecs)
{
    if (m_sampleMs < 0)
        return;
    addInterval(m_buckets, m_bucketMs, m_maxBuckets, m_sampleMs, heldUntil(msecs), m_sampleSpeed);
    m_sampleMs = -1;
}

qint64 SpeedHistory::heldUntil(qint64 msecs) const
{
    return qBound(m_sampleMs, msecs, m_sampleMs + m_staleMs);
}

void SpeedHistory::addInterval(QList<Bucket> &buckets, qint64 &bucketMs, int maxBuckets, qint64 from, qint64 to,
                               double bytesPerSecond)
{
    if (to <= from)
        return;
    while ((to - 1) / bucketMs >= maxBuckets) {
        QList<Bucket> merged((buckets.size() + 1) / 2);
        for (qsizetype i = 0; i < buckets.size(); ++i) {
            merged[i / 2].knownMs += buckets.at(i).knownMs;
            merged[i / 2].bytes += buckets.at(i).bytes;
        }
        buckets = std::move(merged);
        bucketMs *= 2;
    }
    const qsizetype last = (to - 1) / bucketMs;
    if (buckets.size() <= last)
        buckets.resize(last + 1);
    for (qsizetype i = from / bucketMs; i <= last; ++i) {
        const qint64 overlap = qMin(to, (i + 1) * bucketMs) - qMax(from, i * bucketMs);
        buckets[i].knownMs += double(overlap);
        buckets[i].bytes += double(overlap) * bytesPerSecond;
    }
}

QList<double> SpeedHistory::averages(qint64 nowMsecs, qint64 *bucketMs) const
{
    QList<Bucket> buckets = m_buckets;
    qint64 ms = m_bucketMs;
    if (m_sampleMs >= 0)
        addInterval(buckets, ms, m_maxBuckets, m_sampleMs, heldUntil(nowMsecs), m_sampleSpeed);
    if (bucketMs)
        *bucketMs = ms;
    QList<double> result;
    result.reserve(buckets.size());
    for (const Bucket &bucket : std::as_const(buckets))
        result << (bucket.knownMs > 0.0 ? bucket.bytes / bucket.knownMs : -1.0);
    return result;
}

double SpeedHistory::peak(qint64 nowMsecs) const
{
    const QList<double> values = averages(nowMsecs);
    return values.isEmpty() ? 0.0 : qMax(0.0, *std::max_element(values.cbegin(), values.cend()));
}

qint64 SpeedHistory::niceScale(double bytesPerSecond)
{
    if (bytesPerSecond <= 1.0)
        return 1;
    qint64 unit = 1;
    while (bytesPerSecond / double(unit) >= 1000.0 && unit < (qint64(1) << 60))
        unit *= 1024;
    const double mantissa = bytesPerSecond / double(unit);
    for (const double power : {1.0, 10.0, 100.0}) {
        for (const double step : {1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0}) {
            if (unit == 1 && step * power != std::floor(step * power))
                continue; // no fractions of a byte
            if (mantissa <= step * power)
                return qint64(step * power * double(unit));
        }
    }
    return 1024 * unit; // e.g. 900 KiB/s -> 1 MiB/s
}
