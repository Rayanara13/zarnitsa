#include "Stats.h"

#include <QString>
#include <cmath>

namespace zr {

static constexpr int kMinutesKept = 1440;

TargetStats::TargetStats(int rawCapacity)
    : m_raw(qMax(16, rawCapacity))
    , m_minutes(kMinutesKept)
{
}

void TargetStats::clear()
{
    m_head = 0;
    m_count = 0;
    for (auto &b : m_minutes) b = MinuteBucket();
    m_totalSent = m_totalRecv = 0;
    m_bestEver = -1.0;
    m_lastOk = 0;
}

void TargetStats::add(qint64 tMs, bool ok, double rttMs)
{
    Sample s;
    s.t = tMs;
    s.ok = ok;
    s.rtt = ok ? float(qMax(0.0, rttMs)) : -1.f;
    m_raw[m_head] = s;
    m_head = (m_head + 1) % m_raw.size();
    if (m_count < m_raw.size()) ++m_count;

    ++m_totalSent;
    if (ok) {
        ++m_totalRecv;
        m_lastOk = tMs;
        if (m_bestEver < 0 || s.rtt < m_bestEver) m_bestEver = s.rtt;
    }

    const qint64 minute = tMs / 60000;
    MinuteBucket &b = m_minutes[int(minute % kMinutesKept)];
    if (b.minute != minute) {
        b = MinuteBucket();
        b.minute = minute;
    }
    if (b.sent < 0xFFFF) ++b.sent;
    if (ok) {
        if (b.recv < 0xFFFF) ++b.recv;
        if (b.recv == 1 || s.rtt < b.min) b.min = s.rtt;
        if (b.recv == 1 || s.rtt > b.max) b.max = s.rtt;
        b.sum += s.rtt;
    }
}

QVector<Sample> TargetStats::lastSamples(int n) const
{
    n = qBound(0, n, m_count);
    QVector<Sample> out;
    out.reserve(n);
    const int cap = m_raw.size();
    for (int i = n; i >= 1; --i)
        out.append(m_raw[(m_head - i + cap * 2) % cap]);
    return out;
}

WindowStats TargetStats::window(int n) const
{
    WindowStats w;
    const QVector<Sample> s = lastSamples(n);
    if (s.isEmpty()) return w;
    double sum = 0.0, jsum = 0.0;
    int jn = 0;
    double prev = -1.0;
    for (const Sample &x : s) {
        ++w.sent;
        if (!x.ok) continue;
        ++w.recv;
        const double r = x.rtt;
        sum += r;
        if (w.min < 0 || r < w.min) w.min = r;
        if (r > w.max) w.max = r;
        if (prev >= 0) { jsum += std::fabs(r - prev); ++jn; }
        prev = r;
    }
    w.lossPct = w.sent ? 100.0 * double(w.sent - w.recv) / double(w.sent) : 0.0;
    if (w.recv) w.avg = sum / w.recv;
    if (jn) w.jitter = jsum / jn;
    else if (w.recv) w.jitter = 0.0;
    const Sample &last = s.last();
    w.last = last.ok ? double(last.rtt) : -1.0;
    return w;
}

double TargetStats::avgOfLastOk(int n) const
{
    const int cap = m_raw.size();
    double sum = 0.0;
    int got = 0;
    for (int i = 1; i <= m_count && got < n; ++i) {
        const Sample &x = m_raw[(m_head - i + cap * 2) % cap];
        if (x.ok) { sum += x.rtt; ++got; }
    }
    return got ? sum / got : -1.0;
}

double TargetStats::uptimePct(qint64 nowMs, int minutes) const
{
    const qint64 nowMin = nowMs / 60000;
    quint64 sent = 0, recv = 0;
    for (const MinuteBucket &b : m_minutes) {
        if (b.minute < 0 || b.minute > nowMin || b.minute <= nowMin - minutes) continue;
        sent += b.sent;
        recv += b.recv;
    }
    if (!sent) return -1.0;
    return 100.0 * double(recv) / double(sent);
}

QVector<MinuteBucket> TargetStats::minuteSeries(qint64 nowMs, int minutes) const
{
    minutes = qBound(1, minutes, kMinutesKept);
    const qint64 nowMin = nowMs / 60000;
    QVector<MinuteBucket> out;
    out.reserve(minutes);
    for (qint64 m = nowMin - minutes + 1; m <= nowMin; ++m) {
        const MinuteBucket &b = m_minutes[int(((m % kMinutesKept) + kMinutesKept) % kMinutesKept)];
        if (b.minute == m) {
            out.append(b);
        } else {
            MinuteBucket empty;
            empty.minute = m;
            out.append(empty);
        }
    }
    return out;
}

double TargetStats::mos(double avgRttMs, double jitterMs, double lossPct)
{
    if (avgRttMs < 0) return 1.0;
    const double jit = jitterMs < 0 ? 0.0 : jitterMs;
    const double eff = avgRttMs + 2.0 * jit + 10.0;
    double r = eff < 160.0 ? 93.2 - eff / 40.0 : 93.2 - (eff - 120.0) / 10.0;
    r -= qBound(0.0, lossPct, 100.0) * 2.5;
    r = qBound(0.0, r, 100.0);
    const double m = 1.0 + 0.035 * r + 0.000007 * r * (r - 60.0) * (100.0 - r);
    return qBound(1.0, m, 4.5);
}

QString TargetStats::mosTitle(double m)
{
    if (m >= 4.0) return QStringLiteral("отлично");
    if (m >= 3.6) return QStringLiteral("хорошо");
    if (m >= 3.1) return QStringLiteral("терпимо");
    if (m >= 2.6) return QStringLiteral("плохо");
    return QStringLiteral("непригодно");
}

} // namespace zr
