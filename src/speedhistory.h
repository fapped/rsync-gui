#pragma once

#include <QList>
#include <QtGlobal>

// The speed of a job over time, for the speed graph in the window. Each sample holds until the next one
// (at most `staleMs`), and the speed is averaged over buckets of equal duration. When the job lasts longer
// than `maxBuckets` buckets, pairs of buckets are merged and the buckets become twice as long, so a long job
// takes bounded memory and the graph always shows the whole job.
class SpeedHistory
{
public:
    explicit SpeedHistory(int maxBuckets = 512, qint64 bucketMs = 250, qint64 staleMs = 5000);

    void clear();
    // The speed measured `msecs` after the start of the job; it holds until the next sample or pause().
    void addSample(qint64 msecs, qint64 bytesPerSecond);
    // The speed is unknown from `msecs` on until the next sample (e.g. a phase that reports no speed).
    void pause(qint64 msecs);

    // The average speed of each bucket up to `nowMsecs`, with the last sample held until then;
    // -1 for buckets without a known speed (gaps in the graph). `bucketMs` receives the duration
    // of a bucket (bucket i starts at i x bucketMs).
    QList<double> averages(qint64 nowMsecs, qint64 *bucketMs = nullptr) const;
    // The highest bucket average up to `nowMsecs` (0 without samples).
    double peak(qint64 nowMsecs) const;

    // The top of the graph's scale: the smallest 1, 1.5, 2, 3, 4, 5, 6 or 8 x 10^n bytes per second of
    // a binary unit (B, KiB, MiB, ...) that is at least `bytesPerSecond`; e.g. 37 MiB/s -> 40 MiB/s,
    // 101 MiB/s -> 150 MiB/s, 900 KiB/s -> 1 MiB/s.
    static qint64 niceScale(double bytesPerSecond);

private:
    struct Bucket
    {
        double knownMs = 0.0; // the part of the bucket with a known speed
        double bytes = 0.0;   // the speed integrated over that part (bytes/s x ms)
    };
    static void addInterval(QList<Bucket> &buckets, qint64 &bucketMs, int maxBuckets, qint64 from, qint64 to,
                            double bytesPerSecond);
    // The end of the interval of the current sample when the time is `msecs`.
    qint64 heldUntil(qint64 msecs) const;

    QList<Bucket> m_buckets;
    qint64 m_bucketMs;
    const qint64 m_initialBucketMs;
    const int m_maxBuckets;
    const qint64 m_staleMs;
    qint64 m_sampleMs = -1; // the time of the current sample, -1 when the speed is unknown
    double m_sampleSpeed = 0.0;
};
